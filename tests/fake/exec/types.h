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

#ifndef FAKE_EXEC_TYPES_H
#define FAKE_EXEC_TYPES_H
#include <stdint.h>
#include <stddef.h>
typedef unsigned char  UBYTE;
typedef signed char    BYTE;
typedef unsigned short UWORD;
typedef short          WORD;
typedef uint32_t       ULONG;
typedef int32_t        LONG;
typedef int32_t        BOOL;
typedef void          *APTR;
typedef char          *STRPTR;
typedef const char    *CONST_STRPTR;
#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif
#ifndef NULL
#define NULL ((void*)0)
#endif
#endif
