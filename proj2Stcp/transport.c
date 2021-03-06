/*
 * transport.c 
 *
 * This file implements the STCP layer that sits between the
 * mysocket and network layers. You are required to fill in the STCP
 * functionality in this file. 
 *
 */


#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/time.h>
#include <unistd.h>
#include "mysock.h"
#include "mysock_impl.h"
#include "stcp_api.h"
#include "simclist.h"
#include "transport.h"

#define MAXPAYLOAD 536
#define LOCALRECVLEN 3072
#define CONGESTIONWIN 3072
#define TIMEOUTSECS 1

enum { CSTATE_LISTEN,
       CSTATE_SYN_RECEIVED,
       CSTATE_SYN_SENT,
       CSTATE_ESTABLISHED,
       CSTATE_FINWAIT1,
       CSTATE_FINWAIT2,
       CSTATE_CLOSE_WAIT,
       CSTATE_LAST_ACK,
       CSTATE_CLOSED };


/* this structure is global to a mysocket descriptor */
typedef struct
{
    bool_t done;    /* TRUE once connection is closed */

    int connection_state;   /* state of the connection (established, etc.) */
    tcp_seq initial_sequence_num;

    /* network send and recv buffers */
    char networkSendBuffer[LOCALRECVLEN];
    char networkRecvBuffer[LOCALRECVLEN];
    /* app send and recv buffers */
    char appRecvBuffer[LOCALRECVLEN];
    char appSendBuffer[LOCALRECVLEN];
    int ourWindowSize;
    int theirWindowSize;

    int lastByteAckd;
    int lastByteWrittenToNet;

    int nextByteExpected;
    int lastByteReceived;
    tcp_seq nextSeqExpected;

    tcp_seq lastSentSeq;
    tcp_seq ackNum;
    /* any other connection-wide global variables go here */
} context_t;


/* struct to hold packet data */
struct packetData
{
  int packetLen; /* length of packet */
  int timeoutPid; /* process id of thread running timeout for packet */
  int numTimeout;  /* whether or not timeout has occurred */
  int ackd;  /* if packet has been ACKed by peer */
  tcp_seq seqNum;  /* sequence number of packet */
  tcp_seq ackNum;  /* ACK number corresponding to packet */
  int buffLoc;  /* if data packet received, index of packet in recvBuffer */
  char packet[MAXPAYLOAD];  /* packet */
  struct tcphdr * header;  /* header */
  struct timespec waitSecs; /* timeout */  
};

typedef struct packetData packetData;


/* struct to hold arguments for run_timeout function */
struct timeoutArgs
{
  packetData * packet;
  list_t * sentPackets;
  context_t * context;
};

typedef struct timeoutArgs timeoutArgs;


static void generate_initial_seq_num(context_t *ctx);
void unpack_and_recv_data(mysocket_t sd, char * recvBuf, int recvBufLen, list_t * sentPackets, list_t * outOfOrderBuf, context_t * ctx);
void pack_and_send_data(mysocket_t sd, char * recvBuf, int recvBufLen, list_t * sentPackets, context_t *ctx);
void stcp_fin_received_from_app(mysocket_t sd, list_t * sentPackets, context_t * ctx);
static void control_loop(mysocket_t sd, context_t *ctx, list_t *sentPackets, list_t *outOfOrderBuf);
int min(int x, int y);
void send_packet(mysocket_t sd, packetData * packData, list_t * sentPackets, context_t * ctx);
int seeker(const void *el, const void * key);
int seq_comparator(const void *a, const void *b);
size_t packet_data_size(const void *el);
int timespec_compare(const struct timespec* left, const struct timespec* right);


/* initialise the transport layer, and start the main loop, handling
 * any data from the peer or the application.  this function should not
 * return until the connection is closed.
 */
void transport_init(mysocket_t sd, bool_t is_active)
{
    context_t *ctx;

    ctx = (context_t *) calloc(1, sizeof(context_t));
    assert(ctx);

    generate_initial_seq_num(ctx);

    /* initialize send and receive buffer indexes */
    ctx->ourWindowSize = LOCALRECVLEN;
    /* should their window size be set now? */
    ctx->theirWindowSize = LOCALRECVLEN;
    ctx->lastByteAckd = 0;
    ctx->lastByteWrittenToNet = 0;

    ctx->nextByteExpected = 0;
    ctx->lastByteReceived = 0;

    /* Send a SYN packet here if is_active, or wait for one
     * to arrive if !is_active.  after the handshake completes, unblock the
     * application with stcp_unblock_application(sd).
     */
    mysock_context_t *sockCtx = _mysock_get_context(sd);
    struct tcphdr * sendPacket = malloc(sizeof(struct tcphdr));
    struct tcphdr * recvPacket = malloc(sizeof(struct tcphdr));

    /* initialize global list of sent packets which have not received acks */
    list_t * sentPackets = (list_t *) malloc(sizeof(list_t));
    list_init(sentPackets);
    list_attributes_copy(sentPackets, packet_data_size, 1);
    list_attributes_comparator(sentPackets, seq_comparator);
    list_attributes_seeker(sentPackets, (element_seeker) seeker);
 
    list_t * outOfOrderBuf = (list_t *) malloc(sizeof(list_t));
    list_init(outOfOrderBuf);
    list_attributes_copy(outOfOrderBuf, packet_data_size, 1);
    list_attributes_comparator(outOfOrderBuf, seq_comparator);
    list_attributes_seeker(outOfOrderBuf, (element_seeker) seeker);


    if (sockCtx->is_active)
    {
      /* send a SYN packet */
      memset(sendPacket, 0, sizeof(struct tcphdr));
      /* set sequence # */
      sendPacket->th_seq = ctx->initial_sequence_num;
      /* no options; data offset is 5 words */
      sendPacket->th_off = 5;
      sendPacket->th_flags = TH_SYN;
      /* set window size */
      sendPacket->th_win = ctx->ourWindowSize;
      stcp_network_send(sd, (void *) sendPacket, sizeof(struct tcphdr), NULL);
      ctx->lastSentSeq = ctx->initial_sequence_num + 1;
      ctx->connection_state = CSTATE_SYN_SENT;
    }
    else
    {
      ctx->connection_state = CSTATE_LISTEN;
    }
    /* wait for packets until connection established */
    while (ctx->connection_state != CSTATE_ESTABLISHED)
    {
      memset(recvPacket, 0, sizeof(struct tcphdr));
      stcp_network_recv(sd, recvPacket, sizeof(struct tcphdr));
      assert(recvPacket);

      if (((TH_ACK & recvPacket->th_flags) || ((TH_ACK & recvPacket->th_flags) && (TH_SYN & recvPacket->th_flags))) && 
          (ctx->connection_state == CSTATE_SYN_RECEIVED))
      {
        if ((TH_ACK & recvPacket->th_flags) && (TH_SYN & recvPacket->th_flags))
        { /* received SYN ACK from peer */
          /* verify context's SEQ number */
          assert((ctx->lastSentSeq) == recvPacket->th_ack);
          ctx->initial_sequence_num = ctx->lastSentSeq;
          /* set ACK number */
          ctx->ackNum = recvPacket->th_seq + 1;
          ctx->nextSeqExpected = recvPacket->th_seq + 1;
          /* set window size of peer */
          ctx->theirWindowSize = min(CONGESTIONWIN, recvPacket->th_win);

          /* send ACK */
          memset(sendPacket, 0, sizeof(struct tcphdr));
          /* set sequence # */
          sendPacket->th_seq = ctx->initial_sequence_num;
          /* set ACK # */
          sendPacket->th_ack = ctx->ackNum;
          /* no options; data offset is 5 words */
          sendPacket->th_off = 5;
          sendPacket->th_flags = TH_ACK;
          /*set my window size */
          sendPacket->th_win = ctx->ourWindowSize;
          stcp_network_send(sd, (void *) sendPacket, sizeof(struct tcphdr), NULL);
        }
        else
        { /* only received ACK from peer */
          /* verify context's SEQ number */
          assert((ctx->lastSentSeq) == recvPacket->th_ack);
          ctx->initial_sequence_num = ctx->lastSentSeq;
          /* set window size of peer */
          ctx->theirWindowSize = min(CONGESTIONWIN, recvPacket->th_win);
        }
        ctx->connection_state = CSTATE_ESTABLISHED;
      }
      else if ((TH_SYN & recvPacket->th_flags) && 
              ((ctx->connection_state == CSTATE_SYN_SENT) || (ctx->connection_state == CSTATE_LISTEN)))
      {
        /* set ACK number */
        ctx->ackNum = recvPacket->th_seq + 1;
        ctx->nextSeqExpected = recvPacket->th_seq + 1;
        /* set window size of peer */
        ctx->theirWindowSize = min(CONGESTIONWIN, recvPacket->th_win);

        /* send SYN ACK */
        memset(sendPacket, 0, sizeof(struct tcphdr));
        /* set sequence # */
        sendPacket->th_seq = ctx->initial_sequence_num;
        /* set ACK # */
        sendPacket->th_ack = ctx->ackNum;
        /* no options; data offset is 5 words */
        sendPacket->th_off = 5;
        sendPacket->th_flags = TH_SYN | TH_ACK;
        /* set our window size */
        sendPacket->th_win = ctx->ourWindowSize;
        stcp_network_send(sd, (void *) sendPacket, sizeof(struct tcphdr), NULL);
        ctx->lastSentSeq = ctx->initial_sequence_num + 1;
        ctx->connection_state = CSTATE_SYN_RECEIVED;
      }
      else if (((TH_SYN & recvPacket->th_flags) && (TH_ACK & recvPacket->th_flags)) &&
               (ctx->connection_state == CSTATE_SYN_SENT))
      {
        /* verify context's SEQ number */
        assert((ctx->lastSentSeq) == recvPacket->th_ack);
        ctx->initial_sequence_num = ctx->lastSentSeq;
        /* set ACK number */
        ctx->ackNum = recvPacket->th_seq + 1;
        ctx->nextSeqExpected = recvPacket->th_seq + 1;
        /* set window size of peer */
        ctx->theirWindowSize = min(CONGESTIONWIN, recvPacket->th_win);

        /* send ACK */
        memset(sendPacket, 0, sizeof(struct tcphdr));
        /* set sequence # */
        sendPacket->th_seq = ctx->initial_sequence_num;
        /* set ACK # */
        sendPacket->th_ack = ctx->ackNum;
        /* no options; data offset is 5 words */
        sendPacket->th_off = 5;
        sendPacket->th_flags = TH_ACK;
        /*set my window size */
        sendPacket->th_win = ctx->ourWindowSize;
        stcp_network_send(sd, (void *) sendPacket, sizeof(struct tcphdr), NULL);
        ctx->connection_state = CSTATE_ESTABLISHED;
      }
    }

    ctx->connection_state = CSTATE_ESTABLISHED;
    /* allow mysocket descriptor to track STCP context */
    stcp_set_context(sd, (void *) ctx);

    /* unblock the app */    
    stcp_unblock_application(sd);

    control_loop(sd, ctx, sentPackets, outOfOrderBuf);

    /* do any cleanup here */
    free(sendPacket);
    free(recvPacket);

    list_destroy(sentPackets);
    free(sentPackets);
    list_destroy(outOfOrderBuf);
    free(outOfOrderBuf);
    free(ctx);
}


/* generate random initial sequence number for an STCP connection */
static void generate_initial_seq_num(context_t *ctx)
{
    assert(ctx);
#ifdef FIXED_INITNUM
    /* please don't change this! */
    ctx->initial_sequence_num = 1;
#else
    /* initialize random seed */
    srand(time(NULL));
    /* generate random number between 0 and 255 */
    int initSeq = rand() % 256;
    ctx->initial_sequence_num = initSeq;
#endif
}


/* unpack_and_recv_data() takes one network packet sent, removes the data
 * from the packet, sends data to the app,
 * Sends the data to the app. 
 * Sends an ACK message across the network for all SYN packs set
 * Also catches cases for FIN packets sent.
 */
void unpack_and_recv_data(mysocket_t sd, char * recvBuf, int recvBufLen, list_t * sentPackets, list_t * outOfOrderBuf, context_t * ctx)
{
  int headerLen = sizeof(struct tcphdr);
  int packDataLen;
  int totalPackLen;
  char * packet;
  struct tcphdr * header;
  char * data;
  struct tcphdr * ackResponse;
  ackResponse = (struct tcphdr *) malloc(sizeof(struct tcphdr));
  memset(ackResponse, 0, sizeof(struct tcphdr));

  /* set packet data length and packet length */
  totalPackLen = recvBufLen;
  packDataLen = recvBufLen - headerLen;

  /* point packet to appropriate position in recvBuf */
  packet = recvBuf;
  /* obtain header */
  header = (struct tcphdr *) packet;
  /* update peer's window size */
  ctx->theirWindowSize = min(CONGESTIONWIN, header->th_win);
  if ((header->th_flags & TH_SYN) || (header->th_flags & TH_FIN))
  {
    /* if header's SEQ number less than our ACK number, send ACK&discard data*/
    if (header->th_seq < ctx->ackNum)
    {
      ctx->ourWindowSize += totalPackLen;
      /* send ACK */
      memset(ackResponse, 0, headerLen);
      ackResponse->th_seq = ctx->initial_sequence_num;
      /* set ACK #; we're still expecting the last set ack num as recv'd seq */
      ackResponse->th_ack = ctx->ackNum;
      /* no options; data offset is 5 words */
      ackResponse->th_off = 5;
      ackResponse->th_flags = TH_ACK;
      ackResponse->th_win = ctx->ourWindowSize;
      stcp_network_send(sd, (void *) ackResponse, headerLen, NULL);
      free(ackResponse);
      return;
    }

    /* if packet is already in outOfOrderBuf, drop packet */
    packetData * inBufPacket = (packetData *) list_seek(outOfOrderBuf, &header->th_ack);
    if (inBufPacket != NULL)
    {
      ctx->ourWindowSize += totalPackLen;
      /* send ACK */
      memset(ackResponse, 0, headerLen);
      ackResponse->th_seq = ctx->initial_sequence_num;
      /* set ACK #; we're still expecting the last set ack num as recv'd seq */
      ackResponse->th_ack = ctx->ackNum;
      /* no options; data offset is 5 words */
      ackResponse->th_off = 5;
      ackResponse->th_flags = TH_ACK;
      ackResponse->th_win = ctx->ourWindowSize;
      stcp_network_send(sd, (void *) ackResponse, headerLen, NULL);
      free(ackResponse);
      return;
    }
    /* if packet sent is out of order, insert into buffer */
    if (ctx->nextSeqExpected != header->th_seq)
    {
      /* make packData struct */
      ctx->ourWindowSize += totalPackLen;
      packetData * packData;
      packData = (packetData *) malloc(sizeof(packetData));
      memset(packData, 0, sizeof(packetData));
      packData->packetLen = totalPackLen;
      packData->numTimeout = 0;
      packData->seqNum = header->th_seq;
      packData->ackNum = packData->seqNum + packData->packetLen;
      memcpy(packData->packet, packet, packData->packetLen);
      packData->header = (struct tcphdr *) packData->packet;

      list_append(outOfOrderBuf, packData);
      list_sort(outOfOrderBuf, 1);
    }
    /* if received expected data packet, send packet to app and check if 
       subsequent packets are in outOfOrderBuf */
    if (ctx->nextSeqExpected == header->th_seq)
    {
      data = &packet[headerLen];

      /* send the data to the app */
      if (header->th_flags == TH_SYN)
      {
        stcp_app_send(sd, data, packDataLen);
        /*ctx->nextSeqExpected = header->th_seq + totalPackLen;*/
        ctx->nextSeqExpected += totalPackLen;
        ctx->ackNum = header->th_seq + totalPackLen;
        ctx->ourWindowSize += totalPackLen;
      }
      else if (header->th_flags == TH_FIN)
      {
        stcp_fin_received(sd);
        if ((ctx->connection_state == CSTATE_FINWAIT1) ||
            (ctx->connection_state == CSTATE_FINWAIT2))
        {
          ctx->connection_state = CSTATE_CLOSED;
          ctx->done = 1;
        }
        else
        {
          ctx->connection_state = CSTATE_CLOSE_WAIT;
        }
      }

      /* send all subsequent packets in outOfOrderBuf and delete from buffer
         upon sending */
      int deleteFromListIndx = 0;
      int deleteFromList[list_size(outOfOrderBuf)];
       
      list_iterator_start(outOfOrderBuf);
      while(list_iterator_hasnext(outOfOrderBuf))
      {
        packetData * packData = (packetData *) list_iterator_next(outOfOrderBuf);
        int packIndex;
        if (packData->seqNum == header->th_seq)
        {
          packIndex = list_locate(outOfOrderBuf, packData);
          deleteFromList[deleteFromListIndx] = packIndex;
          deleteFromListIndx++;
        }
        else if (packData->seqNum > header->th_seq)
        {
          packIndex = list_locate(outOfOrderBuf, packData);
          deleteFromList[deleteFromListIndx] = packIndex;
          deleteFromListIndx++;
          if (packData->header->th_flags == TH_SYN)
          {
            data = &packData->packet[headerLen];
            stcp_app_send(sd, data, packData->packetLen - headerLen);
            /*ctx->nextSeqExpected = packData->header->th_seq + packData->packetLen;*/
            ctx->nextSeqExpected += packData->packetLen;
            ctx->ackNum = packData->header->th_seq + packData->packetLen;
          }
          else if (packData->header->th_flags == TH_FIN)
          {
            stcp_fin_received(sd);
            if ((ctx->connection_state == CSTATE_FINWAIT1) ||
                (ctx->connection_state == CSTATE_FINWAIT2))
            {
              ctx->connection_state = CSTATE_CLOSED;
              ctx->done = 1;
            }
            else
            {
              ctx->connection_state = CSTATE_CLOSE_WAIT;
            }
          }
        }
      }
      /* delete sent packets from outOfOrderBuf */
      int deleteIndx = deleteFromListIndx - 1;
      for (; deleteIndx > -1; deleteIndx--)
      {
        list_delete_at(outOfOrderBuf, deleteFromList[deleteIndx]);
        list_sort(outOfOrderBuf, 1);
      }
    }
    /* send ACK */
    memset(ackResponse, 0, headerLen);
    ackResponse->th_seq = ctx->initial_sequence_num;
    ackResponse->th_ack = ctx->ackNum;
    /* no options; data offset is 5 words */
    ackResponse->th_off = 5;
    ackResponse->th_flags = TH_ACK;
    ackResponse->th_win = ctx->ourWindowSize;
    stcp_network_send(sd, (void *) ackResponse, headerLen, NULL);
  }
  else if (header->th_flags == TH_ACK)
  {
    /* if receiving ACK, check to see if ACK number matches the ackNum
       of any packets in sentPackets */
    packetData * packData = (packetData *) list_seek(sentPackets, &header->th_ack);
    if (packData != NULL)
    {
      /* notify that the packet has been ackd */
      packData->ackd = 1;
      /* If last ack response corresponds to an item in outOfOrderBuf,
         mark each packet prior to the last packet sent to the app as ackd */
      list_iterator_start(sentPackets);
      while (list_iterator_hasnext(sentPackets))
      {
        packetData * beforeAckd = (packetData *) list_iterator_next(sentPackets);
        if (beforeAckd->seqNum < packData->seqNum)
        {
          beforeAckd->ackd = 1;
        }
        else
        {
          break;
        }
      }
      list_iterator_stop(sentPackets);
    }
    ctx->ourWindowSize += totalPackLen;
  }
  free(ackResponse);
}


/* pack_and_send_data() puts all data received by application
 * (in recvBuf), creates packets containing the data and 
 * a corresponding header, puts those packets into sendBuf,
 * and sends all of the packets.
 */
void pack_and_send_data(mysocket_t sd, char * recvBuf, int recvBufLen, list_t * sentPackets, context_t * ctx)
{
  int maxPackDataLen = MAXPAYLOAD - sizeof(struct tcphdr);
  int headerLen = sizeof(struct tcphdr);
  int packDataLen;
  int totalPackLen;

  struct tcphdr * header;
  header = (struct tcphdr *) malloc(headerLen);
  memset(header, 0, headerLen);

  char * packet;
  packet = (char *) malloc(sizeof(char)*MAXPAYLOAD);
  memset(packet, 0, sizeof(char)*MAXPAYLOAD);
  
  int recvBufIdx = 0;
  while (recvBufIdx < recvBufLen)
  {
    /* set packet data length */
    if ((recvBufLen - (recvBufIdx+1)) < maxPackDataLen)
    {
      packDataLen = recvBufLen - recvBufIdx;
    }
    else
    {
      packDataLen = maxPackDataLen;
    }
    /* set packet length*/
    totalPackLen = packDataLen + headerLen;

    /* create header */
    header->th_seq = ctx->initial_sequence_num;
    /* no options; data offset is 5 words */
    header->th_off = 5;
    header->th_flags = TH_SYN;
    header->th_win = ctx->ourWindowSize;
    ctx->lastSentSeq = ctx->initial_sequence_num + totalPackLen;

    /* construct packet from header and data */
    memcpy(packet, header, headerLen);
    memcpy(packet+headerLen, &recvBuf[recvBufIdx], packDataLen);

    recvBufIdx += packDataLen;

    packetData * packData;
    packData = (packetData *) malloc(sizeof(packetData));
    memset(packData, 0, sizeof(packetData));
    packData->packetLen = totalPackLen;
    packData->numTimeout = 0;
    packData->seqNum = header->th_seq;
    packData->ackNum = packData->seqNum + packData->packetLen;
    memcpy(packData->packet, packet, packData->packetLen);
    packData->header = (struct tcphdr *) packData->packet;
    packData->ackd = 0;

    /* send packet and start timeout for packet */
    send_packet(sd, packData, sentPackets, ctx);

    ctx->initial_sequence_num += totalPackLen;
    memset(header, 0, headerLen);
    memset(packet, 0, sizeof(char)*MAXPAYLOAD);
  }
  free(header);
  free(packet);
}


/* send_packet(): Send one packet and start timeout thread for this packet */
void send_packet(mysocket_t sd, packetData * packData, list_t * sentPackets, context_t * ctx)
{
  /* start the timeout */
  struct timeval elapsed;
  (void) gettimeofday(&elapsed, NULL);
  TIMEVAL_TO_TIMESPEC(&elapsed, &packData->waitSecs);
  /* Add the offset for timeout in seconds */
  packData->waitSecs.tv_sec += TIMEOUTSECS;

  packetData * isInSentBuf = (packetData *) list_seek(sentPackets, &packData->ackNum);
  if (isInSentBuf == NULL)
  {     
    /* insert the packet into sentPackets global list and sort list */
    list_append(sentPackets, packData);
    list_sort(sentPackets, 1);
  }
  stcp_network_send(sd, packData->packet, packData->packetLen, NULL);
  
  return;
}


/* control_loop() is the main STCP loop; it repeatedly waits for one of the
 * following to happen:
 *   - incoming data from the peer
 *   - new data from the application (via mywrite())
 *   - the socket to be closed (via myclose())
 *   - a timeout
 */
static void control_loop(mysocket_t sd, context_t *ctx, list_t *sentPackets, list_t *outOfOrderBuf)
{
    assert(ctx);

    while (!ctx->done)
    {
        unsigned int event;
        size_t len = 0;

        int deleteFromListIndx = 0;
        int deleteFromList[list_size(sentPackets)];

        list_sort(sentPackets, 1);
        /* look for packets which have already been ackd*/
        list_iterator_start(sentPackets);
        while (list_iterator_hasnext(sentPackets))
        {
          packetData * packData = (packetData *) list_iterator_next(sentPackets);
          if (packData->ackd == 1)
          {
              if ((packData->header->th_flags == TH_FIN) &&
                 (ctx->connection_state == CSTATE_FINWAIT1))
              {
                ctx->connection_state = CSTATE_FINWAIT2;
              }
              if ((packData->header->th_flags == TH_FIN) &&
                (ctx->connection_state == CSTATE_LAST_ACK))
              {
                ctx->connection_state = CSTATE_CLOSED;
                ctx->done = 1;
              }
              int packetIndex = list_locate(sentPackets, packData);
              packData = list_get_at(sentPackets, packetIndex);
              deleteFromList[deleteFromListIndx] = packetIndex;
              deleteFromListIndx++;
          }
        }
        list_iterator_stop(sentPackets);
 
        /* delete all ackd packets from sentPackets list */ 
        int deleteIndx = deleteFromListIndx - 1;
        for (; deleteIndx > -1; deleteIndx--)
        {
          packetData * packet = list_get_at(sentPackets, deleteFromList[deleteIndx]);
          if (packet)
          {
            list_delete_at(sentPackets, deleteFromList[deleteIndx]);
            list_sort(sentPackets, 1);
          }
        }

        /* BEGIN LOOKING FOR LOWEST TIMESPEC */
        int smallestTimeoutAck = -1;
        packetData * smallestTimeoutPack;
        list_iterator_start(sentPackets);
        if (list_iterator_hasnext(sentPackets))
        {
          smallestTimeoutPack = (packetData *) list_iterator_next(sentPackets);
          smallestTimeoutAck = smallestTimeoutPack->ackNum;
          while (list_iterator_hasnext(sentPackets))
          {
            packetData * packData = (packetData *) list_iterator_next(sentPackets);
            if (timespec_compare(&smallestTimeoutPack->waitSecs, &packData->waitSecs) == 1)
              smallestTimeoutAck = packData->ackNum;
          }
        }
        list_iterator_stop(sentPackets);    

        struct timespec * timeSpecOrNull;
        /* Pass the lowest timeout timespec of sent packets into wait_for_event*/
        if (smallestTimeoutAck != -1)
        {
          smallestTimeoutPack = (packetData *) list_seek(sentPackets, &smallestTimeoutAck);
          timeSpecOrNull = &smallestTimeoutPack->waitSecs;
        }
        else
        {
          timeSpecOrNull = NULL;
        }
        


        event = stcp_wait_for_event(sd, ANY_EVENT, timeSpecOrNull);
        if (event & NETWORK_DATA)
        {
          /* if recv buffer is has reached its end, read into start of buffer */
          if (ctx->lastByteReceived >= LOCALRECVLEN)
          {
            int maxPackLen = min(ctx->ourWindowSize, MAXPAYLOAD);
            if (ctx->ourWindowSize - MAXPAYLOAD < 0)
            {
              maxPackLen = min(maxPackLen, MAXPAYLOAD - ctx->ourWindowSize);
            }
            ctx->lastByteReceived = 0;
            len = stcp_network_recv(sd, ctx->networkRecvBuffer, maxPackLen);
          }
          else
          {
            int maxPackLen;
            /* guarantee there's enough room for entire packet in window */
            if (ctx->ourWindowSize < LOCALRECVLEN - ctx->lastByteReceived)
            {
              maxPackLen = ctx->ourWindowSize;
            }
            else
            {
              maxPackLen = LOCALRECVLEN - ctx->lastByteReceived;
            }
            maxPackLen = min(maxPackLen, MAXPAYLOAD);
            maxPackLen = min(maxPackLen, ctx->ourWindowSize);
            if (ctx->ourWindowSize - MAXPAYLOAD < 0)
            {
              maxPackLen = min(maxPackLen, MAXPAYLOAD - ctx->ourWindowSize);
            }
            len = stcp_network_recv(sd, ctx->networkRecvBuffer, maxPackLen);
          }
          /* accept less data into recv buffer; adjust window size */
          ctx->ourWindowSize -= len;
          ctx->lastByteReceived += len;
          /* send all packets to app and send all corresponding ACKs through network */
          if (len > 0)
            unpack_and_recv_data(sd, ctx->networkRecvBuffer, len, sentPackets, outOfOrderBuf, ctx);
        }
        /* check whether it was the network, app, or a close request */
        if (event == TIMEOUT)
        {
          /* don't process any packets received while in CLOSED states */
          /* Drop packet if it has already timed out 6 times */
          if ((ctx->connection_state == CSTATE_CLOSED) ||
              (ctx->connection_state == CSTATE_LAST_ACK) ||
              (ctx->connection_state == CSTATE_FINWAIT1) ||
              (ctx->connection_state == CSTATE_FINWAIT2) ||
              (smallestTimeoutPack->numTimeout == 6))
          {  /* if connection is closed, assume all packets already sent to peer*/
            if ((smallestTimeoutPack->header->th_flags == TH_FIN) &&
                ((ctx->connection_state == CSTATE_LAST_ACK) ||
                (ctx->connection_state == CSTATE_FINWAIT1) ||
                (ctx->connection_state == CSTATE_FINWAIT2)))
            {
              ctx->connection_state = CSTATE_CLOSED;
              ctx->done = 1;
            }
            int packetIndex = list_locate(sentPackets, smallestTimeoutPack);
            list_delete_at(sentPackets, packetIndex);
            list_sort(sentPackets, 1);
          }
          else
          {
            /* Go back N transmission scheme */
            send_packet(sd, smallestTimeoutPack, sentPackets, ctx);
            smallestTimeoutPack->numTimeout++;
            /* resend all following packets which have not been acked */
            int subsequentPackIndex = list_locate(sentPackets, smallestTimeoutPack) + 1;
            packetData * subsequentPacks = (packetData *) list_get_at(sentPackets, subsequentPackIndex);
            while (subsequentPacks != NULL)
            {
              send_packet(sd, subsequentPacks, sentPackets, ctx);
              subsequentPackIndex++;
              subsequentPacks = (packetData *) list_get_at(sentPackets, subsequentPackIndex);
            }
          }
        }
        if (event & APP_DATA)
        {
          char * appData = ctx->appRecvBuffer;
          len = stcp_app_recv(sd, appData, ctx->theirWindowSize);
          /* pack up and send app data */
          pack_and_send_data(sd, appData, len, sentPackets, ctx);
        }
        if (event & APP_CLOSE_REQUESTED)
        {
          stcp_fin_received_from_app(sd, sentPackets, ctx);
        }
  }
}


/* stcp_fin_received_from_app: sends a FIN packet to peer, changes
 * CSTATE to FIN_WAIT1. Upon receiving an ACK corresponding to the
 * FIN packet, changes CSTATE to CLOSED.
 */
void stcp_fin_received_from_app(mysocket_t sd, list_t * sentPackets, context_t * ctx)
{
  struct tcphdr * sendPacket;
  sendPacket = (struct tcphdr *) malloc(sizeof(struct tcphdr));
  memset(sendPacket, 0, sizeof(struct tcphdr));
  
  /* send FIN packet across network */
  memset(sendPacket, 0, sizeof(struct tcphdr));
  sendPacket->th_seq = ctx->initial_sequence_num;
  /* no options; data offset is 5 words */
  sendPacket->th_off = 5;
  sendPacket->th_flags = TH_FIN;
  sendPacket->th_win = ctx->ourWindowSize;

  packetData * packData;
  packData = (packetData *) malloc(sizeof(packetData));
  memset(packData, 0, sizeof(packetData));
  packData->packetLen = sizeof(struct tcphdr);
  packData->numTimeout = 0;
  packData->seqNum = sendPacket->th_seq;
  packData->ackNum = packData->seqNum + packData->packetLen;
  memcpy(packData->packet, sendPacket, packData->packetLen);
  packData->header = (struct tcphdr *) packData->packet;
  packData->ackd = 0;

  if (ctx->connection_state == CSTATE_CLOSE_WAIT)
  {
    ctx->connection_state = CSTATE_LAST_ACK;
  }
  else
  {
    ctx->connection_state = CSTATE_FINWAIT1;
  }

  send_packet(sd, packData, sentPackets, ctx);
  
  free(sendPacket);
  return;
}

/* our_dprintf
 *
 * Send a formatted message to stdout.
 * 
 * format               A printf-style format string.
 *
 * This function is equivalent to a printf, but may be
 * changed to log errors to a file if desired.
 *
 * Calls to this function are generated by the dprintf amd
 * dperror macros in transport.h
 */
void our_dprintf(const char *format,...)
{
  va_list argptr;
  char buffer[1024];

  assert(format);
  va_start(argptr, format);
  vsnprintf(buffer, sizeof(buffer), format, argptr);
  va_end(argptr);
  fputs(buffer, stdout);
  fflush(stdout);
}


int min(int x, int y)
{
  if (x < y)
  {
    return x;
  }
  else
  {
    return y;
  }
}


/* BEGIN SIMCLIST FUNCTIONS */
/* packet_data_size:
 * Returns size of the packetData struct.
 */
size_t packet_data_size(const void *el)
{
  return sizeof(packetData);
}


/*
 * compare packets by seq number *
 * this function compares two elements:
 * <0: a greater than b
 * 0: a equivalent to b
 * >0: b greater than a
 */
int seq_comparator(const void *a, const void *b)
{
  packetData *seqA = (packetData *) a;
  packetData *seqB = (packetData *) b;
  int sA, sB;
  sA = seqA->seqNum;
  sB = seqB->seqNum;
  return (sA < sB) - (sA > sB);
}


/* return "match" when the packet ackNum matches the key */
int seeker(const void *el, const void * key)
{
  /* let's assume el and key being always != NULL */
  const packetData *data = (packetData *) el;
  const tcp_seq * intKey = key;
  if (data->ackNum == *intKey)
    return 1;
  return 0;
}


/* BEGIN TIMESPEC HELPER FUNCTIONS */

/**
 * Compare timespec.
 *
 */
int timespec_compare(const struct timespec* left, const struct timespec* right)
{
    if (left->tv_sec < right->tv_sec) {
        return -1;
    } else if (left->tv_sec > right->tv_sec) {
        return 1;
    } else if (left->tv_nsec < right->tv_nsec) {
        return -1;
    } else if (left->tv_nsec > right->tv_nsec) {
         return 1;
    }
    return 0;
}

