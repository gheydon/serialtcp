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
 * sysinfo.c -- DLGSysInfo: a DLG Professional door showing what the machine
 * and the BBS are doing right now.
 *
 * A DLG door is nothing more exotic than a program whose Output() happens to
 * be the caller's stream: DLG's TPT-Handler is the console handler for the
 * process, so ordinary printf() goes down the line.  That is why there is no
 * door framework here and no DLG-specific startup -- run it from a Shell and
 * you get the same report on your own screen.
 *
 * Three sources feed the report:
 *
 *   Exec          CPU, FPU, chipset, Kickstart version, memory.
 *   dlg.library   which ports are active, and which port this is.  Called
 *                 through hand-written LVO stubs so this builds without the
 *                 DLG SDK, which is not freely redistributable.
 *   DLGConfig:    which serial device and unit each port is mounted on,
 *                 read straight from its .port file.
 *   SerialTCPd    the live state of each node: on-line or not, how long, and
 *                 -- for the caller's own node only -- where they came from.
 *
 * Every one of those is optional.  Run it on a machine with no DLG and no
 * daemon and you still get the system half of the report.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <exec/lists.h>
#include <graphics/gfxbase.h>
#include <dos/dos.h>
#include <dos/dosextens.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <string.h>

#include "statcommon.h"

#define VERSION_STRING "DLGSysInfo 1.0 (11.9.2026)"
static const char *verstag = "$VER: " VERSION_STRING;

/* ------------------------------------------------------------------ */
/* dlg.library, without the DLG SDK                                    */
/* ------------------------------------------------------------------ */

/*
 * Library vector offsets, taken from the function table in the DLG source
 * (include/pragmas/dlg.h).  Only calls that cannot block or change a port are
 * used.  Neither GetDevName() nor ListPorts() goes near a port's handler:
 * the first walks the DOS device list, the second asks ResMan.
 *
 * Two obvious calls are deliberately avoided.  TCheckCarrier() kills the
 * session on a port with no carrier.  TDevQuery() sends a control message to
 * each port's TPT-Handler and waits for the reply, so one busy or wedged
 * handler would stall the whole report.  The port's .port file has the same
 * facts and can be read without asking anyone.
 */
#define DLG_GETDEVNAME    "-498"     /* LONG GetDevName(a0 char *buf)        */
#define DLG_LISTPORTS     "-336"     /* LONG ListPorts(a0 buf, a1 passwd)    */

#define DLG_CALL_A0(base, lvo, a0v)                                     \
({                                                                      \
    register void *__base __asm("a6") = (void *)(base);                 \
    register LONG  __ret  __asm("d0");                                  \
    register void *__a0   __asm("a0") = (void *)(a0v);                  \
    __asm volatile ("jsr %%a6@(" lvo ":W)"                              \
                    : "=d"(__ret), "+a"(__a0)                           \
                    : "a"(__base)                                       \
                    : "fp0", "fp1", "cc", "memory", "d1", "a1");        \
    __ret;                                                              \
})

#define DLG_CALL_A0A1(base, lvo, a0v, a1v)                              \
({                                                                      \
    register void *__base __asm("a6") = (void *)(base);                 \
    register LONG  __ret  __asm("d0");                                  \
    register void *__a0   __asm("a0") = (void *)(a0v);                  \
    register void *__a1   __asm("a1") = (void *)(a1v);                  \
    __asm volatile ("jsr %%a6@(" lvo ":W)"                              \
                    : "=d"(__ret), "+a"(__a0), "+a"(__a1)               \
                    : "a"(__base)                                       \
                    : "fp0", "fp1", "cc", "memory", "d1");              \
    __ret;                                                              \
})

static struct Library *DLGBase = NULL;

static LONG dlg_getdevname(char *buf)
{
    return DLG_CALL_A0(DLGBase, DLG_GETDEVNAME, buf);
}

static LONG dlg_listports(char *buf, const char *passwd)
{
    return DLG_CALL_A0A1(DLGBase, DLG_LISTPORTS, buf, passwd);
}

/*
 * DLGConfig:Port/<port>.port is DLG's struct Port (dlg/portconfig.h), the
 * fixed binary record its ConfigLib reads in ReadGlobals().  It opens with the
 * serial device name in 36 bytes and the unit number in the byte after; the
 * rest (globals, modem and display file names) is of no interest here.
 */
#define PORT_DEVNAME_LEN  36

static BOOL port_device(const char *port, char *dev, ULONG devlen, UBYTE *unit)
{
    char  path[40];
    UBYTE rec[PORT_DEVNAME_LEN + 1];
    BPTR  fh;
    LONG  got;
    ULONG n;

    snprintf(path, sizeof(path), "DLGConfig:Port/%s.port", port);

    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh)
        return FALSE;
    got = Read(fh, rec, sizeof(rec));
    Close(fh);

    if (got != (LONG)sizeof(rec) || rec[0] == '\0')
        return FALSE;

    for (n = 0; n < PORT_DEVNAME_LEN && n < devlen - 1 && rec[n]; n++)
        dev[n] = (char)rec[n];
    dev[n] = '\0';

    *unit = rec[PORT_DEVNAME_LEN];
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Formatting helpers                                                  */
/* ------------------------------------------------------------------ */

#define LABEL "  %-20s"

/* Kilobytes with thousands separators: 16,384K reads better than 16384K on a
 * machine where the interesting numbers are five digits long. */
static void fmt_kb(char *buf, ULONG size, ULONG bytes)
{
    ULONG k = bytes / 1024;
    char  digits[16];
    int   n, i, o = 0;

    n = snprintf(digits, sizeof(digits), "%lu", (unsigned long)k);

    for (i = 0; i < n && o < (int)size - 3; i++)
    {
        if (i > 0 && ((n - i) % 3) == 0)
            buf[o++] = ',';
        buf[o++] = digits[i];
    }

    buf[o++] = 'K';
    buf[o]   = '\0';
}

static void fmt_uptime(char *buf, ULONG size, ULONG secs)
{
    ULONG d = secs / 86400;
    ULONG h = (secs % 86400) / 3600;
    ULONG m = (secs % 3600) / 60;

    if (d)
        snprintf(buf, size, "%lu day%s, %lu:%02lu",
                 (unsigned long)d, d == 1 ? "" : "s",
                 (unsigned long)h, (unsigned long)m);
    else
        snprintf(buf, size, "%lu:%02lu:%02lu",
                 (unsigned long)h, (unsigned long)m,
                 (unsigned long)(secs % 60));
}

/* ------------------------------------------------------------------ */
/* The machine                                                         */
/* ------------------------------------------------------------------ */

/* Older NDKs predate the 68060, so do not rely on the header having it. */
#ifndef AFF_68060
#define AFF_68060 (1L << 7)
#endif

static const char *cpu_name(UWORD flags)
{
    if (flags & AFF_68060) return "68060";
    if (flags & AFF_68040) return "68040";
    if (flags & AFF_68030) return "68030";
    if (flags & AFF_68020) return "68020";
    if (flags & AFF_68010) return "68010";
    return "68000";
}

static const char *fpu_name(UWORD flags)
{
    /* AFF_FPU40 means the FPU is part of the CPU, so there is no separate
     * coprocessor to name. */
    if (flags & AFF_FPU40)  return (flags & AFF_68060) ? "68060 (built in)"
                                                       : "68040 (built in)";
    if (flags & AFF_68882)  return "68882";
    if (flags & AFF_68881)  return "68881";
    return "none";
}

/*
 * Kickstart version numbers are what the machine actually reports; the
 * marketing name is what people recognise.  Show both.
 */
static const char *os_name(UWORD ver)
{
    switch (ver)
    {
        case 33: return "1.2";
        case 34: return "1.3";
        case 36: return "2.0";
        case 37: return "2.04";
        case 38: return "2.1";
        case 39: return "3.0";
        case 40: return "3.1";
        case 44: return "3.5";
        case 45: return "3.9";
        case 46: return "3.1.4";
        case 47: return "3.2";
        default: return NULL;
    }
}

static void show_system(void)
{
    struct ExecBase *eb = SysBase;
    struct GfxBase  *gb;
    struct Library  *vb;
    const char      *osn;
    UWORD            attn = eb->AttnFlags;

    printf("\n System\n");

    printf(LABEL "%s\n", "CPU", cpu_name(attn));
    printf(LABEL "%s\n", "FPU", fpu_name(attn));

    gb = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 0);
    if (gb)
    {
        UBYTE cr = gb->ChipRevBits0;
        const char *chips;

        if ((cr & (GFXF_AA_ALICE | GFXF_AA_LISA)) == (GFXF_AA_ALICE | GFXF_AA_LISA))
            chips = "AGA";
        else if (cr & (GFXF_HR_AGNUS | GFXF_HR_DENISE))
            chips = "ECS";
        else
            chips = "OCS";

        printf(LABEL "%s, %s (%u Hz)\n", "Chipset", chips,
               (gb->DisplayFlags & PAL) ? "PAL" : "NTSC",
               (unsigned)eb->VBlankFrequency);

        CloseLibrary((struct Library *)gb);
    }

    osn = os_name(eb->LibNode.lib_Version);
    if (osn)
        printf(LABEL "%s (Kickstart %u.%u)\n", "AmigaOS", osn,
               (unsigned)eb->LibNode.lib_Version,
               (unsigned)eb->LibNode.lib_Revision);
    else
        printf(LABEL "Kickstart %u.%u\n", "AmigaOS",
               (unsigned)eb->LibNode.lib_Version,
               (unsigned)eb->LibNode.lib_Revision);

    /* version.library carries the Workbench release, which can differ from
     * the ROM -- a 3.1 machine running 3.2 from disk, for instance. */
    vb = OpenLibrary((CONST_STRPTR)"version.library", 0);
    if (vb)
    {
        printf(LABEL "%u.%u\n", "Workbench",
               (unsigned)vb->lib_Version, (unsigned)vb->lib_Revision);
        CloseLibrary(vb);
    }

    /* Exec has no uptime counter, but the RAM disk is created once at boot
     * and never again, so its volume date is the time the machine came up. */
    {
        struct DosList  *dl;
        struct DateStamp created;
        BOOL             got = FALSE;

        dl = LockDosList(LDF_VOLUMES | LDF_READ);
        if (dl)
        {
            struct DosList *ram = FindDosEntry(dl, (CONST_STRPTR)"Ram Disk",
                                               LDF_VOLUMES);
            if (ram)
            {
                created = ram->dol_misc.dol_volume.dol_VolumeDate;
                got     = TRUE;
            }
            UnLockDosList(LDF_VOLUMES | LDF_READ);
        }

        if (got)
        {
            struct DateStamp now;
            LONG             secs;

            DateStamp(&now);
            secs = (now.ds_Days   - created.ds_Days)   * 86400
                 + (now.ds_Minute - created.ds_Minute) * 60
                 + (now.ds_Tick   - created.ds_Tick) / TICKS_PER_SECOND;

            if (secs > 0)
            {
                char up[32];
                fmt_uptime(up, sizeof(up), (ULONG)secs);
                printf(LABEL "%s\n", "Up since boot", up);
            }
        }
    }

    /* A rough measure of how busy the machine is, and cheap to get. */
    {
        struct Node *n;
        int          tasks = 0;

        Forbid();
        for (n = eb->TaskReady.lh_Head; n->ln_Succ; n = n->ln_Succ)
            tasks++;
        for (n = eb->TaskWait.lh_Head; n->ln_Succ; n = n->ln_Succ)
            tasks++;
        Permit();

        printf(LABEL "%d\n", "Tasks running", tasks + 1);  /* +1: this one */
    }
}

static void show_memory(void)
{
    char freeb[24], totb[24];

    printf("\n Memory\n");

    fmt_kb(freeb, sizeof(freeb), AvailMem(MEMF_CHIP));
    fmt_kb(totb,  sizeof(totb),  AvailMem(MEMF_CHIP | MEMF_TOTAL));
    printf(LABEL "%s free of %s\n", "Chip", freeb, totb);

    if (AvailMem(MEMF_FAST | MEMF_TOTAL))
    {
        fmt_kb(freeb, sizeof(freeb), AvailMem(MEMF_FAST));
        fmt_kb(totb,  sizeof(totb),  AvailMem(MEMF_FAST | MEMF_TOTAL));
        printf(LABEL "%s free of %s\n", "Fast", freeb, totb);
    }
    else
        printf(LABEL "none fitted\n", "Fast");

    fmt_kb(freeb, sizeof(freeb), AvailMem(MEMF_ANY));
    fmt_kb(totb,  sizeof(totb),  AvailMem(MEMF_ANY | MEMF_TOTAL));
    printf(LABEL "%s free of %s\n", "Total", freeb, totb);

    fmt_kb(freeb, sizeof(freeb), AvailMem(MEMF_ANY | MEMF_LARGEST));
    printf(LABEL "%s\n", "Largest free block", freeb);
}

/* ------------------------------------------------------------------ */
/* The BBS                                                             */
/* ------------------------------------------------------------------ */

#define MAX_PORTS 32

struct PortLine
{
    char   name[4];             /* "TR1"                                    */
    char   dev[PORT_DEVNAME_LEN + 1];  /* serial device behind it, or ""    */
    UBYTE  unit;
    BOOL   gotDev;
    BOOL   isMine;
    const struct STNodeStatus *node;   /* NULL if not a SerialTCP node      */
};

/*
 * Match a DLG port to a SerialTCP node.  The port knows which device and unit
 * it was mounted on; the daemon knows what unit 3 is doing.  Anything not on
 * serialtcp.device -- a real modem on a multi-serial card, say -- simply has
 * no node and is reported without a state.
 */
static const struct STNodeStatus *node_for(const struct Snapshot *snap,
                                           const struct PortLine *p)
{
    UWORD i;

    if (!snap->ok || !p->gotDev)
        return NULL;

    if (strcmp(p->dev, ST_DEVICE_NAME) != 0)
        return NULL;

    for (i = 0; i < snap->numNodes; i++)
        if (snap->nodes[i].ns_Unit == p->unit)
            return &snap->nodes[i];

    return NULL;
}

/*
 * Static rather than automatic: between them these are around 4.5K, and a
 * door inherits whatever stack the port's startup script set.  A Shell
 * default of 4K would not survive them.
 */
static struct PortLine  g_Ports[MAX_PORTS];
static struct Snapshot  g_Snap;

static void show_bbs(const struct Snapshot *snap)
{
    struct PortLine *ports = g_Ports;
    char            list[MAX_PORTS * 3 + 8];
    char            mine[8];
    const char     *s;
    int             count = 0, online = 0, known = 0, i;
    BOOL            haveMine;

    printf("\n BBS\n");

    if (!DLGBase)
    {
        printf(LABEL "dlg.library is not available -- no port information\n",
               "Nodes");
        return;
    }

    mine[0] = '\0';
    haveMine = (dlg_getdevname(mine) == 0);

    list[0] = '\0';
    if (dlg_listports(list, "") != 0)
    {
        printf(LABEL "the resource manager is not running\n", "Nodes");
        return;
    }

    /* ListPorts() returns the names run together, three characters each. */
    for (s = list; *s && count < MAX_PORTS; s += 3)
    {
        struct PortLine  *p = &ports[count++];

        memcpy(p->name, s, 3);
        p->name[3] = '\0';

        p->dev[0]  = '\0';
        p->unit    = 0;
        p->gotDev  = FALSE;
        p->isMine  = (haveMine && strncmp(mine, s, 3) == 0);

        p->gotDev = port_device(p->name, p->dev, sizeof(p->dev), &p->unit);

        p->node = node_for(snap, p);

        if (p->node)
        {
            known++;
            if (p->node->ns_State == STS_ONLINE || p->node->ns_State == STS_ESCAPED)
                online++;
        }
    }

    printf(LABEL "%s\n", "Your node", haveMine ? mine : "(not a BBS line)");
    printf(LABEL "%d\n", "Nodes active", count);

    if (known)
    {
        printf(LABEL "%d\n", "Nodes in use", online);

        /* Run from a Shell rather than down a line, none of those callers is
         * you, so do not quietly subtract one of them. */
        if (haveMine)
            printf(LABEL "%d\n", "Other users on-line",
                   online > 0 ? online - 1 : 0);
        else
            printf(LABEL "%d\n", "Users on-line", online);
    }

    if (!count)
        return;

    printf("\n  Node  Device                      Status      Time on\n");
    printf("  ----  --------------------------  ----------  ---------\n");

    for (i = 0; i < count; i++)
    {
        const struct PortLine *p = &ports[i];
        char  dev[PORT_DEVNAME_LEN + 8];
        char  dur[16];
        const char *state;

        /* No readable .port file: nothing to say about the device. */
        if (p->gotDev)
            snprintf(dev, sizeof(dev), "%s %u", p->dev, (unsigned)p->unit);
        else
            snprintf(dev, sizeof(dev), "%s", "-");

        dur[0] = '\0';
        if (p->node)
        {
            state = state_plain(p->node->ns_State, p->node->ns_Flags);
            if (p->node->ns_ConnectSecs)
                fmt_uptime(dur, sizeof(dur), p->node->ns_ConnectSecs);
        }
        else
            state = "-";

        if (p->isMine)
            printf("  %-4s  %-26s  %-10s  %-9s  <- you\n",
                   p->name, dev, state, dur);
        else if (dur[0])
            printf("  %-4s  %-26s  %-10s  %s\n", p->name, dev, state, dur);
        else
            printf("  %-4s  %-26s  %s\n", p->name, dev, state);
    }

    /*
     * Deliberately not printed above: where the other callers are connecting
     * from.  Your own address is yours to see; theirs is not.
     */
    for (i = 0; i < count; i++)
        if (ports[i].isMine && ports[i].node && ports[i].node->ns_Peer[0])
        {
            printf("\n" LABEL "%s\n", "Calling from", ports[i].node->ns_Peer);
            break;
        }
}

/* ------------------------------------------------------------------ */
/* SerialTCP                                                           */
/* ------------------------------------------------------------------ */

static void show_serialtcp(const struct Snapshot *snap)
{
    char up[32];

    printf("\n SerialTCP\n");

    if (!snap->ok)
    {
        printf(LABEL "not running\n", "Daemon");
        return;
    }

    fmt_uptime(up, sizeof(up), snap->uptime);
    printf(LABEL "running, up %s\n", "Daemon", up);
    printf(LABEL "%lu node%s\n", "Serving",
           (unsigned long)snap->numNodes, snap->numNodes == 1 ? "" : "s");
    printf(LABEL "%lu", "Calls taken", (unsigned long)snap->totalCalls);
    if (snap->busyCalls)
        printf(" (%lu turned away busy)", (unsigned long)snap->busyCalls);
    printf("\n");

    if (snap->queueMax)
        printf(LABEL "%u waiting, room for %u\n", "Queue",
               (unsigned)snap->queued, (unsigned)snap->queueMax);
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    struct Snapshot *snap = &g_Snap;

    (void)verstag;

    if (argc > 1 && (!strcmp(argv[1], "?") || !strcmp(argv[1], "-h")))
    {
        printf("%s\n", VERSION_STRING);
        printf("A DLG door -- and a plain Shell command -- showing what this\n"
               "machine and this BBS are doing.  It takes no arguments.\n");
        return 0;
    }

    memset(snap, 0, sizeof(*snap));
    if (daemon_present())
        query_daemon(snap);

    /* Opened once and kept for the whole report: without it the BBS section
     * simply says so and the rest still prints. */
    DLGBase = OpenLibrary((CONST_STRPTR)"dlg.library", 0);

    printf("\n System information\n");
    printf(" ==================\n");

    show_system();
    show_memory();
    show_bbs(snap);
    show_serialtcp(snap);

    printf("\n");
    fflush(stdout);

    if (DLGBase)
        CloseLibrary(DLGBase);

    return 0;
}
