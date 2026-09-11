/*
 * SerialTCP -- a virtual serial device and modem daemon for AmigaOS.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <https://www.gnu.org/licenses/>.
 */

/*
 * fifo.c -- the byte FIFO used for both directions of every node.
 *
 * Single producer, single consumer, but both are the daemon task, so no
 * locking is needed. Kept in its own file so it can be compiled and tested
 * natively on the build host.
 */

#include <exec/types.h>
#include <string.h>

#include "daemon.h"

/* ------------------------------------------------------------------ */
/* FIFO                                                                */
/* ------------------------------------------------------------------ */

void fifo_init(struct Fifo *f, UBYTE *buf, ULONG size)
{
    f->f_Buf   = buf;
    f->f_Size  = size;
    f->f_Head  = 0;
    f->f_Count = 0;
}

void fifo_clear(struct Fifo *f)
{
    f->f_Head  = 0;
    f->f_Count = 0;
}

ULONG fifo_count(const struct Fifo *f)
{
    return f->f_Count;
}

ULONG fifo_space(const struct Fifo *f)
{
    return f->f_Size - f->f_Count;
}

/* Returns the byte at logical offset `index`, or -1 if out of range. */
LONG fifo_peek(const struct Fifo *f, ULONG index)
{
    if (index >= f->f_Count)
        return -1;
    return (LONG)f->f_Buf[(f->f_Head + index) % f->f_Size];
}

ULONG fifo_put(struct Fifo *f, const UBYTE *src, ULONG len)
{
    ULONG space = fifo_space(f);
    ULONG n, tail, chunk;

    if (len > space)
        len = space;

    n    = len;
    tail = (f->f_Head + f->f_Count) % f->f_Size;

    while (n)
    {
        chunk = f->f_Size - tail;
        if (chunk > n)
            chunk = n;
        memcpy(f->f_Buf + tail, src, chunk);
        src  += chunk;
        n    -= chunk;
        tail  = (tail + chunk) % f->f_Size;
    }

    f->f_Count += len;
    return len;
}

ULONG fifo_get(struct Fifo *f, UBYTE *dst, ULONG len)
{
    ULONG n, chunk;

    if (len > f->f_Count)
        len = f->f_Count;

    n = len;
    while (n)
    {
        chunk = f->f_Size - f->f_Head;
        if (chunk > n)
            chunk = n;
        memcpy(dst, f->f_Buf + f->f_Head, chunk);
        dst      += chunk;
        n        -= chunk;
        f->f_Head = (f->f_Head + chunk) % f->f_Size;
    }

    f->f_Count -= len;
    return len;
}
