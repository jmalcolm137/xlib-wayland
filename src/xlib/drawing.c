/* drawing.c — graphics contexts and the Xlib drawing primitives. */
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------- helpers */

MwCanvas *mw_canvas_for_drawable(Display *d, Drawable dr, struct _XGC *gc,
                                 int *w, int *h)
{
    /* Core drawing must observe anything Render has drawn to the same drawable
     * (Render requests are buffered and dispatched lazily). */
    mw_render_drain(d);
    int sw = 0, sh = 0, depth = 0;
    MwSurface *surf = mw_drawable_surface(d, dr, &sw, &sh, &depth);
    if (w) *w = sw;
    if (h) *h = sh;
    if (!surf) return NULL;
    /* An empty clip-rectangle list disables output altogether. */
    static const XRectangle nothing = { 0, 0, 0, 0 };
    const XRectangle *clip = NULL;
    int nclip = 0;
    if (gc && gc->has_clip_rects) {
        clip = gc->nclip > 0 ? gc->clip_rects : &nothing;
        nclip = gc->nclip > 0 ? gc->nclip : 1;
    }
    MwCanvas *c = mw_canvas_begin(surf, clip, nclip,
                                  gc ? gc->clip_x_origin : 0,
                                  gc ? gc->clip_y_origin : 0);
    if (gc) mw_apply_gc(c, d, gc);
    return c;
}

void mw_apply_gc(MwCanvas *c, Display *d, struct _XGC *gc)
{
    (void)d;
    mw_set_operator(c, gc->function);
    mw_set_source_argb(c, 0xff000000u | (uint32_t)(gc->foreground & 0xffffff));
    mw_set_line(c, gc->line_width, gc->line_style, gc->cap_style, gc->join_style,
                gc->dash_offset,
                gc->dash_list ? gc->dash_list : &gc->dashes,
                gc->dash_list ? gc->ndash : 1);
    mw_set_fill(c, gc->fill_style, gc->fill_rule, gc->arc_mode);
    mw_set_plane_mask(c, gc->plane_mask);
    if (gc->xfont && gc->xfont->rfont)
        mw_set_font(c, gc->xfont->rfont);
    /* A depth-1 clip mask (XSetClipMask) clips all drawing to its set bits. */
    if (gc->clip_mask != None) {
        MwPixmap *pm = mw_pixmap(d, gc->clip_mask);
        if (pm) mw_clip_mask(c, pm->surface, gc->clip_x_origin, gc->clip_y_origin);
    }
}

/* Fill the current path with the right fill style. */
static void fill_path(MwCanvas *c, struct _XGC *gc, Display *d)
{
    Pixmap src = None;
    if (gc->fill_style == FillStippled || gc->fill_style == FillOpaqueStippled)
        src = gc->stipple;
    else if (gc->fill_style == FillTiled)
        src = gc->tile;
    MwPixmap *pm = src ? mw_pixmap(d, src) : NULL;
    if (pm) {
        mw_fill_pattern(c, pm->surface, gc->ts_x_origin, gc->ts_y_origin,
                        gc->fill_style, (uint32_t)gc->foreground,
                        (uint32_t)gc->background);
        return;
    }
    mw_fill_path(c);
}

/* GCClipMask replaces any clip-rectangle list (and vice versa). */
static void drop_clip_rects(struct _XGC *gc)
{
    free(gc->clip_rects);
    gc->clip_rects = NULL;
    gc->nclip = 0;
    gc->has_clip_rects = false;
}

/* ---------------------------------------------------------------- GCs */

struct _XGC *mw_create_gc_internal(Display *d, Drawable dr, unsigned long mask,
                                   XGCValues *v)
{
    (void)dr;
    struct _XGC *gc = calloc(1, sizeof *gc);
    if (!gc) return NULL;
    gc->gid = mw_alloc_id(d);
    gc->function = GXcopy;
    gc->plane_mask = AllPlanes;
    gc->foreground = MWSCR(d)->black_pixel;
    gc->background = MWSCR(d)->white_pixel;
    gc->line_width = 0;
    gc->line_style = LineSolid;
    gc->cap_style = CapButt;
    gc->join_style = JoinMiter;
    gc->fill_style = FillSolid;
    gc->fill_rule = EvenOddRule;
    gc->arc_mode = ArcPieSlice;
    gc->tile = None;
    gc->stipple = None;
    gc->font = None;
    gc->subwindow_mode = ClipByChildren;
    gc->graphics_exposures = True;
    gc->clip_mask = None;
    gc->dashes = 4;

    if (v) {
        if (mask & GCFunction)          gc->function = v->function;
        if (mask & GCPlaneMask)         gc->plane_mask = v->plane_mask;
        if (mask & GCForeground)        gc->foreground = v->foreground;
        if (mask & GCBackground)        gc->background = v->background;
        if (mask & GCLineWidth)         gc->line_width = v->line_width;
        if (mask & GCLineStyle)         gc->line_style = v->line_style;
        if (mask & GCCapStyle)          gc->cap_style = v->cap_style;
        if (mask & GCJoinStyle)         gc->join_style = v->join_style;
        if (mask & GCFillStyle)         gc->fill_style = v->fill_style;
        if (mask & GCFillRule)          gc->fill_rule = v->fill_rule;
        if (mask & GCArcMode)           gc->arc_mode = v->arc_mode;
        if (mask & GCTile)              gc->tile = v->tile;
        if (mask & GCStipple)           gc->stipple = v->stipple;
        if (mask & GCTileStipXOrigin)   gc->ts_x_origin = v->ts_x_origin;
        if (mask & GCTileStipYOrigin)   gc->ts_y_origin = v->ts_y_origin;
        if (mask & GCSubwindowMode)     gc->subwindow_mode = v->subwindow_mode;
        if (mask & GCGraphicsExposures) gc->graphics_exposures = v->graphics_exposures;
        if (mask & GCClipXOrigin)       gc->clip_x_origin = v->clip_x_origin;
        if (mask & GCClipYOrigin)       gc->clip_y_origin = v->clip_y_origin;
        if (mask & GCClipMask)          gc->clip_mask = v->clip_mask;
        if (mask & GCDashOffset)        gc->dash_offset = v->dash_offset;
        if (mask & GCDashList)          gc->dashes = v->dashes;
    }
    MwXFont *xf = (v && (mask & GCFont)) ? mw_font(d, v->font) : NULL;
    mw_gc_set_xfont(d, gc, xf ? xf : mw_default_font(d));
    mw_register(d, gc->gid, MW_OBJ_GC, gc);
    return gc;
}

GC XCreateGC(Display *d, Drawable dr, unsigned long mask, XGCValues *values)
{
    return mw_create_gc_internal(d, dr, mask, values);
}

int XChangeGC(Display *d, GC gc, unsigned long mask, XGCValues *v)
{
    if (!gc || !v) return 0;
    if (mask & GCFunction)          gc->function = v->function;
    if (mask & GCPlaneMask)         gc->plane_mask = v->plane_mask;
    if (mask & GCForeground)        gc->foreground = v->foreground;
    if (mask & GCBackground)        gc->background = v->background;
    if (mask & GCLineWidth)         gc->line_width = v->line_width;
    if (mask & GCLineStyle)         gc->line_style = v->line_style;
    if (mask & GCCapStyle)          gc->cap_style = v->cap_style;
    if (mask & GCJoinStyle)         gc->join_style = v->join_style;
    if (mask & GCFillStyle)         gc->fill_style = v->fill_style;
    if (mask & GCFillRule)          gc->fill_rule = v->fill_rule;
    if (mask & GCArcMode)           gc->arc_mode = v->arc_mode;
    if (mask & GCTile)              gc->tile = v->tile;
    if (mask & GCStipple)           gc->stipple = v->stipple;
    if (mask & GCTileStipXOrigin)   gc->ts_x_origin = v->ts_x_origin;
    if (mask & GCTileStipYOrigin)   gc->ts_y_origin = v->ts_y_origin;
    if (mask & GCFont)              mw_gc_set_xfont(d, gc, mw_font(d, v->font));
    if (mask & GCSubwindowMode)     gc->subwindow_mode = v->subwindow_mode;
    if (mask & GCGraphicsExposures) gc->graphics_exposures = v->graphics_exposures;
    if (mask & GCClipXOrigin)       gc->clip_x_origin = v->clip_x_origin;
    if (mask & GCClipYOrigin)       gc->clip_y_origin = v->clip_y_origin;
    if (mask & GCClipMask)          { gc->clip_mask = v->clip_mask; drop_clip_rects(gc); }
    if (mask & GCDashOffset)        gc->dash_offset = v->dash_offset;
    if (mask & GCDashList)          gc->dashes = v->dashes;
    return 1;
}

/* Copy the GC components named by `mask` from `src` to `dest`.  The clip mask /
 * resolved clip rectangles must be duplicated rather than aliased, because the
 * destination owns them and frees them in XFreeGC.  Motif's own mwm client
 * calls this, so without it the rest of the Open Motif tree fails to link. */
int XCopyGC(Display *d, GC src, unsigned long mask, GC dest)
{
    if (!src || !dest || src == dest) return 0;

#define CP(field, bit) if (mask & (bit)) dest->field = src->field
    CP(function, GCFunction); CP(plane_mask, GCPlaneMask);
    CP(foreground, GCForeground); CP(background, GCBackground);
    CP(line_width, GCLineWidth); CP(line_style, GCLineStyle);
    CP(cap_style, GCCapStyle); CP(join_style, GCJoinStyle);
    CP(fill_style, GCFillStyle); CP(fill_rule, GCFillRule);
    CP(arc_mode, GCArcMode);
    CP(tile, GCTile); CP(stipple, GCStipple);
    CP(ts_x_origin, GCTileStipXOrigin); CP(ts_y_origin, GCTileStipYOrigin);
    CP(subwindow_mode, GCSubwindowMode); CP(graphics_exposures, GCGraphicsExposures);
    CP(clip_x_origin, GCClipXOrigin); CP(clip_y_origin, GCClipYOrigin);
    CP(dash_offset, GCDashOffset); CP(dashes, GCDashList);
#undef CP
    if (mask & GCDashList) {
        free(dest->dash_list);
        dest->dash_list = NULL;
        dest->ndash = 0;
        if (src->dash_list && src->ndash > 0) {
            dest->dash_list = malloc((size_t)src->ndash);
            if (dest->dash_list) {
                memcpy(dest->dash_list, src->dash_list, (size_t)src->ndash);
                dest->ndash = src->ndash;
            }
        }
    }
    if (mask & GCFont) mw_gc_set_xfont(d, dest, src->xfont);

    if (mask & GCClipMask) {
        dest->clip_mask = src->clip_mask;
        /* The resolved clip rectangles are derived state we own, so they must
         * be duplicated rather than aliased (XFreeGC frees them).  The shim's
         * Region type has no XCopyRegion equivalent, so only the rectangle
         * form is reproduced -- that is what the drawing code consults. */
        free(dest->clip_rects);
        dest->clip_rects = NULL;
        dest->nclip = 0;
        /* An empty list is a clip too (it disables output). */
        dest->has_clip_rects = src->has_clip_rects;
        if (src->clip_rects && src->nclip > 0) {
            dest->clip_rects = malloc((size_t)src->nclip * sizeof *dest->clip_rects);
            if (dest->clip_rects) {
                memcpy(dest->clip_rects, src->clip_rects,
                       (size_t)src->nclip * sizeof *dest->clip_rects);
                dest->nclip = src->nclip;
                dest->has_clip_rects = true;
            }
        }
    }
    return 1;
}

Status XGetGCValues(Display *d, GC gc, unsigned long mask, XGCValues *v)
{
    (void)d;
    if (!gc || !v) return 0;
    memset(v, 0, sizeof *v);
    if (mask & GCFunction)          v->function = gc->function;
    if (mask & GCPlaneMask)         v->plane_mask = gc->plane_mask;
    if (mask & GCForeground)        v->foreground = gc->foreground;
    if (mask & GCBackground)        v->background = gc->background;
    if (mask & GCLineWidth)         v->line_width = gc->line_width;
    if (mask & GCLineStyle)         v->line_style = gc->line_style;
    if (mask & GCCapStyle)          v->cap_style = gc->cap_style;
    if (mask & GCJoinStyle)         v->join_style = gc->join_style;
    if (mask & GCFillStyle)         v->fill_style = gc->fill_style;
    if (mask & GCFillRule)          v->fill_rule = gc->fill_rule;
    if (mask & GCArcMode)           v->arc_mode = gc->arc_mode;
    if (mask & GCTile)              v->tile = gc->tile;
    if (mask & GCStipple)           v->stipple = gc->stipple;
    if (mask & GCTileStipXOrigin)   v->ts_x_origin = gc->ts_x_origin;
    if (mask & GCTileStipYOrigin)   v->ts_y_origin = gc->ts_y_origin;
    if (mask & GCFont)              v->font = gc->font;
    if (mask & GCClipMask)          v->clip_mask = gc->clip_mask;
    return 1;
}

int XFreeGC(Display *d, GC gc)
{
    if (!gc) return 0;
    free(gc->clip_rects);
    free(gc->dash_list);
    if (gc->clip_region) XDestroyRegion(gc->clip_region);
    mw_gc_set_xfont(d, gc, NULL);
    mw_unregister(d, gc->gid);
    free(gc);
    return 1;
}

int XSetForeground(Display *d, GC gc, unsigned long fg) { (void)d; gc->foreground = fg; return 1; }
int XSetBackground(Display *d, GC gc, unsigned long bg) { (void)d; gc->background = bg; return 1; }
int XSetFunction(Display *d, GC gc, int f) { (void)d; gc->function = f; return 1; }
int XSetPlaneMask(Display *d, GC gc, unsigned long m) { (void)d; gc->plane_mask = m; return 1; }
int XSetState(Display *d, GC gc, unsigned long fg, unsigned long bg, int func,
              unsigned long pm)
{ (void)d; gc->foreground = fg; gc->background = bg; gc->function = func; gc->plane_mask = pm; return 1; }
int XSetLineAttributes(Display *d, GC gc, unsigned int lw, int ls, int cap, int join)
{ (void)d; gc->line_width = (int)lw; gc->line_style = ls; gc->cap_style = cap; gc->join_style = join; return 1; }
int XSetFillStyle(Display *d, GC gc, int fs) { (void)d; gc->fill_style = fs; return 1; }
int XSetFillRule(Display *d, GC gc, int fr) { (void)d; gc->fill_rule = fr; return 1; }
int XSetArcMode(Display *d, GC gc, int am) { (void)d; gc->arc_mode = am; return 1; }
int XSetStipple(Display *d, GC gc, Pixmap p) { (void)d; gc->stipple = p; return 1; }
int XSetTile(Display *d, GC gc, Pixmap p) { (void)d; gc->tile = p; return 1; }
int XSetTSOrigin(Display *d, GC gc, int x, int y) { (void)d; gc->ts_x_origin = x; gc->ts_y_origin = y; return 1; }
int XSetSubwindowMode(Display *d, GC gc, int m) { (void)d; gc->subwindow_mode = m; return 1; }
int XSetGraphicsExposures(Display *d, GC gc, Bool b) { (void)d; gc->graphics_exposures = b; return 1; }
int XSetDashes(Display *d, GC gc, int dash_offset, _Xconst char *dash_list, int n)
{
    (void)d;
    gc->dash_offset = dash_offset;
    gc->dashes = (n > 0 && dash_list) ? dash_list[0] : 0;
    free(gc->dash_list);
    gc->dash_list = NULL;
    gc->ndash = 0;
    if (n > 0 && dash_list) {
        gc->dash_list = malloc((size_t)n);
        if (gc->dash_list) {
            memcpy(gc->dash_list, dash_list, (size_t)n);
            gc->ndash = n;
        }
    }
    return 1;
}
int XSetClipOrigin(Display *d, GC gc, int x, int y) { (void)d; gc->clip_x_origin = x; gc->clip_y_origin = y; return 1; }
int XSetClipMask(Display *d, GC gc, Pixmap p)
{
    (void)d;
    gc->clip_mask = p;
    /* A clip mask (or None, which cairo uses to clear the clip before a
     * drawing op) replaces any rectangle clip set with XSetClipRectangles.
     * Keeping a stale rectangle clip discarded pixels: cairo clips the first
     * tile into a scratch pixmap, clears the clip with XSetClipMask(None),
     * then copies the next tile (GIMP's ruler lost its ticks this way). */
    drop_clip_rects(gc);
    return 1;
}

int XSetClipRectangles(Display *d, GC gc, int clip_x, int clip_y,
                       XRectangle *rects, int n, int ordering)
{
    (void)d; (void)ordering;
    if (n < 0 || !rects) n = 0;
    XRectangle *copy = n ? malloc(sizeof(XRectangle) * (size_t)n) : NULL;
    if (n && !copy) return 0;
    if (n && rects) memcpy(copy, rects, sizeof(XRectangle) * (size_t)n);
    free(gc->clip_rects);
    gc->clip_rects = copy;
    gc->nclip = n;
    /* An empty list is a clip too: it disables output. */
    gc->has_clip_rects = true;
    gc->clip_mask = None;
    gc->clip_x_origin = clip_x;
    gc->clip_y_origin = clip_y;
    return 1;
}

/* As in libX11: the region's rectangles become the clip, with the clip
 * origin reset to (0,0). */
int XSetRegion(Display *d, GC gc, Region r)
{
    if (gc->clip_region) XDestroyRegion(gc->clip_region);
    gc->clip_region = XCreateRegion();
    if (gc->clip_region) XUnionRegion(gc->clip_region, r, gc->clip_region);
    XRectangle *rects = NULL;
    int n = mw_region_rectangles(r, &rects);
    if (n < 0) return 0;
    XSetClipRectangles(d, gc, 0, 0, rects, n, YXBanded);
    free(rects);
    return 1;
}

/* ------------------------------------------------------------ primitives */

static void after_draw(Display *d, Drawable dr)
{
    MwWindow *win = mw_window(d, dr);
    if (win) mw_window_damage(win);
}

static void after_draw_rect(Display *d, Drawable dr, int x, int y, int w, int h)
{
    MwWindow *win = mw_window(d, dr);
    if (win) mw_window_damage_rect(win, x, y, w, h);
}

int XDrawPoint(Display *d, Drawable dr, GC gc, int x, int y)
{
    int w, h;
    MwCanvas *c = mw_canvas_for_drawable(d, dr, gc, &w, &h);
    if (!c) return 0;
    mw_set_fill(c, FillSolid, WindingRule, ArcPieSlice);
    mw_rect(c, x, y, 1, 1);
    mw_fill_path(c);
    mw_canvas_end(c);
    after_draw_rect(d, dr, x, y, 1, 1);
    return 1;
}

int XDrawPoints(Display *d, Drawable dr, GC gc, XPoint *pts, int n, int mode)
{
    int w, h;
    MwCanvas *c = mw_canvas_for_drawable(d, dr, gc, &w, &h);
    if (!c) return 0;
    int px = 0, py = 0;
    for (int i = 0; i < n; i++) {
        if (i == 0 || mode != CoordModePrevious) { px = pts[i].x; py = pts[i].y; }
        else                                     { px += pts[i].x; py += pts[i].y; }
        mw_rect(c, px, py, 1, 1);
    }
    mw_set_fill(c, FillSolid, WindingRule, ArcPieSlice);
    mw_fill_path(c);
    mw_canvas_end(c);
    after_draw(d, dr);
    return 1;
}

int XDrawLine(Display *d, Drawable dr, GC gc, int x1, int y1, int x2, int y2)
{
    int w, h;
    MwCanvas *c = mw_canvas_for_drawable(d, dr, gc, &w, &h);
    if (!c) return 0;
    mw_set_snap(c, (gc->line_width <= 0 || (gc->line_width & 1)) ? 1 : 0);
    // 0.5 offset to match X pixel grid for 1px lines
    mw_polygon(c, (XPoint[]){{ (short)x1, (short)y1 }, { (short)x2, (short)y2 }}, 2, 0, 0);
    mw_stroke_path(c);
    mw_canvas_end(c);
    after_draw(d, dr);
    return 1;
}

int XDrawLines(Display *d, Drawable dr, GC gc, XPoint *pts, int n, int mode)
{
    int w, h;
    MwCanvas *c = mw_canvas_for_drawable(d, dr, gc, &w, &h);
    if (!c) return 0;
    mw_set_snap(c, (gc->line_width <= 0 || (gc->line_width & 1)) ? 1 : 0);
    mw_polygon(c, pts, n, mode == CoordModePrevious ? 1 : 0, 0);
    mw_stroke_path(c);
    mw_canvas_end(c);
    after_draw(d, dr);
    return 1;
}

int XDrawSegments(Display *d, Drawable dr, GC gc, XSegment *segs, int n)
{
    int w, h;
    MwCanvas *c = mw_canvas_for_drawable(d, dr, gc, &w, &h);
    if (!c) return 0;
    mw_set_snap(c, (gc->line_width <= 0 || (gc->line_width & 1)) ? 1 : 0);
    mw_segments(c, segs, n);
    mw_stroke_path(c);
    mw_canvas_end(c);
    after_draw(d, dr);
    return 1;
}

int XDrawRectangle(Display *d, Drawable dr, GC gc, int x, int y,
                   unsigned int w, unsigned int h)
{
    int dw, dh;
    MwCanvas *c = mw_canvas_for_drawable(d, dr, gc, &dw, &dh);
    if (!c) return 0;
    mw_set_snap(c, (gc->line_width <= 0 || (gc->line_width & 1)) ? 1 : 0);
    mw_rect(c, x, y, w, h);
    mw_stroke_path(c);
    mw_canvas_end(c);
    after_draw(d, dr);
    return 1;
}

int XDrawRectangles(Display *d, Drawable dr, GC gc, XRectangle *r, int n)
{
    int w, h;
    MwCanvas *c = mw_canvas_for_drawable(d, dr, gc, &w, &h);
    if (!c) return 0;
    mw_set_snap(c, (gc->line_width <= 0 || (gc->line_width & 1)) ? 1 : 0);
    mw_rects(c, r, n, 0);
    mw_stroke_path(c);
    mw_canvas_end(c);
    after_draw(d, dr);
    return 1;
}

int XFillRectangle(Display *d, Drawable dr, GC gc, int x, int y,
                   unsigned int w, unsigned int h)
{
    int dw, dh;
    MwCanvas *c;
    /* An empty rectangle draws nothing (X11 protocol).  Motif relies on it:
     * XmText issues XFillRectangle(win, gc, 0, 0, 0, 0) as a no-op, and
     * filling the whole surface instead painted every text field black. */
    if (w == 0 || h == 0) return 1;
    c = mw_canvas_for_drawable(d, dr, gc, &dw, &dh);
    if (!c) return 0;
    mw_rect(c, x, y, w, h);
    fill_path(c, gc, d);
    mw_canvas_end(c);
    after_draw(d, dr);
    return 1;
}

int XFillRectangles(Display *d, Drawable dr, GC gc, XRectangle *r, int n)
{
    int w, h;
    MwCanvas *c;
    if (!r || n <= 0) return 1;
    c = mw_canvas_for_drawable(d, dr, gc, &w, &h);
    if (!c) return 0;
    mw_rects(c, r, n, 1);
    fill_path(c, gc, d);
    mw_canvas_end(c);
    after_draw(d, dr);
    return 1;
}

int XDrawArc(Display *d, Drawable dr, GC gc, int x, int y,
             unsigned int w, unsigned int h, int a1, int a2)
{
    int dw, dh;
    MwCanvas *c = mw_canvas_for_drawable(d, dr, gc, &dw, &dh);
    if (!c) return 0;
    mw_set_snap(c, (gc->line_width <= 0 || (gc->line_width & 1)) ? 1 : 0);
    /* arc_mode only applies to fills; an outline is just the arc. */
    mw_arc(c, x, y, w, h, a1, a2, -1);
    mw_stroke_path(c);
    mw_canvas_end(c);
    after_draw(d, dr);
    return 1;
}

int XDrawArcs(Display *d, Drawable dr, GC gc, XArc *arcs, int n)
{
    int w, h;
    MwCanvas *c = mw_canvas_for_drawable(d, dr, gc, &w, &h);
    if (!c) return 0;
    mw_set_snap(c, (gc->line_width <= 0 || (gc->line_width & 1)) ? 1 : 0);
    for (int i = 0; i < n; i++)
        mw_arc(c, arcs[i].x, arcs[i].y, arcs[i].width, arcs[i].height,
               arcs[i].angle1, arcs[i].angle2, -1);
    mw_stroke_path(c);
    mw_canvas_end(c);
    after_draw(d, dr);
    return 1;
}

int XFillArc(Display *d, Drawable dr, GC gc, int x, int y,
             unsigned int w, unsigned int h, int a1, int a2)
{
    int dw, dh;
    MwCanvas *c = mw_canvas_for_drawable(d, dr, gc, &dw, &dh);
    if (!c) return 0;
    mw_arc(c, x, y, w, h, a1, a2, gc->arc_mode);
    fill_path(c, gc, d);
    mw_canvas_end(c);
    after_draw(d, dr);
    return 1;
}

int XFillArcs(Display *d, Drawable dr, GC gc, XArc *arcs, int n)
{
    int w, h;
    MwCanvas *c = mw_canvas_for_drawable(d, dr, gc, &w, &h);
    if (!c) return 0;
    for (int i = 0; i < n; i++)
        mw_arc(c, arcs[i].x, arcs[i].y, arcs[i].width, arcs[i].height,
               arcs[i].angle1, arcs[i].angle2, gc->arc_mode);
    fill_path(c, gc, d);
    mw_canvas_end(c);
    after_draw(d, dr);
    return 1;
}

int XFillPolygon(Display *d, Drawable dr, GC gc, XPoint *pts, int n,
                 int shape, int mode)
{
    (void)shape;
    int w, h;
    MwCanvas *c = mw_canvas_for_drawable(d, dr, gc, &w, &h);
    if (!c) return 0;
    mw_polygon(c, pts, n, mode == CoordModePrevious ? 1 : 0, 1);
    fill_path(c, gc, d);
    mw_canvas_end(c);
    after_draw(d, dr);
    return 1;
}

/* X promises a NoExpose (or GraphicsExpose) event after a copy when the GC was
 * created with graphics_exposures set, and clients use it to know the copy has
 * finished.  xterm scrolls its screen with XCopyArea and then waits for one in
 * CopyWait(); with no event it blocked forever the first time output scrolled
 * off the bottom, and the terminal froze -- alive, still reading its pty, but
 * no longer processing any events.  Our copies are synchronous and always
 * complete, so the answer is always NoExpose.  The request codes are
 * X_CopyArea/X_CopyPlane from Xproto.h. */
#define MW_X_CopyArea   62
#define MW_X_CopyPlane  63
static void send_copy_exposures(Display *d, Drawable dst, GC gc, int major,
                                bool clipped, int dx, int dy, int w, int h)
{
    if (!gc || !gc->graphics_exposures) return;
    if (clipped && w > 0 && h > 0) {
        XGraphicsExposeEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.type = GraphicsExpose;
        ev.display = d;
        ev.drawable = dst;
        ev.x = dx; ev.y = dy; ev.width = w; ev.height = h;
        ev.count = 0;
        ev.major_code = major;
        ev.minor_code = 0;
        mw_put_event(d, (XEvent *)&ev);
        return;
    }
    XNoExposeEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = NoExpose;
    ev.display = d;
    ev.drawable = dst;
    ev.major_code = major;
    ev.minor_code = 0;
    mw_put_event(d, (XEvent *)&ev);
}

/* Clip a copy's source rectangle to the source drawable, moving the
 * destination with it: X leaves destination pixels whose source lies
 * outside the drawable untouched.  False when nothing is left. */
/* CopyArea/CopyPlane carry INT16 offsets and CARD16 sizes on the wire, so
 * libX11 truncates them; doing the same keeps every sum below in int range
 * (the raster's self-copy clamps rely on that). */
static void copy_wire_truncate(int *sx, int *sy, unsigned int *w,
                               unsigned int *h, int *dx, int *dy)
{
    *sx = (short)*sx; *sy = (short)*sy;
    *dx = (short)*dx; *dy = (short)*dy;
    *w = (unsigned short)*w; *h = (unsigned short)*h;
}

static bool clip_to_source(int sw, int sh, int *sx, int *sy, int *w, int *h,
                           int *dx, int *dy, bool *clipped)
{
    int ow = *w, oh = *h;
    if (*sx < 0) { *dx -= *sx; *w += *sx; *sx = 0; }
    if (*sy < 0) { *dy -= *sy; *h += *sy; *sy = 0; }
    if (*sx + *w > sw) *w = sw - *sx;
    if (*sy + *h > sh) *h = sh - *sy;
    if (clipped) *clipped = (*w != ow || *h != oh);
    return *w > 0 && *h > 0;
}

int XCopyArea(Display *d, Drawable src, Drawable dst, GC gc,
              int sx, int sy, unsigned int w, unsigned int h, int dx, int dy)
{
    copy_wire_truncate(&sx, &sy, &w, &h, &dx, &dy);
    if (MW_TRACE())
        fprintf(stderr, "MW: XCopyArea src=0x%lx dst=0x%lx (%d,%d)->(%d,%d) %ux%u%s\n",
                src, dst, sx, sy, dx, dy, w, h, src == dst ? " SELF" : "");
    int sw, sh, dh2;
    mw_render_drain(d);
    MwSurface *ss = mw_drawable_surface(d, src, &sw, &sh, &dh2);
    int cw = (int)w, ch = (int)h;
    int orig_dx = dx, orig_dy = dy;
    bool clipped = false;
    if (!ss || !clip_to_source(sw, sh, &sx, &sy, &cw, &ch, &dx, &dy, &clipped)) {
        send_copy_exposures(d, dst, gc, MW_X_CopyArea, ss != NULL && clipped,
                            orig_dx, orig_dy, (int)w, (int)h);
        return ss != NULL;
    }
    int dw, dhh;
    MwCanvas *c = mw_canvas_for_drawable(d, dst, gc, &dw, &dhh);
    if (!c) return 0;
    mw_canvas_copy(c, ss, sx, sy, dx, dy, cw, ch);
    mw_canvas_end(c);
    after_draw_rect(d, dst, dx, dy, cw, ch);
    send_copy_exposures(d, dst, gc, MW_X_CopyArea, clipped,
                        orig_dx, orig_dy, (int)w, (int)h);
    return 1;
}

int XCopyPlane(Display *d, Drawable src, Drawable dst, GC gc,
               int sx, int sy, unsigned int w, unsigned int h,
               int dx, int dy, unsigned long plane)
{
    copy_wire_truncate(&sx, &sy, &w, &h, &dx, &dy);
    int sw, sh, dep;
    mw_render_drain(d);
    MwSurface *ss = mw_drawable_surface(d, src, &sw, &sh, &dep);
    /* plane has exactly one bit set; its index selects the bit. */
    int bit = plane ? __builtin_ctzl(plane) : 0;
    int cw = (int)w, ch = (int)h;
    int orig_dx = dx, orig_dy = dy;
    bool clipped = false;
    if (!ss || !clip_to_source(sw, sh, &sx, &sy, &cw, &ch, &dx, &dy, &clipped)) {
        send_copy_exposures(d, dst, gc, MW_X_CopyPlane, ss != NULL && clipped,
                            orig_dx, orig_dy, (int)w, (int)h);
        return ss != NULL;
    }
    int dw, dhh;
    MwCanvas *c = mw_canvas_for_drawable(d, dst, gc, &dw, &dhh);
    if (!c) return 0;
    mw_canvas_copy_plane(c, ss, sx, sy, dx, dy, cw, ch, bit,
                         (uint32_t)gc->foreground, (uint32_t)gc->background);
    mw_canvas_end(c);
    after_draw_rect(d, dst, dx, dy, cw, ch);
    send_copy_exposures(d, dst, gc, MW_X_CopyPlane, clipped,
                        orig_dx, orig_dy, (int)w, (int)h);
    return 1;
}
