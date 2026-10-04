/* drawing.c — graphics contexts and the Xlib drawing primitives. */
#include "internal.h"

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
    MwCanvas *c = mw_canvas_begin(surf, gc && gc->has_clip_rects ? gc->clip_rects : NULL,
                                  gc && gc->has_clip_rects ? gc->nclip : 0,
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
                gc->dash_offset, &gc->dashes, 1);
    mw_set_fill(c, gc->fill_style, gc->fill_rule, gc->arc_mode);
    if (gc->xfont && gc->xfont->rfont)
        mw_set_font(c, gc->xfont->rfont);
    if ((gc->fill_style == FillStippled || gc->fill_style == FillOpaqueStippled)
        && gc->stipple) {
        MwPixmap *pm = mw_pixmap(d, gc->stipple);
        if (pm) mw_set_stipple(c, pm->surface, gc->ts_x_origin, gc->ts_y_origin);
    } else if (gc->fill_style == FillTiled && gc->tile) {
        MwPixmap *pm = mw_pixmap(d, gc->tile);
        if (pm) mw_set_stipple(c, pm->surface, gc->ts_x_origin, gc->ts_y_origin);
    }
}

/* Fill the current path with the right fill style. */
static void fill_path(MwCanvas *c, struct _XGC *gc, Display *d)
{
    if ((gc->fill_style == FillStippled || gc->fill_style == FillOpaqueStippled)
        && gc->stipple) {
        MwPixmap *pm = mw_pixmap(d, gc->stipple);
        if (pm) {
            mw_fill_with_stipple(c, pm->surface, gc->ts_x_origin, gc->ts_y_origin);
            return;
        }
    } else if (gc->fill_style == FillTiled && gc->tile) {
        MwPixmap *pm = mw_pixmap(d, gc->tile);
        if (pm) {
            mw_fill_with_stipple(c, pm->surface, gc->ts_x_origin, gc->ts_y_origin);
            return;
        }
    }
    mw_fill_path(c);
}

/* ---------------------------------------------------------------- GCs */

struct _XGC *mw_create_gc_internal(Display *d, Drawable dr, unsigned long mask,
                                   XGCValues *v)
{
    (void)dr;
    struct _XGC *gc = calloc(1, sizeof *gc);
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
        if (mask & GCFont)              gc->font = v->font;
        if (mask & GCSubwindowMode)     gc->subwindow_mode = v->subwindow_mode;
        if (mask & GCGraphicsExposures) gc->graphics_exposures = v->graphics_exposures;
        if (mask & GCClipXOrigin)       gc->clip_x_origin = v->clip_x_origin;
        if (mask & GCClipYOrigin)       gc->clip_y_origin = v->clip_y_origin;
        if (mask & GCClipMask)          gc->clip_mask = v->clip_mask;
        if (mask & GCDashOffset)        gc->dash_offset = v->dash_offset;
        if (mask & GCDashList)          gc->dashes = v->dashes;
    }
    if (gc->font == None) {
        /* default font */
        Font def = XLoadFont(d, "fixed");
        if (def != None) gc->font = def;
    }
    gc->xfont = gc->font ? mw_font(d, gc->font) : NULL;
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
    if (mask & GCFont) { gc->font = v->font; gc->xfont = mw_font(d, v->font); }
    if (mask & GCSubwindowMode)     gc->subwindow_mode = v->subwindow_mode;
    if (mask & GCGraphicsExposures) gc->graphics_exposures = v->graphics_exposures;
    if (mask & GCClipXOrigin)       gc->clip_x_origin = v->clip_x_origin;
    if (mask & GCClipYOrigin)       gc->clip_y_origin = v->clip_y_origin;
    if (mask & GCClipMask)          gc->clip_mask = v->clip_mask;
    if (mask & GCDashOffset)        gc->dash_offset = v->dash_offset;
    if (mask & GCDashList)          gc->dashes = v->dashes;
    return 1;
}

/* Copy the GC components named by `mask` from `src` to `dest`.  X11 explicitly
 * does not copy the font (GCFont in the mask is ignored), and the clip mask /
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

    if (mask & GCClipMask) {
        dest->clip_mask = src->clip_mask;
        /* The resolved clip rectangles are derived state we own, so they must
         * be duplicated rather than aliased (XFreeGC frees them).  The shim's
         * Region type has no XCopyRegion equivalent, so only the rectangle
         * form is reproduced -- that is what the drawing code consults. */
        free(dest->clip_rects);
        dest->clip_rects = NULL;
        dest->nclip = 0;
        dest->has_clip_rects = false;
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
    (void)d;
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
    if (gc->clip_region) XDestroyRegion(gc->clip_region);
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
{ (void)d; gc->dash_offset = dash_offset; gc->dashes = n ? dash_list[0] : 0; return 1; }
int XSetClipOrigin(Display *d, GC gc, int x, int y) { (void)d; gc->clip_x_origin = x; gc->clip_y_origin = y; return 1; }
int XSetClipMask(Display *d, GC gc, Pixmap p) { (void)d; gc->clip_mask = p; return 1; }

int XSetClipRectangles(Display *d, GC gc, int clip_x, int clip_y,
                       XRectangle *rects, int n, int ordering)
{
    (void)d; (void)ordering;
    free(gc->clip_rects);
    gc->clip_rects = n ? malloc(sizeof(XRectangle) * n) : NULL;
    if (rects) memcpy(gc->clip_rects, rects, sizeof(XRectangle) * n);
    gc->nclip = n;
    gc->has_clip_rects = n > 0;
    gc->clip_x_origin = clip_x;
    gc->clip_y_origin = clip_y;
    return 1;
}

int XSetRegion(Display *d, GC gc, Region r)
{
    (void)d;
    if (gc->clip_region) XDestroyRegion(gc->clip_region);
    gc->clip_region = XCreateRegion();
    XUnionRegion(gc->clip_region, r, gc->clip_region);
    XRectangle box;
    XClipBox(r, &box);
    free(gc->clip_rects);
    gc->clip_rects = malloc(sizeof(XRectangle));
    gc->clip_rects[0] = box;
    gc->nclip = 1;
    gc->has_clip_rects = true;
    return 1;
}

/* ------------------------------------------------------------ primitives */

static void after_draw(Display *d, Drawable dr)
{
    MwWindow *win = mw_window(d, dr);
    if (win) mw_window_damage(win);
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
    after_draw(d, dr);
    return 1;
}

int XDrawPoints(Display *d, Drawable dr, GC gc, XPoint *pts, int n, int mode)
{
    (void)mode;
    int w, h;
    MwCanvas *c = mw_canvas_for_drawable(d, dr, gc, &w, &h);
    if (!c) return 0;
    for (int i = 0; i < n; i++) mw_rect(c, pts[i].x, pts[i].y, 1, 1);
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
    mw_polygon(c, pts, n, 0, 0);
    mw_stroke_path(c);
    mw_canvas_end(c);
    after_draw(d, dr);
    (void)mode;
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
    mw_arc(c, x, y, w, h, a1, a2, gc->arc_mode);
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
               arcs[i].angle1, arcs[i].angle2, gc->arc_mode);
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
    (void)shape; (void)mode;
    int w, h;
    MwCanvas *c = mw_canvas_for_drawable(d, dr, gc, &w, &h);
    if (!c) return 0;
    mw_polygon(c, pts, n, 0, 1);
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
static void send_copy_exposures(Display *d, Drawable dst, GC gc, int major)
{
    if (!gc || !gc->graphics_exposures) return;
    XNoExposeEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.type = NoExpose;
    ev.display = d;
    ev.drawable = dst;
    ev.major_code = major;
    ev.minor_code = 0;
    mw_put_event(d, (XEvent *)&ev);
}

int XCopyArea(Display *d, Drawable src, Drawable dst, GC gc,
              int sx, int sy, unsigned int w, unsigned int h, int dx, int dy)
{
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: XCopyArea src=0x%lx dst=0x%lx (%d,%d)->(%d,%d) %ux%u%s\n",
                src, dst, sx, sy, dx, dy, w, h, src == dst ? " SELF" : "");
    int sw, sh, dh2;
    MwSurface *ss = mw_drawable_surface(d, src, &sw, &sh, &dh2);
    int dw, dhh;
    MwCanvas *c = mw_canvas_for_drawable(d, dst, gc, &dw, &dhh);
    if (!c || !ss) { if (c) mw_canvas_end(c); return 0; }
    mw_canvas_copy(c, ss, sx, sy, dx, dy, (int)w, (int)h);
    mw_canvas_end(c);
    after_draw(d, dst);
    send_copy_exposures(d, dst, gc, MW_X_CopyArea);
    return 1;
}

int XCopyPlane(Display *d, Drawable src, Drawable dst, GC gc,
               int sx, int sy, unsigned int w, unsigned int h,
               int dx, int dy, unsigned long plane)
{
    int sw, sh, dep;
    MwSurface *ss = mw_drawable_surface(d, src, &sw, &sh, &dep);
    int dw, dhh;
    MwCanvas *c = mw_canvas_for_drawable(d, dst, gc, &dw, &dhh);
    if (!c || !ss) { if (c) mw_canvas_end(c); return 0; }
    mw_set_source_argb(c, 0xff000000u | (uint32_t)(gc->foreground & 0xffffff));
    mw_canvas_copy_plane(c, ss, sx, sy, dx, dy, (int)w, (int)h,
                         plane == 1 ? 0 : 0);
    mw_canvas_end(c);
    after_draw(d, dst);
    send_copy_exposures(d, dst, gc, MW_X_CopyPlane);
    return 1;
}
