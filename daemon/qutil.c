/*
 * SerialTCP -- a virtual serial device and modem daemon for AmigaOS.
 * Copyright (C) 2026 Gordon Heydon
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
 * qutil.c -- the parts of the queue that are pure computation.
 *
 * Kept apart from queue.c, which touches sockets, so these can be compiled and
 * exercised natively by the test harness.  Both have had bugs waiting to
 * happen in them: digit order and buffer bounds in the expander, and the
 * secs/micros comparison in the ordering.
 */

#include <exec/types.h>
#include <devices/timer.h>

#include "daemon.h"

/*
 * Substitute our own %N placeholder with a number.
 *
 * Deliberately not printf: the template comes from a config file, and handing
 * a user-supplied format string to printf is how you get crashes.  %N is the
 * only sequence recognised; everything else, "%" included, is literal.
 *
 * Always NUL-terminates, and never writes more than dstsize bytes.
 */
void queue_expand(char *dst, ULONG dstsize, const char *src, UWORD value)
{
    ULONG i = 0;

    if (!dst || dstsize == 0)
        return;

    if (!src)
    {
        dst[0] = '\0';
        return;
    }

    while (*src && i < dstsize - 1)
    {
        if (src[0] == '%' && (src[1] == 'N' || src[1] == 'n'))
        {
            char  num[8];
            int   d = 0;
            UWORD v = value;

            if (v == 0)
                num[d++] = '0';
            while (v && d < 6)
            {
                num[d++] = (char)('0' + (v % 10));
                v /= 10;
            }

            /* Digits came out least-significant first. */
            while (d-- > 0 && i < dstsize - 1)
                dst[i++] = num[d];

            src += 2;
        }
        else
        {
            dst[i++] = *src++;
        }
    }

    dst[i] = '\0';
}

/* Strict "a happened before b", carrying seconds and microseconds. */
BOOL queue_earlier(const struct timeval *a, const struct timeval *b)
{
    if (a->tv_secs != b->tv_secs)
        return (a->tv_secs < b->tv_secs) ? TRUE : FALSE;

    return (a->tv_micro < b->tv_micro) ? TRUE : FALSE;
}

/*
 * Parse a unit list like "0,2 3" into a bitmask.
 *
 * Separators are commas and whitespace, both accepted and mixed freely,
 * because a config file written by hand will contain both.  Anything that is
 * not a number below maxUnits makes the whole call return FALSE, but the
 * units that did parse are still set in *mask -- a typo in one entry should
 * not silently discard the others.
 */
BOOL parse_unit_list(const char *s, ULONG *mask, UWORD maxUnits)
{
    BOOL ok = TRUE;

    *mask = 0;

    if (!s)
        return FALSE;

    while (*s)
    {
        ULONG v;
        BOOL  digits = FALSE;

        /* Skip separators. */
        while (*s == ',' || *s == ' ' || *s == '\t')
            s++;

        if (!*s)
            break;

        v = 0;
        while (*s >= '0' && *s <= '9')
        {
            v = v * 10 + (ULONG)(*s - '0');
            s++;
            digits = TRUE;

            if (v > 100000)         /* stop runaway values overflowing */
                break;
        }

        if (!digits)
        {
            /* Junk: step over it so we cannot loop forever on it. */
            ok = FALSE;
            while (*s && *s != ',' && *s != ' ' && *s != '\t')
                s++;
            continue;
        }

        if (v >= maxUnits || v >= 32)
        {
            ok = FALSE;
            continue;
        }

        *mask |= (1UL << v);
    }

    return ok;
}

/* xorshift32. Cheap, adequate for deciding when to tell a joke. */
ULONG st_rand(ULONG *state)
{
    ULONG x = *state;

    if (x == 0)
        x = 0x2545F491UL;       /* any non-zero seed will do */

    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;

    *state = x;
    return x;
}

/*
 * Decide what position to tell a waiting caller they are in.
 *
 * This is cosmetic only.  q_oldest_waiting() decides who is actually served
 * next, and it uses arrival time -- nothing here can reorder the queue, delay
 * anybody, or change who gets the next free node.  The worst a lie can do is
 * annoy somebody, which is the entire point.
 *
 * QL_INFLATE is the phone-tree classic: start higher than the truth and creep
 * upward with every notice, so the longer you wait the worse it looks.
 *
 * QL_RANDOM is mostly honest and occasionally pads the number, which is more
 * unsettling than lying every time.
 *
 * The reported position is never below 1, and never below the true position
 * for QL_INFLATE -- telling somebody they are number 1 for ten minutes is a
 * different and much crueller joke.
 */
UWORD queue_reported_position(UWORD truePos, UWORD mode, UWORD lieStart,
                              UWORD notices, UWORD chance, ULONG *rng)
{
    ULONG v;

    if (truePos < 1)
        truePos = 1;

    switch (mode)
    {
    case QL_INFLATE:
        if (lieStart < 1)
            lieStart = 1;

        v = (ULONG)lieStart + (ULONG)notices;

        if (v < truePos)
            v = truePos;
        if (v > 999)
            v = 999;

        return (UWORD)v;

    case QL_RANDOM:
        if (chance == 0)
            return truePos;

        if ((st_rand(rng) % 100UL) < (ULONG)chance)
        {
            v = (ULONG)truePos + 1UL + (st_rand(rng) % 4UL);   /* +1 .. +4 */
            if (v > 999)
                v = 999;
            return (UWORD)v;
        }

        return truePos;

    case QL_OFF:
    default:
        return truePos;
    }
}
