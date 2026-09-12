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
 * SerialTest -- a stand-in BBS node, for smoke-testing serialtcp.device.
 *
 * Opens one unit and behaves roughly the way a BBS does: waits for carrier,
 * greets whoever turns up, echoes what they type, and notices when they hang
 * up.  Everything it sees is also printed to the Amiga console, so a failure
 * can be pinned on the device, the daemon or the network.
 *
 * Carrier is read exactly the way DLG Pro reads it -- io_Status bit 5, active
 * low -- so if this reports carrier correctly, DLG will too.
 *
 * Usage: SerialTest [unit] [device] [dial-target]
 *
 * With a dial target it sends ATDT<target> once the unit is open and then
 * reports whatever the modem says back, which is how the outbound path and
 * the name resolver get exercised without a BBS in the way.
 */

#include <exec/types.h>
#include <exec/io.h>
#include <exec/memory.h>
#include <devices/serial.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>
#include <clib/alib_protos.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *verstag = "$VER: SerialTest 1.0 (11.9.2026)";

#define BUFSIZE 256

static struct MsgPort  *rport, *wport;
static struct IOExtSer *rio,   *wio;

static BOOL open_device(const char *name, ULONG unit)
{
    rport = CreateMsgPort();
    wport = CreateMsgPort();
    if (!rport || !wport)
        return FALSE;

    rio = (struct IOExtSer *)CreateIORequest(rport, sizeof(struct IOExtSer));
    wio = (struct IOExtSer *)CreateIORequest(wport, sizeof(struct IOExtSer));
    if (!rio || !wio)
        return FALSE;

    /* Ask for shared access before opening, exactly as DLG Pro does. */
    rio->io_SerFlags = SERF_SHARED | SERF_XDISABLED;

    if (OpenDevice((CONST_STRPTR)name, unit, (struct IORequest *)rio, 0) != 0)
    {
        printf("OpenDevice(\"%s\", %lu) failed, io_Error = %d\n",
               name, (unsigned long)unit, (int)rio->IOSer.io_Error);

        switch (rio->IOSer.io_Error)
        {
        case 101: printf("  -> SerialTCPd is not running.\n"); break;
        case 102: printf("  -> device/daemon version mismatch.\n"); break;
        case 103: printf("  -> no such unit; raise 'nodes' in the config.\n"); break;
        case 1:   printf("  -> unit already open exclusively.\n"); break;
        default:  break;
        }
        return FALSE;
    }

    /* Same parameters DLG sets. */
    rio->io_SerFlags = SERF_SHARED | SERF_XDISABLED;
    rio->io_ReadLen  = 8;
    rio->io_WriteLen = 8;
    rio->io_BrkTime  = 250000;
    rio->IOSer.io_Command = SDCMD_SETPARAMS;
    DoIO((struct IORequest *)rio);

    if (rio->IOSer.io_Error)
        printf("SDCMD_SETPARAMS returned io_Error %d\n", (int)rio->IOSer.io_Error);

    /* Clone the open request for writing. */
    memcpy(wio, rio, sizeof(struct IOExtSer));
    wio->IOSer.io_Message.mn_ReplyPort = wport;

    return TRUE;
}

static void close_device(void)
{
    if (rio && rio->IOSer.io_Device)
        CloseDevice((struct IORequest *)rio);
    if (wio) DeleteIORequest((struct IORequest *)wio);
    if (rio) DeleteIORequest((struct IORequest *)rio);
    if (wport) DeleteMsgPort(wport);
    if (rport) DeleteMsgPort(rport);
}

/* Bytes waiting, and the current status word. */
static ULONG query(UWORD *status)
{
    rio->IOSer.io_Command = SDCMD_QUERY;
    DoIO((struct IORequest *)rio);
    if (status)
        *status = rio->io_Status;
    return rio->IOSer.io_Actual;
}

/* DLG Pro's test, verbatim: bit 5, active low. */
static BOOL carrier(UWORD status)
{
    return (status & (1 << 5)) ? FALSE : TRUE;
}

static void write_str(const char *s)
{
    wio->IOSer.io_Command = CMD_WRITE;
    wio->IOSer.io_Data    = (APTR)s;
    wio->IOSer.io_Length  = strlen(s);
    DoIO((struct IORequest *)wio);
}

int main(int argc, char **argv)
{
    const char *devname = "serialtcp.device";
    const char *dial    = NULL;
    ULONG       unit    = 0;
    UBYTE       buf[BUFSIZE];
    BOOL        hadCarrier = FALSE;
    UWORD       status;
    ULONG       avail;
    ULONG       total = 0;

    (void)verstag;

    /* Unbuffered: this is usually run with output redirected to a file that
     * is being watched live, and buffered output would only appear at exit. */
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc > 1) unit    = (ULONG)atoi(argv[1]);
    if (argc > 2) devname = argv[2];
    if (argc > 3) dial    = argv[3];

    printf("SerialTest: opening %s unit %lu\n", devname, (unsigned long)unit);

    if (!open_device(devname, unit))
    {
        close_device();
        return 20;
    }

    query(&status);
    printf("opened OK. io_Status = 0x%04x, carrier = %s\n",
           (unsigned)status, carrier(status) ? "YES" : "no");
    if (dial)
    {
        char cmd[128];

        snprintf(cmd, sizeof(cmd), "ATDT%s\r", dial);
        printf("Dialling %s\n", dial);
        write_str(cmd);
    }

    printf("Waiting for a call. Press Ctrl-C to stop.\n");

    for (;;)
    {
        if (SetSignal(0, 0) & SIGBREAKF_CTRL_C)
        {
            printf("\nCtrl-C -- closing.\n");
            break;
        }

        avail = query(&status);

        if (carrier(status) && !hadCarrier)
        {
            hadCarrier = TRUE;
            total = 0;
            printf(">>> CARRIER UP (io_Status = 0x%04x)\n", (unsigned)status);
            write_str("\r\n*** SerialTest ***\r\n"
                      "You are connected through serialtcp.device.\r\n"
                      "Type something and it will be echoed back.\r\n\r\n> ");
        }
        else if (!carrier(status) && hadCarrier)
        {
            hadCarrier = FALSE;
            printf(">>> CARRIER LOST after %lu bytes (io_Status = 0x%04x)\n",
                   (unsigned long)total, (unsigned)status);
        }

        if (avail)
        {
            ULONG n = (avail > BUFSIZE - 1) ? BUFSIZE - 1 : avail;

            rio->IOSer.io_Command = CMD_READ;
            rio->IOSer.io_Data    = (APTR)buf;
            rio->IOSer.io_Length  = n;
            DoIO((struct IORequest *)rio);

            if (rio->IOSer.io_Error)
            {
                printf("CMD_READ failed, io_Error = %d\n", (int)rio->IOSer.io_Error);
                break;
            }

            n = rio->IOSer.io_Actual;
            total += n;

            buf[n] = '\0';
            printf("[recv %lu] %s\n", (unsigned long)n, (char *)buf);

            /* Echo it straight back, so the caller can see the round trip. */
            if (hadCarrier)
            {
                wio->IOSer.io_Command = CMD_WRITE;
                wio->IOSer.io_Data    = (APTR)buf;
                wio->IOSer.io_Length  = n;
                DoIO((struct IORequest *)wio);
            }
        }
        else
        {
            Delay(2);       /* 2 ticks; keeps the poll loop cheap */
        }
    }

    close_device();
    printf("SerialTest: closed.\n");
    return 0;
}
