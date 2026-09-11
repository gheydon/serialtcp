/*
 * Part of SerialTCP. Distributed under the GNU General Public License,
 * version 2 or later; see the LICENSE file at the root of the project.
 *
 * This is not AmigaOS source. It is a minimal re-declaration of the few
 * AmigaOS structures and constants the test harness needs, so that the pure
 * logic in the daemon can be compiled and run on the build host. The
 * declarations are statements of fact about an existing interface, not a
 * substitute for the NDK; build the real thing against the NDK headers.
 */

#ifndef FAKE_DEV_SERIAL_H
#define FAKE_DEV_SERIAL_H
#include <exec/io.h>
struct IOTArray { ULONG TermArray0, TermArray1; };
struct IOExtSer { struct IOStdReq IOSer; ULONG io_CtlChar, io_RBufLen, io_ExtFlags, io_Baud, io_BrkTime;
                  struct IOTArray io_TermArray; UBYTE io_ReadLen, io_WriteLen, io_StopBits, io_SerFlags; UWORD io_Status; };
#define SER_DEFAULT_CTLCHAR 0x11130000
#define SDCMD_QUERY      CMD_NONSTD
#define SDCMD_BREAK     (CMD_NONSTD+1)
#define SDCMD_SETPARAMS (CMD_NONSTD+2)
#define SERF_XDISABLED (1<<7)
#define SERF_EOFMODE   (1<<6)
#define SERF_SHARED    (1<<5)
#define SERF_RAD_BOOGIE (1<<4)
#define SERF_QUEUEDBRK (1<<3)
#define SERF_7WIRE     (1<<2)
#define SERF_PARTY_ODD (1<<1)
#define SERF_PARTY_ON  (1<<0)
#define IO_STATF_XOFFREAD  (1<<12)
#define IO_STATF_XOFFWRITE (1<<11)
#define IO_STATF_READBREAK (1<<10)
#define IO_STATF_WROTEBREAK (1<<9)
#define IO_STATF_OVERRUN   (1<<8)
#define SerErr_DevBusy 1
#define SerErr_InvParam 5
#define SerErr_LineErr 6
#define SERIALNAME "serial.device"
#endif
