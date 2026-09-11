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

#ifndef FAKE_EXEC_NODES_H
#define FAKE_EXEC_NODES_H
#include <exec/types.h>
struct Node { struct Node *ln_Succ, *ln_Pred; UBYTE ln_Type, ln_Pri; char *ln_Name; };
struct MinNode { struct MinNode *mln_Succ, *mln_Pred; };
#define NT_MESSAGE 5
#define NT_REPLYMSG 7
#define NT_MSGPORT 4
#define NT_DEVICE 3
#endif
