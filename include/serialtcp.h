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
 * serialtcp.h -- shared definitions between serialtcp.device and SerialTCPd.
 *
 * Design note
 * -----------
 * The device driver is deliberately a thin shim.  It owns nothing except the
 * Unit structures; all real work (buffering, AT command emulation, telnet
 * negotiation, sockets) happens inside the daemon process.
 *
 * The trick that makes this cheap is that a Unit's MsgPort does not have to be
 * serviced by a task belonging to the device.  When the daemon attaches to a
 * unit it points that unit's MsgPort at *itself*, so IORequests queued by
 * BeginIO() land directly in the daemon's signal.  No handler task per unit, no
 * shared ring buffers, no lock-free trickery -- just Exec message passing
 * inside a single address space.
 *
 * This matters on the Amiga because bsdsocket.library is per-process: sockets
 * opened by the daemon can only be used by the daemon.  A device driver can
 * never touch them directly, so a handoff of some kind is mandatory.  Handing
 * off the whole IORequest is the cheapest handoff available.
 *
 */

#ifndef SERIALTCP_H
#define SERIALTCP_H

#include <exec/types.h>
#include <exec/ports.h>
#include <exec/devices.h>
#include <exec/semaphores.h>

/* ------------------------------------------------------------------ */
/* Identity                                                            */
/* ------------------------------------------------------------------ */

#define ST_DEVICE_NAME   "serialtcp.device"
#define ST_PORT_NAME     "serialtcp.daemon"

#define ST_MAGIC         0x5354434DUL      /* 'STCM' */
#define ST_PROTOCOL_VER  4

/* Hard ceiling on units.  Raise freely; costs one pointer each. */
#define ST_MAX_UNITS     32

/* ------------------------------------------------------------------ */
/* Unit                                                                */
/* ------------------------------------------------------------------ */

/*
 * Allocated by the device on first OpenDevice() of that unit number, freed by
 * the device when the last opener closes.  The daemon reads and writes these
 * fields too, so every non-atomic access on either side is wrapped in
 * Forbid()/Permit().
 */
struct STUnit
{
    struct Unit      su_Unit;          /* must be first: io_Unit points here  */

    ULONG            su_Magic;         /* ST_MAGIC while valid                */
    ULONG            su_UnitNum;

    /* Set by the daemon at attach time, cleared at detach. */
    volatile BOOL    su_Attached;

    /* Serial parameters, as last set via SDCMD_SETPARAMS.  Kept on the unit
     * (not in the daemon) so that a QUERY still answers sanely in the window
     * between the device allocating the unit and the daemon attaching. */
    ULONG            su_Baud;
    ULONG            su_RBufLen;
    ULONG            su_BrkTime;
    ULONG            su_CtlChar;
    ULONG            su_TermArray0;
    ULONG            su_TermArray1;
    UBYTE            su_ReadLen;
    UBYTE            su_WriteLen;
    UBYTE            su_StopBits;
    UBYTE            su_SerFlags;

    /*
     * Modem status in serial.device io_Status form.
     *
     * IMPORTANT: bits 3-7 (DSR, CTS, CD, RTS, DTR) are ACTIVE LOW -- a cleared
     * bit means the line is asserted.  DLG Pro's CarrierDetect() literally does
     *
     *     cd = !(io_Status & (1 << 5));
     *
     * so getting this inverted would mean the BBS sees a permanent carrier and
     * never logs anyone off.  See Handler/Handler/Main.c in the DLG sources.
     */
    volatile UWORD   su_Status;
};

/* io_Status bit positions we care about (see devices/serial.h). */
#define STSTB_DSR        3
#define STSTB_CTS        4
#define STSTB_CD         5
#define STSTB_RTS        6
#define STSTB_DTR        7

#define STSTF_DSR        (1 << STSTB_DSR)
#define STSTF_CTS        (1 << STSTB_CTS)
#define STSTF_CD         (1 << STSTB_CD)
#define STSTF_RTS        (1 << STSTB_RTS)
#define STSTF_DTR        (1 << STSTB_DTR)

/* All modem-control lines de-asserted (all bits high, because active low). */
#define ST_STATUS_IDLE   (STSTF_DSR | STSTF_CTS | STSTF_CD | STSTF_RTS | STSTF_DTR)

/* On-hook but powered: DSR and CTS asserted, no carrier. */
#define ST_STATUS_READY  (STSTF_CD)

/* Connected: DSR, CTS and CD all asserted (all three bits clear). */
#define ST_STATUS_ONLINE (0)

/* ------------------------------------------------------------------ */
/* Daemon rendezvous point                                             */
/* ------------------------------------------------------------------ */

/*
 * The daemon publishes an extended public MsgPort under ST_PORT_NAME.  The
 * device finds it with FindPort() and sanity-checks the magic and version
 * before trusting any of it -- a stale port from an old build must not take
 * the machine down.
 */
struct STDaemonPort
{
    struct MsgPort   dp_Port;          /* must be first                       */
    ULONG            dp_Magic;         /* ST_MAGIC                            */
    UWORD            dp_Version;       /* ST_PROTOCOL_VER                     */
    UWORD            dp_NumUnits;      /* how many units the daemon serves    */
};

/* ------------------------------------------------------------------ */
/* Attach / detach protocol                                            */
/* ------------------------------------------------------------------ */

#define STM_ATTACH   1        /* device -> daemon: adopt this unit           */
#define STM_DETACH   2        /* device -> daemon: release this unit         */

/* am_Error values */
#define STE_OK              0
#define STE_BAD_VERSION     1     /* daemon/device protocol mismatch          */
#define STE_NO_SUCH_UNIT    2     /* unit number >= configured node count     */
#define STE_UNIT_IN_USE     3     /* another opener already owns it exclusively */
#define STE_NO_MEMORY       4
#define STE_SHUTTING_DOWN   5
#define STE_NODES_ACTIVE    6     /* refused: calls are in progress           */

/*
 * Device-specific io_Error values.
 *
 * serial.device's own SerErr_* codes run from 1 to 15 and none of them mean
 * "the thing that does the work isn't running", which is by far the most
 * common reason opening this device fails.  These sit well above that range so
 * they cannot be mistaken for a real serial error.
 */
#define STERR_NO_DAEMON     101   /* SerialTCPd is not running                */
#define STERR_VERSION       102   /* device and daemon protocol mismatch      */
#define STERR_NO_SUCH_UNIT  103   /* unit number beyond the configured nodes  */

struct STAttachMsg
{
    struct Message   am_Msg;
    ULONG            am_Command;       /* STM_ATTACH / STM_DETACH             */
    ULONG            am_Version;       /* ST_PROTOCOL_VER from the device     */
    struct STUnit   *am_Unit;
    ULONG            am_UnitNum;
    ULONG            am_SerFlags;      /* io_SerFlags as passed to OpenDevice */
    LONG             am_Error;         /* STE_*, filled in by the daemon      */
};

/* A listener that feeds the shared pool rather than one specific unit. */
#define ST_NODE_POOL  (-1)

/* ------------------------------------------------------------------ */
/* Status query (used by the SerialTCPStat client)                     */
/* ------------------------------------------------------------------ */

#define STM_STATUS   3        /* client -> daemon: fill in a status block     */

/* Node states as reported to a status client. */
#define STS_DETACHED  0       /* no application has the unit open             */
#define STS_IDLE      1       /* on-hook, waiting for a call                  */
#define STS_DIALING   2       /* outbound connection in progress              */
#define STS_RINGING   3       /* inbound call, not yet answered               */
#define STS_ONLINE    4       /* connected, data flowing                      */
#define STS_ESCAPED   5       /* connected but in command mode (+++)          */

/* ns_Flags bits */
#define STSF_OUTBOUND_ONLY  (1 << 0)   /* never offered an incoming call      */

struct STNodeStatus
{
    UWORD  ns_Unit;
    UWORD  ns_State;          /* STS_*                                        */
    UWORD  ns_Status;         /* raw io_Status, active-low modem bits         */
    UWORD  ns_Rings;          /* rings delivered so far, when ringing         */
    UWORD  ns_Flags;          /* STSF_*                                       */
    ULONG  ns_Baud;
    ULONG  ns_RxQueued;       /* bytes buffered towards the application       */
    ULONG  ns_TxQueued;       /* bytes buffered towards the socket            */
    ULONG  ns_BytesIn;
    ULONG  ns_BytesOut;
    ULONG  ns_ConnectSecs;    /* length of the current call                   */
    char   ns_Peer[64];
};

/*
 * Sent to the daemon's public port.  The client supplies the array; the daemon
 * fills it in.  Same address space, so no copying protocol is needed.
 *
 * sm_Command sits at the same offset as am_Command in struct STAttachMsg, so
 * the daemon can tell the two apart before committing to either layout.
 */
struct STStatusMsg
{
    struct Message        sm_Msg;
    ULONG                 sm_Command;      /* STM_STATUS                      */
    ULONG                 sm_Version;      /* ST_PROTOCOL_VER                 */
    LONG                  sm_Error;        /* STE_*                           */
    UWORD                 sm_MaxNodes;     /* size of the array below         */
    UWORD                 sm_NumNodes;     /* how many the daemon filled in   */
    ULONG                 sm_UptimeSecs;
    ULONG                 sm_TotalCalls;
    ULONG                 sm_BusyCalls;
    ULONG                 sm_QueuedCalls;   /* callers who joined the queue   */
    UWORD                 sm_Queued;        /* waiting right now              */
    UWORD                 sm_QueueMax;      /* 0 when queueing is disabled    */

    /* How queued callers left. These account for every caller who joined,
     * except those still waiting: served + abandoned + timedout + declined
     * + queued == sm_QueuedCalls. */
    ULONG                 sm_QueueServed;      /* got a node in the end       */
    ULONG                 sm_QueueAbandoned;   /* hung up while waiting       */
    ULONG                 sm_QueueTimedOut;    /* queue-timeout expired       */
    ULONG                 sm_QueueDeclined;    /* said no, or never answered  */
    ULONG                 sm_QueueMaxWait;     /* longest wait served, secs   */
    ULONG                 sm_QueueAvgWait;     /* mean wait served, secs      */
    UWORD                 sm_ListenPorts[8];
    /* Which unit each listener feeds: ST_NODE_POOL (-1) for the shared pool,
     * otherwise the unit number it is dedicated to. */
    WORD                  sm_ListenNodes[8];
    UWORD                 sm_NumListeners;
    struct STNodeStatus  *sm_Nodes;
};

/* ------------------------------------------------------------------ */
/* Daemon control                                                      */
/* ------------------------------------------------------------------ */

#define STM_CONTROL   4       /* client -> daemon: do something to yourself   */

#define STC_SHUTDOWN  1       /* exit cleanly                                 */

/*
 * Without STCF_FORCE a shutdown is refused with STE_NODES_ACTIVE while any
 * node still has a call up, so a mistaken click cannot cut off callers.  The
 * client is expected to ask before retrying with the flag set.
 */
#define STCF_FORCE    (1 << 0)

struct STControlMsg
{
    struct Message   cm_Msg;
    ULONG            cm_Command;      /* STM_CONTROL                          */
    ULONG            cm_Version;      /* ST_PROTOCOL_VER                      */
    LONG             cm_Error;        /* STE_*                                */
    ULONG            cm_Action;       /* STC_*                                */
    ULONG            cm_Flags;        /* STCF_*                               */
    UWORD            cm_ActiveNodes;  /* how many were busy, when refused     */
};

#endif /* SERIALTCP_H */
