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
 * SerialTCPStat -- MUI status display and control panel for SerialTCPd.
 *
 * Shows one row per configured node (what it is doing, who is connected, how
 * long for, how much data each way) and can start, stop and restart the
 * daemon.
 *
 * There is no MUI List with a display hook here on purpose.  A List hook needs
 * a register-argument callback, which is exactly the sort of thing that breaks
 * subtly between toolchains.  Node counts are small, so a grid of Text objects
 * updated in place is simpler, entirely portable, and looks the same.  Every
 * cell is sized with MUIA_FixWidthTxt, which is what keeps the columns aligned
 * across rows and stops the layout jumping as values change width.
 *
 * If muimaster.library is missing the program prints the same information to
 * the shell instead, so it still works over a serial console or in a script.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/ports.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <devices/timer.h>
#include <libraries/mui.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#include <clib/alib_protos.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "statcommon.h"
#include "graph.h"

/*
 * MUI 3.8 is muimaster.library 19, and that is what is actually installed on
 * most Amigas. The SDK header's MUIMASTER_VMIN is 20 (MUI 4 and later), so
 * using it would refuse to run on 3.8 for no reason -- nothing here needs
 * anything newer than 3.x. MUI 4 and 5 satisfy 19 as well.
 */
#define ST_MUIMASTER_VMIN 19

#define DEFAULT_ROWS   4

/* MUIMasterBase and IntuitionBase are declared by their proto headers. */

#ifndef MAKE_ID
#define MAKE_ID(a,b,c,d) ((ULONG)(a)<<24 | (ULONG)(b)<<16 | (ULONG)(c)<<8 | (ULONG)(d))
#endif

/* xget() is a convenience every MUI program defines for itself; the official
 * headers only supply the setter. */
#ifndef xget
static ULONG xget(Object *obj, ULONG attr)
{
    ULONG val = 0;
    DoMethod(obj, OM_GET, attr, (ULONG)&val);
    return val;
}
#endif

/* ------------------------------------------------------------------ */
/* Formatting (the MUI-flavoured half; the rest is in statcommon.c)     */
/* ------------------------------------------------------------------ */

static const char *state_name(UWORD s, UWORD flags)
{
    if (s == STS_IDLE && (flags & STSF_OUTBOUND_ONLY))
        return "dial-out";

    switch (s)
    {
    case STS_DETACHED: return "closed";
    case STS_IDLE:     return "waiting";
    case STS_DIALING:  return "dialling";
    case STS_RINGING:  return "\033bRINGING";
    case STS_ONLINE:   return "\033bONLINE";
    case STS_ESCAPED:  return "cmd mode";
    default:           return "?";
    }
}

/* ------------------------------------------------------------------ */
/* MUI interface                                                       */
/* ------------------------------------------------------------------ */

#define COL_NODE   0
#define COL_STATE  1
#define COL_PEER   2
#define COL_BAUD   3
#define COL_TIME   4
#define COL_IN     5
#define COL_OUT    6
#define NUM_COLS   7

#define ID_REFRESH  1
#define ID_START    2
#define ID_STOP     3
#define ID_RESTART  4

static Object *g_App;
static Object *g_Win;
static Object *g_Summary;
static Object *g_Cells[MAX_SHOWN][NUM_COLS];
static Object *g_BtnRefresh, *g_BtnStart, *g_BtnStop, *g_BtnRestart;
static Object *g_ChkAuto;
static Object *g_GraphNodes, *g_GraphQueue;
static Object *g_LblNodes,   *g_LblQueue, *g_LblQueueStats;

static struct MUI_CustomClass *g_GraphClass;

static char    g_LblNodesText[64];

/*
 * Byte totals from the previous refresh, so the graph can show a rate rather
 * than an ever-climbing total.  The daemon's counters are per call, so the sum
 * across nodes drops when somebody hangs up: a decrease is reported as zero
 * rather than as a negative rate.
 */
static ULONG   g_PrevIn, g_PrevOut;
static BOOL    g_HavePrev;
static char    g_LblQueueText[64];
static char    g_LblQueueStatsText[128];

static UWORD   g_ShownNodes;

/* MUI keeps the pointer rather than copying, so these buffers must outlive
 * the objects that point at them. */
static char    g_CellText[MAX_SHOWN][NUM_COLS][32];
static char    g_SummaryText[256];

static const char *col_label[NUM_COLS]  = { "Node", "State", "Caller", "Baud", "Time", "In", "Out" };
static const char *col_sample[NUM_COLS] = { "8888", "dialling ", "255.255.255.255:65535",
                                            "115200", "88:88:88", "8888.8M", "8888.8M" };

static Object *make_cell(int col, char *buf)
{
    return TextObject,
             MUIA_Text_Contents, buf,
             MUIA_Text_SetMin,   FALSE,
             MUIA_FixWidthTxt,   col_sample[col],
           End;
}

static Object *make_header(int col)
{
    return TextObject,
             MUIA_Text_Contents, col_label[col],
             MUIA_Text_PreParse, "\033b\033u",
             MUIA_Text_SetMin,   FALSE,
             MUIA_FixWidthTxt,   col_sample[col],
           End;
}

/*
 * Build the whole table before the ApplicationObject exists.  Adding children
 * to a group that is already part of a built application needs
 * MUIM_Group_InitChange/ExitChange; building it complete via a tag list side-
 * steps the question entirely.
 */
static Object *build_row(int rowIndex, BOOL header)
{
    struct TagItem *tags;
    Object         *row;
    ULONG           t = 0;
    int             c;

    tags = (struct TagItem *)AllocVec((NUM_COLS + 2) * sizeof(struct TagItem),
                                      MEMF_ANY | MEMF_CLEAR);
    if (!tags)
        return NULL;

    tags[t].ti_Tag    = MUIA_Group_Horiz;
    tags[t++].ti_Data = TRUE;

    for (c = 0; c < NUM_COLS; c++)
    {
        Object *o;

        if (header)
        {
            o = make_header(c);
        }
        else
        {
            strcpy(g_CellText[rowIndex][c], "-");
            o = make_cell(c, g_CellText[rowIndex][c]);
            g_Cells[rowIndex][c] = o;
        }

        if (!o)
        {
            FreeVec(tags);
            return NULL;
        }

        tags[t].ti_Tag    = MUIA_Group_Child;
        tags[t++].ti_Data = (ULONG)o;
    }

    tags[t].ti_Tag = TAG_DONE;

    row = MUI_NewObjectA((CONST_STRPTR)MUIC_Group, tags);
    FreeVec(tags);
    return row;
}

static Object *build_table(UWORD nodes)
{
    struct TagItem *tags;
    Object         *rows, *header, *scroll;
    ULONG           t = 0;
    UWORD           i;

    /* The header stays outside the scrolling area so the column titles
     * remain visible however far down the list you are. Both use the same
     * MUIA_FixWidthTxt samples, so they stay lined up. */
    header = build_row(0, TRUE);
    if (!header)
        return NULL;

    tags = (struct TagItem *)AllocVec((nodes + 2) * sizeof(struct TagItem),
                                      MEMF_ANY | MEMF_CLEAR);
    if (!tags)
        return NULL;

    for (i = 0; i < nodes; i++)
    {
        Object *row = build_row(i, FALSE);
        if (!row)
        {
            FreeVec(tags);
            return NULL;
        }
        tags[t].ti_Tag    = MUIA_Group_Child;
        tags[t++].ti_Data = (ULONG)row;
    }
    tags[t].ti_Tag = TAG_DONE;

    rows = MUI_NewObjectA((CONST_STRPTR)MUIC_Group, tags);
    FreeVec(tags);
    if (!rows)
        return NULL;

    /*
     * Scrollable, so a machine with more nodes than fit on screen is still
     * usable. FreeHoriz is off because the columns are fixed width and
     * always fit -- a horizontal scrollbar would only ever be in the way.
     */
    scroll = ScrollgroupObject,
        MUIA_Scrollgroup_FreeHoriz, FALSE,
        MUIA_Scrollgroup_Contents, VirtgroupObject,
            Child, rows,
        End,
    End;

    if (!scroll)
    {
        MUI_DisposeObject(rows);
        return NULL;
    }

    /*
     * The heavier vertical weight makes this the part that grows when the
     * window is resized: more nodes visible is a better use of extra height
     * than taller graphs.
     */
    return VGroup,
        MUIA_Frame,      MUIV_Frame_Group,
        MUIA_VertWeight, 300,
        Child, header,
        Child, scroll,
    End;
}

/*
 * The two history graphs, side by side.  Returns an empty group if the custom
 * class could not be made, so the rest of the window still works -- the graphs
 * are a nicety, not the point of the program.
 */
static Object *build_graphs(void)
{
    strcpy(g_LblNodesText, "Nodes in use");
    strcpy(g_LblQueueText, "Queue");
    strcpy(g_LblQueueStatsText, "");

    if (!g_GraphClass)
        return HGroup, MUIA_ShowMe, FALSE, End;

    g_GraphNodes = NewObject(g_GraphClass->mcc_Class, NULL, TAG_DONE);
    g_GraphQueue = NewObject(g_GraphClass->mcc_Class, NULL, TAG_DONE);

    if (!g_GraphNodes || !g_GraphQueue)
    {
        if (g_GraphNodes) MUI_DisposeObject(g_GraphNodes);
        if (g_GraphQueue) MUI_DisposeObject(g_GraphQueue);
        g_GraphNodes = g_GraphQueue = NULL;
        return HGroup, MUIA_ShowMe, FALSE, End;
    }

    /* Lighter than the node table, so extra window height goes to the list
     * of nodes rather than to taller graphs. */
    return HGroup,
        MUIA_VertWeight, 100,
        Child, VGroup,
            MUIA_Frame,      MUIV_Frame_Group,
            MUIA_FrameTitle, "Nodes and traffic",
            Child, g_LblNodes = TextObject,
                MUIA_Text_Contents, g_LblNodesText,
                MUIA_Text_SetMin,   FALSE,
            End,
            Child, g_GraphNodes,
        End,
        Child, VGroup,
            MUIA_Frame,      MUIV_Frame_Group,
            MUIA_FrameTitle, "Queue",
            Child, g_LblQueue = TextObject,
                MUIA_Text_Contents, g_LblQueueText,
                MUIA_Text_SetMin,   FALSE,
            End,
            Child, g_GraphQueue,
            Child, g_LblQueueStats = TextObject,
                MUIA_Text_Contents, g_LblQueueStatsText,
                MUIA_Text_SetMin,   FALSE,
            End,
        End,
    End;
}

static BOOL build_gui(UWORD nodes)
{
    Object *table;
    Object *graphs;

    g_ShownNodes = nodes;
    strcpy(g_SummaryText, "Querying daemon...");

    table = build_table(nodes);
    if (!table)
        return FALSE;

    graphs = build_graphs();

    g_App = ApplicationObject,
        MUIA_Application_Title,       "SerialTCPStat",
        MUIA_Application_Version,     "$VER: SerialTCPStat 1.0 (11.9.2026)",
        MUIA_Application_Copyright,   "(C) 2026 Gordon Heydon, GPLv2+",
        MUIA_Application_Author,      "SerialTCP",
        MUIA_Application_Description, "Status and control for SerialTCPd",
        MUIA_Application_Base,        "SERIALTCPSTAT",

        SubWindow, g_Win = WindowObject,
            MUIA_Window_Title, "SerialTCP -- node status",
            MUIA_Window_ID,    MAKE_ID('S','T','C','P'),
            WindowContents, VGroup,

                Child, g_Summary = TextObject,
                    MUIA_Text_Contents, g_SummaryText,
                    MUIA_Frame,         MUIV_Frame_Text,
                    MUIA_Background,    MUII_TextBack,
                End,

                Child, table,

                Child, graphs,

                Child, HGroup,
                    Child, g_BtnRefresh = SimpleButton("_Refresh"),
                    Child, g_BtnStart   = SimpleButton("S_tart"),
                    Child, g_BtnStop    = SimpleButton("Sto_p"),
                    Child, g_BtnRestart = SimpleButton("Re_start"),
                End,

                Child, HGroup,
                    Child, HSpace(0),
                    Child, g_ChkAuto = ImageObject,
                        MUIA_Frame,        MUIV_Frame_ImageButton,
                        MUIA_InputMode,    MUIV_InputMode_Toggle,
                        MUIA_Image_Spec,   MUII_CheckMark,
                        MUIA_Selected,     TRUE,
                        MUIA_ShowSelState, FALSE,
                    End,
                    Child, Label("Auto-refresh"),
                    Child, HSpace(0),
                End,
            End,
        End,
    End;

    if (!g_App)
    {
        MUI_DisposeObject(table);
        return FALSE;
    }

    DoMethod(g_Win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             g_App, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);

    DoMethod(g_BtnRefresh, MUIM_Notify, MUIA_Pressed, FALSE,
             g_App, 2, MUIM_Application_ReturnID, ID_REFRESH);
    DoMethod(g_BtnStart, MUIM_Notify, MUIA_Pressed, FALSE,
             g_App, 2, MUIM_Application_ReturnID, ID_START);
    DoMethod(g_BtnStop, MUIM_Notify, MUIA_Pressed, FALSE,
             g_App, 2, MUIM_Application_ReturnID, ID_STOP);
    DoMethod(g_BtnRestart, MUIM_Notify, MUIA_Pressed, FALSE,
             g_App, 2, MUIM_Application_ReturnID, ID_RESTART);

    return TRUE;
}

static void set_cell(UWORD row, int col, const char *text)
{
    if (row >= g_ShownNodes)
        return;

    strncpy(g_CellText[row][col], text, sizeof(g_CellText[row][col]) - 1);
    g_CellText[row][col][sizeof(g_CellText[row][col]) - 1] = '\0';

    set(g_Cells[row][col], MUIA_Text_Contents, g_CellText[row][col]);
}

static void clear_rows(void)
{
    UWORD i;
    int   c;

    for (i = 0; i < g_ShownNodes; i++)
        for (c = 0; c < NUM_COLS; c++)
            set_cell(i, c, "-");
}

static void refresh_gui(void)
{
    struct Snapshot snap;
    UWORD           i;
    char            tmp[32];
    BOOL            running;

    running = query_daemon(&snap);

    /*
     * Feed the history graphs first, so a tick where the daemon is down still
     * records a zero rather than leaving a gap the trace would draw through.
     */
    {
        UWORD inUse = 0;
        ULONG totIn = 0, totOut = 0;
        ULONG rateIn = 0, rateOut = 0;

        if (running)
        {
            for (i = 0; i < snap.numNodes; i++)
            {
                UWORD st = snap.nodes[i].ns_State;
                if (st == STS_ONLINE || st == STS_ESCAPED ||
                    st == STS_RINGING || st == STS_DIALING)
                    inUse++;

                totIn  += snap.nodes[i].ns_BytesIn;
                totOut += snap.nodes[i].ns_BytesOut;
            }

            if (g_HavePrev)
            {
                if (totIn  > g_PrevIn)  rateIn  = totIn  - g_PrevIn;
                if (totOut > g_PrevOut) rateOut = totOut - g_PrevOut;
            }
        }

        g_PrevIn   = totIn;
        g_PrevOut  = totOut;
        g_HavePrev = running;

        /*
         * Series 0 last: it is the one that triggers the redraw, so the two
         * traffic traces are already in place when the panel repaints.  Both
         * of those pass max 0 and find their own scale, which is the only way
         * a byte rate can share a panel with a node count.
         */
        if (g_GraphNodes)
        {
            DoMethod(g_GraphNodes, MUIM_Graph_Push, 1, rateIn,  0, (ULONG)"in");
            DoMethod(g_GraphNodes, MUIM_Graph_Push, 2, rateOut, 0, (ULONG)"out");
            DoMethod(g_GraphNodes, MUIM_Graph_Push, 0, (ULONG)inUse,
                     (ULONG)(running && snap.numNodes ? snap.numNodes : 1),
                     (ULONG)"nodes");
        }

        if (g_GraphQueue)
            DoMethod(g_GraphQueue, MUIM_Graph_Push, 0, (ULONG)(running ? snap.queued : 0),
                     (ULONG)(running && snap.queueMax ? snap.queueMax : 1), 0);

        if (g_LblNodes)
        {
            if (running)
            {
                char inbuf[16], outbuf[16];

                fmt_bytes(inbuf,  sizeof(inbuf),  rateIn);
                fmt_bytes(outbuf, sizeof(outbuf), rateOut);

                snprintf(g_LblNodesText, sizeof(g_LblNodesText),
                         "%u of %u busy   in %s/s  out %s/s",
                         (unsigned)inUse, (unsigned)snap.numNodes, inbuf, outbuf);
            }
            else
                strcpy(g_LblNodesText, "-");
            set(g_LblNodes, MUIA_Text_Contents, g_LblNodesText);
        }

        if (g_LblQueue)
        {
            if (!running)
                strcpy(g_LblQueueText, "-");
            else if (!snap.queueMax)
                strcpy(g_LblQueueText, "queueing off");
            else
                /* Kept short: the panel is only half the window wide, and
                 * the longer form was clipping. The running total lives on
                 * the stats line below instead. */
                snprintf(g_LblQueueText, sizeof(g_LblQueueText),
                         "%u waiting of %u",
                         (unsigned)snap.queued, (unsigned)snap.queueMax);
            set(g_LblQueue, MUIA_Text_Contents, g_LblQueueText);
        }

        if (g_LblQueueStats)
        {
            /*
             * How waiting callers ended up. "left" is the number who hung up
             * rather than wait it out -- the figure worth watching, because a
             * high one means the queue is not worth having at that length.
             */
            if (running && snap.queueMax)
            {
                char wait[24];

                if (snap.qServed)
                    snprintf(wait, sizeof(wait), "%lu:%02lu",
                             (unsigned long)(snap.qAvgWait / 60),
                             (unsigned long)(snap.qAvgWait % 60));
                else
                    strcpy(wait, "-");

                /* Kept terse: this sits under a half-width panel, and the
                 * longer wording was being clipped. */
                snprintf(g_LblQueueStatsText, sizeof(g_LblQueueStatsText),
                         "in %lu  thru %lu  left %lu  wait %s",
                         (unsigned long)snap.queuedCalls,
                         (unsigned long)snap.qServed,
                         (unsigned long)snap.qAbandoned,
                         wait);
            }
            else
            {
                strcpy(g_LblQueueStatsText, "-");
            }

            set(g_LblQueueStats, MUIA_Text_Contents, g_LblQueueStatsText);
        }
    }

    /* Only offer the actions that make sense right now. */
    set(g_BtnStart,   MUIA_Disabled, running);
    set(g_BtnStop,    MUIA_Disabled, !running);
    set(g_BtnRestart, MUIA_Disabled, !running);

    if (!running)
    {
        snprintf(g_SummaryText, sizeof(g_SummaryText),
                 "\033bDaemon not running.\033n  Press Start to launch it.");
        set(g_Summary, MUIA_Text_Contents, g_SummaryText);
        clear_rows();
        return;
    }

    {
        char  ports[64];
        int   off = 0;
        UWORD p;

        ports[0] = '\0';
        for (p = 0; p < snap.numListeners && p < 8; p++)
        {
            /* A dedicated listener is shown as port>node, so it is obvious at
             * a glance which numbers reach the pool and which do not. */
            if (snap.listenNodes[p] == ST_NODE_POOL)
                off += snprintf(ports + off, sizeof(ports) - off, "%s%u",
                                p ? "," : "", (unsigned)snap.listenPorts[p]);
            else
                off += snprintf(ports + off, sizeof(ports) - off, "%s%u>%d",
                                p ? "," : "", (unsigned)snap.listenPorts[p],
                                (int)snap.listenNodes[p]);
        }

        char queue[48];

        /* Only mention the queue when it is switched on, and make it stand out
         * when somebody is actually waiting. */
        if (!snap.queueMax)
            queue[0] = '\0';
        else if (snap.queued)
            snprintf(queue, sizeof(queue), "   \033bQueue %u/%u\033n",
                     (unsigned)snap.queued, (unsigned)snap.queueMax);
        else
            snprintf(queue, sizeof(queue), "   Queue 0/%u", (unsigned)snap.queueMax);

        if (snap.numNodes > g_ShownNodes)
            snprintf(g_SummaryText, sizeof(g_SummaryText),
                     "Port %s   Up %lum   Calls %lu   Busy %lu%s   "
                     "\033b(showing %u of %u nodes -- restart this program)\033n",
                     ports,
                     (unsigned long)(snap.uptime / 60),
                     (unsigned long)snap.totalCalls,
                     (unsigned long)snap.busyCalls,
                     queue,
                     (unsigned)g_ShownNodes, (unsigned)snap.numNodes);
        else
            snprintf(g_SummaryText, sizeof(g_SummaryText),
                     "Port %s   Up %lum   Calls %lu   Busy %lu%s",
                     ports,
                     (unsigned long)(snap.uptime / 60),
                     (unsigned long)snap.totalCalls,
                     (unsigned long)snap.busyCalls,
                     queue);

        set(g_Summary, MUIA_Text_Contents, g_SummaryText);
    }

    for (i = 0; i < g_ShownNodes; i++)
    {
        struct STNodeStatus *n;

        if (i >= snap.numNodes)
        {
            int c;
            for (c = 0; c < NUM_COLS; c++)
                set_cell(i, c, "-");
            continue;
        }

        n = &snap.nodes[i];

        snprintf(tmp, sizeof(tmp), "%u", (unsigned)n->ns_Unit);
        set_cell(i, COL_NODE, tmp);

        set_cell(i, COL_STATE, state_name(n->ns_State, n->ns_Flags));
        set_cell(i, COL_PEER, n->ns_Peer[0] ? n->ns_Peer : "-");

        if (n->ns_Baud)
        {
            snprintf(tmp, sizeof(tmp), "%lu", (unsigned long)n->ns_Baud);
            set_cell(i, COL_BAUD, tmp);
        }
        else
        {
            set_cell(i, COL_BAUD, "-");
        }

        if (n->ns_State == STS_ONLINE || n->ns_State == STS_ESCAPED)
        {
            fmt_duration(tmp, sizeof(tmp), n->ns_ConnectSecs);
            set_cell(i, COL_TIME, tmp);
        }
        else if (n->ns_State == STS_RINGING)
        {
            snprintf(tmp, sizeof(tmp), "ring %u", (unsigned)n->ns_Rings);
            set_cell(i, COL_TIME, tmp);
        }
        else
        {
            set_cell(i, COL_TIME, "-");
        }

        fmt_bytes(tmp, sizeof(tmp), n->ns_BytesIn);
        set_cell(i, COL_IN, tmp);

        fmt_bytes(tmp, sizeof(tmp), n->ns_BytesOut);
        set_cell(i, COL_OUT, tmp);
    }
}

/*
 * Stop the daemon, asking first if callers would be cut off.
 * Returns TRUE if it is now stopped.
 */
static BOOL do_stop(void)
{
    UWORD active = 0;
    LONG  rc;

    rc = stop_daemon(FALSE, &active);

    if (rc == -1)
        return TRUE;                    /* already gone */

    if (rc == STE_NODES_ACTIVE)
    {
        LONG answer = MUI_Request(g_App, g_Win, 0, (char *)"SerialTCP",
                                  (char *)"_Disconnect them|*_Cancel",
                                  (char *)"%ld node(s) still have callers connected.\n"
                                          "Stopping now will cut them off.",
                                  (LONG)active);

        if (answer != 1)
            return FALSE;

        rc = stop_daemon(TRUE, &active);
        if (rc == -1)
            return TRUE;
    }

    if (rc != STE_OK)
    {
        MUI_Request(g_App, g_Win, 0, (char *)"SerialTCP", (char *)"OK",
                    (char *)"The daemon refused to stop (error %ld).", (LONG)rc);
        return FALSE;
    }

    if (!wait_for_stop(50))             /* up to 10 seconds */
    {
        MUI_Request(g_App, g_Win, 0, (char *)"SerialTCP", (char *)"OK",
                    (char *)"The daemon did not shut down in time.");
        return FALSE;
    }

    return TRUE;
}

static void do_start(void)
{
    if (!start_daemon())
        MUI_Request(g_App, g_Win, 0, (char *)"SerialTCP", (char *)"OK",
                    (char *)"Could not start '%s'.\n"
                            "Check that it is in your path and that your\n"
                            "TCP/IP stack is running.", (LONG)g_DaemonCmd);
}

/* ------------------------------------------------------------------ */

static int run_gui(UWORD nodes)
{
    ULONG               sigs = 0;
    ULONG               id;
    struct MsgPort     *tport = NULL;
    struct timerequest *treq  = NULL;
    ULONG               timermask = 0;
    BOOL                timerRunning = FALSE;

    if (!build_gui(nodes))
    {
        printf("SerialTCPStat: could not create the MUI application\n");
        return 20;
    }

    /* A one-second tick drives auto-refresh; MUI has no periodic callback of
     * its own, so we add a timer signal to the wait mask. */
    tport = CreateMsgPort();
    if (tport)
    {
        treq = (struct timerequest *)CreateIORequest(tport, sizeof(struct timerequest));
        if (treq && OpenDevice((CONST_STRPTR)"timer.device", UNIT_VBLANK,
                               (struct IORequest *)treq, 0) == 0)
        {
            timermask = 1UL << tport->mp_SigBit;
        }
        else
        {
            if (treq) DeleteIORequest((struct IORequest *)treq);
            DeleteMsgPort(tport);
            treq  = NULL;
            tport = NULL;
        }
    }

    set(g_Win, MUIA_Window_Open, TRUE);

    if (!xget(g_Win, MUIA_Window_Open))
    {
        printf("SerialTCPStat: could not open the window\n");
        MUI_DisposeObject(g_App);
        return 20;
    }

    refresh_gui();

    for (;;)
    {
        id = DoMethod(g_App, MUIM_Application_NewInput, &sigs);

        if (id == (ULONG)MUIV_Application_ReturnID_Quit)
            break;

        switch (id)
        {
        case ID_REFRESH:
            refresh_gui();
            break;

        case ID_START:
            do_start();
            refresh_gui();
            break;

        case ID_STOP:
            do_stop();
            refresh_gui();
            break;

        case ID_RESTART:
            if (do_stop())
                do_start();
            refresh_gui();
            break;

        default:
            break;
        }

        /* Re-arm the tick only when we are about to wait for it. */
        if (treq && !timerRunning)
        {
            treq->tr_node.io_Command = TR_ADDREQUEST;
            treq->tr_time.tv_secs    = 1;
            treq->tr_time.tv_micro   = 0;
            SendIO((struct IORequest *)treq);
            timerRunning = TRUE;
        }

        if (sigs)
        {
            sigs = Wait(sigs | SIGBREAKF_CTRL_C | timermask);

            if (sigs & SIGBREAKF_CTRL_C)
                break;

            if (timermask && (sigs & timermask))
            {
                while (GetMsg(tport))
                    ;
                timerRunning = FALSE;

                if (xget(g_ChkAuto, MUIA_Selected))
                    refresh_gui();
            }
        }
    }

    if (treq)
    {
        if (timerRunning)
        {
            AbortIO((struct IORequest *)treq);
            WaitIO((struct IORequest *)treq);
        }
        CloseDevice((struct IORequest *)treq);
        DeleteIORequest((struct IORequest *)treq);
    }
    if (tport)
        DeleteMsgPort(tport);

    set(g_Win, MUIA_Window_Open, FALSE);
    MUI_DisposeObject(g_App);
    return 0;
}

/* ------------------------------------------------------------------ */

static void usage(void)
{
    printf("SerialTCPStat 1.0 -- status and control for SerialTCPd\n");
    printf("Usage: SerialTCPStat [-c] [-d command]\n");
    printf("  -c          text mode, no GUI (or use SerialTCPStatus)\n");
    printf("  -d command  command used to start the daemon (default '%s')\n", DAEMON_COMMAND);
}


int main(int argc, char **argv)
{
    struct Snapshot probe;
    UWORD           nodes;
    int             rc, i;
    BOOL            wantText = FALSE;

    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "-c") || !stricmp(argv[i], "CLI"))
            wantText = TRUE;
        else if (!strcmp(argv[i], "-d") && i + 1 < argc)
            g_DaemonCmd = argv[++i];
        else
        {
            usage();
            return 0;
        }
    }

    if (wantText)
        return print_status(FALSE);

    /*
     * Probe first so the table can be built with the right number of rows.
     * If the daemon is down we still open the window -- with it you can start
     * the thing, which is the whole point of having the buttons.
     */
    nodes = query_daemon(&probe) ? probe.numNodes : DEFAULT_ROWS;
    if (nodes == 0)
        nodes = DEFAULT_ROWS;
    if (nodes > MAX_SHOWN)
        nodes = MAX_SHOWN;

    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 37);
    MUIMasterBase = OpenLibrary((CONST_STRPTR)MUIMASTER_NAME, ST_MUIMASTER_VMIN);

    if (!MUIMasterBase || !IntuitionBase)
    {
        if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
        if (MUIMasterBase) CloseLibrary(MUIMasterBase);
        printf("SerialTCPStat: muimaster.library not available, using text mode.\n\n");
        return print_status(FALSE);
    }

    /* Optional: without it the window simply has no graphs. */
    g_GraphClass = graph_create_class();

    rc = run_gui(nodes);

    /* Fails if any object of the class is somehow still alive, which would
     * leave MUI holding a dispatcher pointer into code about to be unloaded.
     * Nothing can be done about it here, but it must not pass unnoticed. */
    if (!graph_delete_class(g_GraphClass))
        printf("SerialTCPStat: warning -- the graph class could not be freed\n");
    g_GraphClass = NULL;

    CloseLibrary(MUIMasterBase);
    CloseLibrary((struct Library *)IntuitionBase);
    return rc;
}
