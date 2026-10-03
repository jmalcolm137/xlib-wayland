/* xshape.c — the SHAPE extension, tracked locally for bounding/clip. */
#include "internal.h"

#include <X11/extensions/shape.h>
#include <stdlib.h>
#include <string.h>

void mw_shape_init(Display *d) { (void)d; }

Bool XShapeQueryExtension(Display *d, int *event_base, int *error_base)
{
    (void)d;
    if (event_base) *event_base = 64;
    if (error_base) *error_base = 64;
    return True;
}

Status XShapeQueryVersion(Display *d, int *major, int *minor)
{
    (void)d;
    if (major) *major = 1;
    if (minor) *minor = 1;
    return 1;
}

void XShapeCombineRectangles(Display *d, Window dest, int kind, int x_off,
                             int y_off, XRectangle *rects, int n_rects,
                             int op, int ordering)
{
    (void)ordering;
    MwWindow *w = mw_window(d, dest);
    if (!w) return;
    if (kind != ShapeBounding && kind != ShapeClip) return;
    if (!w->has_shape) {
        pixman_region32_init_rect(&w->shape, 0, 0, (unsigned)w->w, (unsigned)w->h);
        w->has_shape = true;
    }
    pixman_region32_t add;
    pixman_region32_init(&add);
    for (int i = 0; i < n_rects; i++)
        pixman_region32_union_rect(&add, &add, rects[i].x + x_off,
                                   rects[i].y + y_off, rects[i].width,
                                   rects[i].height);
    switch (op) {
    case ShapeSet:      pixman_region32_copy(&w->shape, &add); break;
    case ShapeUnion:    pixman_region32_union(&w->shape, &w->shape, &add); break;
    case ShapeIntersect:pixman_region32_intersect(&w->shape, &w->shape, &add); break;
    case ShapeSubtract: pixman_region32_subtract(&w->shape, &w->shape, &add); break;
    case ShapeInvert: {
        pixman_region32_t inv;
        pixman_region32_init_rect(&inv, 0, 0, (unsigned)w->w, (unsigned)w->h);
        pixman_region32_subtract(&inv, &inv, &add);
        pixman_region32_copy(&w->shape, &inv);
        pixman_region32_fini(&inv);
        break;
    }
    default: break;
    }
    pixman_region32_fini(&add);
    mw_window_damage(w);
}

void XShapeCombineMask(Display *d, Window dest, int kind, int x_off, int y_off,
                       Pixmap src, int op)
{
    (void)x_off; (void)y_off; (void)src; (void)op;
    MwWindow *w = mw_window(d, dest);
    if (w && kind == ShapeBounding) {
        w->has_shape = false;
        mw_window_damage(w);
    }
}

void XShapeCombineRegion(Display *d, Window dest, int kind, int x_off,
                         int y_off, Region r, int op)
{
    XRectangle box;
    XClipBox(r, &box);
    XRectangle rect = { (short)(box.x + x_off), (short)(box.y + y_off),
                        box.width, box.height };
    XShapeCombineRectangles(d, dest, kind, 0, 0, &rect, 1, op, 0);
}

void XShapeCombineShape(Display *d, Window dest, int kind, int x_off, int y_off,
                        Window src, int src_kind, int op)
{
    (void)src_kind;
    MwWindow *s = mw_window(d, src);
    XRectangle box = { (short)x_off, (short)y_off,
                       (unsigned short)(s ? s->w : 0), (unsigned short)(s ? s->h : 0) };
    XShapeCombineRectangles(d, dest, kind, 0, 0, &box, 1, op, 0);
}

void XShapeOffsetShape(Display *d, Window dest, int kind, int x_off, int y_off)
{
    (void)x_off; (void)y_off; (void)kind;
    MwWindow *w = mw_window(d, dest);
    if (w && w->has_shape) { mw_window_damage(w); mw_flush_damage(d); }
}

Status XShapeQueryExtents(Display *d, Window w, Bool *bounding_shaped,
                          int *xbs, int *ybs, unsigned int *wbs,
                          unsigned int *hbs, Bool *clip_shaped,
                          int *xcs, int *ycs, unsigned int *wcs,
                          unsigned int *hcs)
{
    MwWindow *win = mw_window(d, w);
    if (!win) return 0;
    if (bounding_shaped) *bounding_shaped = win->has_shape;
    if (xbs) *xbs = 0;
    if (ybs) *ybs = 0;
    if (wbs) *wbs = win->w;
    if (hbs) *hbs = win->h;
    if (clip_shaped) *clip_shaped = False;
    if (xcs) *xcs = 0;
    if (ycs) *ycs = 0;
    if (wcs) *wcs = win->w;
    if (hcs) *hcs = win->h;
    return 1;
}

XRectangle *XShapeGetRectangles(Display *d, Window w, int kind, int *count,
                                int *ordering)
{
    (void)kind;
    MwWindow *win = mw_window(d, w);
    if (count) *count = 0;
    if (ordering) *ordering = 0;
    if (!win) return NULL;
    pixman_region32_t reg;
    if (win->has_shape) reg = win->shape;
    else pixman_region32_init_rect(&reg, 0, 0, (unsigned)win->w, (unsigned)win->h);
    int n = 0;
    pixman_box32_t *boxes = pixman_region32_rectangles(&reg, &n);
    XRectangle *out = n ? malloc(sizeof(XRectangle) * n) : NULL;
    for (int i = 0; i < n; i++) {
        out[i].x = (short)boxes[i].x1;
        out[i].y = (short)boxes[i].y1;
        out[i].width = (unsigned short)(boxes[i].x2 - boxes[i].x1);
        out[i].height = (unsigned short)(boxes[i].y2 - boxes[i].y1);
    }
    if (count) *count = n;
    pixman_region32_fini(&reg);
    return out;
}

void XShapeSelectInput(Display *d, Window w, unsigned long mask)
{ (void)d; (void)w; (void)mask; }

unsigned long XShapeInputSelected(Display *d, Window w) { (void)d; (void)w; return 0; }
