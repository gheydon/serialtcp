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
 * config.c -- plain key/value configuration file for SerialTCPd.
 *
 * Deliberately dumb: one directive per line, '#' or ';' starts a comment,
 * blank lines ignored.  Unknown keys are reported but not fatal, so a config
 * written for a newer build still starts an older daemon.
 */

#include <exec/types.h>
#include <proto/dos.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "daemon.h"

struct Config g_Config;

void config_defaults(void)
{
    memset(&g_Config, 0, sizeof(g_Config));

    g_Config.c_Nodes            = 4;
    g_Config.c_ListenPorts[0]   = 23;
    g_Config.c_ListenNodes[0]   = ST_NODE_POOL;
    g_Config.c_NumListeners     = 1;
    g_Config.c_Telnet           = TRUE;
    g_Config.c_AnswerBaud       = 38400;
    g_Config.c_RingsBeforeBusy  = 8;
    g_Config.c_AutoAnswer       = 1;
    g_Config.c_NodeSettle       = 2;
    g_Config.c_RxBufSize        = RXBUF_DEFAULT;
    g_Config.c_TxBufSize        = TXBUF_DEFAULT;
    g_Config.c_LogToFile        = FALSE;
    g_Config.c_BusyMessageEnabled = TRUE;

    strcpy(g_Config.c_BusyMessage,
           "\r\nAll lines are currently in use. Please try again later.\r\n");
    strcpy(g_Config.c_LogFile, "T:serialtcp.log");

    g_Config.c_BusyAction   = WB_BUSY;
    g_Config.c_QueueMax     = 8;
    g_Config.c_QueueTimeout = 600;
    g_Config.c_QueueNotify  = 60;
    g_Config.c_AskTimeout   = 30;
    g_Config.c_QueueLie       = QL_OFF;
    g_Config.c_QueueLieStart  = 3;
    g_Config.c_QueueLieChance = 50;

    strcpy(g_Config.c_QueueMessage,
           "\r\nAll lines are busy. You are now waiting for the next free line.\r\n"
           "Stay connected and you will be put through automatically.\r\n");
    strcpy(g_Config.c_AskMessage,
           "\r\nAll lines are currently in use.\r\n"
           "Would you like to wait for the next free line? [Y/n] ");
    strcpy(g_Config.c_QueuePosMessage,
           "\r\nStill waiting -- you are number %N in the queue.\r\n");
    strcpy(g_Config.c_QueueGiveUpMessage,
           "\r\nSorry, no line became free in time. Please try again later.\r\n");
}

/* Strip leading/trailing whitespace in place, return pointer to first char. */
static char *trim(char *s)
{
    char *e;

    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;

    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        e--;
    *e = '\0';

    return s;
}

/*
 * Copy a value, turning the two escapes a busy banner actually needs (\r and
 * \n) into real control characters.  Also handles \\ and \t.
 */
static void copy_escaped(char *dst, ULONG dstsize, const char *src)
{
    ULONG i = 0;

    /* Allow the value to be wrapped in double quotes. */
    ULONG len = strlen(src);
    if (len >= 2 && src[0] == '"' && src[len - 1] == '"')
    {
        len -= 2;
        src++;
    }

    while (*src && i < dstsize - 1 && len--)
    {
        if (*src == '\\' && len)
        {
            src++;
            len--;
            switch (*src)
            {
            case 'r':  dst[i++] = '\r'; break;
            case 'n':  dst[i++] = '\n'; break;
            case 't':  dst[i++] = '\t'; break;
            case '\\': dst[i++] = '\\'; break;
            default:   dst[i++] = *src; break;
            }
            src++;
        }
        else
        {
            dst[i++] = *src++;
        }
    }

    dst[i] = '\0';
}

static BOOL parse_bool(const char *v)
{
    if (!stricmp(v, "on") || !stricmp(v, "yes") || !stricmp(v, "true") || !strcmp(v, "1"))
        return TRUE;
    return FALSE;
}

BOOL config_load(const char *path)
{
    BPTR  fh;
    char  raw[512];
    char *line, *key, *val;
    int   lineno = 0;

    fh = Open((CONST_STRPTR)path, MODE_OLDFILE);
    if (!fh)
        return FALSE;

    while (FGets(fh, (STRPTR)raw, sizeof(raw) - 1))
    {
        lineno++;

        /* Comments. */
        {
            char *h = strchr(raw, '#');
            if (h) *h = '\0';
            h = strchr(raw, ';');
            if (h) *h = '\0';
        }

        line = trim(raw);
        if (!*line)
            continue;

        key = line;
        val = line;
        while (*val && *val != ' ' && *val != '\t' && *val != '=')
            val++;
        if (*val)
            *val++ = '\0';
        val = trim(val);
        if (*val == '=')
            val = trim(val + 1);

        if (!stricmp(key, "nodes"))
        {
            int n = atoi(val);
            if (n < 1) n = 1;
            if (n > MAX_NODES) n = MAX_NODES;
            g_Config.c_Nodes = (UWORD)n;
        }
        else if (!stricmp(key, "listen"))
        {
            /* First "listen" replaces the default, later ones add. */
            static BOOL seen = FALSE;
            int p = atoi(val);

            if (!seen)
            {
                g_Config.c_NumListeners = 0;
                seen = TRUE;
            }
            if (p > 0 && p < 65536 && g_Config.c_NumListeners < MAX_LISTENERS)
            {
                g_Config.c_ListenNodes[g_Config.c_NumListeners] = ST_NODE_POOL;
                g_Config.c_ListenPorts[g_Config.c_NumListeners++] = (UWORD)p;
            }
        }
        else if (!stricmp(key, "listen-node"))
        {
            /*
             * "listen-node <port> <unit>" -- a private number for one unit,
             * instead of the shared pool. Two values on one line.
             */
            const char *v = val;
            int port = atoi(v);
            int unit = -1;

            while (*v && *v != ' ' && *v != '\t' && *v != ',')
                v++;
            while (*v == ' ' || *v == '\t' || *v == ',')
                v++;

            if (*v >= '0' && *v <= '9')
                unit = atoi(v);

            if (port <= 0 || port >= 65536 || unit < 0)
                printf("serialtcpd: %s line %d: listen-node needs a port and a unit,"
                       " e.g. 'listen-node 2400 3'\n", path, lineno);
            else if (g_Config.c_NumListeners >= MAX_LISTENERS)
                printf("serialtcpd: %s line %d: too many listeners (max %d)\n",
                       path, lineno, MAX_LISTENERS);
            else
            {
                g_Config.c_ListenNodes[g_Config.c_NumListeners] = (WORD)unit;
                g_Config.c_ListenPorts[g_Config.c_NumListeners++] = (UWORD)port;
            }
        }
        else if (!stricmp(key, "bind"))
        {
            strncpy(g_Config.c_BindAddr, val, sizeof(g_Config.c_BindAddr) - 1);
        }
        else if (!stricmp(key, "telnet"))
        {
            g_Config.c_Telnet = parse_bool(val);
        }
        else if (!stricmp(key, "answer-baud") || !stricmp(key, "baud"))
        {
            g_Config.c_AnswerBaud = (ULONG)atoi(val);
        }
        else if (!stricmp(key, "queue-lie"))
        {
            if (!stricmp(val, "inflate") || !stricmp(val, "increase"))
                g_Config.c_QueueLie = QL_INFLATE;
            else if (!stricmp(val, "random") || !stricmp(val, "sometimes"))
                g_Config.c_QueueLie = QL_RANDOM;
            else if (!stricmp(val, "off") || !stricmp(val, "honest") || !stricmp(val, "no"))
                g_Config.c_QueueLie = QL_OFF;
            else
                printf("serialtcpd: %s line %d: queue-lie must be off, inflate or random\n",
                       path, lineno);
        }
        else if (!stricmp(key, "queue-lie-start"))
        {
            int v = atoi(val);
            if (v < 1)   v = 1;
            if (v > 999) v = 999;
            g_Config.c_QueueLieStart = (UWORD)v;
        }
        else if (!stricmp(key, "queue-lie-chance"))
        {
            int v = atoi(val);
            if (v < 0)   v = 0;
            if (v > 100) v = 100;
            g_Config.c_QueueLieChance = (UWORD)v;
        }
        else if (!stricmp(key, "outbound-only") || !stricmp(key, "no-inbound"))
        {
            ULONG m = 0;

            if (!parse_unit_list(val, &m, MAX_NODES))
                printf("serialtcpd: %s line %d: outbound-only: bad unit in '%s'\n",
                       path, lineno, val);

            /* Repeatable: each line adds to the set. */
            g_Config.c_OutboundOnly |= m;
        }
        else if (!stricmp(key, "node-settle"))
        {
            int v = atoi(val);
            if (v < 0)  v = 0;
            if (v > 60) v = 60;
            g_Config.c_NodeSettle = (ULONG)v;
        }
        else if (!stricmp(key, "buffer-size"))
        {
            int b = atoi(val);
            if (b < BUFSIZE_MIN) b = BUFSIZE_MIN;
            if (b > BUFSIZE_MAX) b = BUFSIZE_MAX;
            g_Config.c_RxBufSize = (ULONG)b;
            g_Config.c_TxBufSize = (ULONG)b;
        }
        else if (!stricmp(key, "auto-answer"))
        {
            int a = atoi(val);
            if (a < 0) a = 0;
            if (a > 255) a = 255;
            g_Config.c_AutoAnswer = (UWORD)a;
        }
        else if (!stricmp(key, "rings"))
        {
            int r = atoi(val);
            if (r < 1) r = 1;
            g_Config.c_RingsBeforeBusy = (UWORD)r;
        }
        else if (!stricmp(key, "when-busy"))
        {
            if (!stricmp(val, "queue"))     g_Config.c_BusyAction = WB_QUEUE;
            else if (!stricmp(val, "ask"))  g_Config.c_BusyAction = WB_ASK;
            else if (!stricmp(val, "busy") || !stricmp(val, "drop"))
                                            g_Config.c_BusyAction = WB_BUSY;
            else
                printf("serialtcpd: %s line %d: when-busy must be busy, queue or ask\n",
                       path, lineno);
        }
        else if (!stricmp(key, "queue-max"))
        {
            int q = atoi(val);
            if (q < 1)   q = 1;
            if (q > 64)  q = 64;
            g_Config.c_QueueMax = (UWORD)q;
        }
        else if (!stricmp(key, "queue-timeout"))
        {
            int q = atoi(val);
            if (q < 0) q = 0;
            g_Config.c_QueueTimeout = (ULONG)q;
        }
        else if (!stricmp(key, "queue-notify"))
        {
            int q = atoi(val);
            if (q < 0) q = 0;
            g_Config.c_QueueNotify = (ULONG)q;
        }
        else if (!stricmp(key, "ask-timeout"))
        {
            int q = atoi(val);
            if (q < 5) q = 5;
            g_Config.c_AskTimeout = (ULONG)q;
        }
        else if (!stricmp(key, "queue-message"))
        {
            copy_escaped(g_Config.c_QueueMessage, sizeof(g_Config.c_QueueMessage), val);
        }
        else if (!stricmp(key, "ask-message"))
        {
            copy_escaped(g_Config.c_AskMessage, sizeof(g_Config.c_AskMessage), val);
        }
        else if (!stricmp(key, "queue-position-message"))
        {
            copy_escaped(g_Config.c_QueuePosMessage, sizeof(g_Config.c_QueuePosMessage), val);
        }
        else if (!stricmp(key, "queue-giveup-message"))
        {
            copy_escaped(g_Config.c_QueueGiveUpMessage, sizeof(g_Config.c_QueueGiveUpMessage), val);
        }
        else if (!stricmp(key, "busy-message"))
        {
            copy_escaped(g_Config.c_BusyMessage, sizeof(g_Config.c_BusyMessage), val);
            g_Config.c_BusyMessageEnabled = (g_Config.c_BusyMessage[0] != '\0');
        }
        else if (!stricmp(key, "busy-message-enabled"))
        {
            g_Config.c_BusyMessageEnabled = parse_bool(val);
        }
        else if (!stricmp(key, "log"))
        {
            strncpy(g_Config.c_LogFile, val, sizeof(g_Config.c_LogFile) - 1);
            g_Config.c_LogToFile = (g_Config.c_LogFile[0] != '\0');
        }
        else
        {
            printf("serialtcpd: %s line %d: unknown directive '%s' (ignored)\n",
                   path, lineno, key);
        }
    }

    Close(fh);
    return TRUE;
}
