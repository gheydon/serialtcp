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
 * statcommon.c -- talking to SerialTCPd, shared by the GUI and CLI clients.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dostags.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <clib/alib_protos.h>

#include <stdio.h>
#include <string.h>

#include "statcommon.h"

const char *g_DaemonCmd = DAEMON_COMMAND;

/* ------------------------------------------------------------------ */
/* Protocol                                                            */
/* ------------------------------------------------------------------ */

BOOL daemon_present(void)
{
    struct STDaemonPort *dp;
    BOOL                 found;

    Forbid();
    dp = (struct STDaemonPort *)FindPort((CONST_STRPTR)ST_PORT_NAME);
    found = (dp && dp->dp_Magic == ST_MAGIC && dp->dp_Version == ST_PROTOCOL_VER);
    Permit();

    return found;
}

/*
 * Post a message and wait for the reply.
 *
 * The lookup and the PutMsg happen inside one Forbid so the daemon cannot exit
 * between them and leave us waiting on a reply that will never come.
 */
static BOOL send_to_daemon(struct Message *msg, ULONG msglen)
{
    struct MsgPort      *rp;
    struct STDaemonPort *dp;

    rp = CreateMsgPort();
    if (!rp)
        return FALSE;

    msg->mn_Node.ln_Type = NT_MESSAGE;
    msg->mn_ReplyPort    = rp;
    msg->mn_Length       = (UWORD)msglen;

    Forbid();
    dp = (struct STDaemonPort *)FindPort((CONST_STRPTR)ST_PORT_NAME);
    if (dp && dp->dp_Magic == ST_MAGIC && dp->dp_Version == ST_PROTOCOL_VER)
        PutMsg(&dp->dp_Port, msg);
    else
        dp = NULL;
    Permit();

    if (!dp)
    {
        DeleteMsgPort(rp);
        return FALSE;
    }

    WaitPort(rp);
    GetMsg(rp);
    DeleteMsgPort(rp);
    return TRUE;
}

BOOL query_daemon(struct Snapshot *snap)
{
    struct STStatusMsg sm;

    snap->ok = FALSE;

    memset(&sm, 0, sizeof(sm));
    sm.sm_Command  = STM_STATUS;
    sm.sm_Version  = ST_PROTOCOL_VER;
    sm.sm_MaxNodes = MAX_SHOWN;
    sm.sm_Nodes    = snap->nodes;
    sm.sm_Error    = STE_SHUTTING_DOWN;

    if (!send_to_daemon(&sm.sm_Msg, sizeof(sm)))
        return FALSE;

    if (sm.sm_Error != STE_OK)
        return FALSE;

    snap->ok           = TRUE;
    snap->numNodes     = sm.sm_NumNodes;
    snap->uptime       = sm.sm_UptimeSecs;
    snap->totalCalls   = sm.sm_TotalCalls;
    snap->busyCalls    = sm.sm_BusyCalls;
    snap->numListeners = sm.sm_NumListeners;
    snap->queuedCalls  = sm.sm_QueuedCalls;
    snap->queued       = sm.sm_Queued;
    snap->queueMax     = sm.sm_QueueMax;
    snap->qServed      = sm.sm_QueueServed;
    snap->qAbandoned   = sm.sm_QueueAbandoned;
    snap->qTimedOut    = sm.sm_QueueTimedOut;
    snap->qDeclined    = sm.sm_QueueDeclined;
    snap->qMaxWait     = sm.sm_QueueMaxWait;
    snap->qAvgWait     = sm.sm_QueueAvgWait;
    memcpy(snap->listenPorts, sm.sm_ListenPorts, sizeof(snap->listenPorts));
    memcpy(snap->listenNodes, sm.sm_ListenNodes, sizeof(snap->listenNodes));

    return TRUE;
}

LONG stop_daemon(BOOL force, UWORD *activeOut)
{
    struct STControlMsg cm;

    memset(&cm, 0, sizeof(cm));
    cm.cm_Command = STM_CONTROL;
    cm.cm_Version = ST_PROTOCOL_VER;
    cm.cm_Action  = STC_SHUTDOWN;
    cm.cm_Flags   = force ? STCF_FORCE : 0;
    cm.cm_Error   = STE_SHUTTING_DOWN;

    if (!send_to_daemon(&cm.cm_Msg, sizeof(cm)))
        return -1;

    if (activeOut)
        *activeOut = cm.cm_ActiveNodes;

    return cm.cm_Error;
}

BOOL wait_for_stop(int maxTicks)
{
    int i;

    for (i = 0; i < maxTicks; i++)
    {
        if (!daemon_present())
            return TRUE;
        Delay(10);              /* 10 ticks = 0.2 seconds */
    }

    return !daemon_present();
}

BOOL wait_for_start(int maxTicks)
{
    int i;

    for (i = 0; i < maxTicks; i++)
    {
        if (daemon_present())
            return TRUE;
        Delay(10);
    }

    return daemon_present();
}

/*
 * Launch the daemon detached from us.  SYS_Asynch means DOS owns the streams
 * and closes them, so we hand it NIL: -- the daemon's own log file keeps the
 * record, and its lifetime must not be tied to this program's.
 */
BOOL start_daemon(void)
{
    BPTR in, out;
    LONG rc;

    if (daemon_present())
        return TRUE;

    in  = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
    out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);

    if (!in || !out)
    {
        if (in)  Close(in);
        if (out) Close(out);
        return FALSE;
    }

    rc = SystemTags((CONST_STRPTR)g_DaemonCmd,
                    SYS_Input,  (ULONG)in,
                    SYS_Output, (ULONG)out,
                    SYS_Asynch, TRUE,
                    NP_Name,    (ULONG)"SerialTCPd",
                    TAG_DONE);

    if (rc != 0)
    {
        /* SYS_Asynch only takes ownership of the streams on success. */
        Close(in);
        Close(out);
        return FALSE;
    }

    return wait_for_start(50);      /* up to 10 seconds */
}

/* ------------------------------------------------------------------ */
/* Formatting                                                          */
/* ------------------------------------------------------------------ */

const char *state_plain(UWORD s, UWORD flags)
{
    if (s == STS_IDLE && (flags & STSF_OUTBOUND_ONLY))
        return "dial-out";

    switch (s)
    {
    case STS_DETACHED: return "closed";
    case STS_IDLE:     return "waiting";
    case STS_DIALING:  return "dialling";
    case STS_RINGING:  return "RINGING";
    case STS_ONLINE:   return "ONLINE";
    case STS_ESCAPED:  return "cmd mode";
    default:           return "?";
    }
}

void fmt_duration(char *buf, ULONG size, ULONG secs)
{
    if (secs >= 3600)
        snprintf(buf, size, "%lu:%02lu:%02lu",
                 (unsigned long)(secs / 3600),
                 (unsigned long)((secs % 3600) / 60),
                 (unsigned long)(secs % 60));
    else
        snprintf(buf, size, "%lu:%02lu",
                 (unsigned long)(secs / 60), (unsigned long)(secs % 60));
}

/* Bytes as a short figure, so columns stay narrow. */
void fmt_bytes(char *buf, ULONG size, ULONG n)
{
    if (n < 1024)
        snprintf(buf, size, "%lu", (unsigned long)n);
    else if (n < 1024UL * 1024UL)
        snprintf(buf, size, "%luK", (unsigned long)(n / 1024));
    else
        snprintf(buf, size, "%lu.%luM",
                 (unsigned long)(n / (1024UL * 1024UL)),
                 (unsigned long)(((n % (1024UL * 1024UL)) * 10) / (1024UL * 1024UL)));
}

/* ------------------------------------------------------------------ */
/* The report                                                          */
/* ------------------------------------------------------------------ */

int print_status(BOOL brief)
{
    struct Snapshot snap;
    UWORD           i, inUse = 0;
    char            dur[16], in[16], out[16];

    if (!query_daemon(&snap))
    {
        printf("SerialTCPd is not running.\n");
        return 5;
    }

    for (i = 0; i < snap.numNodes; i++)
    {
        UWORD st = snap.nodes[i].ns_State;
        if (st == STS_ONLINE || st == STS_ESCAPED || st == STS_RINGING || st == STS_DIALING)
            inUse++;
    }

    if (brief)
    {
        /* One line, for a title bar or a script. */
        printf("%u/%u busy", (unsigned)inUse, (unsigned)snap.numNodes);
        if (snap.queueMax)
            printf(", %u queued", (unsigned)snap.queued);
        printf(", %lu calls, %lu busy, up %lum\n",
               (unsigned long)snap.totalCalls,
               (unsigned long)snap.busyCalls,
               (unsigned long)(snap.uptime / 60));
        return 0;
    }

    {
        UWORD dialout = 0, i2;
        for (i2 = 0; i2 < snap.numNodes; i2++)
            if (snap.nodes[i2].ns_Flags & STSF_OUTBOUND_ONLY)
                dialout++;

        printf("SerialTCPd  --  up %lum, %u of %u nodes busy",
               (unsigned long)(snap.uptime / 60), (unsigned)inUse, (unsigned)snap.numNodes);
        if (dialout)
            printf("  (%u dial-out only)", (unsigned)dialout);
        printf("\n");
    }

    for (i = 0; i < snap.numListeners && i < 8; i++)
    {
        if (snap.listenNodes[i] == ST_NODE_POOL)
            printf("listening on %-6u -> any free node\n", (unsigned)snap.listenPorts[i]);
        else
            printf("listening on %-6u -> node %d only\n",
                   (unsigned)snap.listenPorts[i], (int)snap.listenNodes[i]);
    }
    printf("calls %lu, turned away busy %lu\n\n",
           (unsigned long)snap.totalCalls, (unsigned long)snap.busyCalls);

    printf("Node State     Caller               Baud   Time     In      Out\n");
    printf("---- --------- -------------------- ------ -------- ------- -------\n");

    for (i = 0; i < snap.numNodes; i++)
    {
        struct STNodeStatus *n = &snap.nodes[i];
        BOOL up = (n->ns_State == STS_ONLINE || n->ns_State == STS_ESCAPED);

        fmt_duration(dur, sizeof(dur), n->ns_ConnectSecs);
        fmt_bytes(in, sizeof(in), n->ns_BytesIn);
        fmt_bytes(out, sizeof(out), n->ns_BytesOut);

        printf("%4u %-9s %-20s %6lu %-8s %-7s %-7s\n",
               (unsigned)n->ns_Unit,
               state_plain(n->ns_State, n->ns_Flags),
               n->ns_Peer[0] ? n->ns_Peer : "-",
               (unsigned long)n->ns_Baud,
               up ? dur : "-",
               in, out);
    }

    if (snap.queueMax)
    {
        printf("\nQueue: %u waiting of %u places, %lu joined in total\n",
               (unsigned)snap.queued, (unsigned)snap.queueMax,
               (unsigned long)snap.queuedCalls);
        printf("       put through %lu, hung up %lu, timed out %lu, declined %lu\n",
               (unsigned long)snap.qServed,
               (unsigned long)snap.qAbandoned,
               (unsigned long)snap.qTimedOut,
               (unsigned long)snap.qDeclined);
        if (snap.qServed)
        {
            char avg[16], max[16];
            fmt_duration(avg, sizeof(avg), snap.qAvgWait);
            fmt_duration(max, sizeof(max), snap.qMaxWait);
            printf("       wait %s average, %s longest\n", avg, max);
        }
    }

    return 0;
}
