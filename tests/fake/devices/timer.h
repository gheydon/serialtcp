#ifndef FAKE_DEV_TIMER_H
#define FAKE_DEV_TIMER_H
#include <exec/types.h>

/*
 * The host has its own struct timeval with different member names.
 *
 * Pull it in unconditionally FIRST, then map the Amiga names onto its fields.
 * Doing it conditionally was a trap: a translation unit that had already
 * included <stdio.h> got the host struct while one that had not defined its
 * own, so the two disagreed about the layout and comparisons read garbage.
 * Always using the host definition keeps every unit in step.
 */
#include <sys/time.h>

#define tv_secs  tv_sec
#define tv_micro tv_usec

#define UNIT_MICROHZ 0
#define UNIT_VBLANK  1
#endif
