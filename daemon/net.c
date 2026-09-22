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
 * net.c -- everything that touches bsdsocket.library.
 *
 * All socket calls live in this one process by necessity: bsdsocket.library
 * hands out a per-process context, so a socket opened here cannot be used from
 * the device driver's caller.  That constraint is the whole reason this
 * daemon exists.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/socket.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "daemon.h"

extern struct Library *SocketBase;

struct Listener g_Listeners[MAX_LISTENERS];
LONG            g_NumListeners;

/* ------------------------------------------------------------------ */
/* Listener setup                                                      */
/* ------------------------------------------------------------------ */

static LONG make_listener(UWORD port)
{
    struct sockaddr_in sa;
    LONG               s;
    LONG               on = 1;

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
    {
        log_printf("listen %u: socket() failed (errno %d)", (unsigned)port, (int)Errno());
        return -1;
    }

    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&on, sizeof(on));

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port);

    if (g_Config.c_BindAddr[0])
        sa.sin_addr.s_addr = inet_addr((STRPTR)g_Config.c_BindAddr);
    else
        sa.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    {
        log_printf("listen %u: bind failed (errno %d) -- port in use?",
                   (unsigned)port, (int)Errno());
        CloseSocket(s);
        return -1;
    }

    if (listen(s, 4) < 0)
    {
        log_printf("listen %u: listen failed (errno %d)", (unsigned)port, (int)Errno());
        CloseSocket(s);
        return -1;
    }

    IoctlSocket(s, FIONBIO, (char *)&on);

    return s;
}

BOOL net_init(void)
{
    UWORD i;

    g_NumListeners = 0;

    for (i = 0; i < g_Config.c_NumListeners; i++)
    {
        LONG s = make_listener(g_Config.c_ListenPorts[i]);

        if (s < 0)
            continue;

        g_Listeners[g_NumListeners].ln_Sock = s;
        g_Listeners[g_NumListeners].ln_Port = g_Config.c_ListenPorts[i];
        g_Listeners[g_NumListeners].ln_Node = g_Config.c_ListenNodes[i];
        g_NumListeners++;
    }

    return (g_NumListeners > 0);
}

void net_shutdown(void)
{
    LONG i;

    for (i = 0; i < g_NumListeners; i++)
        if (g_Listeners[i].ln_Sock >= 0)
            CloseSocket(g_Listeners[i].ln_Sock);

    g_NumListeners = 0;
}

/* ------------------------------------------------------------------ */
/* Inbound calls                                                       */
/* ------------------------------------------------------------------ */

static void handle_accept(struct Listener *ln)
{
    struct sockaddr_in sa;
    socklen_t          len = sizeof(sa);
    LONG               s;
    LONG               on = 1;
    const char        *peer;
    UWORD              i;

    s = accept(ln->ln_Sock, (struct sockaddr *)&sa, &len);
    if (s < 0)
        return;

    IoctlSocket(s, FIONBIO, (char *)&on);

    peer = (const char *)Inet_NtoA(sa.sin_addr.s_addr);
    g_TotalCalls++;

    /*
     * A listener bound to one unit feeds only that unit -- the caller dialled
     * that node's private number, so putting them anywhere else would defeat
     * the point. Everything else goes to the first free node in the pool;
     * "free" means an application has the unit open (so somebody is actually
     * listening) and it is on-hook, because a node whose BBS is not running
     * must not swallow calls.
     */
    if (ln->ln_Node != ST_NODE_POOL)
    {
        if (ln->ln_Node < (WORD)g_Config.c_Nodes &&
            node_offer_call(&g_Nodes[ln->ln_Node], s, peer))
            return;
    }
    else
    {
        for (i = 0; i < g_Config.c_Nodes; i++)
        {
            /* Skip units reserved to their own listener. */
            if (g_Nodes[i].n_PoolExcluded)
                continue;

            if (node_offer_call(&g_Nodes[i], s, peer))
                return;
        }
    }

    /*
     * Every line is busy.  What happens next is the sysop's choice: hang up,
     * hold them in the queue, or ask them which they would prefer.
     */
    if (g_Config.c_BusyAction != WB_BUSY && queue_offer(s, peer, ln->ln_Node))
        return;

    g_BusyCalls++;
    if (ln->ln_Node != ST_NODE_POOL)
        log_printf("call from %s refused: node %d busy%s", peer, (int)ln->ln_Node,
                   (g_Config.c_BusyAction != WB_BUSY) ? " and the queue is full" : "");
    else
        log_printf("call from %s refused: all %u nodes busy%s", peer,
                   (unsigned)g_Config.c_Nodes,
                   (g_Config.c_BusyAction != WB_BUSY) ? " and the queue is full" : "");

    if (g_Config.c_BusyMessageEnabled && g_Config.c_BusyMessage[0])
    {
        /*
         * Best effort only: the socket is non-blocking, so a short banner on a
         * freshly accepted connection either goes out immediately or not at
         * all.  Never stall the daemon for a caller we are turning away.
         */
        send(s, g_Config.c_BusyMessage, strlen(g_Config.c_BusyMessage), 0);
    }

    CloseSocket(s);
}

/* ------------------------------------------------------------------ */
/* Outbound calls                                                      */
/* ------------------------------------------------------------------ */

/*
 * Parse "host", "host:port", "host,port" or "host port".
 * Anything a dial string might legitimately carry (quotes, trailing junk) is
 * trimmed here rather than in the AT parser.
 */
static BOOL split_target(const char *target, char *host, ULONG hostsize, UWORD *port)
{
    const char *p = target;
    const char *sep;
    ULONG       len;

    while (*p == ' ' || *p == '"')
        p++;

    sep = p;
    while (*sep && *sep != ':' && *sep != ',' && *sep != ' ' && *sep != '"')
        sep++;

    len = (ULONG)(sep - p);
    if (len == 0 || len >= hostsize)
        return FALSE;

    memcpy(host, p, len);
    host[len] = '\0';

    *port = 23;
    if (*sep == ':' || *sep == ',' || *sep == ' ')
    {
        int v = atoi(sep + 1);
        if (v > 0 && v < 65536)
            *port = (UWORD)v;
    }

    return TRUE;
}

/* Open the connection, once there is an address to open it to. */
static void dial_connect(struct STNode *n, ULONG addr, const char *host, UWORD port)
{
    struct sockaddr_in sa;
    LONG               s;
    LONG               on = 1;
    LONG               rc;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(port);
    sa.sin_addr.s_addr = addr;

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
    {
        node_result(n, RC_NO_DIALTONE, 0);
        return;
    }

    IoctlSocket(s, FIONBIO, (char *)&on);

    rc = connect(s, (struct sockaddr *)&sa, sizeof(sa));
    if (rc == 0)
    {
        snprintf(n->n_PeerName, sizeof(n->n_PeerName), "%s:%u", host, (unsigned)port);
        node_online(n, s, n->n_PeerName, FALSE);
        return;
    }

    if (Errno() != EINPROGRESS && Errno() != EWOULDBLOCK)
    {
        log_printf("node %lu: connect to %s:%u failed (errno %d)",
                   (unsigned long)n->n_Num, host, (unsigned)port, (int)Errno());
        CloseSocket(s);
        node_result(n, RC_NO_CARRIER, 0);
        return;
    }

    n->n_Sock  = s;
    n->n_State = NS_DIALING;
    snprintf(n->n_PeerName, sizeof(n->n_PeerName), "%s:%u", host, (unsigned)port);

    st_gettime(&n->n_Timer);
    n->n_TimerActive = TRUE;

    log_printf("node %lu: dialling %s", (unsigned long)n->n_Num, n->n_PeerName);
}

void node_dial(struct STNode *n, const char *target)
{
    char  host[128];
    UWORD port;
    ULONG addr;

    if (n->n_Sock >= 0 || n->n_Resolve)
    {
        node_result(n, RC_ERROR, 0);
        return;
    }

    if (!split_target(target, host, sizeof(host), &port))
    {
        node_result(n, RC_ERROR, 0);
        return;
    }

    /* A literal address needs no lookup at all, which is the common case for
     * a dial string out of a BBS mailer. */
    addr = inet_addr((STRPTR)host);
    if (addr != (ULONG)-1)
    {
        dial_connect(n, addr, host, port);
        return;
    }

    /*
     * Hand the name to the resolver process and return to the main loop.  The
     * node waits in NS_RESOLVING under the same S7 timeout a dial gets, so a
     * DNS server that never answers costs this node its dial and costs the
     * other nodes nothing.
     */
    if (resolve_begin(n, host, port))
    {
        n->n_State = NS_RESOLVING;
        snprintf(n->n_PeerName, sizeof(n->n_PeerName), "%s:%u", host, (unsigned)port);

        st_gettime(&n->n_Timer);
        n->n_TimerActive = TRUE;

        log_printf("node %lu: looking up %s", (unsigned long)n->n_Num, host);
        return;
    }

    /*
     * No resolver process: look the name up here instead.  This blocks every
     * node for the duration, which is why the resolver exists, but a daemon
     * that cannot dial out at all would be worse.
     */
    {
        struct hostent *he = gethostbyname((STRPTR)host);

        if (!he || !he->h_addr_list || !he->h_addr_list[0])
        {
            log_printf("node %lu: cannot resolve '%s'", (unsigned long)n->n_Num, host);
            node_result(n, RC_NO_DIALTONE, 0);
            return;
        }

        memcpy(&addr, he->h_addr_list[0], sizeof(addr));
        dial_connect(n, addr, host, port);
    }
}

/* The resolver has answered.  Called from the main loop, never from the
 * resolver process itself. */
void node_dial_resolved(struct STNode *n, BOOL ok, ULONG addr,
                        const char *host, UWORD port)
{
    n->n_TimerActive = FALSE;

    /* The caller hung up, or the unit was closed, while we were looking. */
    if (n->n_State != NS_RESOLVING)
        return;

    n->n_State = NS_COMMAND;

    if (!ok)
    {
        log_printf("node %lu: cannot resolve '%s'", (unsigned long)n->n_Num, host);
        node_result(n, RC_NO_DIALTONE, 0);
        return;
    }

    dial_connect(n, addr, host, port);
}

/* Called when a dialling socket becomes writable, or the timeout expires. */
static void finish_dial(struct STNode *n)
{
    LONG      err = 0;
    socklen_t len = sizeof(err);

    if (getsockopt(n->n_Sock, SOL_SOCKET, SO_ERROR, (char *)&err, &len) < 0)
        err = Errno();

    if (err == 0)
    {
        LONG sock = n->n_Sock;
        n->n_TimerActive = FALSE;
        node_online(n, sock, n->n_PeerName, FALSE);
    }
    else
    {
        log_printf("node %lu: dial to %s failed (errno %d)",
                   (unsigned long)n->n_Num, n->n_PeerName, (int)err);
        CloseSocket(n->n_Sock);
        n->n_Sock        = -1;
        n->n_TimerActive = FALSE;
        n->n_State       = NS_COMMAND;
        node_set_status(n, ST_STATUS_READY);
        node_result(n, (err == ECONNREFUSED) ? RC_BUSY : RC_NO_CARRIER, 0);
    }
}

/* ------------------------------------------------------------------ */
/* Socket data movement                                                */
/* ------------------------------------------------------------------ */

static void socket_read(struct STNode *n)
{
    UBYTE raw[512];
    UBYTE app[512];
    LONG  got;
    ULONG space, want, produced;

    for (;;)
    {
        /* Never read more than the application-side buffer can absorb, or the
         * decoded bytes would have nowhere to go. */
        space = fifo_space(&n->n_Rx);
        if (space == 0)
            return;

        want = (space < sizeof(raw)) ? space : sizeof(raw);

        got = recv(n->n_Sock, (UBYTE *)raw, want, 0);

        if (got > 0)
        {
            n->n_BytesIn += got;
            produced = telnet_decode(n, raw, (ULONG)got, app, sizeof(app));
            if (produced)
                node_to_app(n, app, produced);

            if ((ULONG)got < want)
                return;                 /* drained the socket for now */
            continue;
        }

        if (got == 0)
        {
            log_printf("node %lu: %s disconnected", (unsigned long)n->n_Num, n->n_PeerName);
            node_hangup(n, TRUE);
            return;
        }

        if (Errno() == EWOULDBLOCK || Errno() == EINTR)
            return;

        log_printf("node %lu: read error (errno %d)", (unsigned long)n->n_Num, (int)Errno());
        node_hangup(n, TRUE);
        return;
    }
}

static void socket_write(struct STNode *n)
{
    UBYTE buf[512];
    ULONG have;
    LONG  sent;

    while ((have = fifo_count(&n->n_Tx)) > 0)
    {
        ULONG chunk = (have < sizeof(buf)) ? have : sizeof(buf);

        /* Peek rather than consume: if the send only partially succeeds we
         * must not lose the remainder. */
        {
            ULONG i;
            for (i = 0; i < chunk; i++)
                buf[i] = (UBYTE)fifo_peek(&n->n_Tx, i);
        }

        sent = send(n->n_Sock, buf, chunk, 0);

        if (sent > 0)
        {
            UBYTE discard[512];
            fifo_get(&n->n_Tx, discard, (ULONG)sent);
            if ((ULONG)sent < chunk)
                return;                 /* socket full; wait for writability */
            continue;
        }

        if (sent < 0 && (Errno() == EWOULDBLOCK || Errno() == EINTR))
            return;

        log_printf("node %lu: write error (errno %d)", (unsigned long)n->n_Num, (int)Errno());
        node_hangup(n, TRUE);
        return;
    }
}

/*
 * A ringing node became readable.  Peek rather than read: if the caller has
 * gone we must stop ringing, but if they merely typed ahead their bytes have
 * to stay in the socket buffer until the call is answered.
 *
 * Whether the hangup reports NO CARRIER depends on how far the call had got.
 * A caller who gives up while the phone is still ringing just stops ringing,
 * exactly as a real line does.  But once the application has sent ATA it is
 * waiting for an answer, so a caller lost during connect-delay's silence has
 * to be reported -- otherwise the ATA is never answered at all and the BBS
 * sits out its own connect timeout before it can take the next call.
 */
static void ringing_check(struct STNode *n)
{
    UBYTE probe;
    LONG  got;

    got = recv(n->n_Sock, &probe, 1, MSG_PEEK);

    if (got == 0)
    {
        log_printf("node %lu: caller %s %s", (unsigned long)n->n_Num, n->n_PeerName,
                   n->n_AnswerPending ? "dropped while answering"
                                      : "gave up before answer");
        node_set_status(n, (UWORD)(n->n_Unit ? (n->n_Unit->su_Status & ~(1 << 2)) : 0));
        node_hangup(n, n->n_AnswerPending);
        return;
    }

    if (got < 0 && Errno() != EWOULDBLOCK && Errno() != EINTR)
    {
        log_printf("node %lu: caller %s vanished (errno %d)",
                   (unsigned long)n->n_Num, n->n_PeerName, (int)Errno());
        node_set_status(n, (UWORD)(n->n_Unit ? (n->n_Unit->su_Status & ~(1 << 2)) : 0));
        node_hangup(n, n->n_AnswerPending);
    }
}

/* ------------------------------------------------------------------ */
/* select() integration                                                */
/* ------------------------------------------------------------------ */

LONG net_build_fds(APTR readfds, APTR writefds)
{
    fd_set *rd  = (fd_set *)readfds;
    fd_set *wr  = (fd_set *)writefds;
    LONG    max = -1;
    LONG    i;
    UWORD   u;

    FD_ZERO(rd);
    FD_ZERO(wr);

    for (i = 0; i < g_NumListeners; i++)
    {
        FD_SET(g_Listeners[i].ln_Sock, rd);
        if (g_Listeners[i].ln_Sock > max)
            max = g_Listeners[i].ln_Sock;
    }

    for (u = 0; u < g_Config.c_Nodes; u++)
    {
        struct STNode *n = &g_Nodes[u];

        /* A node waiting on a name has no socket yet, so it has to be checked
         * before the socket test below sends us past it. */
        if (n->n_State == NS_RESOLVING)
        {
            if (n->n_TimerActive &&
                st_elapsed_ms(&n->n_Timer) >= (LONG)n->n_SReg[7] * 1000)
            {
                log_printf("node %lu: lookup of %s timed out",
                           (unsigned long)n->n_Num, n->n_PeerName);
                resolve_cancel(n);
                n->n_TimerActive = FALSE;
                n->n_State       = NS_COMMAND;
                node_set_status(n, ST_STATUS_READY);
                node_result(n, RC_NO_DIALTONE, 0);
            }
            continue;
        }

        if (n->n_Sock < 0)
            continue;

        if (n->n_State == NS_DIALING)
        {
            FD_SET(n->n_Sock, wr);
        }
        else if (n->n_State == NS_RINGING)
        {
            /* Watch for the caller giving up before anyone answers.  We do not
             * want their data yet -- anything they type ahead must stay in the
             * socket buffer until the call is answered, or it would be fed to
             * the AT parser as if the modem had typed it. */
            FD_SET(n->n_Sock, rd);
        }
        else
        {
            /* Only ask for readability when there is somewhere to put the
             * data; otherwise select() would spin. */
            if (fifo_space(&n->n_Rx) > 0)
                FD_SET(n->n_Sock, rd);

            if (fifo_count(&n->n_Tx) > 0)
                FD_SET(n->n_Sock, wr);
        }

        if (n->n_Sock > max)
            max = n->n_Sock;
    }

    queue_build_fds(rd, &max);

    return max + 1;
}

void net_handle_fds(APTR readfds, APTR writefds)
{
    fd_set *rd = (fd_set *)readfds;
    fd_set *wr = (fd_set *)writefds;
    LONG    i;
    UWORD   u;

    queue_handle_fds(rd);

    for (i = 0; i < g_NumListeners; i++)
        if (FD_ISSET(g_Listeners[i].ln_Sock, rd))
            handle_accept(&g_Listeners[i]);

    for (u = 0; u < g_Config.c_Nodes; u++)
    {
        struct STNode *n = &g_Nodes[u];

        if (n->n_Sock < 0)
            continue;

        if (n->n_State == NS_DIALING)
        {
            if (FD_ISSET(n->n_Sock, wr))
                finish_dial(n);
            else if (n->n_TimerActive &&
                     st_elapsed_ms(&n->n_Timer) >= (LONG)n->n_SReg[7] * 1000)
            {
                log_printf("node %lu: dial timed out", (unsigned long)n->n_Num);
                CloseSocket(n->n_Sock);
                n->n_Sock        = -1;
                n->n_TimerActive = FALSE;
                n->n_State       = NS_COMMAND;
                node_set_status(n, ST_STATUS_READY);
                node_result(n, RC_NO_ANSWER, 0);
            }
            continue;
        }

        if (n->n_State == NS_RINGING)
        {
            if (FD_ISSET(n->n_Sock, rd))
                ringing_check(n);
            continue;
        }

        /* Write first: it frees buffer space that a read may then want. */
        if (n->n_Sock >= 0 && FD_ISSET(n->n_Sock, wr))
            socket_write(n);

        if (n->n_Sock >= 0 && FD_ISSET(n->n_Sock, rd))
            socket_read(n);
    }
}
