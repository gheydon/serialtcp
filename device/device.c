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
 * serialtcp.device -- a serial.device-compatible virtual device whose real
 * work is done by the SerialTCPd daemon process.
 *
 * Built with bebbo's m68k-amigaos-gcc, -nostdlib -nostartfiles, so there is no
 * C runtime here at all: no memset, no printf, nothing.  Everything is either
 * an Exec call or hand-rolled below.
 *
 * The romtag / autoinit boilerplate follows the shape used by jbilander's
 * SimpleDevice, which is the known-good pattern for this toolchain.
 */

#include <exec/types.h>
#include <exec/resident.h>
#include <exec/errors.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/semaphores.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <devices/serial.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include "serialtcp.h"

#define STR(s)  #s
#define XSTR(s) STR(s)

#define DEVICE_VERSION   1
#define DEVICE_REVISION  0
#define DEVICE_PRIORITY  0
#define DEVICE_DATE      "(10.9.26)"
#define DEVICE_ID_STRING "serialtcp " XSTR(DEVICE_VERSION) "." XSTR(DEVICE_REVISION) " " DEVICE_DATE

/*
 * Private io_Flags bit used to tell the daemon that a request it has already
 * dequeued must be abandoned.  Bit 5 is where the pre-V36 serial device kept
 * its own "request aborted" flag (IOSERF_ABORT), so it is safe to reuse.
 */
#define ST_IOF_ABORT (1 << 5)

struct ExecBase   *SysBase;
static BPTR        saved_seg_list;

/*
 * Build with -DST_MINIMAL to drop the console diagnostics entirely.  That
 * saves roughly 370 bytes of the resident device at the cost of a failed open
 * reporting only its io_Error number.  The error codes themselves are always
 * present -- they are free.
 */
#ifndef ST_MINIMAL
struct DosLibrary *DOSBase;          /* opened on demand, only to complain    */
#endif

/* Guarded by dev_Sem.  Open() has to Wait() for the daemon's reply, which
 * breaks the Forbid() that Exec wraps around us, so we cannot rely on Exec to
 * single-thread Open/Close and must do it ourselves. */
static struct SignalSemaphore dev_Sem;
static struct STUnit         *dev_Units[ST_MAX_UNITS];
static UWORD                  dev_UnitOpens[ST_MAX_UNITS];
static UBYTE                  dev_UnitExclusive[ST_MAX_UNITS];

char device_name[]      = ST_DEVICE_NAME;
char device_id_string[] = DEVICE_ID_STRING;

/* ------------------------------------------------------------------ */
/* romtag                                                              */
/* ------------------------------------------------------------------ */

int __attribute__((no_reorder)) _start(void)
{
    return -1;
}

asm("romtag:                                \n"
    "       dc.w    "XSTR(RTC_MATCHWORD)"   \n"
    "       dc.l    romtag                  \n"
    "       dc.l    endcode                 \n"
    "       dc.b    "XSTR(RTF_AUTOINIT)"    \n"
    "       dc.b    "XSTR(DEVICE_VERSION)"  \n"
    "       dc.b    "XSTR(NT_DEVICE)"       \n"
    "       dc.b    "XSTR(DEVICE_PRIORITY)" \n"
    "       dc.l    _device_name            \n"
    "       dc.l    _device_id_string       \n"
    "       dc.l    _auto_init_tables       \n"
    "endcode:                               \n");

/* ------------------------------------------------------------------ */
/* Tiny helpers (no libc available)                                    */
/* ------------------------------------------------------------------ */

static void st_bzero(APTR p, ULONG len)
{
    UBYTE *b = (UBYTE *)p;
    while (len--)
        *b++ = 0;
}

static void st_init_list(struct List *l)
{
    l->lh_Head     = (struct Node *)&l->lh_Tail;
    l->lh_Tail     = NULL;
    l->lh_TailPred = (struct Node *)&l->lh_Head;
    l->lh_Type     = NT_MESSAGE;
}

/* ------------------------------------------------------------------ */
/* Explaining failures                                                 */
/* ------------------------------------------------------------------ */

#ifdef ST_MINIMAL

#define explain(err)  ((void)0)

#else

static ULONG st_strlen(const char *s)
{
    const char *p = s;
    while (*p) p++;
    return (ULONG)(p - s);
}

/*
 * Say why an open failed.
 *
 * An io_Error alone usually surfaces as "couldn't open serialtcp.device",
 * which tells a sysop nothing -- and "the daemon isn't running" is by far the
 * most common cause. One short line on the caller's console fixes that.
 *
 * Kept deliberately spare: this device is meant to stay small, so there is one
 * terse string per cause and no formatting code at all. The precise unit
 * number is already in the io_Error the caller got back, and SerialTCPStat
 * gives the full picture.
 *
 * Only done for Shell processes. A plain Task has no DOS context, and a
 * Workbench-launched process has no console to write to; in those cases the
 * error code has to speak for itself.
 */
static void explain(LONG err)
{
    struct Process    *proc = (struct Process *)FindTask(NULL);
    struct DosLibrary *dos;
    const char        *msg;
    BPTR               out;

    switch (err)
    {
    case STERR_NO_DAEMON:
        msg = "serialtcp.device: SerialTCPd is not running.\n";
        break;
    case STERR_VERSION:
        msg = "serialtcp.device: version mismatch with SerialTCPd.\n";
        break;
    case STERR_NO_SUCH_UNIT:
        msg = "serialtcp.device: no such unit; raise 'nodes' in the config.\n";
        break;
    default:
        return;
    }

    if (!proc || proc->pr_Task.tc_Node.ln_Type != NT_PROCESS || !proc->pr_CLI)
        return;

    dos = (struct DosLibrary *)OpenLibrary((CONST_STRPTR)"dos.library", 36);
    if (!dos)
        return;

    DOSBase = dos;
    out = Output();
    if (out)
        Write(out, (APTR)msg, (LONG)st_strlen(msg));

    DOSBase = NULL;
    CloseLibrary((struct Library *)dos);
}

#endif /* ST_MINIMAL */

/* ------------------------------------------------------------------ */
/* Daemon rendezvous                                                   */
/* ------------------------------------------------------------------ */

/*
 * Locate the daemon and validate it.  Must be called inside Forbid() and the
 * returned pointer is only trustworthy until you Permit().
 */
static struct STDaemonPort *find_daemon(void)
{
    struct STDaemonPort *dp = (struct STDaemonPort *)FindPort((CONST_STRPTR)ST_PORT_NAME);

    if (!dp)
        return NULL;
    if (dp->dp_Magic != ST_MAGIC)
        return NULL;
    if (dp->dp_Version != ST_PROTOCOL_VER)
        return NULL;

    return dp;
}

/*
 * Send an attach/detach request and wait for the answer.  Uses a stack-resident
 * reply port, which is safe because we do not return until the reply is back.
 * Returns an STE_* code.
 */
static LONG talk_to_daemon(ULONG command, struct STUnit *unit, ULONG unitnum, ULONG serflags)
{
    struct MsgPort       rp;
    struct STAttachMsg   am;
    struct STDaemonPort *dp;
    BYTE                 sigbit;

    sigbit = AllocSignal(-1);
    if (sigbit == -1)
        return STE_NO_MEMORY;

    st_bzero(&rp, sizeof(rp));
    rp.mp_Node.ln_Type = NT_MSGPORT;
    rp.mp_Flags        = PA_SIGNAL;
    rp.mp_SigBit       = sigbit;
    rp.mp_SigTask      = FindTask(NULL);
    st_init_list(&rp.mp_MsgList);

    st_bzero(&am, sizeof(am));
    am.am_Msg.mn_Node.ln_Type = NT_MESSAGE;
    am.am_Msg.mn_ReplyPort    = &rp;
    am.am_Msg.mn_Length       = sizeof(am);
    am.am_Command             = command;
    am.am_Version             = ST_PROTOCOL_VER;
    am.am_Unit                = unit;
    am.am_UnitNum             = unitnum;
    am.am_SerFlags            = serflags;
    am.am_Error               = STE_SHUTTING_DOWN;

    /* Find and post inside one Forbid so the daemon cannot vanish between the
     * lookup and the PutMsg. */
    Forbid();
    dp = find_daemon();
    if (dp)
        PutMsg(&dp->dp_Port, &am.am_Msg);
    Permit();

    if (!dp)
    {
        FreeSignal(sigbit);
        return STE_SHUTTING_DOWN;
    }

    WaitPort(&rp);
    GetMsg(&rp);

    FreeSignal(sigbit);
    return am.am_Error;
}

/* ------------------------------------------------------------------ */
/* Open / Close / Expunge                                              */
/* ------------------------------------------------------------------ */

static BPTR do_expunge(struct Library *dev)
{
    BPTR seg_list;

    if (dev->lib_OpenCnt != 0)
    {
        dev->lib_Flags |= LIBF_DELEXP;
        return 0;
    }

    seg_list = saved_seg_list;
    Remove(&dev->lib_Node);
    FreeMem((char *)dev - dev->lib_NegSize, dev->lib_NegSize + dev->lib_PosSize);
    return seg_list;
}

static void do_open(struct Library *dev, struct IORequest *ioreq, ULONG unitnum, ULONG flags)
{
    struct IOExtSer *ser = (struct IOExtSer *)ioreq;
    struct STUnit   *unit;
    ULONG            serflags;
    LONG             err;
    BOOL             created = FALSE;

    ioreq->io_Error                   = IOERR_OPENFAIL;
    ioreq->io_Message.mn_Node.ln_Type = NT_REPLYMSG;

    if (unitnum >= ST_MAX_UNITS)
    {
        ioreq->io_Error = STERR_NO_SUCH_UNIT;
        explain(STERR_NO_SUCH_UNIT);
        return;
    }

    /*
     * The caller is allowed to preset io_SerFlags before OpenDevice() to ask
     * for shared access -- DLG Pro does exactly this (Handler/Handler/Serial.c
     * sets io_SerFlags then calls OpenDevice).  We must honour it, but only if
     * the request is actually big enough to hold the field.
     */
    if (ioreq->io_Message.mn_Length >= (UWORD)sizeof(struct IOExtSer))
        serflags = ser->io_SerFlags;
    else
        serflags = 0;

    ObtainSemaphore(&dev_Sem);

    unit = dev_Units[unitnum];

    if (unit && dev_UnitExclusive[unitnum])
    {
        /* Somebody already holds it exclusively. */
        ioreq->io_Error = SerErr_DevBusy;
        ReleaseSemaphore(&dev_Sem);
        return;
    }

    if (unit && !(serflags & SERF_SHARED))
    {
        /* We want it exclusively but it is already open. */
        ioreq->io_Error = SerErr_DevBusy;
        ReleaseSemaphore(&dev_Sem);
        return;
    }

    if (!unit)
    {
        unit = (struct STUnit *)AllocMem(sizeof(struct STUnit), MEMF_PUBLIC | MEMF_CLEAR);
        if (!unit)
        {
            ReleaseSemaphore(&dev_Sem);
            return;
        }

        unit->su_Magic   = ST_MAGIC;
        unit->su_UnitNum = unitnum;
        unit->su_Status  = ST_STATUS_IDLE;

        /* serial.device defaults, so a QUERY before any SETPARAMS is sane. */
        unit->su_Baud       = 38400;
        unit->su_RBufLen    = 512;
        unit->su_BrkTime    = 250000;
        unit->su_CtlChar    = SER_DEFAULT_CTLCHAR;
        unit->su_ReadLen    = 8;
        unit->su_WriteLen   = 8;
        unit->su_StopBits   = 1;
        unit->su_SerFlags   = (UBYTE)serflags;

        unit->su_Unit.unit_MsgPort.mp_Node.ln_Type = NT_MSGPORT;
        unit->su_Unit.unit_MsgPort.mp_Flags        = PA_IGNORE;
        unit->su_Unit.unit_MsgPort.mp_SigTask      = NULL;
        st_init_list(&unit->su_Unit.unit_MsgPort.mp_MsgList);

        dev_Units[unitnum] = unit;
        created = TRUE;

        /*
         * Hand the unit to the daemon.  It fills in mp_SigTask/mp_SigBit and
         * flips the port to PA_SIGNAL, so from here on BeginIO() delivers
         * straight into the daemon's Wait().
         */
        err = talk_to_daemon(STM_ATTACH, unit, unitnum, serflags);
        if (err != STE_OK)
        {
            LONG ioerr;

            dev_Units[unitnum] = NULL;
            FreeMem(unit, sizeof(struct STUnit));

            switch (err)
            {
            case STE_UNIT_IN_USE:   ioerr = SerErr_DevBusy;      break;
            case STE_BAD_VERSION:   ioerr = STERR_VERSION;       break;
            case STE_NO_SUCH_UNIT:  ioerr = STERR_NO_SUCH_UNIT;  break;
            case STE_SHUTTING_DOWN: ioerr = STERR_NO_DAEMON;     break;
            default:                ioerr = IOERR_OPENFAIL;      break;
            }

            ioreq->io_Error = ioerr;
            ReleaseSemaphore(&dev_Sem);

            /* Outside the semaphore: this writes to the console and we do not
             * want to hold up other opens while it does. */
            explain(ioerr);
            return;
        }
    }

    dev_UnitOpens[unitnum]++;
    if (!(serflags & SERF_SHARED))
        dev_UnitExclusive[unitnum] = 1;

    unit->su_Unit.unit_OpenCnt++;
    dev->lib_OpenCnt++;

    ioreq->io_Device = (struct Device *)dev;
    ioreq->io_Unit   = (struct Unit *)unit;
    ioreq->io_Error  = 0;

    /* Reflect current parameters back to the caller, as serial.device does. */
    if (ioreq->io_Message.mn_Length >= (UWORD)sizeof(struct IOExtSer))
    {
        ser->io_CtlChar   = unit->su_CtlChar;
        ser->io_RBufLen   = unit->su_RBufLen;
        ser->io_ExtFlags  = 0;
        ser->io_Baud      = unit->su_Baud;
        ser->io_BrkTime   = unit->su_BrkTime;
        ser->io_ReadLen   = unit->su_ReadLen;
        ser->io_WriteLen  = unit->su_WriteLen;
        ser->io_StopBits  = unit->su_StopBits;
        ser->io_SerFlags  = unit->su_SerFlags;
        ser->io_Status    = unit->su_Status;
    }

    (void)created;
    (void)flags;

    ReleaseSemaphore(&dev_Sem);
}

static BPTR do_close(struct Library *dev, struct IORequest *ioreq)
{
    struct STUnit *unit = (struct STUnit *)ioreq->io_Unit;
    ULONG          unitnum;

    ioreq->io_Device = NULL;
    ioreq->io_Unit   = NULL;

    if (unit && unit->su_Magic == ST_MAGIC)
    {
        unitnum = unit->su_UnitNum;

        ObtainSemaphore(&dev_Sem);

        if (unit->su_Unit.unit_OpenCnt > 0)
            unit->su_Unit.unit_OpenCnt--;

        if (dev_UnitOpens[unitnum] > 0)
            dev_UnitOpens[unitnum]--;

        if (dev_UnitOpens[unitnum] == 0)
        {
            dev_UnitExclusive[unitnum] = 0;

            /*
             * Tell the daemon to hang up and stop servicing the port.  It
             * replies only once it has flushed and replied every request it
             * still holds for this unit, so the unit is quiescent afterwards.
             */
            talk_to_daemon(STM_DETACH, unit, unitnum, 0);

            dev_Units[unitnum] = NULL;
            unit->su_Magic     = 0;
            FreeMem(unit, sizeof(struct STUnit));
        }

        ReleaseSemaphore(&dev_Sem);
    }

    dev->lib_OpenCnt--;

    if (dev->lib_OpenCnt == 0 && (dev->lib_Flags & LIBF_DELEXP))
        return do_expunge(dev);

    return 0;
}

/* ------------------------------------------------------------------ */
/* BeginIO / AbortIO                                                   */
/* ------------------------------------------------------------------ */

static void do_begin_io(struct Library *dev, struct IORequest *ioreq)
{
    struct STUnit *unit = (struct STUnit *)ioreq->io_Unit;

    (void)dev;

    if (!unit || unit->su_Magic != ST_MAGIC)
    {
        ioreq->io_Error = IOERR_BADADDRESS;
        ioreq->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
        if (!(ioreq->io_Flags & IOF_QUICK))
            ReplyMsg(&ioreq->io_Message);
        return;
    }

    /*
     * Everything is serviced by the daemon, so nothing can ever complete
     * quickly.  Clear IOF_QUICK to tell the caller it must wait for the reply.
     */
    ioreq->io_Flags &= ~(IOF_QUICK | ST_IOF_ABORT);
    ioreq->io_Error  = 0;

    Forbid();

    if (!unit->su_Attached)
    {
        /*
         * The daemon went away underneath us.  Fail rather than queue into a
         * port nobody is listening to, which would hang the BBS forever.
         */
        Permit();
        ioreq->io_Error = STERR_NO_DAEMON;
        ioreq->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
        ReplyMsg(&ioreq->io_Message);
        return;
    }

    ioreq->io_Message.mn_Node.ln_Type = NT_MESSAGE;
    PutMsg(&unit->su_Unit.unit_MsgPort, &ioreq->io_Message);

    Permit();
}

static ULONG do_abort_io(struct Library *dev, struct IORequest *ioreq)
{
    struct STUnit  *unit = (struct STUnit *)ioreq->io_Unit;
    struct Node    *n;
    ULONG           rc = 0;

    (void)dev;

    if (!unit || unit->su_Magic != ST_MAGIC)
        return IOERR_NOCMD;

    Forbid();

    /*
     * Two cases.  Either the request is still sitting on the unit's port
     * because the daemon has not looked at it yet -- in which case we can pull
     * it out and reply ourselves -- or the daemon already has it, and all we
     * can do is flag it and prod the daemon into noticing.
     */
    for (n = unit->su_Unit.unit_MsgPort.mp_MsgList.lh_Head;
         n->ln_Succ;
         n = n->ln_Succ)
    {
        if (n == &ioreq->io_Message.mn_Node)
        {
            Remove(n);
            ioreq->io_Error = IOERR_ABORTED;
            ioreq->io_Message.mn_Node.ln_Type = NT_REPLYMSG;
            ReplyMsg(&ioreq->io_Message);
            Permit();
            return 0;
        }
    }

    ioreq->io_Flags |= ST_IOF_ABORT;

    if (unit->su_Unit.unit_MsgPort.mp_Flags == PA_SIGNAL && unit->su_Unit.unit_MsgPort.mp_SigTask)
        Signal((struct Task *)unit->su_Unit.unit_MsgPort.mp_SigTask,
               1UL << unit->su_Unit.unit_MsgPort.mp_SigBit);

    Permit();
    return rc;
}

/* ------------------------------------------------------------------ */
/* Autoinit glue                                                       */
/* ------------------------------------------------------------------ */

static struct Library __attribute__((used)) *
init_device(struct ExecBase *sys_base asm("a6"), BPTR seg_list asm("a0"), struct Library *dev asm("d0"))
{
    UWORD i;

    SysBase        = sys_base;
    saved_seg_list = seg_list;

    dev->lib_Node.ln_Type = NT_DEVICE;
    dev->lib_Node.ln_Name = device_name;
    dev->lib_Flags        = LIBF_SUMUSED | LIBF_CHANGED;
    dev->lib_Version      = DEVICE_VERSION;
    dev->lib_Revision     = DEVICE_REVISION;
    dev->lib_IdString     = (APTR)device_id_string;

    InitSemaphore(&dev_Sem);

    for (i = 0; i < ST_MAX_UNITS; i++)
    {
        dev_Units[i]         = NULL;
        dev_UnitOpens[i]     = 0;
        dev_UnitExclusive[i] = 0;
    }

    return dev;
}

static BPTR __attribute__((used)) expunge(struct Library *dev asm("a6"))
{
    return do_expunge(dev);
}

static void __attribute__((used)) open(struct Library *dev asm("a6"), struct IORequest *ioreq asm("a1"),
                                       ULONG unitnum asm("d0"), ULONG flags asm("d1"))
{
    do_open(dev, ioreq, unitnum, flags);
}

static BPTR __attribute__((used)) close(struct Library *dev asm("a6"), struct IORequest *ioreq asm("a1"))
{
    return do_close(dev, ioreq);
}

static void __attribute__((used)) begin_io(struct Library *dev asm("a6"), struct IORequest *ioreq asm("a1"))
{
    do_begin_io(dev, ioreq);
}

static ULONG __attribute__((used)) abort_io(struct Library *dev asm("a6"), struct IORequest *ioreq asm("a1"))
{
    return do_abort_io(dev, ioreq);
}

static ULONG device_vectors[] =
{
    (ULONG)open,
    (ULONG)close,
    (ULONG)expunge,
    0,
    (ULONG)begin_io,
    (ULONG)abort_io,
    (ULONG)-1
};

const ULONG auto_init_tables[4] =
{
    sizeof(struct Library),
    (ULONG)device_vectors,
    0,
    (ULONG)init_device
};
