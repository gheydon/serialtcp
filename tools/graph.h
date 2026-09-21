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
 * graph.h -- scrolling history graph custom class for SerialTCPStat.
 */

#ifndef SERIALTCP_GRAPH_H
#define SERIALTCP_GRAPH_H

#include <exec/types.h>
#include <libraries/mui.h>

/* Two minutes of history at the client's one-second refresh. */
#define GRAPH_SAMPLES 120

/* Spacing of the vertical grid lines, in samples -- so one every 30 seconds. */
#define GRAPH_GRID_SAMPLES 30

/* How many traces one graph can hold. Each keeps its own history and its own
 * scale, so a count of nodes and a byte rate can share a panel without one
 * flattening the other. */
#define GRAPH_SERIES  3

#define MUIM_Graph_Push   (TAG_USER | 0x10000001)
#define MUIM_Graph_Clear  (TAG_USER | 0x10000002)

struct MUIP_Graph_Push
{
    ULONG        MethodID;
    ULONG        series;    /* 0 .. GRAPH_SERIES-1                          */
    ULONG        value;
    ULONG        max;       /* full scale; 0 scales to this series' own peak */
    CONST_STRPTR name;      /* legend text, or NULL. Kept, not copied.      */
};

/*
 * Pushing series 0 is what triggers the redraw, so push it last: a graph with
 * three traces would otherwise repaint three times a second for no reason.
 */

struct MUI_CustomClass *graph_create_class(void);
BOOL                    graph_delete_class(struct MUI_CustomClass *mcc);

#endif
