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
 * util.c -- byte FIFO, time helpers and logging for SerialTCPd.
 */

#include <exec/types.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "daemon.h"

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */

extern struct Device *TimerBase;

void st_gettime(struct timeval *tv)
{
    if (TimerBase)
        GetSysTime(tv);
    else
    {
        tv->tv_secs  = 0;
        tv->tv_micro = 0;
    }
}

/* Milliseconds elapsed since `since`.  Saturates rather than overflowing. */
LONG st_elapsed_ms(const struct timeval *since)
{
    struct timeval now;
    LONG           secs, usecs;

    st_gettime(&now);

    secs  = (LONG)now.tv_secs  - (LONG)since->tv_secs;
    usecs = (LONG)now.tv_micro - (LONG)since->tv_micro;

    if (usecs < 0)
    {
        usecs += 1000000;
        secs  -= 1;
    }

    if (secs < 0)
        return 0;
    if (secs > 2000000)
        return 0x7FFFFFFF;

    return secs * 1000 + usecs / 1000;
}

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */

static BPTR g_LogFH = 0;

void log_open(void)
{
    if (g_Config.c_LogToFile && g_Config.c_LogFile[0])
    {
        g_LogFH = Open((CONST_STRPTR)g_Config.c_LogFile, MODE_READWRITE);
        if (g_LogFH)
            Seek(g_LogFH, 0, OFFSET_END);
    }
}

void log_close(void)
{
    if (g_LogFH)
    {
        Close(g_LogFH);
        g_LogFH = 0;
    }
}

void log_printf(const char *fmt, ...)
{
    char           buf[512];
    char           line[600];
    va_list        ap;
    struct timeval tv;
    ULONG          t, h, m, s;
    int            len;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';

    st_gettime(&tv);
    t = tv.tv_secs % 86400;
    h = t / 3600;
    m = (t % 3600) / 60;
    s = t % 60;

    len = snprintf(line, sizeof(line) - 1, "[%02lu:%02lu:%02lu] %s\n",
                   (unsigned long)h, (unsigned long)m, (unsigned long)s, buf);
    if (len < 0)
        return;
    if (len > (int)sizeof(line) - 1)
        len = (int)sizeof(line) - 1;

    /* Console first, so you can watch it live in a shell. */
    Write(Output(), (APTR)line, len);

    if (g_LogFH)
    {
        Write(g_LogFH, (APTR)line, len);
        Flush(g_LogFH);
    }
}
