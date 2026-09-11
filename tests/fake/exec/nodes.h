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
