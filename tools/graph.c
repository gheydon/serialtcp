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
 * graph.c -- a small scrolling history graph as a MUI custom class.
 *
 * One instance shows up to GRAPH_SERIES traces over time, Task Manager style:
 * newest sample at the right, older ones scrolling off to the left, drawn over
 * a grid.  SerialTCPStat uses two of them: nodes in use with the byte rates in
 * and out beside it, and queue depth on its own.
 *
 * Each trace keeps its own history and its own scale.  That is what lets a
 * node count that never exceeds four share a panel with a byte rate in the
 * thousands: scaled together the node line would be pinned flat to the floor.
 *
 * This is the one place in the project that needs a register-argument
 * callback, because that is how MUI dispatches to a custom class.  The
 * parameter-in-register syntax is the same one the device driver uses for its
 * library vectors, so it is a pattern already known to work with this
 * toolchain.
 *
 * Samples are stored as a plain ring of bytes with a separate scale, rather
 * than pre-scaled pixel heights, so the graph stays correct when the window is
 * resized or when the node count changes underneath it.
 */

#include <exec/types.h>
#include <graphics/gfxmacros.h>
#include <intuition/intuition.h>
#include <libraries/mui.h>

#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/utility.h>
#include <proto/muimaster.h>
#include <clib/alib_protos.h>

#include <string.h>

#include "graph.h"

struct GraphSeries
{
    ULONG        gs_Hist[GRAPH_SAMPLES];
    UWORD        gs_Pos;      /* where the next sample goes                 */
    UWORD        gs_Count;    /* how many are valid, up to GRAPH_SAMPLES    */
    ULONG        gs_Max;      /* full scale; 0 means scale to its own peak  */
    CONST_STRPTR gs_Name;     /* legend text, or NULL                       */
};

struct GraphData
{
    struct GraphSeries gd_S[GRAPH_SERIES];
};

/*
 * One pen per trace, all three of them dark on a light background: MPEN_SHINE
 * and MPEN_MARK are not used because a scheme is free to make either of them
 * white, and a white trace on the standard grey is barely there.
 *
 * Colour alone is not enough to tell three lines apart on a 4-colour screen,
 * so the third is dashed as well.  The legend draws its key with the same pen
 * and pattern, so the two always agree.
 */
static const UWORD series_pen[GRAPH_SERIES] = { MPEN_FILL, MPEN_TEXT, MPEN_SHADOW };
static const UWORD series_pat[GRAPH_SERIES] = { 0xFFFF,    0xFFFF,    0xCCCC     };

/* ------------------------------------------------------------------ */

static ULONG mNew(struct IClass *cl, Object *obj, struct opSet *msg)
{
    struct GraphData *d;

    obj = (Object *)DoSuperMethodA(cl, obj, (Msg)msg);
    if (!obj)
        return 0;

    d = INST_DATA(cl, obj);
    memset(d, 0, sizeof(*d));

    return (ULONG)obj;
}

static ULONG mAskMinMax(struct IClass *cl, Object *obj, struct MUIP_AskMinMax *msg)
{
    DoSuperMethodA(cl, obj, (Msg)msg);

    /*
     * Ask for the full sample count as the default width so the graph starts
     * out showing its whole history, but stay shrinkable: on a 640-pixel
     * Workbench the window has to fit.
     */
    msg->MinMaxInfo->MinWidth  += 80;
    msg->MinMaxInfo->DefWidth  += GRAPH_SAMPLES;
    msg->MinMaxInfo->MaxWidth   = MUI_MAXMAX;

    msg->MinMaxInfo->MinHeight += 24;
    msg->MinMaxInfo->DefHeight += 44;
    msg->MinMaxInfo->MaxHeight  = MUI_MAXMAX;

    return 0;
}

/* Value of the sample `age` steps back from the newest, or -1 if not held. */
static LONG sample_at(struct GraphSeries *s, UWORD age)
{
    LONG idx;

    if (age >= s->gs_Count)
        return -1;

    idx = (LONG)s->gs_Pos - 1 - (LONG)age;
    while (idx < 0)
        idx += GRAPH_SAMPLES;

    return (LONG)s->gs_Hist[idx];
}

/*
 * Full scale for a trace: what the caller asked for, or the tallest sample it
 * holds when the caller had no idea -- a byte rate has no natural ceiling, so
 * it has to find its own.
 */
static ULONG series_scale(struct GraphSeries *s)
{
    ULONG peak = 0;
    UWORD i;

    if (s->gs_Max)
        return s->gs_Max;

    for (i = 0; i < s->gs_Count; i++)
        if (s->gs_Hist[i] > peak)
            peak = s->gs_Hist[i];

    return peak ? peak : 1;
}

/* One trace, newest sample at the right. */
static void draw_series(struct RastPort *rp, struct GraphSeries *s,
                        LONG l, LONG t, LONG r, LONG b)
{
    ULONG scale = series_scale(s);
    LONG  prevx = -1, prevy = 0;
    LONG  x;

    for (x = r; x >= l; x--)
    {
        UWORD age = (UWORD)(r - x);
        LONG  v   = sample_at(s, age);
        LONG  y;

        if (v < 0)
            break;

        if ((ULONG)v > scale)
            v = (LONG)scale;

        y = b - (LONG)(((b - t) * (LONG)v) / (LONG)scale);
        if (y < t)
            y = t;

        /* Join to the previous sample so the trace is continuous, including
         * across a run of equal values and down to zero. */
        if (prevx >= 0)
        {
            Move(rp, prevx, prevy);
            Draw(rp, x, y);
        }
        else
        {
            WritePixel(rp, x, y);
        }

        prevx = x;
        prevy = y;
    }
}

/*
 * A key along the top: a dash in each trace's own pen followed by its name.
 * Without it three lines on one panel are a guessing game.  Skipped when the
 * panel is too short or too narrow to take it, which keeps the graph usable
 * when the window is dragged small.
 */
static void draw_legend(struct RastPort *rp, struct GraphData *d,
                        const UWORD *pens, LONG l, LONG t, LONG r)
{
#define LEG_DASH 10
#define LEG_GAP   3
#define LEG_SEP   8
    LONG  width = 0;
    LONG  x, y;
    UWORD i, named = 0;

    for (i = 0; i < GRAPH_SERIES; i++)
    {
        if (!d->gd_S[i].gs_Name || !d->gd_S[i].gs_Count)
            continue;
        width += LEG_DASH + LEG_GAP
               + TextLength(rp, (CONST_STRPTR)d->gd_S[i].gs_Name,
                            (ULONG)strlen(d->gd_S[i].gs_Name)) + LEG_SEP;
        named++;
    }

    if (named < 2 || width <= 0 || width > (r - l) - 4)
        return;

    x = r - 2 - width + LEG_SEP;
    y = t + 1;

    /* Clear the strip first: a trace running along the top would otherwise
     * read straight through the key. */
    SetAPen(rp, pens[MPEN_BACKGROUND]);
    RectFill(rp, x - LEG_GAP, y, r - 1, y + rp->TxHeight);

    for (i = 0; i < GRAPH_SERIES; i++)
    {
        struct GraphSeries *s = &d->gd_S[i];
        LONG                mid = y + rp->TxHeight / 2;
        ULONG               len;

        if (!s->gs_Name || !s->gs_Count)
            continue;

        len = (ULONG)strlen(s->gs_Name);

        SetAPen(rp, pens[series_pen[i]]);
        SetDrPt(rp, series_pat[i]);
        Move(rp, x, mid);
        Draw(rp, x + LEG_DASH - 1, mid);
        SetDrPt(rp, 0xFFFF);
        x += LEG_DASH + LEG_GAP;

        SetAPen(rp, pens[MPEN_TEXT]);
        Move(rp, x, y + rp->TxBaseline);
        Text(rp, (CONST_STRPTR)s->gs_Name, len);
        x += TextLength(rp, (CONST_STRPTR)s->gs_Name, len) + LEG_SEP;
    }
}

static ULONG mDraw(struct IClass *cl, Object *obj, struct MUIP_Draw *msg)
{
    struct GraphData *d  = INST_DATA(cl, obj);
    struct RastPort  *rp = _rp(obj);
    const UWORD      *pens;
    LONG              l, t, w, h, r, b;
    LONG              x, i;

    DoSuperMethodA(cl, obj, (Msg)msg);

    if (!(msg->flags & (MADF_DRAWOBJECT | MADF_DRAWUPDATE)))
        return 0;

    l = _mleft(obj);
    t = _mtop(obj);
    w = _mwidth(obj);
    h = _mheight(obj);

    if (w <= 2 || h <= 2)
        return 0;

    r = l + w - 1;
    b = t + h - 1;

    pens = _pens(obj);

    /* Ground. */
    SetAPen(rp, pens[MPEN_BACKGROUND]);
    RectFill(rp, l, t, r, b);

    /* Grid: quarters horizontally, every 20 samples vertically. Quiet enough
     * to read the trace against, which is the whole point of having it. */
    SetAPen(rp, pens[MPEN_HALFSHADOW]);
    for (i = 1; i < 4; i++)
    {
        LONG y = t + (h * i) / 4;
        Move(rp, l, y);
        Draw(rp, r, y);
    }
    for (x = r - 20; x > l; x -= 20)
    {
        Move(rp, x, t);
        Draw(rp, x, b);
    }

    /*
     * The traces: lines only, no fill beneath them, so where two cross both
     * stay readable and the grid shows through.  Drawn back to front so
     * series 0 -- the one the panel is named after -- ends up on top.
     */
    for (i = GRAPH_SERIES - 1; i >= 0; i--)
    {
        if (!d->gd_S[i].gs_Count)
            continue;
        SetAPen(rp, pens[series_pen[i]]);
        SetDrPt(rp, series_pat[i]);
        draw_series(rp, &d->gd_S[i], l, t, r, b);
    }
    SetDrPt(rp, 0xFFFF);

    if (h >= 30)
        draw_legend(rp, d, pens, l, t, r);

    /* Frame it, so it reads as a panel rather than a hole in the window. */
    SetAPen(rp, pens[MPEN_SHADOW]);
    Move(rp, l, b);
    Draw(rp, l, t);
    Draw(rp, r, t);

    return 0;
}

static ULONG mPush(struct IClass *cl, Object *obj, struct MUIP_Graph_Push *msg)
{
    struct GraphData   *d = INST_DATA(cl, obj);
    struct GraphSeries *s;

    if (msg->series >= GRAPH_SERIES)
        return 0;

    s = &d->gd_S[msg->series];

    /*
     * The scale can move: nodes can be reconfigured and the queue size is not
     * known until the daemon answers. Storing raw values and rescaling at draw
     * time means old samples stay meaningful when it does.
     */
    s->gs_Max = msg->max;
    if (msg->name)
        s->gs_Name = msg->name;

    s->gs_Hist[s->gs_Pos] = msg->value;
    s->gs_Pos = (UWORD)((s->gs_Pos + 1) % GRAPH_SAMPLES);

    if (s->gs_Count < GRAPH_SAMPLES)
        s->gs_Count++;

    /* Only series 0 repaints, so a caller feeding three traces gets one
     * redraw a tick rather than three.  See graph.h. */
    if (msg->series == 0)
        MUI_Redraw(obj, MADF_DRAWOBJECT);

    return 0;
}

static ULONG mClear(struct IClass *cl, Object *obj, Msg msg)
{
    struct GraphData *d = INST_DATA(cl, obj);
    UWORD             i;

    for (i = 0; i < GRAPH_SERIES; i++)
    {
        d->gd_S[i].gs_Pos   = 0;
        d->gd_S[i].gs_Count = 0;
        memset(d->gd_S[i].gs_Hist, 0, sizeof(d->gd_S[i].gs_Hist));
    }

    MUI_Redraw(obj, MADF_DRAWOBJECT);
    return 0;
}

/* ------------------------------------------------------------------ */

/*
 * MUI hands the dispatcher its arguments in registers: class in A0, object in
 * A2, message in A1.
 */
static ULONG dispatcher(struct IClass *cl asm("a0"), Object *obj asm("a2"), Msg msg asm("a1"))
{
    switch (msg->MethodID)
    {
    case OM_NEW:            return mNew(cl, obj, (struct opSet *)msg);
    case MUIM_AskMinMax:    return mAskMinMax(cl, obj, (struct MUIP_AskMinMax *)msg);
    case MUIM_Draw:         return mDraw(cl, obj, (struct MUIP_Draw *)msg);
    case MUIM_Graph_Push:   return mPush(cl, obj, (struct MUIP_Graph_Push *)msg);
    case MUIM_Graph_Clear:  return mClear(cl, obj, msg);
    }

    return DoSuperMethodA(cl, obj, msg);
}

struct MUI_CustomClass *graph_create_class(void)
{
    return MUI_CreateCustomClass(NULL, (CONST_STRPTR)MUIC_Area, NULL,
                                 sizeof(struct GraphData), (APTR)dispatcher);
}

BOOL graph_delete_class(struct MUI_CustomClass *mcc)
{
    if (!mcc)
        return TRUE;

    return MUI_DeleteCustomClass(mcc) ? TRUE : FALSE;
}
