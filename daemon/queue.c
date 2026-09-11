/*
 * SerialTCP -- a virtual serial device and modem daemon for AmigaOS.
 * Copyright (C) 2026 Gordon Heydon
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
 * queue.c -- holding callers when every node is busy.
 *
 * Three behaviours, chosen with `when-busy` in the config:
 *
 *   busy    say so and hang up.  The classic modem answer.
 *   queue   hold the caller until a node frees up, telling them where they
 *           are in the line every so often.
 *   ask     put the choice to them: wait, or call back later.
 *
 * A queued caller has been accept()ed but has no node, so nothing here can go
 * through struct STNode.  That is why the telnet codec was made independent of
 * it -- each waiting caller carries their own small Telnet state.
 *
 * Type-ahead is preserved.  For a caller who is merely waiting we never read
 * their socket, only MSG_PEEK it to notice them hanging up, so anything they
 * type while waiting is still there when a node finally answers.  Only the
 * `ask` prompt consumes input, and only until they answer it.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <devices/timer.h>

#include <proto/exec.h>
#include <proto/socket.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>

#include <string.h>
#include <stdio.h>

#include "daemon.h"

extern struct Library *SocketBase;

struct QueueEntry *g_Queue;

ULONG g_QueueServed    = 0;
ULONG g_QueueAbandoned = 0;
ULONG g_QueueTimedOut  = 0;
ULONG g_QueueDeclined  = 0;
ULONG g_QueueMaxWait   = 0;
ULONG g_QueueTotalWait = 0;

/* Seeded once at startup; only ever used to decide when to tell a fib. */
static ULONG g_LieRng = 0;

/* Why a queued caller went away. Each maps to one counter and one log line. */
#define QD_ABANDONED  0
#define QD_TIMEOUT    1
#define QD_DECLINED   2
#define QD_NOANSWER   3
#define QD_ERROR      4

/* ------------------------------------------------------------------ */

BOOL queue_init(void)
{
    UWORD i;
    struct timeval now;

    /* Seed from the clock, so the fibs are not identical on every run. */
    st_gettime(&now);
    g_LieRng = now.tv_secs ^ (now.tv_micro << 8);

    if (g_Config.c_BusyAction == WB_BUSY)
    {
        /* Nothing ever waits, so do not pay for the array. */
        g_Queue = NULL;
        return TRUE;
    }

    g_Queue = (struct QueueEntry *)AllocVec(sizeof(struct QueueEntry) * g_Config.c_QueueMax,
                                            MEMF_ANY | MEMF_CLEAR);
    if (!g_Queue)
        return FALSE;

    for (i = 0; i < g_Config.c_QueueMax; i++)
    {
        g_Queue[i].q_State = QS_FREE;
        g_Queue[i].q_Sock  = -1;
    }

    return TRUE;
}

void queue_cleanup(void)
{
    UWORD i;

    if (!g_Queue)
        return;

    for (i = 0; i < g_Config.c_QueueMax; i++)
    {
        if (g_Queue[i].q_Sock >= 0)
        {
            CloseSocket(g_Queue[i].q_Sock);
            g_Queue[i].q_Sock = -1;
        }
        g_Queue[i].q_State = QS_FREE;
    }

    FreeVec(g_Queue);
    g_Queue = NULL;
}

UWORD queue_count(void)
{
    UWORD i, n = 0;

    if (!g_Queue)
        return 0;

    for (i = 0; i < g_Config.c_QueueMax; i++)
        if (g_Queue[i].q_State != QS_FREE)
            n++;

    return n;
}

/* ------------------------------------------------------------------ */
/* Talking to a caller who has no node                                 */
/* ------------------------------------------------------------------ */

/*
 * Best effort, and deliberately so.  These are short banners on a socket that
 * has just been accepted, so they fit in the send buffer; tracking partial
 * writes for a caller we may never even serve is not worth the machinery.
 * If it does not fit, they simply miss that one notice.
 */
static void q_send(struct QueueEntry *q, const char *s)
{
    ULONG len;

    if (!s || !*s || q->q_Sock < 0)
        return;

    len = strlen(s);
    send(q->q_Sock, (APTR)s, (LONG)len, 0);
}

/* Send any telnet negotiation the codec staged in response to the caller. */
static void q_flush_telnet(struct QueueEntry *q)
{
    UBYTE resp[SUBNEG_SIZE];
    ULONG len;

    len = telnet_take_responses(&q->q_Tel, resp, sizeof(resp));
    if (len && q->q_Sock >= 0)
        send(q->q_Sock, resp, (LONG)len, 0);
}

static void q_drop(struct QueueEntry *q, UWORD reason, const char *msg)
{
    const char *why;
    ULONG       waited = (ULONG)(st_elapsed_ms(&q->q_Arrived) / 1000);

    if (q->q_Sock >= 0)
    {
        if (msg)
            q_send(q, msg);
        CloseSocket(q->q_Sock);
        q->q_Sock = -1;
    }

    /*
     * Only count a caller against the queue if they actually joined it.
     * Someone who declined at the ask prompt, or never answered, never
     * became a waiter -- counting them as abandoned would make the queue
     * look far worse than it is.
     */
    switch (reason)
    {
    case QD_ABANDONED:
        why = "hung up while waiting";
        g_QueueAbandoned++;
        break;
    case QD_TIMEOUT:
        why = "gave up waiting (timeout)";
        g_QueueTimedOut++;
        break;
    case QD_DECLINED:
        why = "declined to wait";
        g_QueueDeclined++;
        break;
    case QD_NOANSWER:
        why = "did not answer the prompt";
        g_QueueDeclined++;
        break;
    default:
        why = "connection error";
        if (q->q_State == QS_WAITING)
            g_QueueAbandoned++;
        break;
    }

    log_printf("queue: %s -- %s after %lu seconds", q->q_Peer, why,
               (unsigned long)waited);

    q->q_State = QS_FREE;
    q->q_InLen = 0;
}

/* ------------------------------------------------------------------ */
/* Ordering                                                            */
/* ------------------------------------------------------------------ */

/*
 * 1-based position among those waiting, oldest first.
 *
 * Only waiters after the same thing are counted: somebody holding for one
 * specific unit is not behind the callers queued for the shared pool, and
 * telling them otherwise would be wrong rather than merely rude.
 */
static UWORD q_position(struct QueueEntry *q)
{
    UWORD i, pos = 1;

    for (i = 0; i < g_Config.c_QueueMax; i++)
    {
        struct QueueEntry *e = &g_Queue[i];

        if (e == q || e->q_State != QS_WAITING)
            continue;

        if (e->q_Node != q->q_Node)
            continue;

        if (queue_earlier(&e->q_Arrived, &q->q_Arrived))
            pos++;
    }

    return pos;
}

static void q_enter_waiting(struct QueueEntry *q)
{
    char  buf[256];
    UWORD truePos, shown;

    q->q_State   = QS_WAITING;
    q->q_InLen   = 0;
    q->q_Notices = 0;
    st_gettime(&q->q_LastNotify);

    truePos = q_position(q);
    shown   = queue_reported_position(truePos, g_Config.c_QueueLie,
                                      g_Config.c_QueueLieStart, q->q_Notices,
                                      g_Config.c_QueueLieChance, &g_LieRng);
    q->q_LastPos = shown;

    q_send(q, g_Config.c_QueueMessage);

    queue_expand(buf, sizeof(buf), g_Config.c_QueuePosMessage, shown);
    q_send(q, buf);

    g_QueuedCalls++;

    /* The log always records the truth, whatever the caller was told. */
    if (shown != truePos)
        log_printf("queue: %s waiting, position %u (told %u)",
                   q->q_Peer, (unsigned)truePos, (unsigned)shown);
    else
        log_printf("queue: %s waiting, position %u", q->q_Peer, (unsigned)truePos);
}

/* ------------------------------------------------------------------ */
/* Accepting into the queue                                            */
/* ------------------------------------------------------------------ */

BOOL queue_offer(LONG sock, const char *peer, WORD node)
{
    struct QueueEntry *q = NULL;
    UWORD              i;

    if (!g_Queue || g_Config.c_BusyAction == WB_BUSY)
        return FALSE;

    for (i = 0; i < g_Config.c_QueueMax; i++)
    {
        if (g_Queue[i].q_State == QS_FREE)
        {
            q = &g_Queue[i];
            break;
        }
    }

    if (!q)
        return FALSE;               /* queue full: caller gets the busy banner */

    memset(q, 0, sizeof(*q));
    q->q_Sock = sock;
    q->q_Node = node;
    strncpy(q->q_Peer, peer ? peer : "?", sizeof(q->q_Peer) - 1);
    st_gettime(&q->q_Arrived);

    /*
     * Enabled so we stay in sync with a client that starts negotiating before
     * anybody answers.  Server side, because from the caller's point of view
     * they have reached a server.
     */
    telnet_reset(&q->q_Tel, g_Config.c_Telnet, TRUE);

    if (node != ST_NODE_POOL)
        log_printf("queue: %s is holding for node %d specifically",
                   q->q_Peer, (int)node);

    if (g_Config.c_BusyAction == WB_ASK)
    {
        q->q_State = QS_ASKING;
        st_gettime(&q->q_LastNotify);
        q_send(q, g_Config.c_AskMessage);
        log_printf("queue: %s asked whether to wait", q->q_Peer);
    }
    else
    {
        q_enter_waiting(q);
    }

    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Promotion onto a free node                                          */
/* ------------------------------------------------------------------ */

/*
 * The longest-waiting caller this node is allowed to serve: either one who
 * will take any free unit, or one holding specifically for this one.
 */
static struct QueueEntry *q_oldest_for_node(UWORD node)
{
    struct QueueEntry *best = NULL;
    UWORD              i;

    for (i = 0; i < g_Config.c_QueueMax; i++)
    {
        struct QueueEntry *e = &g_Queue[i];

        if (e->q_State != QS_WAITING)
            continue;

        if (e->q_Node != ST_NODE_POOL && e->q_Node != (WORD)node)
            continue;

        /* A reserved unit only serves callers who asked for it by name. */
        if (e->q_Node == ST_NODE_POOL && g_Nodes[node].n_PoolExcluded)
            continue;

        if (!best || queue_earlier(&e->q_Arrived, &best->q_Arrived))
            best = e;
    }

    return best;
}

/*
 * Walk the nodes rather than the queue.  Going this way round means a unit
 * with its own dedicated listener is matched against the callers waiting for
 * it, without the pool callers ahead of them in time blocking the match.
 */
void queue_dispatch(void)
{
    UWORD i;

    if (!g_Queue)
        return;

    for (i = 0; i < g_Config.c_Nodes; i++)
    {
        struct QueueEntry *q = q_oldest_for_node(i);
        ULONG              waited;

        if (!q)
            continue;

        if (!node_offer_call(&g_Nodes[i], q->q_Sock, q->q_Peer))
            continue;               /* node not free (or dial-out only) */

        waited = (ULONG)(st_elapsed_ms(&q->q_Arrived) / 1000);

        g_QueueServed++;
        g_QueueTotalWait += waited;
        if (waited > g_QueueMaxWait)
            g_QueueMaxWait = waited;

        log_printf("queue: %s waited %lu seconds, now on node %u",
                   q->q_Peer, (unsigned long)waited, (unsigned)i);

        /* The node owns the socket now. */
        q->q_Sock  = -1;
        q->q_State = QS_FREE;
    }
}

/* ------------------------------------------------------------------ */
/* select() integration                                                */
/* ------------------------------------------------------------------ */

void queue_build_fds(APTR readfds, LONG *maxfd)
{
    fd_set *rd = (fd_set *)readfds;
    UWORD   i;

    if (!g_Queue)
        return;

    for (i = 0; i < g_Config.c_QueueMax; i++)
    {
        struct QueueEntry *q = &g_Queue[i];

        if (q->q_State == QS_FREE || q->q_Sock < 0)
            continue;

        FD_SET(q->q_Sock, rd);

        if (q->q_Sock > *maxfd)
            *maxfd = q->q_Sock;
    }
}

/* Read the answer to the ask prompt. */
static void q_handle_ask(struct QueueEntry *q)
{
    UBYTE raw[64];
    UBYTE txt[64];
    LONG  got;
    ULONG n, i;

    got = recv(q->q_Sock, raw, sizeof(raw), 0);

    if (got == 0)
    {
        q_drop(q, QD_NOANSWER, NULL);
        return;
    }

    if (got < 0)
    {
        if (Errno() == EWOULDBLOCK || Errno() == EINTR)
            return;
        q_drop(q, QD_ERROR, NULL);
        return;
    }

    n = telnet_decode_t(&q->q_Tel, raw, (ULONG)got, txt, sizeof(txt));
    q_flush_telnet(q);

    for (i = 0; i < n; i++)
    {
        UBYTE c = txt[i];

        if (c == 'y' || c == 'Y')
        {
            q_send(q, "Y\r\n");
            q_enter_waiting(q);
            return;
        }

        if (c == 'n' || c == 'N')
        {
            q_send(q, "N\r\n");
            g_BusyCalls++;
            q_drop(q, QD_DECLINED, g_Config.c_BusyMessage);
            return;
        }

        /* A bare Return takes the default, which the prompt shows as Y. */
        if (c == '\r' || c == '\n')
        {
            q_send(q, "\r\n");
            q_enter_waiting(q);
            return;
        }

        /* Anything else is noise -- keep waiting for a real answer. */
    }
}

/*
 * A waiting caller became readable.  Peek only: their keystrokes must survive
 * until a node answers, so the sole purpose here is noticing a hangup.
 */
static void q_handle_waiting(struct QueueEntry *q)
{
    UBYTE probe;
    LONG  got;

    got = recv(q->q_Sock, &probe, 1, MSG_PEEK);

    if (got == 0)
    {
        q_drop(q, QD_ABANDONED, NULL);
        return;
    }

    if (got < 0 && Errno() != EWOULDBLOCK && Errno() != EINTR)
        q_drop(q, QD_ERROR, NULL);
}

void queue_handle_fds(APTR readfds)
{
    fd_set *rd = (fd_set *)readfds;
    UWORD   i;

    if (!g_Queue)
        return;

    for (i = 0; i < g_Config.c_QueueMax; i++)
    {
        struct QueueEntry *q = &g_Queue[i];

        if (q->q_State == QS_FREE || q->q_Sock < 0)
            continue;

        if (!FD_ISSET(q->q_Sock, rd))
            continue;

        if (q->q_State == QS_ASKING)
            q_handle_ask(q);
        else
            q_handle_waiting(q);
    }
}

/* ------------------------------------------------------------------ */
/* Timers                                                              */
/* ------------------------------------------------------------------ */

void queue_pump(void)
{
    UWORD i;

    if (!g_Queue)
        return;

    for (i = 0; i < g_Config.c_QueueMax; i++)
    {
        struct QueueEntry *q = &g_Queue[i];
        ULONG              waited;

        if (q->q_State == QS_FREE || q->q_Sock < 0)
            continue;

        waited = (ULONG)(st_elapsed_ms(&q->q_Arrived) / 1000);

        if (q->q_State == QS_ASKING)
        {
            if (waited >= g_Config.c_AskTimeout)
            {
                g_BusyCalls++;
                q_drop(q, QD_NOANSWER, g_Config.c_BusyMessage);
            }
            continue;
        }

        /* Waiting. */
        if (g_Config.c_QueueTimeout && waited >= g_Config.c_QueueTimeout)
        {
            q_drop(q, QD_TIMEOUT, g_Config.c_QueueGiveUpMessage);
            continue;
        }

        if (g_Config.c_QueueNotify &&
            (ULONG)(st_elapsed_ms(&q->q_LastNotify) / 1000) >= g_Config.c_QueueNotify)
        {
            UWORD truePos = q_position(q);
            UWORD shown;
            char  buf[256];

            st_gettime(&q->q_LastNotify);

            q->q_Notices++;
            shown = queue_reported_position(truePos, g_Config.c_QueueLie,
                                            g_Config.c_QueueLieStart, q->q_Notices,
                                            g_Config.c_QueueLieChance, &g_LieRng);

            /*
             * Only speak up when the number has changed.  Repeating "you are
             * number 3" every minute is noise; moving from 3 to 2 is not.
             * (With queue-lie inflate it changes every time, which is exactly
             * the intended effect.)
             */
            if (shown != q->q_LastPos)
            {
                q->q_LastPos = shown;
                queue_expand(buf, sizeof(buf), g_Config.c_QueuePosMessage, shown);
                q_send(q, buf);
            }
        }
    }
}
