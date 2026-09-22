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
 * node.c -- per-node state machine, serial command emulation and data pumping.
 *
 * Each node is one virtual modem: one unit of serialtcp.device on the
 * application side, one TCP socket on the network side.
 */

#include <exec/types.h>
#include <exec/io.h>
#include <exec/errors.h>
#include <exec/lists.h>
#include <devices/serial.h>
#include <devices/timer.h>

#include <proto/exec.h>
#include <clib/alib_protos.h>
#include <proto/socket.h>

#include <sys/socket.h>
#include <netinet/in.h>

#include <string.h>
#include <stdio.h>

#include "daemon.h"

extern struct Library *SocketBase;

struct STNode *g_Nodes;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static UWORD status_with(struct STNode *n, UWORD bitsClear, UWORD bitsSet)
{
    UWORD s = n->n_Unit ? n->n_Unit->su_Status : ST_STATUS_IDLE;
    s &= ~bitsClear;
    s |= bitsSet;
    return s;
}

void node_set_status(struct STNode *n, UWORD status)
{
    if (n->n_Unit)
        n->n_Unit->su_Status = status;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

BOOL node_init(struct STNode *n, ULONG num)
{
    memset(n, 0, sizeof(*n));

    n->n_Num   = num;
    n->n_Sock  = -1;
    n->n_State = NS_DETACHED;

    n->n_OutboundOnly = (num < 32 && (g_Config.c_OutboundOnly & (1UL << num)))
                            ? TRUE : FALSE;

    n->n_RxBuf = (UBYTE *)AllocVec(g_Config.c_RxBufSize, MEMF_ANY);
    n->n_TxBuf = (UBYTE *)AllocVec(g_Config.c_TxBufSize, MEMF_ANY);

    if (!n->n_RxBuf || !n->n_TxBuf)
    {
        node_cleanup(n);
        return FALSE;
    }

    fifo_init(&n->n_Rx, n->n_RxBuf, g_Config.c_RxBufSize);
    fifo_init(&n->n_Tx, n->n_TxBuf, g_Config.c_TxBufSize);

    NewList(&n->n_Reads);
    NewList(&n->n_Writes);

    telnet_reset(&n->n_Tel, g_Config.c_Telnet, TRUE);
    at_reset(n);

    return TRUE;
}

void node_cleanup(struct STNode *n)
{
    if (n->n_Sock >= 0)
    {
        CloseSocket(n->n_Sock);
        n->n_Sock = -1;
    }

    if (n->n_RxBuf)
    {
        FreeVec(n->n_RxBuf);
        n->n_RxBuf = NULL;
    }

    if (n->n_TxBuf)
    {
        FreeVec(n->n_TxBuf);
        n->n_TxBuf = NULL;
    }
}

void node_attach(struct STNode *n, struct STUnit *unit)
{
    n->n_Unit     = unit;
    n->n_Attached = TRUE;
    n->n_State    = NS_COMMAND;

    fifo_clear(&n->n_Rx);
    fifo_clear(&n->n_Tx);
    at_reset(n);

    /* On-hook but powered up: DSR and CTS asserted, carrier absent. */
    node_set_status(n, ST_STATUS_READY);

    unit->su_Attached = TRUE;

    /* Freshly opened: start the settle clock. */
    st_gettime(&n->n_ReadyTime);

    log_printf("node %lu: attached (unit opened)", (unsigned long)n->n_Num);
}

/* Reply to every request we are still holding for this node. */
static void flush_pending(struct STNode *n, LONG error)
{
    struct IORequest *io;

    while ((io = (struct IORequest *)RemHead(&n->n_Reads)))
    {
        io->io_Error = error;
        io->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
        ReplyMsg(&io->io_Message);
    }

    while ((io = (struct IORequest *)RemHead(&n->n_Writes)))
    {
        io->io_Error = error;
        io->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
        ReplyMsg(&io->io_Message);
    }
}

void node_detach(struct STNode *n)
{
    struct Message *msg;

    /* Whoever is closing the device does not want a NO CARRIER; they are gone. */
    node_hangup(n, FALSE);

    if (n->n_Unit)
    {
        /*
         * Stop new requests arriving, then drain anything already queued on the
         * port, then reply everything we hold.  After this returns the unit is
         * completely quiescent and the device can free it.
         */
        Forbid();
        n->n_Unit->su_Attached                        = FALSE;
        n->n_Unit->su_Unit.unit_MsgPort.mp_Flags      = PA_IGNORE;
        n->n_Unit->su_Unit.unit_MsgPort.mp_SigTask    = NULL;
        Permit();

        while ((msg = GetMsg(&n->n_Unit->su_Unit.unit_MsgPort)))
        {
            struct IORequest *io = (struct IORequest *)msg;
            io->io_Error = IOERR_ABORTED;
            io->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
            ReplyMsg(&io->io_Message);
        }
    }

    flush_pending(n, IOERR_ABORTED);

    n->n_Unit     = NULL;
    n->n_Attached = FALSE;
    n->n_State    = NS_DETACHED;

    fifo_clear(&n->n_Rx);
    fifo_clear(&n->n_Tx);

    log_printf("node %lu: detached (unit closed)", (unsigned long)n->n_Num);
}

/* ------------------------------------------------------------------ */
/* Connection management                                               */
/* ------------------------------------------------------------------ */

void node_online(struct STNode *n, LONG sock, const char *peer, BOOL server)
{
    n->n_Sock  = sock;
    n->n_State = NS_ONLINE;

    strncpy(n->n_PeerName, peer ? peer : "?", sizeof(n->n_PeerName) - 1);
    n->n_PeerName[sizeof(n->n_PeerName) - 1] = '\0';

    telnet_reset(&n->n_Tel, g_Config.c_Telnet, server);
    telnet_start(n);

    n->n_PlusCount   = 0;
    n->n_TimerActive = FALSE;
    n->n_RingCount   = 0;

    /* Carrier up: DSR, CTS and CD all asserted (bits clear), RI off. */
    node_set_status(n, ST_STATUS_ONLINE);

    n->n_ConnectBaud = n->n_Unit && n->n_Unit->su_Baud ? n->n_Unit->su_Baud
                                                       : g_Config.c_AnswerBaud;

    n->n_BytesIn  = 0;
    n->n_BytesOut = 0;
    st_gettime(&n->n_ConnectTime);

    node_result(n, RC_CONNECT, n->n_ConnectBaud);

    log_printf("node %lu: online with %s (%s)", (unsigned long)n->n_Num,
               n->n_PeerName, server ? "inbound" : "outbound");
}

void node_hangup(struct STNode *n, BOOL sendNoCarrier)
{
    /* A name lookup still in flight is no longer wanted by anyone. */
    resolve_cancel(n);

    if (n->n_Sock >= 0)
    {
        CloseSocket(n->n_Sock);
        n->n_Sock = -1;
        log_printf("node %lu: hung up on %s", (unsigned long)n->n_Num, n->n_PeerName);
    }

    n->n_PeerName[0] = '\0';
    n->n_PlusCount   = 0;
    n->n_RingCount   = 0;
    n->n_TimerActive = FALSE;

    /* Anything still queued for the socket is now meaningless. */
    fifo_clear(&n->n_Tx);

    /*
     * Discard anything the caller sent that the application never got round to
     * reading.  This matters: without it, bytes typed by one caller stay
     * buffered across the hangup and are handed to the application during the
     * *next* caller's session -- one person's keystrokes turning up inside
     * somebody else's login.
     *
     * Order matters. The clear has to happen before the result code below, or
     * NO CARRIER would be thrown away with the stale data.
     */
    fifo_clear(&n->n_Rx);

    if (n->n_State != NS_DETACHED)
    {
        n->n_State = NS_COMMAND;
        node_set_status(n, ST_STATUS_READY);

        /* Back on-hook: the settle clock restarts here too. */
        st_gettime(&n->n_ReadyTime);

        if (sendNoCarrier)
            node_result(n, RC_NO_CARRIER, 0);
    }
}

/*
 * Offer an accepted connection to this node.  Returns FALSE if the node cannot
 * take it, which is how main.c decides whether to hand out a BUSY.
 */
BOOL node_offer_call(struct STNode *n, LONG sock, const char *peer)
{
    /*
     * A dial-out-only node is never given an incoming call, even when it is
     * sitting idle.  Checking it here rather than at the accept site means the
     * queue honours it too, for free.
     */
    if (n->n_OutboundOnly)
        return FALSE;

    if (!n->n_Attached || n->n_State != NS_COMMAND || n->n_Sock >= 0)
        return FALSE;

    /*
     * A node that has only just come free is not ready yet.
     *
     * A BBS tears a session down after carrier drops, and DLG Pro closes and
     * reopens the unit while doing it. Handing it a new caller inside that
     * window means the caller is connected and then dropped a second later
     * when the application closes the device -- the worst possible moment,
     * since for a queued caller it happens exactly when they are finally
     * served. Waiting a couple of seconds lets the teardown finish.
     */
    if (g_Config.c_NodeSettle &&
        (ULONG)st_elapsed_ms(&n->n_ReadyTime) < g_Config.c_NodeSettle * 1000)
        return FALSE;

    n->n_Sock      = sock;
    n->n_State     = NS_RINGING;
    n->n_RingCount = 0;

    strncpy(n->n_PeerName, peer ? peer : "?", sizeof(n->n_PeerName) - 1);
    n->n_PeerName[sizeof(n->n_PeerName) - 1] = '\0';

    /* Ring indicator is bit 2 and is active HIGH, unlike the others. */
    node_set_status(n, status_with(n, 0, (1 << 2)));

    /* First RING goes out immediately; the rest are paced by node_pump(). */
    node_result(n, RC_RING, 0);
    n->n_RingCount = 1;
    st_gettime(&n->n_Timer);
    n->n_TimerActive = TRUE;

    log_printf("node %lu: ringing, caller %s", (unsigned long)n->n_Num, n->n_PeerName);

    /* S0=1 means "answer on the first ring", so it has to be checked here and
     * not only on the next tick, or every auto-answer would be one ring late. */
    if (n->n_SReg[0] && n->n_RingCount >= n->n_SReg[0])
        node_answer(n);

    return TRUE;
}

void node_answer(struct STNode *n)
{
    LONG sock;

    if (n->n_State != NS_RINGING || n->n_Sock < 0)
    {
        node_result(n, RC_ERROR, 0);
        return;
    }

    sock = n->n_Sock;
    node_set_status(n, status_with(n, (1 << 2), 0));   /* RI off */
    node_online(n, sock, n->n_PeerName, TRUE);
}

/* ------------------------------------------------------------------ */
/* Data movement                                                       */
/* ------------------------------------------------------------------ */

void node_to_app(struct STNode *n, const UBYTE *data, ULONG len)
{
    fifo_put(&n->n_Rx, data, len);
}

ULONG node_raw_out(struct STNode *n, const UBYTE *data, ULONG len)
{
    return fifo_put(&n->n_Tx, data, len);
}

ULONG node_app_out(struct STNode *n, const UBYTE *data, ULONG len)
{
    UBYTE  tmp[512];
    ULONG  consumed = 0;
    ULONG  chunk, produced, space;

    while (consumed < len)
    {
        space = fifo_space(&n->n_Tx);

        /* Encoding can double the data, so only feed in what is guaranteed to
         * fit even in the worst case. */
        chunk = (space / 2);
        if (chunk > sizeof(tmp) / 2)
            chunk = sizeof(tmp) / 2;
        if (chunk > len - consumed)
            chunk = len - consumed;
        if (chunk == 0)
            break;

        produced = telnet_encode(n, data + consumed, chunk, tmp, sizeof(tmp));
        fifo_put(&n->n_Tx, tmp, produced);
        consumed += chunk;
    }

    return consumed;
}

/*
 * The online write path, which is also where the +++ escape is detected.
 *
 * A run of pluses is held back rather than forwarded immediately, because we do
 * not yet know whether it is an escape sequence or just data.  If the guard
 * time passes with nothing after it, node_pump() releases them as ordinary
 * data; if a non-plus arrives first, we release them here.  This is what stops
 * a Zmodem transfer containing "+++" from dropping the caller into command
 * mode.
 */
void node_online_write(struct STNode *n, const UBYTE *data, ULONG len, ULONG *consumed)
{
    ULONG i = 0;
    ULONG start;
    ULONG guard_ms;
    UBYTE plus[3];
    UWORD k;

    *consumed = 0;

    guard_ms = (ULONG)n->n_SReg[12] * 20;
    if (guard_ms == 0)
        guard_ms = 1000;

    while (i < len)
    {
        if (data[i] == n->n_SReg[2] && n->n_PlusCount < 3)
        {
            /* The first plus only starts a sequence if the line has been quiet
             * for the guard time. */
            if (n->n_PlusCount == 0 &&
                (ULONG)st_elapsed_ms(&n->n_LastDataTime) < guard_ms)
            {
                /* Not an escape: fall through and treat it as data. */
            }
            else
            {
                n->n_PlusCount++;
                st_gettime(&n->n_PlusTime);
                st_gettime(&n->n_LastDataTime);
                i++;
                (*consumed)++;
                continue;
            }
        }

        /* Anything that is not part of a pending escape run cancels it, and the
         * held-back pluses become data again. */
        if (n->n_PlusCount > 0)
        {
            for (k = 0; k < n->n_PlusCount && k < 3; k++)
                plus[k] = (UBYTE)n->n_SReg[2];

            if (node_app_out(n, plus, n->n_PlusCount) < n->n_PlusCount)
                return;                 /* no room; retry on the next pass */

            n->n_PlusCount = 0;
        }

        /* Consume this byte even when it is a plus outside the guard time,
         * then stop before the next candidate escape character. */
        start = i++;
        while (i < len && data[i] != n->n_SReg[2])
            i++;

        if (i > start)
        {
            ULONG took = node_app_out(n, data + start, i - start);
            *consumed += took;
            n->n_BytesOut += took;
            st_gettime(&n->n_LastDataTime);

            if (took < i - start)
                return;                 /* buffer full; the rest waits */
        }
    }
}

/* ------------------------------------------------------------------ */
/* Telnet wrappers                                                     */
/* ------------------------------------------------------------------ */

/*
 * The codec stages its negotiation replies rather than sending them, so that
 * a caller waiting in the queue -- who has no node -- can use it too.  These
 * wrappers add the missing half: flush whatever it staged to this node's
 * socket output.
 */
static void telnet_flush(struct STNode *n)
{
    UBYTE resp[SUBNEG_SIZE];
    ULONG len;

    len = telnet_take_responses(&n->n_Tel, resp, sizeof(resp));
    if (len)
        node_raw_out(n, resp, len);
}

void telnet_start(struct STNode *n)
{
    telnet_start_t(&n->n_Tel);
    telnet_flush(n);
}

ULONG telnet_decode(struct STNode *n, const UBYTE *in, ULONG inlen, UBYTE *out, ULONG outmax)
{
    ULONG got = telnet_decode_t(&n->n_Tel, in, inlen, out, outmax);
    telnet_flush(n);
    return got;
}

ULONG telnet_encode(struct STNode *n, const UBYTE *in, ULONG inlen, UBYTE *out, ULONG outmax)
{
    return telnet_encode_t(&n->n_Tel, in, inlen, out, outmax);
}

/* ------------------------------------------------------------------ */
/* Result codes                                                        */
/* ------------------------------------------------------------------ */

static const char *result_text(UWORD code)
{
    switch (code)
    {
    case RC_OK:          return "OK";
    case RC_CONNECT:     return "CONNECT";
    case RC_RING:        return "RING";
    case RC_NO_CARRIER:  return "NO CARRIER";
    case RC_ERROR:       return "ERROR";
    case RC_NO_DIALTONE: return "NO DIALTONE";
    case RC_BUSY:        return "BUSY";
    case RC_NO_ANSWER:   return "NO ANSWER";
    default:             return "ERROR";
    }
}

void node_reply_text(struct STNode *n, const char *s)
{
    node_to_app(n, (const UBYTE *)s, strlen(s));
}

void node_result(struct STNode *n, UWORD code, ULONG connectBaud)
{
    char buf[64];

    if (n->n_Quiet)
        return;

    if (n->n_Verbose)
    {
        if (code == RC_CONNECT && connectBaud)
            snprintf(buf, sizeof(buf), "\r\nCONNECT %lu\r\n", (unsigned long)connectBaud);
        else
            snprintf(buf, sizeof(buf), "\r\n%s\r\n", result_text(code));
    }
    else
    {
        /* Numeric results.  CONNECT with a speed is code 1 on a plain modem;
         * the extended codes (5, 10, ...) are speed specific and not worth
         * emulating, so we always report 1. */
        snprintf(buf, sizeof(buf), "%u\r", (unsigned)code);
    }

    node_reply_text(n, buf);
}

/* ------------------------------------------------------------------ */
/* Serial command emulation                                            */
/* ------------------------------------------------------------------ */

static BOOL is_terminator(struct STUnit *u, UBYTE c)
{
    /*
     * io_TermArray is eight bytes treated as a set of terminator characters,
     * stored high byte first across the two longwords.  Standard practice is
     * to pad with the highest terminator value, so duplicates are harmless.
     */
    ULONG a0 = u->su_TermArray0;
    ULONG a1 = u->su_TermArray1;
    int   i;

    for (i = 3; i >= 0; i--)
        if ((UBYTE)((a0 >> (i * 8)) & 0xFF) == c)
            return TRUE;

    for (i = 3; i >= 0; i--)
        if ((UBYTE)((a1 >> (i * 8)) & 0xFF) == c)
            return TRUE;

    return FALSE;
}

/* Try to satisfy one queued read.  Returns TRUE if it completed. */
static BOOL try_read(struct STNode *n, struct IOExtSer *io)
{
    struct STUnit *u    = n->n_Unit;
    UBYTE         *dst  = (UBYTE *)io->IOSer.io_Data;
    ULONG          want = io->IOSer.io_Length;
    BOOL           eofmode;
    UBYTE          c;

    if (!dst)
    {
        io->IOSer.io_Error = IOERR_BADADDRESS;
        return TRUE;
    }

    eofmode = (u && (u->su_SerFlags & SERF_EOFMODE)) ? TRUE : FALSE;

    /*
     * io_Length of -1 means "read until a terminator", which only makes sense
     * in EOF mode.  Treat it as an unbounded read in that case.
     */
    if ((LONG)want == -1)
    {
        want    = 0x7FFFFFFF;
        eofmode = TRUE;
    }

    while (io->IOSer.io_Actual < want && fifo_count(&n->n_Rx) > 0)
    {
        fifo_get(&n->n_Rx, &c, 1);
        dst[io->IOSer.io_Actual++] = c;

        if (eofmode && u && is_terminator(u, c))
            return TRUE;
    }

    return (io->IOSer.io_Actual >= want);
}

/* Try to push one queued write onward.  Returns TRUE if it completed. */
static BOOL try_write(struct STNode *n, struct IOExtSer *io)
{
    UBYTE *src = (UBYTE *)io->IOSer.io_Data;
    ULONG  len = io->IOSer.io_Length;
    ULONG  remaining, taken;

    if (!src)
    {
        io->IOSer.io_Error = IOERR_BADADDRESS;
        return TRUE;
    }

    /* io_Length == -1 means the data is a NUL-terminated string. */
    if ((LONG)len == -1)
        len = strlen((const char *)src);

    remaining = len - io->IOSer.io_Actual;
    if (remaining == 0)
        return TRUE;

    if (n->n_State == NS_ONLINE)
    {
        node_online_write(n, src + io->IOSer.io_Actual, remaining, &taken);
    }
    else
    {
        /*
         * On-hook: the application is talking to the modem itself, so the bytes
         * go to the AT parser.  It always consumes everything (a command line
         * longer than the buffer is simply truncated, exactly as a real modem
         * with a 40-character buffer would do).
         */
        at_feed(n, src + io->IOSer.io_Actual, remaining);
        taken = remaining;
    }

    io->IOSer.io_Actual += taken;
    return (io->IOSer.io_Actual >= len);
}

static void complete(struct IORequest *io)
{
    io->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
    ReplyMsg(&io->io_Message);
}

/* Handle one freshly-arrived IORequest. */
static void dispatch(struct STNode *n, struct IOExtSer *io)
{
    struct STUnit *u = n->n_Unit;

    io->IOSer.io_Error  = 0;
    io->IOSer.io_Actual = 0;

    switch (io->IOSer.io_Command)
    {
    case CMD_READ:
        if (try_read(n, io))
            complete((struct IORequest *)io);
        else
            AddTail(&n->n_Reads, &io->IOSer.io_Message.mn_Node);
        break;

    case CMD_WRITE:
        if (try_write(n, io))
            complete((struct IORequest *)io);
        else
            AddTail(&n->n_Writes, &io->IOSer.io_Message.mn_Node);
        break;

    case SDCMD_QUERY:
        io->IOSer.io_Actual = fifo_count(&n->n_Rx);
        io->io_Status       = u ? u->su_Status : ST_STATUS_IDLE;
        complete((struct IORequest *)io);
        break;

    case SDCMD_SETPARAMS:
        if (u)
        {
            /* A zero baud rate is meaningless; reject it as the real device
             * would rather than silently accepting nonsense. */
            if (io->io_Baud == 0)
            {
                io->IOSer.io_Error = SerErr_InvParam;
            }
            else
            {
                u->su_Baud       = io->io_Baud;
                u->su_BrkTime    = io->io_BrkTime;
                u->su_CtlChar    = io->io_CtlChar;
                u->su_RBufLen    = io->io_RBufLen;
                u->su_ReadLen    = io->io_ReadLen;
                u->su_WriteLen   = io->io_WriteLen;
                u->su_StopBits   = io->io_StopBits;
                u->su_SerFlags   = io->io_SerFlags;
                u->su_TermArray0 = io->io_TermArray.TermArray0;
                u->su_TermArray1 = io->io_TermArray.TermArray1;
            }
        }
        io->io_Status = u ? u->su_Status : ST_STATUS_IDLE;
        complete((struct IORequest *)io);
        break;

    case SDCMD_BREAK:
        /*
         * Map a serial BREAK onto the telnet BREAK command when we are online.
         * DLG Pro uses SDCMD_BREAK as part of its hangup path, so this has to
         * at least succeed quietly rather than returning an error.
         */
        if (n->n_State == NS_ONLINE && n->n_Tel.t_Enabled)
        {
            UBYTE brk[2];
            brk[0] = TEL_IAC;
            brk[1] = TEL_BRK;
            node_raw_out(n, brk, 2);
        }
        if (u)
            u->su_Status |= IO_STATF_WROTEBREAK;
        io->io_Status = u ? u->su_Status : ST_STATUS_IDLE;
        complete((struct IORequest *)io);
        break;

    case CMD_CLEAR:
        /* Throw away buffered input without disturbing queued requests. */
        fifo_clear(&n->n_Rx);
        complete((struct IORequest *)io);
        break;

    case CMD_FLUSH:
        /* Abort everything queued, but not the request doing the flushing. */
        flush_pending(n, IOERR_ABORTED);
        complete((struct IORequest *)io);
        break;

    case CMD_RESET:
        flush_pending(n, IOERR_ABORTED);
        fifo_clear(&n->n_Rx);
        fifo_clear(&n->n_Tx);
        at_reset(n);
        if (u)
        {
            u->su_Baud     = g_Config.c_AnswerBaud;
            u->su_BrkTime  = 250000;
            u->su_CtlChar  = SER_DEFAULT_CTLCHAR;
            u->su_ReadLen  = 8;
            u->su_WriteLen = 8;
            u->su_StopBits = 1;
        }
        complete((struct IORequest *)io);
        break;

    case CMD_START:
    case CMD_STOP:
    case CMD_UPDATE:
        /* Nothing to do: we have no hardware transmitter to pause, and there
         * is no write-through cache to update. Succeed quietly. */
        complete((struct IORequest *)io);
        break;

    default:
        io->IOSer.io_Error = IOERR_NOCMD;
        complete((struct IORequest *)io);
        break;
    }
}

/*
 * Collect newly queued requests from the unit port and retry everything that
 * is still pending.
 */
void node_service_io(struct STNode *n)
{
    struct Message   *msg;
    struct Node      *nd, *next;
    struct IOExtSer  *io;

    if (!n->n_Attached || !n->n_Unit)
        return;

    while ((msg = GetMsg(&n->n_Unit->su_Unit.unit_MsgPort)))
        dispatch(n, (struct IOExtSer *)msg);

    /* Retry pending reads.  Note the successor is captured before the body
     * runs, because completing a request unlinks it. */
    for (nd = n->n_Reads.lh_Head; (next = nd->ln_Succ) != NULL; nd = next)
    {
        io = (struct IOExtSer *)nd;

        if (io->IOSer.io_Flags & ST_IOF_ABORT)
        {
            Remove(nd);
            io->IOSer.io_Error = IOERR_ABORTED;
            complete((struct IORequest *)io);
        }
        else if (try_read(n, io))
        {
            Remove(nd);
            complete((struct IORequest *)io);
        }
    }

    /* Retry pending writes. */
    for (nd = n->n_Writes.lh_Head; (next = nd->ln_Succ) != NULL; nd = next)
    {
        io = (struct IOExtSer *)nd;

        if (io->IOSer.io_Flags & ST_IOF_ABORT)
        {
            Remove(nd);
            io->IOSer.io_Error = IOERR_ABORTED;
            complete((struct IORequest *)io);
        }
        else if (try_write(n, io))
        {
            Remove(nd);
            complete((struct IORequest *)io);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Timers                                                              */
/* ------------------------------------------------------------------ */

void node_pump(struct STNode *n)
{
    ULONG guard_ms;

    if (!n->n_Attached)
        return;

    /* Ring cadence for an unanswered inbound call. */
    if (n->n_State == NS_RINGING && n->n_TimerActive)
    {
        if (st_elapsed_ms(&n->n_Timer) >= 3000)
        {
            st_gettime(&n->n_Timer);

            if (n->n_RingCount >= g_Config.c_RingsBeforeBusy)
            {
                log_printf("node %lu: no answer after %u rings, dropping %s",
                           (unsigned long)n->n_Num, (unsigned)n->n_RingCount, n->n_PeerName);
                node_set_status(n, status_with(n, (1 << 2), 0));
                node_hangup(n, FALSE);
                return;
            }

            node_result(n, RC_RING, 0);
            n->n_RingCount++;

            /* S0 holds the auto-answer ring count; 0 disables auto-answer. */
            if (n->n_SReg[0] && n->n_RingCount >= n->n_SReg[0])
                node_answer(n);
        }
    }

    /*
     * +++ escape.  The sequence only counts if it is surrounded by a guard
     * period of silence, so a file transfer that happens to contain three plus
     * signs cannot knock the modem back into command mode.
     */
    guard_ms = (ULONG)n->n_SReg[12] * 20;
    if (guard_ms == 0)
        guard_ms = 1000;

    if (n->n_State == NS_ONLINE && n->n_PlusCount == 3)
    {
        if ((ULONG)st_elapsed_ms(&n->n_PlusTime) >= guard_ms)
        {
            n->n_PlusCount = 0;
            n->n_State     = NS_ESCAPED;
            node_result(n, RC_OK, 0);
            log_printf("node %lu: escaped to command mode", (unsigned long)n->n_Num);
        }
    }
    else if (n->n_State == NS_ONLINE && n->n_PlusCount > 0)
    {
        /* An incomplete run of pluses: if nothing followed them in time they
         * were just data, so let them through. */
        if ((ULONG)st_elapsed_ms(&n->n_PlusTime) >= guard_ms)
        {
            UBYTE plus[3];
            UWORD i;
            for (i = 0; i < n->n_PlusCount && i < 3; i++)
                plus[i] = '+';
            node_app_out(n, plus, n->n_PlusCount);
            n->n_PlusCount = 0;
        }
    }
}
