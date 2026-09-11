#ifndef FAKE_EXEC_PORTS_H
#define FAKE_EXEC_PORTS_H
#include <exec/lists.h>
struct Task;
struct MsgPort { struct Node mp_Node; UBYTE mp_Flags, mp_SigBit; void *mp_SigTask; struct List mp_MsgList; };
struct Message { struct Node mn_Node; struct MsgPort *mn_ReplyPort; UWORD mn_Length; };
#define PA_SIGNAL 0
#define PA_IGNORE 2
#endif
