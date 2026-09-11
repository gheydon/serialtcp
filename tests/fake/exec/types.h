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
