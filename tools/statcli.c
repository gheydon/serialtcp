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
 * SerialTCPStatus -- shell status and control for SerialTCPd.
 *
 * The same information the MUI client shows, without needing MUI or a screen.
 * Useful over a serial console, from a script, or from the BBS's own shell.
 *
 * Exit codes are meaningful so this can be used in scripts:
 *   0   daemon is running (or the requested action succeeded)
 *   5   daemon is not running
 *   10  the action failed
 *   20  bad arguments
 */

#include <exec/types.h>
#include <dos/dos.h>

#include <proto/exec.h>
#include <proto/dos.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "statcommon.h"

static const char *verstag = "$VER: SerialTCPStatus 1.0 (11.9.2026)";

#define RC_OK        0
#define RC_NOTRUNNING 5
#define RC_FAILED    10
#define RC_BADARGS   20

static void usage(void)
{
    printf("SerialTCPStatus -- status and control for SerialTCPd\n\n");
    printf("Usage: SerialTCPStatus [command] [options]\n\n");
    printf("Commands:\n");
    printf("  (none)         show the current status\n");
    printf("  start          launch the daemon\n");
    printf("  stop [force]   ask it to exit; 'force' cuts off callers in progress\n");
    printf("  restart        stop then start\n\n");
    printf("Options:\n");
    printf("  -b             one brief line, for scripts or a title bar\n");
    printf("  -w [seconds]   watch: refresh until Ctrl-C (default every 2s)\n");
    printf("  -d <command>   command used to start the daemon (default '%s')\n", DAEMON_COMMAND);
    printf("  -q             no output; the exit code says whether it is running\n\n");
    printf("Exit codes: 0 running/ok, 5 not running, 10 failed, 20 bad arguments\n");
}

/* Ctrl-C without pulling in the whole signal machinery. */
static BOOL breaking(void)
{
    return (SetSignal(0, 0) & SIGBREAKF_CTRL_C) ? TRUE : FALSE;
}

static int do_watch(int seconds, BOOL brief)
{
    if (seconds < 1)
        seconds = 2;

    printf("Watching every %d seconds -- press Ctrl-C to stop.\n", seconds);

    for (;;)
    {
        /* Home the cursor and clear, so it reads as a live display rather
         * than a scrolling wall of text. */
        printf("\033[0;0H\033[J");
        fflush(stdout);

        print_status(brief);
        fflush(stdout);

        if (breaking())
            break;

        Delay((LONG)seconds * 50);      /* 50 ticks per second */

        if (breaking())
            break;
    }

    printf("\n");
    return RC_OK;
}

static int do_stop(BOOL force)
{
    UWORD active = 0;
    LONG  rc;

    if (!daemon_present())
    {
        printf("SerialTCPd is not running.\n");
        return RC_NOTRUNNING;
    }

    rc = stop_daemon(force, &active);

    if (rc == -1)
        return RC_OK;                   /* it went away on its own */

    if (rc == STE_NODES_ACTIVE)
    {
        printf("Refused: %u node(s) still have callers connected.\n", (unsigned)active);
        printf("Use 'SerialTCPStatus stop force' to disconnect them anyway.\n");
        return RC_FAILED;
    }

    if (rc != STE_OK)
    {
        printf("The daemon refused to stop (error %ld).\n", (long)rc);
        return RC_FAILED;
    }

    if (!wait_for_stop(50))
    {
        printf("The daemon did not shut down within 10 seconds.\n");
        return RC_FAILED;
    }

    printf("Stopped.\n");
    return RC_OK;
}

static int do_start(void)
{
    if (daemon_present())
    {
        printf("SerialTCPd is already running.\n");
        return RC_OK;
    }

    if (!start_daemon())
    {
        printf("Could not start '%s'.\n", g_DaemonCmd);
        printf("Check that it is in your path and that your TCP/IP stack is running.\n");
        return RC_FAILED;
    }

    printf("Started.\n");
    return RC_OK;
}

int main(int argc, char **argv)
{
    BOOL brief = FALSE, quiet = FALSE, watch = FALSE, force = FALSE;
    int  watchSecs = 2;
    int  i;
    const char *command = NULL;

    (void)verstag;

    for (i = 1; i < argc; i++)
    {
        const char *a = argv[i];

        if (!strcmp(a, "?") || !stricmp(a, "help") || !strcmp(a, "-h") || !strcmp(a, "--help"))
        {
            usage();
            return RC_OK;
        }
        else if (!strcmp(a, "-b") || !stricmp(a, "brief"))
        {
            brief = TRUE;
        }
        else if (!strcmp(a, "-q") || !stricmp(a, "quiet"))
        {
            quiet = TRUE;
        }
        else if (!strcmp(a, "-w") || !stricmp(a, "watch"))
        {
            watch = TRUE;
            /* An optional number may follow. */
            if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
                watchSecs = atoi(argv[++i]);
        }
        else if (!strcmp(a, "-d") && i + 1 < argc)
        {
            g_DaemonCmd = argv[++i];
        }
        else if (!stricmp(a, "force"))
        {
            force = TRUE;
        }
        else if (!stricmp(a, "start") || !stricmp(a, "stop") || !stricmp(a, "restart"))
        {
            command = a;
        }
        else
        {
            printf("Unknown argument '%s'\n\n", a);
            usage();
            return RC_BADARGS;
        }
    }

    if (quiet)
        return daemon_present() ? RC_OK : RC_NOTRUNNING;

    if (command)
    {
        if (!stricmp(command, "start"))
            return do_start();

        if (!stricmp(command, "stop"))
            return do_stop(force);

        /* restart */
        {
            int rc = do_stop(force);
            if (rc != RC_OK && rc != RC_NOTRUNNING)
                return rc;
            return do_start();
        }
    }

    if (watch)
        return do_watch(watchSecs, brief);

    return print_status(brief);
}
