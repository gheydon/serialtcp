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
 * resolve.c -- name lookups, off the main loop.
 *
 * Everything else the daemon does is non-blocking: sockets are in non-blocking
 * mode and one WaitSelect() covers every node at once.  gethostbyname() is the
 * exception, and bsdsocket offers no asynchronous form of it, so a DNS server
 * that takes ten seconds to answer used to stop the whole daemon dead for ten
 * seconds -- callers online at the time included.
 *
 * So the lookup happens in a process of its own.  The main loop sends it a
 * request and carries on; the reply arrives on a message port, which is just
 * another signal to wait on.  The node sits in NS_RESOLVING meanwhile and the
 * usual S7 dial timeout still applies to it.
 *
 * bsdsocket.library is per-process, which is the whole reason the daemon owns
 * every socket.  That applies here too: the resolver opens its own library
 * base and calls through it, rather than touching the daemon's.  It has to be
 * called by hand for that reason -- the usual stubs all go through the single
 * global SocketBase, which belongs to the main process.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dostags.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <clib/alib_protos.h>

#include <netdb.h>

#include <string.h>

#include "daemon.h"

/* Set by the resolver, read by the daemon. NULL means there is no resolver. */
static struct MsgPort *volatile g_ResolverPort = NULL;

static struct MsgPort  *g_ReplyPort = NULL;
static struct Task     *g_Parent    = NULL;
static BYTE             g_ReadySig  = -1;
static BYTE             g_DeadSig   = -1;
static UWORD            g_Outstanding = 0;

/*
 * Set while shutting down: replies are then collected and thrown away rather
 * than acted on, so nothing starts dialling as the daemon is going down.
 */
static BOOL             g_Draining  = FALSE;

/* ------------------------------------------------------------------ */
/* The resolver process                                                */
/* ------------------------------------------------------------------ */

/*
 * gethostbyname() through a library base of our choosing.  bsdsocket's LVO for
 * it is -0xd2, taking the name in a0.
 */
static struct hostent *sock_gethostbyname(struct Library *base, char *name)
{
    register struct Library *__base __asm("a6") = base;
    register struct hostent *__ret  __asm("d0");
    register char           *__a0   __asm("a0") = name;

    __asm volatile ("jsr %%a6@(-210:W)"
                    : "=d"(__ret), "+a"(__a0)
                    : "a"(__base)
                    : "fp0", "fp1", "cc", "memory", "d1", "a1");
    return __ret;
}

static void resolver_entry(void)
{
    struct MsgPort    *port;
    struct Library    *sock = NULL;
    struct ResolveReq *rr;
    BOOL               running = TRUE;

    port = CreateMsgPort();
    if (port)
        sock = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);

    /*
     * Publish the port only if this process is actually able to serve
     * requests; the daemon takes NULL as "no resolver" and looks names up in
     * line instead, which is slower but still works.
     */
    g_ResolverPort = (port && sock) ? port : NULL;
    Signal(g_Parent, 1UL << g_ReadySig);

    if (port && sock)
    {
        while (running)
        {
            WaitPort(port);

            while ((rr = (struct ResolveReq *)GetMsg(port)))
            {
                struct hostent *he;

                /* The quit request is never replied: the daemon waits for the
                 * death signal below, which cannot arrive early. */
                if (rr->rr_Quit)
                {
                    running = FALSE;
                    continue;
                }

                rr->rr_Ok = FALSE;

                he = sock_gethostbyname(sock, rr->rr_Host);
                if (he && he->h_addr_list && he->h_addr_list[0])
                {
                    memcpy(&rr->rr_Addr, he->h_addr_list[0], sizeof(rr->rr_Addr));
                    rr->rr_Ok = TRUE;
                }

                ReplyMsg(&rr->rr_Msg);
            }
        }
    }

    if (sock)
        CloseLibrary(sock);
    if (port)
        DeleteMsgPort(port);

    /*
     * Forbid() before the last signal, so the daemon cannot wake up, finish
     * cleaning up and exit while this process is still executing code that
     * belongs to it.  Returning from here lands in dos.library's exit path,
     * which Waits and so breaks the Forbid -- by which point nothing of ours
     * is on this process's program counter any more.
     */
    Forbid();
    g_ResolverPort = NULL;
    Signal(g_Parent, 1UL << g_DeadSig);
}

/* ------------------------------------------------------------------ */
/* Starting and stopping                                               */
/* ------------------------------------------------------------------ */

static void release_locals(void)
{
    if (g_ReplyPort)
    {
        DeleteMsgPort(g_ReplyPort);
        g_ReplyPort = NULL;
    }
    if (g_ReadySig != -1)
    {
        FreeSignal(g_ReadySig);
        g_ReadySig = -1;
    }
    if (g_DeadSig != -1)
    {
        FreeSignal(g_DeadSig);
        g_DeadSig = -1;
    }
}

BOOL resolver_start(void)
{
    struct Process *proc;

    g_Parent    = FindTask(NULL);
    g_ReplyPort = CreateMsgPort();
    g_ReadySig  = AllocSignal(-1);
    g_DeadSig   = AllocSignal(-1);

    if (!g_ReplyPort || g_ReadySig == -1 || g_DeadSig == -1)
    {
        release_locals();
        return FALSE;
    }

    /* 8K of stack: gethostbyname is the deepest thing it will ever call, and
     * this process exists only for the life of the daemon. */
    proc = CreateNewProcTags(NP_Entry,     (Tag)resolver_entry,
                             NP_Name,      (Tag)"SerialTCPd resolver",
                             NP_StackSize, 8192,
                             NP_Priority,  0,
                             TAG_DONE);
    if (!proc)
    {
        release_locals();
        return FALSE;
    }

    Wait(1UL << g_ReadySig);

    if (!g_ResolverPort)
    {
        /* It started but could not open bsdsocket.library. Let it finish
         * before giving up on it, or its exit would race our cleanup. */
        Wait(1UL << g_DeadSig);
        release_locals();
        return FALSE;
    }

    return TRUE;
}

void resolver_stop(void)
{
    struct ResolveReq quit;

    if (!g_ResolverPort)
    {
        release_locals();
        return;
    }

    /*
     * Collect every outstanding lookup first.  The requests belong to us and
     * cannot be freed while the resolver still holds them, so waiting here is
     * not optional -- and each one is bounded by the lookup it is waiting on.
     */
    g_Draining = TRUE;
    while (g_Outstanding > 0)
    {
        WaitPort(g_ReplyPort);
        resolver_handle_replies();
    }

    memset(&quit, 0, sizeof(quit));
    quit.rr_Msg.mn_Node.ln_Type = NT_MESSAGE;
    quit.rr_Msg.mn_Length       = sizeof(quit);
    quit.rr_Msg.mn_ReplyPort    = g_ReplyPort;
    quit.rr_Quit                = TRUE;

    PutMsg(g_ResolverPort, &quit.rr_Msg);
    Wait(1UL << g_DeadSig);

    release_locals();
}

BOOL resolver_available(void)
{
    return g_ResolverPort != NULL;
}

/* ------------------------------------------------------------------ */
/* Requests                                                            */
/* ------------------------------------------------------------------ */

ULONG resolver_sigmask(void)
{
    return g_ReplyPort ? (1UL << g_ReplyPort->mp_SigBit) : 0;
}

BOOL resolve_begin(struct STNode *n, const char *host, UWORD port)
{
    struct ResolveReq *rr;

    if (!g_ResolverPort || g_Draining || n->n_Resolve)
        return FALSE;

    rr = (struct ResolveReq *)AllocVec(sizeof(*rr), MEMF_ANY | MEMF_CLEAR);
    if (!rr)
        return FALSE;

    rr->rr_Msg.mn_Node.ln_Type = NT_MESSAGE;
    rr->rr_Msg.mn_Length       = sizeof(*rr);
    rr->rr_Msg.mn_ReplyPort    = g_ReplyPort;
    rr->rr_Node                = n;
    rr->rr_Port                = port;

    strncpy(rr->rr_Host, host, sizeof(rr->rr_Host) - 1);

    n->n_Resolve = rr;
    g_Outstanding++;

    PutMsg(g_ResolverPort, &rr->rr_Msg);
    return TRUE;
}

void resolve_cancel(struct STNode *n)
{
    if (!n->n_Resolve)
        return;

    /*
     * The resolver still has it, so this cannot be freed here.  Orphan it
     * instead: the reply handler frees whatever comes back with no node
     * attached and does nothing else with it.  Only this process ever writes
     * rr_Node, so no locking is needed.
     */
    n->n_Resolve->rr_Node = NULL;
    n->n_Resolve          = NULL;
}

void resolver_handle_replies(void)
{
    struct ResolveReq *rr;

    if (!g_ReplyPort)
        return;

    while ((rr = (struct ResolveReq *)GetMsg(g_ReplyPort)))
    {
        struct STNode *n = rr->rr_Node;

        if (g_Outstanding)
            g_Outstanding--;

        if (n)
        {
            n->n_Resolve = NULL;

            if (!g_Draining)
                node_dial_resolved(n, rr->rr_Ok, rr->rr_Addr,
                                   rr->rr_Host, rr->rr_Port);
        }

        FreeVec(rr);
    }
}
