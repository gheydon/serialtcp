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

#ifndef FAKE_EXEC_DEVICES_H
#define FAKE_EXEC_DEVICES_H
#include <exec/ports.h>
struct Unit { struct MsgPort unit_MsgPort; UBYTE unit_flags, unit_pad; UWORD unit_OpenCnt; };
struct Device { int d; };
#endif
