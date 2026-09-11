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
 * atcmd.c -- Hayes AT command emulation.
 *
 * The guiding principle here is that a BBS package sends a modem init string
 * it was configured with years ago and expects OK back.  A real modem ignores
 * settings it does not implement rather than failing the whole string, so a
 * strict parser that returns ERROR on the first unknown letter would break
 * setups that work fine on real hardware.  We therefore accept-and-ignore the
 * whole family of well-known but irrelevant commands (speaker volume, error
 * correction, compression, ...) and only return ERROR for genuine nonsense.
 */

#include <exec/types.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>

#include "daemon.h"

/* ------------------------------------------------------------------ */

void at_reset(struct STNode *n)
{
    memset(n->n_SReg, 0, sizeof(n->n_SReg));

    n->n_SReg[0]  = (UBYTE)g_Config.c_AutoAnswer;  /* rings before auto-answer */
    n->n_SReg[2]  = 43;    /* escape character '+'                            */
    n->n_SReg[3]  = 13;    /* carriage return                                 */
    n->n_SReg[4]  = 10;    /* line feed                                       */
    n->n_SReg[5]  = 8;     /* backspace                                       */
    n->n_SReg[6]  = 2;     /* wait before blind dial                          */
    n->n_SReg[7]  = 60;    /* wait for carrier after dial (seconds)           */
    n->n_SReg[8]  = 2;     /* comma pause                                     */
    n->n_SReg[9]  = 6;     /* carrier detect response time                    */
    n->n_SReg[10] = 14;    /* carrier loss hangup delay                       */
    n->n_SReg[11] = 95;    /* DTMF duration                                   */
    n->n_SReg[12] = 50;    /* escape guard time, in 20ms units                */

    n->n_Echo     = TRUE;
    n->n_Verbose  = TRUE;
    n->n_Quiet    = FALSE;
    n->n_DcdMode  = 1;     /* &C1: DCD follows the real carrier               */
    n->n_DtrMode  = 2;     /* &D2: losing DTR hangs up                        */
    n->n_CmdLen   = 0;
}

/* ------------------------------------------------------------------ */
/* Number parsing                                                      */
/* ------------------------------------------------------------------ */

/* Read an optional decimal number, defaulting to `dflt` when absent. */
static int get_num(const char **pp, int dflt)
{
    const char *p = *pp;
    int         v = 0;
    BOOL        any = FALSE;

    while (*p >= '0' && *p <= '9')
    {
        v   = v * 10 + (*p - '0');
        p++;
        any = TRUE;
    }

    *pp = p;
    return any ? v : dflt;
}

/* ------------------------------------------------------------------ */
/* Command execution                                                   */
/* ------------------------------------------------------------------ */

/*
 * Commands that real modems accept but that mean nothing to a TCP connection.
 * Each takes an optional numeric argument which we parse and discard.
 */
static BOOL is_ignorable(char c)
{
    switch (c)
    {
    case 'B':   /* Bell/CCITT mode          */
    case 'L':   /* speaker volume           */
    case 'M':   /* speaker control          */
    case 'N':   /* negotiate handshake      */
    case 'P':   /* pulse dial default       */
    case 'T':   /* tone dial default        */
    case 'W':   /* connect message format   */
    case 'X':   /* result code set          */
    case 'Y':   /* long space disconnect    */
        return TRUE;
    default:
        return FALSE;
    }
}

/* Handle the AT& family. */
static BOOL do_amp(struct STNode *n, const char **pp)
{
    char c = (char)toupper((unsigned char)**pp);
    int  v;

    if (!c)
        return FALSE;

    (*pp)++;
    v = get_num(pp, 0);

    switch (c)
    {
    case 'F':                       /* factory defaults */
        at_reset(n);
        return TRUE;

    case 'C':                       /* DCD behaviour */
        n->n_DcdMode = (UBYTE)v;
        if (v == 0)
        {
            /* &C0 forces DCD permanently on.  Honour it literally: some old
             * setups rely on it, even though it hides real disconnects. */
            node_set_status(n, (UWORD)(n->n_Unit ? (n->n_Unit->su_Status & ~STSTF_CD) : 0));
        }
        return TRUE;

    case 'D':                       /* DTR behaviour */
        n->n_DtrMode = (UBYTE)v;
        return TRUE;

    case 'K':                       /* flow control    */
    case 'Q':                       /* async mode      */
    case 'S':                       /* DSR behaviour   */
    case 'R':                       /* RTS/CTS         */
    case 'G':                       /* guard tone      */
    case 'W':                       /* store profile   */
    case 'Y':                       /* power-on profile*/
    case 'V':                       /* view settings   */
    case 'Z':                       /* stored number   */
        return TRUE;

    default:
        return FALSE;
    }
}

/* Handle ATSn=v and ATSn? */
static BOOL do_sreg(struct STNode *n, const char **pp)
{
    const char *p = *pp;
    int         reg, val;
    char        buf[24];

    reg = get_num(&p, -1);
    if (reg < 0)
        return FALSE;

    if (*p == '=')
    {
        p++;
        val = get_num(&p, -1);
        if (val < 0)
            return FALSE;

        if (reg < (int)sizeof(n->n_SReg))
            n->n_SReg[reg] = (UBYTE)val;

        *pp = p;
        return TRUE;
    }

    if (*p == '?')
    {
        p++;
        val = (reg < (int)sizeof(n->n_SReg)) ? n->n_SReg[reg] : 0;
        snprintf(buf, sizeof(buf), "\r\n%03d\r\n", val);
        node_reply_text(n, buf);
        *pp = p;
        return TRUE;
    }

    return FALSE;
}

static void do_info(struct STNode *n, int which)
{
    char buf[96];

    switch (which)
    {
    case 0:
        snprintf(buf, sizeof(buf), "\r\nSerialTCP\r\n");
        break;
    case 3:
        snprintf(buf, sizeof(buf), "\r\nSerialTCPd virtual modem 1.0\r\n");
        break;
    case 4:
        snprintf(buf, sizeof(buf), "\r\nNode %lu, telnet %s\r\n",
                 (unsigned long)n->n_Num, g_Config.c_Telnet ? "on" : "off");
        break;
    default:
        snprintf(buf, sizeof(buf), "\r\n1.0\r\n");
        break;
    }

    node_reply_text(n, buf);
}

/*
 * Execute one complete command line (without the terminating CR).
 */
static void execute(struct STNode *n, const char *line)
{
    const char *p = line;
    BOOL        quiet_result = FALSE;
    char        c;

    /* Skip leading whitespace. */
    while (*p == ' ' || *p == '\t')
        p++;

    /* "A/" repeats the previous command and has no AT prefix. */
    if ((p[0] == 'A' || p[0] == 'a') && p[1] == '/')
    {
        if (n->n_LastCmd[0])
            execute(n, (const char *)n->n_LastCmd);
        else
            node_result(n, RC_ERROR, 0);
        return;
    }

    /* A bare "AT" is a valid no-op used to test the modem. */
    if (!((p[0] == 'A' || p[0] == 'a') && (p[1] == 'T' || p[1] == 't')))
    {
        /* An empty line is silently ignored, as on a real modem. */
        if (!*p)
            return;
        node_result(n, RC_ERROR, 0);
        return;
    }

    p += 2;

    strncpy((char *)n->n_LastCmd, line, sizeof(n->n_LastCmd) - 1);
    n->n_LastCmd[sizeof(n->n_LastCmd) - 1] = '\0';

    while (*p)
    {
        c = (char)toupper((unsigned char)*p);
        p++;

        if (c == ' ' || c == '\t')
            continue;

        switch (c)
        {
        case 'A':                       /* answer */
            node_answer(n);
            return;

        case 'D':                       /* dial -- consumes the rest of the line */
        {
            const char *t = p;

            /* Skip the dial modifier (T, P, I, W, comma, spaces). */
            while (*t == 'T' || *t == 't' || *t == 'P' || *t == 'p' ||
                   *t == 'I' || *t == 'i' || *t == 'W' || *t == 'w' ||
                   *t == ' ' || *t == ',')
                t++;

            node_dial(n, t);
            return;
        }

        case 'E':
            n->n_Echo = (get_num(&p, 1) != 0);
            break;

        case 'H':
            if (get_num(&p, 0) == 0)
            {
                if (n->n_Sock >= 0)
                {
                    /* Deliberately no NO CARRIER: the caller asked for this
                     * hangup, and a real modem answers OK instead. */
                    node_hangup(n, FALSE);
                }
            }
            break;

        case 'I':
            do_info(n, get_num(&p, 0));
            break;

        case 'O':                       /* return to data mode */
            get_num(&p, 0);
            if (n->n_State == NS_ESCAPED && n->n_Sock >= 0)
            {
                n->n_State = NS_ONLINE;
                node_result(n, RC_CONNECT, n->n_ConnectBaud);
                quiet_result = TRUE;
            }
            else
            {
                node_result(n, RC_ERROR, 0);
                return;
            }
            break;

        case 'Q':
            n->n_Quiet = (get_num(&p, 0) != 0);
            break;

        case 'V':
            n->n_Verbose = (get_num(&p, 1) != 0);
            break;

        case 'Z':
            get_num(&p, 0);
            node_hangup(n, FALSE);
            at_reset(n);
            break;

        case 'S':
            if (!do_sreg(n, &p))
            {
                node_result(n, RC_ERROR, 0);
                return;
            }
            break;

        case '&':
            if (!do_amp(n, &p))
            {
                node_result(n, RC_ERROR, 0);
                return;
            }
            break;

        case '%':                       /* %C compression, %E escape, ... */
        case '\\':                      /* \N mode, \Q flow control, ...  */
        case '*':
            if (*p)
                p++;                    /* skip the sub-letter */
            get_num(&p, 0);
            break;

        case '+':
            /* +++ typed in command mode is meaningless; ignore the run. */
            while (*p == '+')
                p++;
            break;

        default:
            if (is_ignorable(c))
            {
                get_num(&p, 0);
                break;
            }
            node_result(n, RC_ERROR, 0);
            return;
        }
    }

    if (!quiet_result)
        node_result(n, RC_OK, 0);
}

/* ------------------------------------------------------------------ */
/* Character feed                                                      */
/* ------------------------------------------------------------------ */

void at_feed(struct STNode *n, const UBYTE *data, ULONG len)
{
    ULONG i;
    UBYTE c;
    UBYTE cr = n->n_SReg[3] ? n->n_SReg[3] : 13;
    UBYTE lf = n->n_SReg[4] ? n->n_SReg[4] : 10;
    UBYTE bs = n->n_SReg[5] ? n->n_SReg[5] : 8;

    for (i = 0; i < len; i++)
    {
        c = data[i];

        if (c == cr)
        {
            if (n->n_Echo)
            {
                UBYTE eol[2];
                eol[0] = cr;
                eol[1] = lf;
                node_to_app(n, eol, 2);
            }

            n->n_Cmd[n->n_CmdLen] = '\0';
            execute(n, (const char *)n->n_Cmd);
            n->n_CmdLen = 0;
            continue;
        }

        if (c == lf)
        {
            /*
             * A real modem terminates on CR only.  We also accept a bare LF,
             * but only when something is already buffered -- so CR LF still
             * executes exactly once (the CR runs it and empties the buffer,
             * then the LF finds nothing to do).
             *
             * This matters for DLG Pro's command-mode hangup, which writes
             * "ATH0" followed by "\n".  Its handler normally expands that LF
             * to CR LF on the way out, but not when the port is in pass-thru
             * mode, and a command that silently never executes would leave the
             * line up.
             */
            if (n->n_CmdLen)
            {
                n->n_Cmd[n->n_CmdLen] = '\0';
                execute(n, (const char *)n->n_Cmd);
                n->n_CmdLen = 0;
            }
            continue;
        }

        if (c == bs || c == 127)
        {
            if (n->n_CmdLen)
            {
                n->n_CmdLen--;
                if (n->n_Echo)
                    node_to_app(n, (const UBYTE *)"\b \b", 3);
            }
            continue;
        }

        if (n->n_Echo)
            node_to_app(n, &c, 1);

        /* Overlong lines are truncated, not rejected -- same as a real modem
         * running out of its 40-character command buffer. */
        if (n->n_CmdLen < CMDBUF_SIZE - 1)
            n->n_Cmd[n->n_CmdLen++] = c;
    }
}
