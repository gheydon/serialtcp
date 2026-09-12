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
 * main.c -- SerialTCPd: the virtual modem daemon behind serialtcp.device.
 *
 * Memory policy: every allocation here uses MEMF_ANY, never MEMF_CHIP.
 * Nothing in this program is touched by custom-chip DMA, so on a machine with
 * Fast RAM Exec will satisfy all of it from Fast (its memory list is searched
 * in priority order and Fast ranks above Chip), leaving Chip RAM entirely for
 * the display and audio.  On a Chip-only machine it still works, it just has
 * nowhere else to go.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <exec/execbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <devices/timer.h>

#include <proto/exec.h>
#include <clib/alib_protos.h>
#include <proto/dos.h>
#include <proto/socket.h>
#include <proto/timer.h>

#include <sys/types.h>
#include <sys/socket.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "daemon.h"

struct Library *SocketBase = NULL;
struct Device  *TimerBase  = NULL;

static struct MsgPort      *g_TimerPort = NULL;
static struct timerequest  *g_TimerReq  = NULL;
static struct STDaemonPort *g_Port      = NULL;
static BYTE                 g_PortSig   = -1;
static BYTE                 g_IoSig     = -1;
static BOOL                 g_PortAdded = FALSE;

BOOL           g_Quit       = FALSE;
ULONG          g_TotalCalls = 0;
ULONG          g_BusyCalls  = 0;
ULONG          g_QueuedCalls = 0;
struct timeval g_StartTime;

#define VERSION_STRING "SerialTCPd 1.0 (10.9.2026)"
static const char *verstag = "$VER: " VERSION_STRING;

/* ------------------------------------------------------------------ */
/* Timer                                                               */
/* ------------------------------------------------------------------ */

static BOOL timer_open(void)
{
    g_TimerPort = CreateMsgPort();
    if (!g_TimerPort)
        return FALSE;

    g_TimerReq = (struct timerequest *)CreateIORequest(g_TimerPort, sizeof(struct timerequest));
    if (!g_TimerReq)
        return FALSE;

    if (OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ,
                   (struct IORequest *)g_TimerReq, 0) != 0)
        return FALSE;

    TimerBase = (struct Device *)g_TimerReq->tr_node.io_Device;
    return TRUE;
}

static void timer_close(void)
{
    if (TimerBase)
    {
        CloseDevice((struct IORequest *)g_TimerReq);
        TimerBase = NULL;
    }
    if (g_TimerReq)
        DeleteIORequest((struct IORequest *)g_TimerReq);
    if (g_TimerPort)
        DeleteMsgPort(g_TimerPort);

    g_TimerReq  = NULL;
    g_TimerPort = NULL;
}

/* ------------------------------------------------------------------ */
/* Public port                                                         */
/* ------------------------------------------------------------------ */

static BOOL port_create(void)
{
    struct MsgPort *existing;

    /* Refuse to start twice: two daemons fighting over one port name would
     * hand units to whichever replied first and corrupt both. */
    Forbid();
    existing = FindPort((CONST_STRPTR)ST_PORT_NAME);
    Permit();

    if (existing)
    {
        printf("SerialTCPd: already running (port '%s' exists)\n", ST_PORT_NAME);
        return FALSE;
    }

    g_PortSig = AllocSignal(-1);
    g_IoSig   = AllocSignal(-1);
    if (g_PortSig == -1 || g_IoSig == -1)
    {
        printf("SerialTCPd: out of signals\n");
        return FALSE;
    }

    g_Port = (struct STDaemonPort *)AllocVec(sizeof(struct STDaemonPort), MEMF_ANY | MEMF_CLEAR);
    if (!g_Port)
        return FALSE;

    g_Port->dp_Port.mp_Node.ln_Type = NT_MSGPORT;
    g_Port->dp_Port.mp_Node.ln_Pri  = 0;
    g_Port->dp_Port.mp_Node.ln_Name = (char *)ST_PORT_NAME;
    g_Port->dp_Port.mp_Flags        = PA_SIGNAL;
    g_Port->dp_Port.mp_SigBit       = g_PortSig;
    g_Port->dp_Port.mp_SigTask      = FindTask(NULL);
    NewList(&g_Port->dp_Port.mp_MsgList);

    g_Port->dp_Magic    = ST_MAGIC;
    g_Port->dp_Version  = ST_PROTOCOL_VER;
    g_Port->dp_NumUnits = g_Config.c_Nodes;

    AddPort(&g_Port->dp_Port);
    g_PortAdded = TRUE;
    return TRUE;
}

static void port_delete(void)
{
    if (g_Port)
    {
        if (g_PortAdded)
        {
            RemPort(&g_Port->dp_Port);
            g_PortAdded = FALSE;
        }
        FreeVec(g_Port);
        g_Port = NULL;
    }
    if (g_PortSig != -1)
        FreeSignal(g_PortSig);
    if (g_IoSig != -1)
        FreeSignal(g_IoSig);

    g_PortSig = -1;
    g_IoSig   = -1;
}

/* ------------------------------------------------------------------ */
/* Attach / detach / status requests                                   */
/* ------------------------------------------------------------------ */

static UWORD map_state(struct STNode *n)
{
    switch (n->n_State)
    {
    case NS_DETACHED: return STS_DETACHED;
    case NS_COMMAND:  return STS_IDLE;
    case NS_DIALING:  return STS_DIALING;
    case NS_RINGING:  return STS_RINGING;
    case NS_ONLINE:   return STS_ONLINE;
    case NS_ESCAPED:  return STS_ESCAPED;
    default:          return STS_DETACHED;
    }
}

static void handle_status(struct STStatusMsg *sm)
{
    UWORD i, count;

    if (sm->sm_Version != ST_PROTOCOL_VER)
    {
        sm->sm_Error = STE_BAD_VERSION;
        return;
    }

    if (!sm->sm_Nodes || sm->sm_MaxNodes == 0)
    {
        sm->sm_Error = STE_NO_MEMORY;
        return;
    }

    count = g_Config.c_Nodes;
    if (count > sm->sm_MaxNodes)
        count = sm->sm_MaxNodes;

    for (i = 0; i < count; i++)
    {
        struct STNode       *n  = &g_Nodes[i];
        struct STNodeStatus *ns = &sm->sm_Nodes[i];

        memset(ns, 0, sizeof(*ns));

        ns->ns_Unit       = (UWORD)n->n_Num;
        ns->ns_State      = map_state(n);
        ns->ns_Status     = n->n_Unit ? n->n_Unit->su_Status : ST_STATUS_IDLE;
        ns->ns_Rings      = n->n_RingCount;
        ns->ns_Flags      = n->n_OutboundOnly ? STSF_OUTBOUND_ONLY : 0;
        ns->ns_Baud       = n->n_Unit ? n->n_Unit->su_Baud : 0;
        ns->ns_RxQueued   = fifo_count(&n->n_Rx);
        ns->ns_TxQueued   = fifo_count(&n->n_Tx);
        ns->ns_BytesIn    = n->n_BytesIn;
        ns->ns_BytesOut   = n->n_BytesOut;

        if (n->n_State == NS_ONLINE || n->n_State == NS_ESCAPED)
            ns->ns_ConnectSecs = (ULONG)(st_elapsed_ms(&n->n_ConnectTime) / 1000);

        strncpy(ns->ns_Peer, n->n_PeerName, sizeof(ns->ns_Peer) - 1);
    }

    sm->sm_NumNodes     = count;
    sm->sm_UptimeSecs   = (ULONG)(st_elapsed_ms(&g_StartTime) / 1000);
    sm->sm_TotalCalls   = g_TotalCalls;
    sm->sm_BusyCalls    = g_BusyCalls;
    sm->sm_QueuedCalls  = g_QueuedCalls;
    sm->sm_Queued       = queue_count();
    sm->sm_QueueServed    = g_QueueServed;
    sm->sm_QueueAbandoned = g_QueueAbandoned;
    sm->sm_QueueTimedOut  = g_QueueTimedOut;
    sm->sm_QueueDeclined  = g_QueueDeclined;
    sm->sm_QueueMaxWait   = g_QueueMaxWait;
    sm->sm_QueueAvgWait   = g_QueueServed ? (g_QueueTotalWait / g_QueueServed) : 0;
    sm->sm_QueueMax     = (g_Config.c_BusyAction == WB_BUSY) ? 0 : g_Config.c_QueueMax;
    sm->sm_NumListeners = (UWORD)g_NumListeners;

    for (i = 0; i < (UWORD)g_NumListeners && i < 8; i++)
    {
        sm->sm_ListenPorts[i] = g_Listeners[i].ln_Port;
        sm->sm_ListenNodes[i] = g_Listeners[i].ln_Node;
    }

    sm->sm_Error = STE_OK;
}

static void handle_attach(struct STAttachMsg *am)
{
    struct STNode *n;

    if (am->am_Version != ST_PROTOCOL_VER)
    {
        am->am_Error = STE_BAD_VERSION;
        return;
    }

    if (am->am_UnitNum >= g_Config.c_Nodes)
    {
        am->am_Error = STE_NO_SUCH_UNIT;
        return;
    }

    n = &g_Nodes[am->am_UnitNum];

    if (n->n_Attached)
    {
        am->am_Error = STE_UNIT_IN_USE;
        return;
    }

    if (g_Quit)
    {
        am->am_Error = STE_SHUTTING_DOWN;
        return;
    }

    /*
     * Point the unit's message port at ourselves.  From here on, BeginIO() in
     * the device delivers requests straight into our Wait() -- no per-unit
     * handler task required.
     *
     * The port must be live before su_Attached goes TRUE, because that flag is
     * what BeginIO() checks before it dares to PutMsg().
     */
    am->am_Unit->su_Unit.unit_MsgPort.mp_SigBit  = g_IoSig;
    am->am_Unit->su_Unit.unit_MsgPort.mp_SigTask = FindTask(NULL);
    am->am_Unit->su_Unit.unit_MsgPort.mp_Flags   = PA_SIGNAL;

    node_attach(n, am->am_Unit);

    am->am_Error = STE_OK;
}

static void handle_detach(struct STAttachMsg *am)
{
    struct STNode *n;

    if (am->am_UnitNum >= g_Config.c_Nodes)
    {
        am->am_Error = STE_NO_SUCH_UNIT;
        return;
    }

    n = &g_Nodes[am->am_UnitNum];

    if (n->n_Attached && n->n_Unit == am->am_Unit)
        node_detach(n);

    am->am_Error = STE_OK;
}

/*
 * Count nodes with a call actually in progress.  A node that is merely open
 * and waiting does not stop a shutdown; one with a caller on it does.
 */
static UWORD active_nodes(void)
{
    UWORD i, n = 0;

    for (i = 0; i < g_Config.c_Nodes; i++)
    {
        switch (g_Nodes[i].n_State)
        {
        case NS_ONLINE:
        case NS_ESCAPED:
        case NS_RINGING:
        case NS_DIALING:
            n++;
            break;
        default:
            break;
        }
    }

    return n;
}

static void handle_control(struct STControlMsg *cm)
{
    UWORD active;

    if (cm->cm_Version != ST_PROTOCOL_VER)
    {
        cm->cm_Error = STE_BAD_VERSION;
        return;
    }

    switch (cm->cm_Action)
    {
    case STC_SHUTDOWN:
        active = active_nodes();
        cm->cm_ActiveNodes = active;

        if (active && !(cm->cm_Flags & STCF_FORCE))
        {
            log_printf("shutdown refused: %u node(s) still in use", (unsigned)active);
            cm->cm_Error = STE_NODES_ACTIVE;
            return;
        }

        if (active)
            log_printf("forced shutdown with %u node(s) in use", (unsigned)active);
        else
            log_printf("shutdown requested");

        g_Quit       = TRUE;
        cm->cm_Error = STE_OK;
        break;

    default:
        cm->cm_Error = STE_BAD_VERSION;
        break;
    }
}

static void handle_port_messages(void)
{
    struct Message *msg;

    while ((msg = GetMsg(&g_Port->dp_Port)))
    {
        /* Both message types keep their command word immediately after the
         * struct Message, so this peek is safe before we know which it is. */
        ULONG cmd = ((struct STAttachMsg *)msg)->am_Command;

        switch (cmd)
        {
        case STM_ATTACH: handle_attach((struct STAttachMsg *)msg); break;
        case STM_DETACH: handle_detach((struct STAttachMsg *)msg); break;
        case STM_STATUS: handle_status((struct STStatusMsg *)msg); break;
        case STM_CONTROL: handle_control((struct STControlMsg *)msg); break;
        default:
            ((struct STAttachMsg *)msg)->am_Error = STE_BAD_VERSION;
            break;
        }

        ReplyMsg(msg);
    }
}

/* ------------------------------------------------------------------ */
/* Startup / shutdown                                                  */
/* ------------------------------------------------------------------ */

static BOOL nodes_create(void)
{
    UWORD i;

    g_Nodes = (struct STNode *)AllocVec(sizeof(struct STNode) * g_Config.c_Nodes,
                                        MEMF_ANY | MEMF_CLEAR);
    if (!g_Nodes)
        return FALSE;

    for (i = 0; i < g_Config.c_Nodes; i++)
    {
        if (!node_init(&g_Nodes[i], i))
            return FALSE;
    }

    return TRUE;
}

static void nodes_destroy(void)
{
    UWORD i;

    if (!g_Nodes)
        return;

    for (i = 0; i < g_Config.c_Nodes; i++)
    {
        if (g_Nodes[i].n_Attached)
            node_detach(&g_Nodes[i]);
        node_cleanup(&g_Nodes[i]);
    }

    FreeVec(g_Nodes);
    g_Nodes = NULL;
}

/* ------------------------------------------------------------------ */
/* Detaching from the shell                                            */
/* ------------------------------------------------------------------ */

/*
 * A daemon that holds on to the Shell it was launched from is a nuisance: the
 * window cannot be used for anything else and closing it kills the BBS.  So
 * unless told otherwise we relaunch ourselves as a background process and let
 * the foreground copy exit straight away, which is what the user expects from
 * typing the command by hand.
 *
 * Re-running the command is deliberate.  The obvious alternative -- spawning a
 * process with CreateNewProc() and exiting -- does not work here, because the
 * Shell unloads the command's code the moment the foreground copy returns, and
 * the background copy would be executing memory that had just been freed.
 */

/* Case-insensitive compare, so both DETACH and detach are accepted. */
static BOOL arg_is(const char *s, const char *word)
{
    while (*s && *word)
    {
        char a = *s++, b = *word++;
        if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (b >= 'a' && b <= 'z') b = (char)(b - 32);
        if (a != b)
            return FALSE;
    }
    return *s == '\0' && *word == '\0';
}

/*
 * Where our own executable lives.  GetProgramName() gives the name as it was
 * typed, which may be a bare command found on the path, so it is resolved
 * against GetProgramDir() to get something the background Shell can find
 * regardless of what its own path and current directory are.
 */
static BOOL program_path(char *buf, LONG len)
{
    char name[128];
    BPTR dir;

    if (!GetProgramName((STRPTR)name, sizeof(name)))
        return FALSE;

    dir = GetProgramDir();
    if (!dir)
    {
        strncpy(buf, name, len - 1);
        buf[len - 1] = '\0';
        return TRUE;
    }

    if (!NameFromLock(dir, (STRPTR)buf, len))
        return FALSE;

    return AddPart((STRPTR)buf, (CONST_STRPTR)FilePart((CONST_STRPTR)name), len) ? TRUE : FALSE;
}

/*
 * Returns TRUE if a background copy has been started and this process should
 * exit quietly.  FALSE means carry on and run the daemon here.
 */
static BOOL detach_self(const char *conf)
{
    struct Process              *me = (struct Process *)FindTask(NULL);
    struct CommandLineInterface *cli;
    char  path[256];
    char  cmd[400];
    BPTR  in, out;
    LONG  rc;

    /* Started from Workbench: there is no Shell to give back. */
    if (!me->pr_CLI)
        return FALSE;

    /* Already in the background -- launched with Run, or from a script that
     * did.  Detaching again would just add a pointless second process. */
    cli = (struct CommandLineInterface *)BADDR(me->pr_CLI);
    if (cli->cli_Background)
        return FALSE;

    if (!program_path(path, sizeof(path)))
    {
        printf("SerialTCPd: cannot work out my own path, staying in the shell\n");
        return FALSE;
    }

    snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\" NODETACH", path, conf);

    in  = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
    if (!in || !out)
    {
        if (in)  Close(in);
        if (out) Close(out);
        printf("SerialTCPd: cannot open NIL:, staying in the shell\n");
        return FALSE;
    }

    /* SYS_Asynch hands ownership of both handles to the new process, which
     * closes them when it exits -- so they must not be closed here unless the
     * call itself failed. */
    rc = SystemTags((CONST_STRPTR)cmd,
                    SYS_Input,  (Tag)in,
                    SYS_Output, (Tag)out,
                    SYS_Asynch, (Tag)TRUE,
                    NP_Name,    (Tag)"SerialTCPd",
                    TAG_DONE);

    if (rc == -1)
    {
        Close(in);
        Close(out);
        printf("SerialTCPd: could not start a background process, "
               "staying in the shell\n");
        return FALSE;
    }

    return TRUE;
}

static ULONG memory_estimate(void)
{
    return (ULONG)sizeof(struct STNode) * g_Config.c_Nodes
         + (g_Config.c_RxBufSize + g_Config.c_TxBufSize) * g_Config.c_Nodes
         + sizeof(struct STDaemonPort)
         + ((g_Config.c_BusyAction == WB_BUSY)
                ? 0
                : (ULONG)sizeof(struct QueueEntry) * g_Config.c_QueueMax);
}

int main(int argc, char **argv)
{
    const char *conf = "S:serialtcp.conf";
    ULONG       sigs, waitmask;
    fd_set      rd, wr;
    LONG        nfds, rc;
    struct timeval tv;
    UWORD       i;
    int         a;
    BOOL        noDetach = FALSE;

    (void)verstag;

    for (a = 1; a < argc; a++)
    {
        if (!strcmp(argv[a], "?") || arg_is(argv[a], "-h") || arg_is(argv[a], "HELP"))
        {
            printf("%s\n", VERSION_STRING);
            printf("Usage: SerialTCPd [configfile] [NODETACH]\n");
            printf("  configfile  where to read settings from (default %s)\n", conf);
            printf("  NODETACH    stay in this shell instead of going into\n"
                   "              the background\n");
            return 0;
        }

        if (arg_is(argv[a], "NODETACH") || arg_is(argv[a], "FOREGROUND"))
            noDetach = TRUE;
        else
            conf = argv[a];
    }

    /*
     * Do this before anything is opened or allocated: the background copy
     * starts from scratch, so there is nothing here worth setting up first.
     */
    if (!noDetach && detach_self(conf))
    {
        printf("SerialTCPd: running in the background. "
               "Use SerialTCPStatus to see what it is doing,\n"
               "             or SerialTCPd NODETACH to keep it in this shell.\n");
        return 0;
    }

    config_defaults();
    if (!config_load(conf))
        printf("SerialTCPd: no config at '%s', using defaults\n", conf);

    /* Timer first: log_printf() timestamps every line from it, so opening it
     * afterwards would stamp the startup lines 00:00:00. */
    if (!timer_open())
    {
        printf("SerialTCPd: FATAL: cannot open timer.device\n");
        return 20;
    }

    st_gettime(&g_StartTime);

    log_open();
    log_printf("%s starting", VERSION_STRING);

    SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
    if (!SocketBase)
    {
        log_printf("FATAL: cannot open bsdsocket.library -- is your TCP/IP stack running?");
        log_close();
        timer_close();
        return 20;
    }

    if (!nodes_create())
    {
        log_printf("FATAL: out of memory allocating %u nodes", (unsigned)g_Config.c_Nodes);
        goto cleanup;
    }

    if (!queue_init())
    {
        log_printf("FATAL: out of memory allocating the caller queue");
        goto cleanup;
    }

    if (!port_create())
        goto cleanup;

    if (!net_init())
    {
        log_printf("FATAL: no listening sockets could be created");
        goto cleanup;
    }

    {
        LONG li;

        /* Reserve every unit that has a listener of its own. */
        for (li = 0; li < g_NumListeners; li++)
        {
            WORD nd = g_Listeners[li].ln_Node;
            if (nd != ST_NODE_POOL && nd < (WORD)g_Config.c_Nodes)
                g_Nodes[nd].n_PoolExcluded = TRUE;
        }

        for (li = 0; li < g_NumListeners; li++)
        {
            if (g_Listeners[li].ln_Node == ST_NODE_POOL)
                log_printf("listening on port %u -> shared pool",
                           (unsigned)g_Listeners[li].ln_Port);
            else
            {
                WORD nd = g_Listeners[li].ln_Node;

                log_printf("listening on port %u -> node %d only "
                           "(reserved: pool callers will not be sent there)",
                           (unsigned)g_Listeners[li].ln_Port, (int)nd);

                if (nd >= (WORD)g_Config.c_Nodes)
                    log_printf("WARNING: port %u is bound to node %d, which does "
                               "not exist (nodes = %u)",
                               (unsigned)g_Listeners[li].ln_Port, (int)nd,
                               (unsigned)g_Config.c_Nodes);
                else if (g_Nodes[nd].n_OutboundOnly)
                    log_printf("WARNING: port %u is bound to node %d, but that node "
                               "is outbound-only and will never answer",
                               (unsigned)g_Listeners[li].ln_Port, (int)nd);
            }
        }
    }

    log_printf("%u nodes, telnet %s, about %lu bytes of buffers",
               (unsigned)g_Config.c_Nodes,
               g_Config.c_Telnet ? "on" : "off",
               (unsigned long)memory_estimate());
    /*
     * Say which nodes will never answer, and shout if that is all of them --
     * otherwise the first symptom is every caller getting a busy signal for
     * no visible reason.
     */
    {
        UWORD i, inbound = 0;
        char  list[128];
        int   off = 0;

        list[0] = '\0';
        for (i = 0; i < g_Config.c_Nodes; i++)
        {
            if (g_Nodes[i].n_OutboundOnly)
                off += snprintf(list + off, sizeof(list) - off, "%s%u",
                                off ? "," : "", (unsigned)i);
            else
                inbound++;
        }

        if (list[0])
            log_printf("dial-out only (never answer): node %s", list);

        if (inbound == 0)
            log_printf("WARNING: every node is dial-out only -- no incoming "
                       "call can ever be answered");
    }

    log_printf("when all nodes are busy: %s%s",
               g_Config.c_BusyAction == WB_QUEUE ? "queue the caller" :
               g_Config.c_BusyAction == WB_ASK   ? "ask the caller whether to wait" :
                                                   "return busy and hang up",
               g_Config.c_BusyAction == WB_BUSY ? "" : " (queue-max reached = busy)");
    log_printf("ready -- open %s unit 0..%u, or press Ctrl-C to stop",
               ST_DEVICE_NAME, (unsigned)(g_Config.c_Nodes - 1));

    waitmask = (1UL << g_PortSig) | (1UL << g_IoSig) | SIGBREAKF_CTRL_C;

    while (!g_Quit)
    {
        nfds = net_build_fds(&rd, &wr);

        sigs = waitmask;

        /*
         * A short timeout keeps the ring cadence, dial timeouts and the +++
         * guard timer ticking even when nothing at all is happening on the
         * sockets.  250ms is far finer than any of those need and costs
         * essentially nothing.
         */
        tv.tv_secs  = 0;
        tv.tv_micro = 250000;

        rc = WaitSelect(nfds, &rd, &wr, NULL, (struct __timeval *)&tv, &sigs);

        if (sigs & SIGBREAKF_CTRL_C)
        {
            log_printf("Ctrl-C: shutting down");
            g_Quit = TRUE;
        }

        if (rc > 0)
        {
            net_handle_fds(&rd, &wr);
        }
        else
        {
            /* Timed out or was interrupted by a signal.  The fd sets are not
             * meaningful now, so clear them and run the pass anyway: it is
             * what drives the time-based state changes. */
            FD_ZERO(&rd);
            FD_ZERO(&wr);
            net_handle_fds(&rd, &wr);
        }

        if (sigs & (1UL << g_PortSig))
            handle_port_messages();

        /*
         * Always sweep every node.  Exec coalesces signals, so one wakeup can
         * stand for several requests across several units; scanning
         * unconditionally is simpler than trying to work out which.
         */
        for (i = 0; i < g_Config.c_Nodes; i++)
        {
            node_service_io(&g_Nodes[i]);
            node_pump(&g_Nodes[i]);
        }

        /*
         * Queue housekeeping runs after the nodes, so a node that just hung up
         * this pass is already on-hook and can take the next waiter without an
         * extra trip round the loop.
         */
        queue_pump();
        queue_dispatch();
    }

cleanup:
    log_printf("shutting down");

    net_shutdown();

    /*
     * Stop accepting attaches before tearing nodes down, so a BBS opening the
     * device during shutdown gets a clean failure rather than a half-built
     * unit.
     */
    if (g_Port && g_PortAdded)
    {
        Forbid();
        RemPort(&g_Port->dp_Port);
        g_PortAdded = FALSE;
        Permit();

        /* Anything that slipped in before the port vanished still needs a
         * reply, or its sender waits forever. */
        handle_port_messages();
    }

    queue_cleanup();
    nodes_destroy();
    port_delete();
    timer_close();

    if (SocketBase)
        CloseLibrary(SocketBase);

    log_printf("stopped");
    log_close();
    return 0;
}
