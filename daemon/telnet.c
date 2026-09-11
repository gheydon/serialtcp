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
 * telnet.c -- RFC 854 option negotiation and data transparency.
 *
 * Two jobs:
 *
 *  1. Negotiate BINARY in both directions.  This is not optional for a BBS:
 *     without it Zmodem transfers corrupt on any byte that happens to be 0xFF,
 *     and high-bit ANSI art gets mangled.
 *
 *  2. Keep the data path transparent.  0xFF on the wire means IAC, so a real
 *     0xFF in the data stream has to be doubled, and in NVT (non-binary) mode a
 *     bare CR must be followed by NUL.
 *
 * Negotiation refusals are answered exactly once per request and we never
 * answer a WONT/DONT that does not change our state, which is what keeps the
 * classic negotiation ping-pong loop from happening.
 */

#include <exec/types.h>
#include <string.h>

#include "daemon.h"

/* Options we are willing to turn on.  Everything else is refused. */
static BOOL we_support_local(struct Telnet *t, UBYTE opt)
{
    switch (opt)
    {
    case TELOPT_BINARY: return TRUE;
    case TELOPT_SGA:    return TRUE;
    case TELOPT_ECHO:   return t->t_Server;   /* only a server offers to echo */
    default:            return FALSE;
    }
}

static BOOL we_support_remote(struct Telnet *t, UBYTE opt)
{
    switch (opt)
    {
    case TELOPT_BINARY: return TRUE;
    case TELOPT_SGA:    return TRUE;
    case TELOPT_ECHO:   return !t->t_Server;  /* a client lets the server echo */
    default:            return FALSE;
    }
}

/* Stage a three-byte negotiation command for the caller to send. */
static void send_cmd(struct Telnet *t, UBYTE verb, UBYTE opt)
{
    if (t->t_RespLen + 3 > SUBNEG_SIZE)
        return;                         /* flooded: decline to answer */

    t->t_RespBuf[t->t_RespLen++] = TEL_IAC;
    t->t_RespBuf[t->t_RespLen++] = verb;
    t->t_RespBuf[t->t_RespLen++] = opt;
}

ULONG telnet_take_responses(struct Telnet *t, UBYTE *out, ULONG outmax)
{
    ULONG n = t->t_RespLen;

    if (n > outmax)
        n = outmax;

    if (n)
        memcpy(out, t->t_RespBuf, n);

    /* Anything that would not fit is dropped rather than left half-sent: a
     * partial IAC sequence would desynchronise the peer. */
    t->t_RespLen = 0;
    return n;
}

void telnet_reset(struct Telnet *t, BOOL enabled, BOOL server)
{
    memset(t, 0, sizeof(*t));
    t->t_Enabled = enabled;
    t->t_Server  = server;
    t->t_State   = TS_DATA;
}

/* Opening offer, sent as soon as a connection is established. */
void telnet_start_t(struct Telnet *t)
{
    if (!t->t_Enabled || t->t_SentInitial)
        return;

    t->t_SentInitial = TRUE;

    if (t->t_Server)
    {
        send_cmd(t, TEL_WILL, TELOPT_ECHO);
        send_cmd(t, TEL_WILL, TELOPT_SGA);
    }
    else
    {
        send_cmd(t, TEL_DO, TELOPT_SGA);
    }

    /* Ask for 8-bit clean in both directions. */
    send_cmd(t, TEL_WILL, TELOPT_BINARY);
    send_cmd(t, TEL_DO,   TELOPT_BINARY);
}

static void handle_will(struct Telnet *t, UBYTE opt)
{
    if (we_support_remote(t, opt))
    {
        if (opt == TELOPT_BINARY)
        {
            if (t->t_BinaryRemote)
                return;                 /* already on: stay quiet */
            t->t_BinaryRemote = TRUE;
        }
        send_cmd(t, TEL_DO, opt);
    }
    else
    {
        send_cmd(t, TEL_DONT, opt);
    }
}

static void handle_wont(struct Telnet *t, UBYTE opt)
{
    if (opt == TELOPT_BINARY)
    {
        if (!t->t_BinaryRemote)
            return;                     /* already off: no state change */
        t->t_BinaryRemote = FALSE;
    }

    send_cmd(t, TEL_DONT, opt);
}

static void handle_do(struct Telnet *t, UBYTE opt)
{
    if (we_support_local(t, opt))
    {
        if (opt == TELOPT_BINARY)
        {
            if (t->t_BinaryLocal)
                return;
            t->t_BinaryLocal = TRUE;
        }
        send_cmd(t, TEL_WILL, opt);
    }
    else
    {
        send_cmd(t, TEL_WONT, opt);
    }
}

static void handle_dont(struct Telnet *t, UBYTE opt)
{
    if (opt == TELOPT_BINARY)
    {
        if (!t->t_BinaryLocal)
            return;
        t->t_BinaryLocal = FALSE;
    }

    send_cmd(t, TEL_WONT, opt);
}

/*
 * Wire bytes in, application bytes out.
 *
 * Negotiation replies are staged in the Telnet state for the caller to drain,
 * so they never pass through the encoder and never get IAC-doubled.
 *
 * Returns the number of application bytes written to `out`.  Never writes more
 * than inlen bytes, so `outmax >= inlen` is always sufficient.
 */
ULONG telnet_decode_t(struct Telnet *t, const UBYTE *in, ULONG inlen, UBYTE *out, ULONG outmax)
{
    ULONG          o = 0;
    ULONG          i;
    UBYTE          c;

    if (!t->t_Enabled)
    {
        ULONG len = (inlen < outmax) ? inlen : outmax;
        memcpy(out, in, len);
        return len;
    }

    for (i = 0; i < inlen && o < outmax; i++)
    {
        c = in[i];

        switch (t->t_State)
        {
        case TS_DATA:
            if (c == TEL_IAC)
            {
                t->t_State = TS_IAC;
            }
            else if (!t->t_BinaryRemote && c == 0 && t->t_OutLastCR)
            {
                /* NVT CR NUL means a bare CR.  Swallow the NUL. */
                t->t_OutLastCR = FALSE;
            }
            else
            {
                t->t_OutLastCR = (!t->t_BinaryRemote && c == '\r');
                out[o++] = c;
            }
            break;

        case TS_IAC:
            switch (c)
            {
            case TEL_IAC:                       /* escaped 0xFF */
                out[o++]       = TEL_IAC;
                t->t_OutLastCR = FALSE;
                t->t_State     = TS_DATA;
                break;
            case TEL_WILL: t->t_State = TS_WILL; break;
            case TEL_WONT: t->t_State = TS_WONT; break;
            case TEL_DO:   t->t_State = TS_DO;   break;
            case TEL_DONT: t->t_State = TS_DONT; break;
            case TEL_SB:
                t->t_State  = TS_SB;
                t->t_SubLen = 0;
                break;
            default:
                /* NOP, DM, BRK, IP, AO, AYT, EC, EL, GA -- nothing useful for
                 * a BBS session, so drop them. */
                t->t_State = TS_DATA;
                break;
            }
            break;

        case TS_WILL: handle_will(t, c); t->t_State = TS_DATA; break;
        case TS_WONT: handle_wont(t, c); t->t_State = TS_DATA; break;
        case TS_DO:   handle_do(t, c);   t->t_State = TS_DATA; break;
        case TS_DONT: handle_dont(t, c); t->t_State = TS_DATA; break;

        case TS_SB:
            if (c == TEL_IAC)
                t->t_State = TS_SB_IAC;
            else if (t->t_SubLen < SUBNEG_SIZE)
                t->t_SubBuf[t->t_SubLen++] = c;
            break;

        case TS_SB_IAC:
            if (c == TEL_SE)
            {
                /* Subnegotiation complete.  We do not act on any of them, but
                 * consuming them correctly is what keeps the stream in sync. */
                t->t_State = TS_DATA;
            }
            else if (c == TEL_IAC)
            {
                if (t->t_SubLen < SUBNEG_SIZE)
                    t->t_SubBuf[t->t_SubLen++] = TEL_IAC;
                t->t_State = TS_SB;
            }
            else
            {
                t->t_State = TS_SB;
            }
            break;
        }
    }

    return o;
}

/*
 * Application bytes in, wire bytes out.
 *
 * Worst case expansion is 2x (every byte an IAC, or every byte a CR needing a
 * NUL), so the caller must supply outmax >= 2 * inlen.  All input is consumed.
 */
ULONG telnet_encode_t(struct Telnet *t, const UBYTE *in, ULONG inlen, UBYTE *out, ULONG outmax)
{
    ULONG          o = 0;
    ULONG          i;
    UBYTE          c;

    if (!t->t_Enabled)
    {
        ULONG len = (inlen < outmax) ? inlen : outmax;
        memcpy(out, in, len);
        return len;
    }

    for (i = 0; i < inlen; i++)
    {
        c = in[i];

        if (o + 2 > outmax)
            break;

        if (c == TEL_IAC)
        {
            out[o++] = TEL_IAC;
            out[o++] = TEL_IAC;
        }
        else if (!t->t_BinaryLocal && c == '\r')
        {
            /* NVT: a CR that is not part of CR LF must be followed by NUL. */
            out[o++] = '\r';
            if (i + 1 < inlen && in[i + 1] == '\n')
            {
                out[o++] = '\n';
                i++;
            }
            else
            {
                out[o++] = '\0';
            }
        }
        else
        {
            out[o++] = c;
        }
    }

    return o;
}
