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
 * One instance shows one series over time, Task Manager style: newest sample
 * at the right, older ones scrolling off to the left, drawn as a filled area
 * over a grid.  SerialTCPStat uses two of them, for nodes in use and for queue
 * depth.
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

struct GraphData
{
    UBYTE  gd_Hist[GRAPH_SAMPLES];
    UWORD  gd_Pos;        /* where the next sample goes                     */
    UWORD  gd_Count;      /* how many are valid, up to GRAPH_SAMPLES        */
    UWORD  gd_Max;        /* full-scale value; never zero                   */
};

/* ------------------------------------------------------------------ */

static ULONG mNew(struct IClass *cl, Object *obj, struct opSet *msg)
{
    struct GraphData *d;

    obj = (Object *)DoSuperMethodA(cl, obj, (Msg)msg);
    if (!obj)
        return 0;

    d = INST_DATA(cl, obj);
    memset(d, 0, sizeof(*d));
    d->gd_Max = 1;

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
static LONG sample_at(struct GraphData *d, UWORD age)
{
    LONG idx;

    if (age >= d->gd_Count)
        return -1;

    idx = (LONG)d->gd_Pos - 1 - (LONG)age;
    while (idx < 0)
        idx += GRAPH_SAMPLES;

    return (LONG)d->gd_Hist[idx];
}

static ULONG mDraw(struct IClass *cl, Object *obj, struct MUIP_Draw *msg)
{
    struct GraphData *d  = INST_DATA(cl, obj);
    struct RastPort  *rp = _rp(obj);
    const UWORD      *pens;
    LONG              l, t, w, h, r, b;
    LONG              x, i;
    LONG              prevx = -1, prevy = 0;

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
     * The trace, newest sample at the right. A line only -- no fill beneath
     * it, so overlapping detail stays readable and the grid shows through.
     *
     * MPEN_FILL rather than MPEN_SHINE: on the standard grey MUI background a
     * white line nearly disappears, whereas the fill pen is a strong colour
     * in every scheme.
     */
    SetAPen(rp, pens[MPEN_FILL]);
    for (x = r; x >= l; x--)
    {
        UWORD age = (UWORD)(r - x);
        LONG  v   = sample_at(d, age);
        LONG  y;

        if (v < 0)
            break;

        if ((UWORD)v > d->gd_Max)
            v = d->gd_Max;

        y = b - ((b - t) * v) / (LONG)d->gd_Max;
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

    /* Frame it, so it reads as a panel rather than a hole in the window. */
    SetAPen(rp, pens[MPEN_SHADOW]);
    Move(rp, l, b);
    Draw(rp, l, t);
    Draw(rp, r, t);

    return 0;
}

static ULONG mPush(struct IClass *cl, Object *obj, struct MUIP_Graph_Push *msg)
{
    struct GraphData *d = INST_DATA(cl, obj);

    /*
     * The scale can move: nodes can be reconfigured and the queue size is not
     * known until the daemon answers. Storing raw values and rescaling at draw
     * time means old samples stay meaningful when it does.
     */
    d->gd_Max = (UWORD)(msg->max ? msg->max : 1);

    d->gd_Hist[d->gd_Pos] = (UBYTE)(msg->value > 255 ? 255 : msg->value);
    d->gd_Pos = (UWORD)((d->gd_Pos + 1) % GRAPH_SAMPLES);

    if (d->gd_Count < GRAPH_SAMPLES)
        d->gd_Count++;

    MUI_Redraw(obj, MADF_DRAWOBJECT);
    return 0;
}

static ULONG mClear(struct IClass *cl, Object *obj, Msg msg)
{
    struct GraphData *d = INST_DATA(cl, obj);

    d->gd_Pos   = 0;
    d->gd_Count = 0;
    memset(d->gd_Hist, 0, sizeof(d->gd_Hist));

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

void graph_delete_class(struct MUI_CustomClass *mcc)
{
    if (mcc)
        MUI_DeleteCustomClass(mcc);
}
