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

#ifndef FAKE_EXEC_PORTS_H
#define FAKE_EXEC_PORTS_H
#include <exec/lists.h>
struct Task;
struct MsgPort { struct Node mp_Node; UBYTE mp_Flags, mp_SigBit; void *mp_SigTask; struct List mp_MsgList; };
struct Message { struct Node mn_Node; struct MsgPort *mn_ReplyPort; UWORD mn_Length; };
#define PA_SIGNAL 0
#define PA_IGNORE 2
#endif
