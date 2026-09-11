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

#ifndef FAKE_EXEC_IO_H
#define FAKE_EXEC_IO_H
#include <exec/ports.h>
#include <exec/devices.h>
struct IORequest { struct Message io_Message; struct Device *io_Device; struct Unit *io_Unit;
                   UWORD io_Command; UBYTE io_Flags; BYTE io_Error; };
struct IOStdReq { struct Message io_Message; struct Device *io_Device; struct Unit *io_Unit;
                  UWORD io_Command; UBYTE io_Flags; BYTE io_Error;
                  ULONG io_Actual, io_Length; APTR io_Data; ULONG io_Offset; };
#define CMD_INVALID 0
#define CMD_RESET   1
#define CMD_READ    2
#define CMD_WRITE   3
#define CMD_UPDATE  4
#define CMD_CLEAR   5
#define CMD_STOP    6
#define CMD_START   7
#define CMD_FLUSH   8
#define CMD_NONSTD  9
#define IOERR_ABORTED   (-2)
#define IOERR_NOCMD     (-3)
#define IOERR_BADADDRESS (-5)
#define IOF_QUICK 1
#endif
