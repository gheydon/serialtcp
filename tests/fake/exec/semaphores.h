#ifndef FAKE_EXEC_SEM_H
#define FAKE_EXEC_SEM_H
#include <exec/lists.h>
struct SignalSemaphore { struct Node ss_Link; WORD ss_NestCount; struct MinList ss_WaitQueue; };
#endif
