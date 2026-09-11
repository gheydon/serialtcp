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
 * statcommon.h -- talking to SerialTCPd, shared by the GUI and CLI clients.
 *
 * Both SerialTCPStat (MUI) and SerialTCPStatus (shell) link this, so there is
 * one implementation of the protocol and one set of formatting rules rather
 * than two that drift apart.
 */

#ifndef SERIALTCP_STATCOMMON_H
#define SERIALTCP_STATCOMMON_H

#include <exec/types.h>
#include "serialtcp.h"

#define MAX_SHOWN      ST_MAX_UNITS
#define DAEMON_COMMAND "SerialTCPd"

struct Snapshot
{
    BOOL                 ok;
    UWORD                numNodes;
    ULONG                uptime;
    ULONG                totalCalls;
    ULONG                busyCalls;
    UWORD                listenPorts[8];
    WORD                 listenNodes[8];
    UWORD                numListeners;

    ULONG                queuedCalls;
    UWORD                queued;
    UWORD                queueMax;
    ULONG                qServed, qAbandoned, qTimedOut, qDeclined;
    ULONG                qMaxWait, qAvgWait;

    struct STNodeStatus  nodes[MAX_SHOWN];
};

/* The command used to launch the daemon; set from the command line. */
extern const char *g_DaemonCmd;

BOOL  daemon_present(void);
BOOL  query_daemon(struct Snapshot *snap);

/* Returns an STE_* code, or -1 if the daemon is not running. */
LONG  stop_daemon(BOOL force, UWORD *activeOut);
BOOL  start_daemon(void);
BOOL  wait_for_stop(int maxTicks);
BOOL  wait_for_start(int maxTicks);

/* Formatting, shared so both clients read the same. */
/* An idle node that never answers reads as "dial-out", not "waiting" --
 * otherwise a sysop sees an idle line and wonders why calls skip it. */
const char *state_plain(UWORD state, UWORD flags);
void  fmt_duration(char *buf, ULONG size, ULONG secs);
void  fmt_bytes(char *buf, ULONG size, ULONG n);

/* The full text report. Returns a shell return code. */
int   print_status(BOOL brief);

#endif
