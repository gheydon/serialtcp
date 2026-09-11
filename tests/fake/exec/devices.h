#ifndef FAKE_EXEC_DEVICES_H
#define FAKE_EXEC_DEVICES_H
#include <exec/ports.h>
struct Unit { struct MsgPort unit_MsgPort; UBYTE unit_flags, unit_pad; UWORD unit_OpenCnt; };
struct Device { int d; };
#endif
