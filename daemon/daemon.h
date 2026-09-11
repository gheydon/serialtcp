/*
 * SerialTCP -- a virtual serial device and modem daemon for AmigaOS.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <https://www.gnu.org/licenses/>.
 */

/*
 * daemon.h -- internal structures for SerialTCPd.
 */

#ifndef SERIALTCPD_H
#define SERIALTCPD_H

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/ports.h>
#include <devices/serial.h>
#include <devices/timer.h>

#include "serialtcp.h"

/* Private io_Flags bit the device sets when AbortIO() catches a request we
 * have already dequeued.  Must match device/device.c. */
#define ST_IOF_ABORT (1 << 5)

#define MAX_NODES        ST_MAX_UNITS
#define MAX_LISTENERS    8

/*
 * Buffer sizing.  These are per node and allocated at startup, so on real
 * hardware they are the dominant cost: keep them small.  2K each is ample --
 * at the 38400 baud we advertise the application drains at under 4K/sec while
 * the daemon loops orders of magnitude faster than that, so the buffer only
 * has to absorb scheduling jitter, not sustained throughput.
 */
#define RXBUF_DEFAULT    2048      /* socket -> application            */
#define TXBUF_DEFAULT    2048      /* application -> socket            */
#define BUFSIZE_MIN      512
#define BUFSIZE_MAX      16384

#define CMDBUF_SIZE      128       /* one AT command line              */
#define SUBNEG_SIZE      64        /* telnet subnegotiation scratch    */

/* ------------------------------------------------------------------ */
/* Byte FIFO                                                           */
/* ------------------------------------------------------------------ */

struct Fifo
{
    UBYTE  *f_Buf;
    ULONG   f_Size;
    ULONG   f_Head;      /* next byte to read  */
    ULONG   f_Count;     /* bytes currently held */
};

void  fifo_init(struct Fifo *f, UBYTE *buf, ULONG size);
ULONG fifo_space(const struct Fifo *f);
ULONG fifo_count(const struct Fifo *f);
ULONG fifo_put(struct Fifo *f, const UBYTE *src, ULONG len);
ULONG fifo_get(struct Fifo *f, UBYTE *dst, ULONG len);
void  fifo_clear(struct Fifo *f);
LONG  fifo_peek(const struct Fifo *f, ULONG index);

/* ------------------------------------------------------------------ */
/* Telnet                                                              */
/* ------------------------------------------------------------------ */

#define TEL_IAC   255
#define TEL_DONT  254
#define TEL_DO    253
#define TEL_WONT  252
#define TEL_WILL  251
#define TEL_SB    250
#define TEL_GA    249
#define TEL_EL    248
#define TEL_EC    247
#define TEL_AYT   246
#define TEL_AO    245
#define TEL_IP    244
#define TEL_BRK   243
#define TEL_DM    242
#define TEL_NOP   241
#define TEL_SE    240

#define TELOPT_BINARY  0
#define TELOPT_ECHO    1
#define TELOPT_SGA     3
#define TELOPT_TTYPE  24
#define TELOPT_NAWS   31

enum TelState
{
    TS_DATA = 0,
    TS_IAC,
    TS_WILL,
    TS_WONT,
    TS_DO,
    TS_DONT,
    TS_SB,
    TS_SB_IAC
};

struct Telnet
{
    BOOL          t_Enabled;        /* FALSE = plain TCP, no IAC handling  */
    BOOL          t_Server;         /* TRUE when we answered the call      */
    enum TelState t_State;
    UBYTE         t_SubOpt;
    UBYTE         t_SubBuf[SUBNEG_SIZE];
    UWORD         t_SubLen;

    BOOL          t_BinaryLocal;    /* we have agreed to send 8-bit clean  */
    BOOL          t_BinaryRemote;   /* peer has agreed to send 8-bit clean */
    BOOL          t_SentInitial;

    BOOL          t_OutLastCR;      /* for CR -> CR NUL in NVT mode        */

    /*
     * Negotiation replies are staged here rather than pushed straight at a
     * socket, which keeps this codec free of any dependency on struct STNode.
     * That is what lets a caller waiting in the queue -- who has no node yet --
     * use the same decoder.  Drain it after every call.
     *
     * If a peer floods us with option requests faster than we drain, the
     * overflow is simply dropped: declining to answer a flood is both correct
     * telnet behaviour and a free denial-of-service mitigation.
     */
    UBYTE         t_RespBuf[64];
    UWORD         t_RespLen;
};

struct STNode;

/* --- the codec proper: no knowledge of nodes or sockets ---------------- */

void  telnet_reset(struct Telnet *t, BOOL enabled, BOOL server);
void  telnet_start_t(struct Telnet *t);
ULONG telnet_decode_t(struct Telnet *t, const UBYTE *in, ULONG inlen, UBYTE *out, ULONG outmax);
ULONG telnet_encode_t(struct Telnet *t, const UBYTE *in, ULONG inlen, UBYTE *out, ULONG outmax);

/* Move any staged negotiation replies out. Returns bytes written. */
ULONG telnet_take_responses(struct Telnet *t, UBYTE *out, ULONG outmax);

/* --- node-flavoured wrappers, which also flush replies to the socket --- */

void  telnet_start(struct STNode *n);
ULONG telnet_decode(struct STNode *n, const UBYTE *in, ULONG inlen, UBYTE *out, ULONG outmax);
ULONG telnet_encode(struct STNode *n, const UBYTE *in, ULONG inlen, UBYTE *out, ULONG outmax);

/* ------------------------------------------------------------------ */
/* Node state                                                          */
/* ------------------------------------------------------------------ */

enum NodeState
{
    NS_DETACHED = 0,   /* no application has the unit open                 */
    NS_COMMAND,        /* on-hook, AT command mode                         */
    NS_DIALING,        /* outbound connect() in flight                     */
    NS_RINGING,        /* inbound call waiting to be answered              */
    NS_ONLINE,         /* data passes between application and socket       */
    NS_ESCAPED         /* +++ seen: command mode, carrier still up         */
};

struct STNode
{
    ULONG            n_Num;
    struct STUnit   *n_Unit;
    BOOL             n_Attached;

    /*
     * Set from the config: this node dials out only and is never offered an
     * incoming call.  Use it for a unit you drive from a terminal program,
     * so callers are not routed onto a line you are sitting on.
     */
    BOOL             n_OutboundOnly;

    /*
     * Set when a listener is bound to this unit.  Giving a node its own
     * number means reserving it: pool callers must not land on a line
     * somebody dialled directly.  Distinct from n_OutboundOnly, which stops
     * the node answering anything at all.
     */
    BOOL             n_PoolExcluded;

    enum NodeState   n_State;
    LONG             n_Sock;           /* -1 when no socket                */

    /* Buffers are allocated once at startup, sized by config.  Storing them
     * as pointers rather than inline arrays is what keeps struct STNode small
     * enough to allocate only as many as are actually configured. */
    struct Fifo      n_Rx;
    struct Fifo      n_Tx;
    UBYTE           *n_RxBuf;
    UBYTE           *n_TxBuf;

    struct List      n_Reads;          /* queued CMD_READ  IORequests      */
    struct List      n_Writes;         /* queued CMD_WRITE IORequests      */

    struct Telnet    n_Tel;

    /* --- Hayes emulation ------------------------------------------- */
    UBYTE            n_Cmd[CMDBUF_SIZE];
    UWORD            n_CmdLen;
    UBYTE            n_LastCmd[CMDBUF_SIZE];   /* for A/                   */
    UBYTE            n_SReg[32];
    BOOL             n_Echo;
    BOOL             n_Quiet;          /* ATQ1: emit no result codes       */
    BOOL             n_Verbose;        /* ATV1: word results, not digits   */
    UBYTE            n_DcdMode;        /* &C0 = DCD forced on, &C1 = real  */
    UBYTE            n_DtrMode;        /* &D0 ignore, &D2 hang up          */

    /* +++ escape detection */
    UWORD            n_PlusCount;
    struct timeval   n_LastDataTime;   /* last byte from application       */
    struct timeval   n_PlusTime;

    /* Ring cadence / dial timeout / answer delay */
    struct timeval   n_Timer;
    UWORD            n_RingCount;
    BOOL             n_TimerActive;

    ULONG            n_ConnectBaud;    /* what we claim in CONNECT <n>     */
    ULONG            n_BytesIn;        /* statistics, for the status client */
    ULONG            n_BytesOut;
    struct timeval   n_ConnectTime;

    /*
     * When this node last became free, either by hanging up or by the
     * application (re)opening the unit.  A node is not offered a new call
     * until it has been settled for c_NodeSettle seconds -- see
     * node_offer_call() for why that matters.
     */
    struct timeval   n_ReadyTime;
    char             n_PeerName[64];   /* for logging                      */
};

/* ------------------------------------------------------------------ */
/* The waiting queue                                                   */
/* ------------------------------------------------------------------ */

/* What to do with a caller when every node is busy. */
#define WB_BUSY   0        /* say so and hang up (the classic behaviour)   */
#define WB_QUEUE  1        /* hold them until a node frees up              */
#define WB_ASK    2        /* offer them the choice                        */

/*
 * How honest to be about queue position.  The lie is purely cosmetic: the
 * number the caller is told changes, the order they are actually served in
 * never does.
 */
#define QL_OFF      0      /* tell the truth                               */
#define QL_INFLATE  1      /* start high and creep upward, phone-tree style */
#define QL_RANDOM   2      /* mostly honest, occasionally fibs              */

#define QS_FREE     0
#define QS_ASKING   1      /* prompted, waiting for an answer              */
#define QS_WAITING  2      /* holding for a node                           */

struct QueueEntry
{
    UWORD           q_State;
    LONG            q_Sock;
    WORD            q_Node;         /* ST_NODE_POOL, or the unit awaited */
    char            q_Peer[64];

    struct timeval  q_Arrived;
    struct timeval  q_LastNotify;

    /*
     * A waiting caller has no node, so they carry their own telnet state.
     * We only negotiate enough to stay in sync with a client that starts
     * talking option codes at us before anyone has answered.
     */
    struct Telnet   q_Tel;

    UBYTE           q_In[16];
    UWORD           q_InLen;
    UWORD           q_LastPos;      /* last position we told them about     */
    UWORD           q_Notices;      /* how many position notices they have had */
};

/* Pure helpers, in qutil.c so the test harness can reach them. */
void  queue_expand(char *dst, ULONG dstsize, const char *src, UWORD value);
BOOL  queue_earlier(const struct timeval *a, const struct timeval *b);

/*
 * Parse a unit list like "0,2 3" into a bitmask.  Returns FALSE if anything
 * in it was not a number below maxUnits; *mask still receives whatever did
 * parse, so a partly-bad line is not silently dropped entirely.
 */
BOOL  parse_unit_list(const char *s, ULONG *mask, UWORD maxUnits);

/* Cheap xorshift, for the queue-lie feature. Not for anything that matters. */
ULONG st_rand(ULONG *state);

/*
 * What position to *tell* the caller they are in.  truePos is their real
 * place; the return value is what goes on the wire.  With QL_OFF the two are
 * always equal.
 */
UWORD queue_reported_position(UWORD truePos, UWORD mode, UWORD lieStart,
                              UWORD notices, UWORD chance, ULONG *rng);

BOOL  queue_init(void);
void  queue_cleanup(void);

/*
 * Take a caller who could not be given a node.  `node` is ST_NODE_POOL for a
 * caller who will accept any free unit, or a unit number for one who arrived
 * on a dedicated listener and must wait for that unit specifically.
 * FALSE = could not accept.
 */
BOOL  queue_offer(LONG sock, const char *peer, WORD node);

void  queue_build_fds(APTR readfds, LONG *maxfd);
void  queue_handle_fds(APTR readfds);
void  queue_pump(void);        /* timeouts and position notices            */
void  queue_dispatch(void);    /* hand waiters to nodes that have freed up */
UWORD queue_count(void);

extern struct QueueEntry *g_Queue;

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

struct Config
{
    UWORD  c_Nodes;
    UWORD  c_ListenPorts[MAX_LISTENERS];
    /* Unit each listener feeds; ST_NODE_POOL for the shared pool. */
    WORD   c_ListenNodes[MAX_LISTENERS];
    UWORD  c_NumListeners;
    BOOL   c_Telnet;              /* speak telnet, not raw TCP            */
    ULONG  c_AnswerBaud;          /* reported in CONNECT                  */
    UWORD  c_RingsBeforeBusy;     /* how long to ring before giving up    */
    UWORD  c_AutoAnswer;          /* default S0 value                     */
    ULONG  c_OutboundOnly;        /* bitmask of units excluded from the pool */
    ULONG  c_NodeSettle;          /* seconds a node must be idle first    */
    ULONG  c_RxBufSize;           /* per node, bytes                      */
    ULONG  c_TxBufSize;
    BOOL   c_LogToFile;
    char   c_LogFile[128];
    char   c_BusyMessage[256];
    BOOL   c_BusyMessageEnabled;
    char   c_BindAddr[64];        /* empty = INADDR_ANY                   */

    /* What happens when every node is in use. */
    UWORD  c_BusyAction;          /* WB_BUSY / WB_QUEUE / WB_ASK          */
    UWORD  c_QueueMax;            /* how many callers may wait            */
    ULONG  c_QueueTimeout;        /* seconds before giving up on a waiter */
    ULONG  c_QueueNotify;         /* seconds between position notices     */
    ULONG  c_AskTimeout;          /* seconds to answer the prompt         */
    UWORD  c_QueueLie;            /* QL_*                                 */
    UWORD  c_QueueLieStart;       /* where QL_INFLATE begins              */
    UWORD  c_QueueLieChance;      /* percent, for QL_RANDOM               */
    char   c_QueueMessage[192];   /* shown on joining the queue           */
    char   c_AskMessage[192];     /* the question itself                  */
    char   c_QueuePosMessage[96]; /* position notice; %N = position       */
    char   c_QueueGiveUpMessage[160];
};

extern struct Config g_Config;

BOOL config_load(const char *path);
void config_defaults(void);

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */

void log_open(void);
void log_close(void);
void log_printf(const char *fmt, ...);

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

/* Allocated at startup with exactly g_Config.c_Nodes entries. */
extern struct STNode  *g_Nodes;
/*
 * A listening socket, and what it feeds.  Most setups have one listener on
 * port 23 feeding the pool; a listener bound to a single unit gives that unit
 * its own private number, the way telser.device does it, for a line you want
 * callers to reach directly.
 */
struct Listener
{
    LONG   ln_Sock;
    UWORD  ln_Port;
    WORD   ln_Node;      /* ST_NODE_POOL, or the unit it is dedicated to */
};

extern struct Listener g_Listeners[MAX_LISTENERS];
extern LONG            g_NumListeners;
extern BOOL          g_Quit;

/* ------------------------------------------------------------------ */
/* Node / serial plumbing                                              */
/* ------------------------------------------------------------------ */

BOOL node_init(struct STNode *n, ULONG num);
void node_cleanup(struct STNode *n);
void node_attach(struct STNode *n, struct STUnit *unit);
void node_detach(struct STNode *n);
void node_hangup(struct STNode *n, BOOL sendNoCarrier);
void node_set_status(struct STNode *n, UWORD status);
void node_online(struct STNode *n, LONG sock, const char *peer, BOOL server);

/* Move bytes application <-> socket, and complete whatever IO can now be
 * completed.  Called every time round the main loop. */
void node_pump(struct STNode *n);
void node_service_io(struct STNode *n);

/* Queue data destined for the application (already telnet-decoded). */
void node_to_app(struct STNode *n, const UBYTE *data, ULONG len);

/* Append already-wire-format bytes to the socket output buffer.  Used by the
 * telnet negotiator, which must not be IAC-escaped. Returns bytes accepted. */
ULONG node_raw_out(struct STNode *n, const UBYTE *data, ULONG len);

/* Application bytes heading for the socket: telnet-encoded on the way.
 * Returns how many application bytes were consumed. */
ULONG node_app_out(struct STNode *n, const UBYTE *data, ULONG len);

/* Inbound call handling. */
BOOL node_offer_call(struct STNode *n, LONG sock, const char *peer);
void node_answer(struct STNode *n);

/* Emit a modem result code to the application. */
#define RC_OK           0
#define RC_CONNECT      1
#define RC_RING         2
#define RC_NO_CARRIER   3
#define RC_ERROR        4
#define RC_NO_DIALTONE  6
#define RC_BUSY         7
#define RC_NO_ANSWER    8

void node_result(struct STNode *n, UWORD code, ULONG connectBaud);
void node_reply_text(struct STNode *n, const char *s);

/* AT engine */
void at_reset(struct STNode *n);
void at_feed(struct STNode *n, const UBYTE *data, ULONG len);

/* Outbound dialling and the network layer (net.c). */
void node_dial(struct STNode *n, const char *target);
void node_online_write(struct STNode *n, const UBYTE *data, ULONG len, ULONG *consumed);

BOOL net_init(void);
void net_shutdown(void);
LONG net_build_fds(APTR readfds, APTR writefds);
void net_handle_fds(APTR readfds, APTR writefds);

extern ULONG          g_TotalCalls;
extern ULONG          g_BusyCalls;
extern ULONG          g_QueuedCalls;

/* How queued callers ended up. See queue.c. */
extern ULONG          g_QueueServed;
extern ULONG          g_QueueAbandoned;
extern ULONG          g_QueueTimedOut;
extern ULONG          g_QueueDeclined;
extern ULONG          g_QueueMaxWait;      /* seconds */
extern ULONG          g_QueueTotalWait;    /* seconds, for the mean */
extern struct timeval g_StartTime;

/* Time helpers */
void  st_gettime(struct timeval *tv);
LONG  st_elapsed_ms(const struct timeval *since);

#endif /* SERIALTCPD_H */
