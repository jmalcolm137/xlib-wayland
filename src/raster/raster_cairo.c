/* raster_cairo.c — cairo backend for the mw_raster_* interface.
 *
 * This is the only translation unit that includes cairo.  Anything Skia (or
 * another backend) would need to reimplement is confined to this file.
 */
#include "raster.h"

#include <cairo.h>
#include <cairo-ft.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ------------------------------------------------------------- surfaces */

struct MwSurface {
    cairo_surface_t *cs;
    bool             owned;   /* cs was created with _create (owns memory) */
    void            *data;    /* for create_for_data */
    cairo_t         *keep;    /* retained for wrapping data surfaces */
    int              w, h;    /* LOGICAL size (device-independent units) */
    int              stride;  /* PHYSICAL row pitch in bytes */
    int              scale;   /* device scale, >= 1 */
};

/* Shared constructor.  `data` is NULL for an owned surface; `scale` is the
 * HiDPI device scale.  The backing store is always w*scale x h*scale physical
 * pixels, while MwSurface.w/h stay logical -- cairo's device scale does the
 * mapping, so every caller keeps working in application coordinates. */
static MwSurface *surface_new(void *data, int w, int h, int stride, int scale,
                              bool owned)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (scale < 1) scale = 1;
    MwSurface *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->w = w;
    s->h = h;
    s->scale = scale;
    s->owned = owned;
    int pw = w * scale, ph = h * scale;
    if (owned) {
        s->cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
    } else {
        if (stride <= 0) stride = pw * 4;
        s->cs = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_ARGB32,
                                                    pw, ph, stride);
        s->data = data;
    }
    if (!s->cs) { free(s); return NULL; }
    if (scale > 1) cairo_surface_set_device_scale(s->cs, scale, scale);
    s->stride = cairo_image_surface_get_stride(s->cs);
    if (owned) s->data = cairo_image_surface_get_data(s->cs);
    return s;
}

MwSurface *mw_surface_create(int w, int h)
{
    return surface_new(NULL, w, h, 0, 1, true);
}

MwSurface *mw_surface_create_scaled(int w, int h, int scale)
{
    return surface_new(NULL, w, h, 0, scale, true);
}

MwSurface *mw_surface_create_for_data(void *data, int w, int h, int stride)
{
    return surface_new(data, w, h, stride, 1, false);
}

MwSurface *mw_surface_create_for_data_scaled(void *data, int w, int h,
                                             int stride, int scale)
{
    return surface_new(data, w, h, stride, scale, false);
}

void mw_surface_destroy(MwSurface *s)
{
    if (!s) return;
    if (s->keep) cairo_destroy(s->keep);
    if (s->cs) cairo_surface_destroy(s->cs);
    free(s);
}

int  mw_surface_width(const MwSurface *s)  { return s ? s->w : 0; }
int  mw_surface_height(const MwSurface *s) { return s ? s->h : 0; }
int  mw_surface_stride(const MwSurface *s) { return s ? s->stride : 0; }
void *mw_surface_data(const MwSurface *s)  { return s ? s->data : NULL; }
int  mw_surface_scale(const MwSurface *s)  { return s ? s->scale : 1; }

void mw_surface_clear(MwSurface *s, uint32_t argb)
{
    if (!s) return;
    cairo_t *cr = cairo_create(s->cs);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    double a = ((argb >> 24) & 0xff) / 255.0;
    double r = ((argb >> 16) & 0xff) / 255.0 * (a ? a : 0);
    double g = ((argb >>  8) & 0xff) / 255.0 * (a ? a : 0);
    double b = ((argb      ) & 0xff) / 255.0 * (a ? a : 0);
    /* premultiplied source */
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_mark_dirty(s->cs);
}

void mw_surface_write_png(MwSurface *s, const char *path)
{
    if (s) cairo_surface_write_to_png(s->cs, path);
}

void mw_surface_mark_dirty(MwSurface *s)
{
    if (s) cairo_surface_mark_dirty(s->cs);
}

void *mw_surface_native(MwSurface *s)
{
    return s ? (void *)s->cs : NULL;
}

/* ------------------------------------------------------------- canvas */

struct MwCanvas {
    cairo_t   *cr;
    MwSurface *surface;
    int        clip_x_origin, clip_y_origin;
    int        gx;
    int        lw;              /* line width */
    double     snap;            /* 0 or 0.5 pixel-grid nudge for strokes */
    uint32_t   plane_mask;      /* X plane mask (RGB bits) */
};

/* ----------------------------------------------------- bitwise raster ops
 *
 * cairo only composites, but X GCs combine source and destination bits
 * (GXxor rubber banding, GXinvert highlights) under a plane mask.  GXcopy
 * with every plane enabled goes through cairo; everything else is applied
 * exactly, pixel by pixel: the shape's coverage is rendered into an A8 mask
 * with the same clip and line attributes, then the operation is applied to
 * the covered pixels.  The pixel's alpha byte is left as it was. */
static uint32_t rop_px(int gx, uint32_t s, uint32_t d)
{
    switch (gx) {
    case GXclear:        return 0;
    case GXand:          return s & d;
    case GXandReverse:   return s & ~d;
    case GXcopy:         return s;
    case GXandInverted:  return ~s & d;
    case GXnoop:         return d;
    case GXxor:          return s ^ d;
    case GXor:           return s | d;
    case GXnor:          return ~(s | d);
    case GXequiv:        return ~(s ^ d);
    case GXinvert:       return ~d;
    case GXorReverse:    return s | ~d;
    case GXcopyInverted: return ~s;
    case GXorInverted:   return ~s | d;
    case GXnand:         return ~(s & d);
    case GXset:          return 0xffffffffu;
    default:             return s;
    }
}

static bool canvas_uses_rop(const MwCanvas *c)
{
    return c->gx != GXcopy || (c->plane_mask & 0xffffff) != 0xffffff;
}

static inline uint32_t rop_apply_px(const MwCanvas *c, uint32_t s, uint32_t d)
{
    uint32_t m = c->plane_mask & 0xffffff;
    uint32_t r = rop_px(c->gx, s, d);
    return (d & ~m) | (r & m);
}

/* Physical-pixel bounds of the canvas clip intersected with [x1,x2)x[y1,y2)
 * (user space).  False when empty. */
static bool clip_bounds(MwCanvas *c, double x1, double y1, double x2, double y2,
                        int *px0, int *py0, int *px1, int *py1)
{
    double cx1, cy1, cx2, cy2;
    cairo_clip_extents(c->cr, &cx1, &cy1, &cx2, &cy2);
    int sc = c->surface->scale;
    double l = fmax(x1, cx1), t = fmax(y1, cy1);
    double r = fmin(x2, cx2), b = fmin(y2, cy2);
    int ix0 = (int)floor(l * sc), iy0 = (int)floor(t * sc);
    int ix1 = (int)ceil(r * sc),  iy1 = (int)ceil(b * sc);
    if (ix0 < 0) ix0 = 0;
    if (iy0 < 0) iy0 = 0;
    if (ix1 > c->surface->w * sc) ix1 = c->surface->w * sc;
    if (iy1 > c->surface->h * sc) iy1 = c->surface->h * sc;
    *px0 = ix0; *py0 = iy0; *px1 = ix1; *py1 = iy1;
    return ix1 > ix0 && iy1 > iy0;
}

/* Install the canvas clip on another cairo_t (rectangles are all the shim
 * ever clips with; anything else falls back to the clip extents). */
static void copy_clip(MwCanvas *c, cairo_t *to)
{
    cairo_rectangle_list_t *rl = cairo_copy_clip_rectangle_list(c->cr);
    if (rl->status == CAIRO_STATUS_SUCCESS) {
        for (int i = 0; i < rl->num_rectangles; i++)
            cairo_rectangle(to, rl->rectangles[i].x, rl->rectangles[i].y,
                            rl->rectangles[i].width, rl->rectangles[i].height);
    } else {
        double x1, y1, x2, y2;
        cairo_clip_extents(c->cr, &x1, &y1, &x2, &y2);
        cairo_rectangle(to, x1, y1, x2 - x1, y2 - y1);
    }
    cairo_rectangle_list_destroy(rl);
    cairo_clip(to);
}

/* Rasterise the current path (filled or stroked) as an A8 coverage mask over
 * the physical box [*px0,*px1) x [*py0,*py1).  Consumes the path. */
static cairo_surface_t *path_coverage(MwCanvas *c, bool stroke,
                                      int *px0, int *py0, int *px1, int *py1)
{
    double x1, y1, x2, y2;
    if (stroke) cairo_stroke_extents(c->cr, &x1, &y1, &x2, &y2);
    else        cairo_fill_extents(c->cr, &x1, &y1, &x2, &y2);
    if (!clip_bounds(c, x1, y1, x2, y2, px0, py0, px1, py1)) {
        cairo_new_path(c->cr);
        return NULL;
    }
    int sc = c->surface->scale;
    cairo_surface_t *m = cairo_image_surface_create(CAIRO_FORMAT_A8,
                                                    *px1 - *px0, *py1 - *py0);
    cairo_surface_set_device_scale(m, sc, sc);
    cairo_surface_set_device_offset(m, -*px0, -*py0);
    cairo_t *mc = cairo_create(m);
    cairo_set_antialias(mc, CAIRO_ANTIALIAS_NONE);
    copy_clip(c, mc);
    cairo_path_t *path = cairo_copy_path(c->cr);
    cairo_append_path(mc, path);
    cairo_path_destroy(path);
    cairo_new_path(c->cr);
    cairo_set_operator(mc, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(mc, 0, 0, 0, 1);
    if (stroke) {
        cairo_set_line_width(mc, cairo_get_line_width(c->cr));
        cairo_set_line_cap(mc, cairo_get_line_cap(c->cr));
        cairo_set_line_join(mc, cairo_get_line_join(c->cr));
        cairo_set_miter_limit(mc, cairo_get_miter_limit(c->cr));
        int nd = cairo_get_dash_count(c->cr);
        if (nd > 0) {
            double *dd = calloc((size_t)nd, sizeof *dd), off = 0;
            if (dd) {
                cairo_get_dash(c->cr, dd, &off);
                cairo_set_dash(mc, dd, nd, off);
                free(dd);
            }
        }
        cairo_stroke(mc);
    } else {
        cairo_set_fill_rule(mc, cairo_get_fill_rule(c->cr));
        cairo_fill(mc);
    }
    cairo_destroy(mc);
    cairo_surface_flush(m);
    return m;
}

/* Apply the GC's raster op with a solid source over the covered pixels. */
static void rop_solid(MwCanvas *c, bool stroke)
{
    MwSurface *s = c->surface;
    if (!s->data && !cairo_image_surface_get_data(s->cs)) {
        cairo_new_path(c->cr);
        return;
    }
    int x0, y0, x1, y1;
    cairo_surface_t *m = path_coverage(c, stroke, &x0, &y0, &x1, &y1);
    if (!m) return;
    double r, g, b, a;
    cairo_pattern_get_rgba(cairo_get_source(c->cr), &r, &g, &b, &a);
    uint32_t src = ((uint32_t)lround(r * 255) << 16) |
                   ((uint32_t)lround(g * 255) << 8) | (uint32_t)lround(b * 255);
    cairo_surface_flush(s->cs);
    uint32_t *px = (uint32_t *)cairo_image_surface_get_data(s->cs);
    int stride = cairo_image_surface_get_stride(s->cs) / 4;
    const unsigned char *md = cairo_image_surface_get_data(m);
    int ms = cairo_image_surface_get_stride(m);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            if (md[(size_t)(y - y0) * ms + (x - x0)] < 0x80) continue;
            uint32_t *p = &px[(size_t)y * stride + x];
            *p = rop_apply_px(c, src, *p);
        }
    cairo_surface_mark_dirty(s->cs);
    cairo_surface_destroy(m);
}

void mw_set_plane_mask(MwCanvas *c, unsigned long plane_mask)
{
    c->plane_mask = (uint32_t)plane_mask;
}

static cairo_operator_t gx_to_operator(int gx)
{
    switch (gx) {
    case 0x0: return CAIRO_OPERATOR_CLEAR;        /* GXclear */
    case 0x1: return CAIRO_OPERATOR_SOURCE;       /* GXand  (approx) */
    case 0x2: return CAIRO_OPERATOR_SOURCE;       /* GXandReverse (approx) */
    case 0x3: return CAIRO_OPERATOR_SOURCE;       /* GXcopy */
    case 0x4: return CAIRO_OPERATOR_SOURCE;       /* GXandInverted (approx) */
    case 0x5: return CAIRO_OPERATOR_OVER;         /* GXnoop -> keep dest */
    case 0x6: return CAIRO_OPERATOR_XOR;          /* GXxor */
    case 0x7: return CAIRO_OPERATOR_ADD;          /* GXor */
    case 0x8: return CAIRO_OPERATOR_CLEAR;        /* GXnor (approx) */
    case 0x9: return CAIRO_OPERATOR_XOR;          /* GXequiv (approx) */
    case 0xa: return CAIRO_OPERATOR_SOURCE;       /* GXinvert (approx) */
    case 0xb: return CAIRO_OPERATOR_ADD;          /* GXorReverse */
    case 0xc: return CAIRO_OPERATOR_SOURCE;       /* GXcopyInverted */
    case 0xd: return CAIRO_OPERATOR_ADD;          /* GXorInverted */
    case 0xe: return CAIRO_OPERATOR_CLEAR;        /* GXnand (approx) */
    case 0xf: return CAIRO_OPERATOR_SOURCE;       /* GXset */
    default:  return CAIRO_OPERATOR_SOURCE;
    }
}

MwCanvas *mw_canvas_begin(MwSurface *s, const XRectangle *clip, int nclip,
                          int clip_x_origin, int clip_y_origin)
{
    MwCanvas *c = calloc(1, sizeof *c);
    c->surface = s;
    c->cr = cairo_create(s->cs);
    c->clip_x_origin = clip_x_origin;
    c->clip_y_origin = clip_y_origin;
    c->gx = GXcopy;
    c->plane_mask = 0xffffffffu;
    cairo_set_antialias(c->cr, CAIRO_ANTIALIAS_NONE);
    if (clip && nclip > 0)
        mw_clip_rects(c, clip, nclip, clip_x_origin, clip_y_origin);
    return c;
}

void mw_canvas_end(MwCanvas *c)
{
    if (!c) return;
    cairo_destroy(c->cr);
    if (c->surface) cairo_surface_mark_dirty(c->surface->cs);
    free(c);
}

void mw_set_operator(MwCanvas *c, int gx_function)
{
    c->gx = gx_function;
    cairo_set_operator(c->cr, gx_to_operator(gx_function));
}

void mw_set_source_argb(MwCanvas *c, uint32_t argb)
{
    double a = ((argb >> 24) & 0xff) / 255.0;
    double r = ((argb >> 16) & 0xff) / 255.0;
    double g = ((argb >>  8) & 0xff) / 255.0;
    double b = ((argb      ) & 0xff) / 255.0;
    cairo_set_source_rgba(c->cr, r, g, b, a ? a : 1.0);
}

void mw_set_line(MwCanvas *c, int width, int line_style, int cap, int join,
                 int dash_offset, const char *dashes, int ndash)
{
    c->lw = width > 0 ? width : 1;
    cairo_set_line_width(c->cr, width > 0 ? width : 1.0);

    cairo_line_cap_t lc = CAIRO_LINE_CAP_BUTT;
    switch (cap) {
    case 1: lc = CAIRO_LINE_CAP_BUTT; break;        /* CapButt */
    case 2: lc = CAIRO_LINE_CAP_ROUND; break;       /* CapRound */
    case 3: lc = CAIRO_LINE_CAP_SQUARE; break;      /* CapProjecting */
    default: lc = CAIRO_LINE_CAP_BUTT; break;
    }
    cairo_set_line_cap(c->cr, lc);

    cairo_line_join_t lj = CAIRO_LINE_JOIN_MITER;
    switch (join) {
    case JoinMiter: lj = CAIRO_LINE_JOIN_MITER; break;
    case JoinRound: lj = CAIRO_LINE_JOIN_ROUND; break;
    case JoinBevel: lj = CAIRO_LINE_JOIN_BEVEL; break;
    default: lj = CAIRO_LINE_JOIN_MITER; break;
    }
    cairo_set_line_join(c->cr, lj);

    if ((line_style == LineOnOffDash /*2*/ || line_style == LineDoubleDash /*3*/)
        && dashes && ndash > 0) {
        int n = ndash;
        double *d = calloc(n, sizeof(double));
        for (int i = 0; i < n; i++) d[i] = (unsigned char)dashes[i];
        cairo_set_dash(c->cr, d, n, dash_offset);
        free(d);
    } else {
        cairo_set_dash(c->cr, NULL, 0, 0);
    }
}

void mw_set_fill(MwCanvas *c, int fill_style, int fill_rule, int arc_mode)
{
    (void)fill_style;
    cairo_set_fill_rule(c->cr, fill_rule == EvenOddRule /*1*/
                        ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING);
    (void)arc_mode;
}

void mw_clip_rects(MwCanvas *c, const XRectangle *r, int n, int x_org, int y_org)
{
    cairo_reset_clip(c->cr);
    if (n <= 0) return;
    for (int i = 0; i < n; i++) {
        cairo_rectangle(c->cr, r[i].x + x_org, r[i].y + y_org, r[i].width, r[i].height);
    }
    cairo_clip(c->cr);
}

void mw_clip_mask(MwCanvas *c, MwSurface *mask, int x_org, int y_org)
{
    if (!c || !mask) return;
    int mw = mw_surface_width(mask), mh = mw_surface_height(mask);
    const uint32_t *px = mw_surface_data(mask);
    int stride = mw_surface_stride(mask) / 4;
    if (!px || mw <= 0 || mh <= 0 || stride <= 0) return;
    cairo_surface_t *cs = mw_surface_native(mask);
    if (cs) cairo_surface_flush(cs);
    /* A depth-1 clip mask's logical value is bit 0 of each pixel.  Turn each
     * horizontal run of set bits into a 1-pixel-tall rectangle and clip to
     * their union, intersected with any existing clip.  This is exact (a
     * bounding box was not), and honours the clip-origin alignment.  An empty
     * mask clips everything, as X does. */
    cairo_reset_clip(c->cr);
    cairo_new_path(c->cr);
    for (int y = 0; y < mh; y++) {
        const uint32_t *row = px + (size_t)y * stride;
        int x = 0;
        while (x < mw) {
            if (row[x] & 1u) {
                int x0 = x;
                while (x < mw && (row[x] & 1u)) x++;
                cairo_rectangle(c->cr, x_org + x0, y_org + y, x - x0, 1);
            } else {
                x++;
            }
        }
    }
    cairo_clip(c->cr);
}

void mw_paint(MwCanvas *c)
{
    cairo_paint(c->cr);
}

void mw_fill_path(MwCanvas *c)
{
    if (canvas_uses_rop(c)) { rop_solid(c, false); return; }
    cairo_fill(c->cr);
}

void mw_stroke_path(MwCanvas *c)
{
    if (canvas_uses_rop(c)) { rop_solid(c, true); return; }
    cairo_stroke(c->cr);
}

/* Fill the current path with a stipple or tile.  A stipple is a depth-1
 * pixmap whose logical value is bit 0 of each pixel (as for clip masks):
 * FillStippled paints the foreground through the set bits and leaves the
 * rest alone, FillOpaqueStippled paints clear bits in the background.  A
 * tile paints its own pixels.  The pattern is anchored at the tile/stipple
 * origin. */
void mw_fill_pattern(MwCanvas *c, MwSurface *pat, int x_origin, int y_origin,
                     int fill_style, uint32_t fg, uint32_t bg)
{
    if (!c || !pat) return;
    int pw = pat->w, ph = pat->h, psc = pat->scale;
    if (canvas_uses_rop(c) && pw > 0 && ph > 0) {
        /* A raster op applies to stippled and tiled fills too (Motif draws
         * stippled GXxor highlights): per covered pixel, the pattern gives
         * the source -- the tile's pixel, or fg/bg by the stipple bit, with
         * a FillStippled hole leaving the pixel alone. */
        MwSurface *s = c->surface;
        int x0, y0, x1, y1;
        cairo_surface_t *m = path_coverage(c, false, &x0, &y0, &x1, &y1);
        if (!m) return;
        cairo_surface_flush(pat->cs);
        const uint32_t *sp = (const uint32_t *)cairo_image_surface_get_data(pat->cs);
        int ss = cairo_image_surface_get_stride(pat->cs) / 4;
        cairo_surface_flush(s->cs);
        uint32_t *px = (uint32_t *)cairo_image_surface_get_data(s->cs);
        int stride = cairo_image_surface_get_stride(s->cs) / 4;
        const unsigned char *md = cairo_image_surface_get_data(m);
        int ms = cairo_image_surface_get_stride(m);
        int sc = s->scale;
        for (int y = y0; y < y1; y++) {
            int py = ((y / sc - y_origin) % ph + ph) % ph;
            for (int x = x0; x < x1; x++) {
                if (md[(size_t)(y - y0) * ms + (x - x0)] < 0x80) continue;
                int qx = ((x / sc - x_origin) % pw + pw) % pw;
                uint32_t v = sp[(size_t)(py * psc) * ss + (size_t)(qx * psc)];
                uint32_t src;
                if (fill_style == FillTiled) src = v;
                else if (v & 1u)             src = fg;
                else if (fill_style == FillOpaqueStippled) src = bg;
                else continue;
                uint32_t *p = &px[(size_t)y * stride + x];
                *p = rop_apply_px(c, src & 0xffffff, *p);
            }
        }
        cairo_surface_mark_dirty(s->cs);
        cairo_surface_destroy(m);
        return;
    }
    cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
    uint32_t *td = (uint32_t *)cairo_image_surface_get_data(tmp);
    int ts = cairo_image_surface_get_stride(tmp) / 4;
    cairo_surface_flush(pat->cs);
    const uint32_t *sp = (const uint32_t *)cairo_image_surface_get_data(pat->cs);
    int ss = cairo_image_surface_get_stride(pat->cs) / 4;
    for (int y = 0; y < ph; y++)
        for (int x = 0; x < pw; x++) {
            uint32_t v = sp[(size_t)(y * psc) * ss + (size_t)(x * psc)];
            uint32_t out;
            if (fill_style == FillTiled)
                out = 0xff000000u | (v & 0xffffff);
            else if (v & 1u)
                out = 0xff000000u | (fg & 0xffffff);
            else
                out = fill_style == FillOpaqueStippled
                      ? 0xff000000u | (bg & 0xffffff) : 0;
            td[(size_t)y * ts + x] = out;
        }
    cairo_surface_mark_dirty(tmp);
    cairo_pattern_t *p = cairo_pattern_create_for_surface(tmp);
    cairo_pattern_set_extend(p, CAIRO_EXTEND_REPEAT);
    cairo_pattern_set_filter(p, CAIRO_FILTER_NEAREST);
    cairo_matrix_t m;
    cairo_matrix_init_translate(&m, -x_origin, -y_origin);
    cairo_pattern_set_matrix(p, &m);
    cairo_save(c->cr);
    cairo_set_source(c->cr, p);
    cairo_set_operator(c->cr, fill_style == FillStippled
                              ? CAIRO_OPERATOR_OVER : CAIRO_OPERATOR_SOURCE);
    cairo_fill(c->cr);
    cairo_restore(c->cr);
    cairo_pattern_destroy(p);
    cairo_surface_destroy(tmp);
}

/* ------------------------------------------------------------- paths */

void mw_rect(MwCanvas *c, double x, double y, double w, double h)
{
    cairo_rectangle(c->cr, x + c->snap, y + c->snap, w, h);
}

void mw_set_snap(MwCanvas *c, int on)
{
    c->snap = on ? 0.5 : 0.0;
}

static void arc_path(cairo_t *cr, double x, double y, double w, double h,
                     double a1, double a2, int mode)
{
    double cx = x + w / 2.0, cy = y + h / 2.0;
    double rx = w / 2.0, ry = h / 2.0;
    /* X angles: 1/64 degree, CCW from +x, y grows down -> negate. */
    double s = -(a1) * M_PI / (64.0 * 180.0);
    double e = -(a1 + a2) * M_PI / (64.0 * 180.0);
    /* Each arc is its own sub-path: XDrawArcs/XFillArcs must not join one
     * arc to the previous with a line. */
    cairo_new_sub_path(cr);
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, rx, ry);
    /* X measures angles counter-clockwise; with y pointing down that is
     * decreasing cairo angles, so a positive extent runs "negative" in cairo
     * (cairo_arc would draw the complementary arc, and nothing at all for a
     * full circle). */
    if (a2 >= 0)
        cairo_arc_negative(cr, 0, 0, 1.0, s, e);
    else
        cairo_arc(cr, 0, 0, 1.0, s, e);
    cairo_restore(cr);
    if (mode == ArcChord /*1*/) {
        /* close the chord */
    } else if (mode == ArcPieSlice /*2*/) {
        /* path already starts on the ellipse; add center + close done by caller */
    }
}

void mw_arc(MwCanvas *c, double x, double y, double w, double h,
            double a1, double a2, int mode)
{
    /* A zero-size arc would make cairo_scale() singular, which puts the
     * cairo_t into an error state and drops everything else in the batch. */
    if (w <= 0 || h <= 0) return;
    if (mode == ArcPieSlice) {
        double cx = x + w / 2.0, cy = y + h / 2.0;
        cairo_move_to(c->cr, cx, cy);
        double s = -(a1) * M_PI / (64.0 * 180.0);
        double e = -(a1 + a2) * M_PI / (64.0 * 180.0);
        cairo_save(c->cr);
        cairo_translate(c->cr, cx, cy);
        cairo_scale(c->cr, w / 2.0, h / 2.0);
        if (a2 >= 0) cairo_arc_negative(c->cr, 0, 0, 1.0, s, e);
        else         cairo_arc(c->cr, 0, 0, 1.0, s, e);
        cairo_restore(c->cr);
        cairo_close_path(c->cr);
    } else {
        arc_path(c->cr, x, y, w, h, a1, a2, mode);
        if (mode == ArcChord) cairo_close_path(c->cr);
    }
}

void mw_polygon(MwCanvas *c, const XPoint *pts, int n, int relative, int filled)
{
    if (n <= 0) return;
    double x = pts[0].x + c->snap, y = pts[0].y + c->snap;
    cairo_move_to(c->cr, x, y);
    for (int i = 1; i < n; i++) {
        if (relative) { x += pts[i].x; y += pts[i].y; }
        else          { x = pts[i].x + c->snap; y = pts[i].y + c->snap; }
        cairo_line_to(c->cr, x, y);
    }
    if (filled) cairo_close_path(c->cr);
}

void mw_segments(MwCanvas *c, const XSegment *seg, int n)
{
    for (int i = 0; i < n; i++) {
        cairo_move_to(c->cr, seg[i].x1 + c->snap, seg[i].y1 + c->snap);
        cairo_line_to(c->cr, seg[i].x2 + c->snap, seg[i].y2 + c->snap);
    }
}

void mw_rects(MwCanvas *c, const XRectangle *r, int n, int filled)
{
    for (int i = 0; i < n; i++)
        cairo_rectangle(c->cr, r[i].x, r[i].y, r[i].width, r[i].height);
    (void)filled;
}

/* ------------------------------------------------------------- pixels */

/* XCopyArea with a raster op: dst = op(src, dst) per physical pixel, inside
 * the clip.  The source is read (snapshotted) before anything is written,
 * so overlapping self-copies work. */
static void copy_rop(MwCanvas *dst, MwSurface *src, int sx, int sy,
                     int dx, int dy, int w, int h)
{
    MwSurface *ds = dst->surface;
    int x0, y0, x1, y1;
    if (!clip_bounds(dst, dx, dy, dx + w, dy + h, &x0, &y0, &x1, &y1)) return;
    int dsc = ds->scale, ssc = src->scale;
    int bw = x1 - x0, bh = y1 - y0;
    uint32_t *snap = malloc((size_t)bw * bh * 4);
    if (!snap) return;
    cairo_surface_flush(src->cs);
    const uint32_t *sp = (const uint32_t *)cairo_image_surface_get_data(src->cs);
    int sstr = cairo_image_surface_get_stride(src->cs) / 4;
    int spw = src->w * ssc, sph = src->h * ssc;
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) {
            /* destination physical -> source physical */
            int lx = (x0 + x) - dx * dsc, ly = (y0 + y) - dy * dsc;
            int qx = sx * ssc + lx * ssc / dsc, qy = sy * ssc + ly * ssc / dsc;
            snap[(size_t)y * bw + x] =
                (qx >= 0 && qy >= 0 && qx < spw && qy < sph)
                ? sp[(size_t)qy * sstr + qx] : 0;
        }
    /* Coverage of the clip over the box. */
    cairo_surface_t *m = cairo_image_surface_create(CAIRO_FORMAT_A8, bw, bh);
    cairo_surface_set_device_scale(m, dsc, dsc);
    cairo_surface_set_device_offset(m, -x0, -y0);
    cairo_t *mc = cairo_create(m);
    copy_clip(dst, mc);
    cairo_set_source_rgba(mc, 0, 0, 0, 1);
    cairo_paint(mc);
    cairo_destroy(mc);
    cairo_surface_flush(m);
    const unsigned char *md = cairo_image_surface_get_data(m);
    int ms = cairo_image_surface_get_stride(m);
    cairo_surface_flush(ds->cs);
    uint32_t *dp = (uint32_t *)cairo_image_surface_get_data(ds->cs);
    int dstr = cairo_image_surface_get_stride(ds->cs) / 4;
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) {
            if (md[(size_t)y * ms + x] < 0x80) continue;
            uint32_t *p = &dp[(size_t)(y0 + y) * dstr + (x0 + x)];
            *p = rop_apply_px(dst, snap[(size_t)y * bw + x], *p);
        }
    cairo_surface_mark_dirty(ds->cs);
    cairo_surface_destroy(m);
    free(snap);
}

void mw_canvas_copy(MwCanvas *dst, MwSurface *src, int sx, int sy,
                    int dx, int dy, int w, int h)
{
    if (!dst || !src || w <= 0 || h <= 0) return;
    if (canvas_uses_rop(dst)) { copy_rop(dst, src, sx, sy, dx, dy, w, h); return; }

    /* Copying a surface onto itself (XCopyArea used for scrolling, which the
     * Motif text widget does constantly) is undefined in cairo.  Snapshot the
     * source region first. */
    if (src == dst->surface) {
        /* The snapshot must be on the same device-scale grid as the source,
         * otherwise cairo samples the (physical) source at logical pitch and
         * silently downsamples it. */
        int ssc = src->scale > 0 ? src->scale : 1;
        cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                          w * ssc, h * ssc);
        if (ssc > 1) cairo_surface_set_device_scale(tmp, ssc, ssc);
        cairo_t *tc = cairo_create(tmp);
        cairo_set_operator(tc, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_surface(tc, src->cs, -sx, -sy);
        cairo_paint(tc);
        cairo_destroy(tc);
        cairo_surface_flush(tmp);

        cairo_save(dst->cr);
        cairo_rectangle(dst->cr, dx, dy, w, h);
        cairo_clip(dst->cr);
        cairo_set_operator(dst->cr, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_surface(dst->cr, tmp, dx, dy);
        cairo_paint(dst->cr);
        cairo_restore(dst->cr);
        cairo_surface_destroy(tmp);
        return;
    }

    cairo_save(dst->cr);
    cairo_rectangle(dst->cr, dx, dy, w, h);
    cairo_clip(dst->cr);
    cairo_set_source_surface(dst->cr, src->cs, dx - sx, dy - sy);
    cairo_set_operator(dst->cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(dst->cr);
    cairo_restore(dst->cr);
}

void mw_canvas_put_image(MwCanvas *c, MwSurface *img, int dx, int dy,
                         int sx, int sy, int w, int h)
{
    mw_canvas_copy(c, img, sx, sy, dx, dy, w, h);
}

/* XCopyPlane: bit `bit` of each source pixel selects the foreground (set) or
 * the background (clear); the result is copied like XCopyArea, so the GC's
 * raster op and clip apply. */
void mw_canvas_copy_plane(MwCanvas *c, MwSurface *src, int sx, int sy,
                          int dx, int dy, int w, int h, int bit,
                          uint32_t fg, uint32_t bg)
{
    if (!c || !src || w <= 0 || h <= 0 || bit < 0 || bit > 31) return;
    MwSurface *tmp = mw_surface_create(w, h);
    if (!tmp) return;
    uint32_t *td = (uint32_t *)cairo_image_surface_get_data(tmp->cs);
    int ts = cairo_image_surface_get_stride(tmp->cs) / 4;
    cairo_surface_flush(src->cs);
    const uint32_t *sp = (const uint32_t *)cairo_image_surface_get_data(src->cs);
    int sstride = cairo_image_surface_get_stride(src->cs) / 4;
    int ssc = src->scale > 0 ? src->scale : 1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t px = sp[(size_t)((sy + y) * ssc) * sstride +
                             (size_t)((sx + x) * ssc)];
            td[(size_t)y * ts + x] = 0xff000000u |
                (((px >> bit) & 1u) ? (fg & 0xffffff) : (bg & 0xffffff));
        }
    cairo_surface_mark_dirty(tmp->cs);
    mw_canvas_copy(c, tmp, 0, 0, dx, dy, w, h);
    mw_surface_destroy(tmp);
}

void mw_surface_get(MwSurface *s, int x, int y, int w, int h,
                    uint32_t *out, int out_stride_px)
{
    if (!s || !out) return;
    cairo_surface_flush(s->cs);
    const uint32_t *sp = (const uint32_t *)s->data;
    int sstride = s->stride / 4;
    int sc = s->scale > 0 ? s->scale : 1;
    int pw = s->w * sc, ph = s->h * sc;
    /* x,y,w,h are logical (the caller works in drawable coordinates).  On a
     * device-scaled surface each logical pixel covers a sc x sc physical block;
     * return its top-left physical pixel. */
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int sx = (x + i) * sc, sy = (y + j) * sc;
            uint32_t px = 0;
            if (sx >= 0 && sy >= 0 && sx < pw && sy < ph)
                px = sp[(size_t)sy * sstride + sx];
            out[(size_t)j * out_stride_px + i] = px;
        }
    }
}

/* ------------------------------------------------------------- fonts */

struct MwFont {
    cairo_font_face_t  *face;
    cairo_scaled_font_t *scaled;
    FT_Face             ft;      /* owned by cairo (it resizes this face) */
    bool                ft_owned_by_cairo; /* freed with `face` (user data) */
    FT_Face             ft_m;    /* private face for stable metrics */
    int                 pixel_size;
    int                 ascent, descent, max_width;
    int                 wcache[256];   /* lazily computed advance widths */
};

static FT_Library g_ft;
static int        g_ft_ready;

int mw_raster_init(void)
{
    if (!g_ft_ready) {
        if (FT_Init_FreeType(&g_ft) != 0) return -1;
        FcInit();
        g_ft_ready = 1;
    }
    return 0;
}

void mw_raster_fini(void)
{
    if (g_ft_ready) { FT_Done_FreeType(g_ft); g_ft = NULL; g_ft_ready = 0; }
}

/* Parse an XLFD's pixel size (field 7) and family (field 2). */
static void parse_xlfd(const char *pat, char *family, size_t famsz, int *px)
{
    family[0] = 0; *px = 0;
    /* XLFD: "-foundry-family-weight-slant-setwidth-addstyle-pixelsize-...".
     * Fields may be empty (consecutive '-'), so we must NOT collapse them the
     * way strtok does. */
    if (!pat || pat[0] != '-') {
        /* Not an XLFD: treat the whole string as a family name. */
        if (pat && *pat) snprintf(family, famsz, "%s", pat);
        return;
    }
    const char *p = pat + 1;              /* skip leading '-' */
    const char *field[14];
    size_t flen[14];
    int nf = 0;
    while (nf < 14) {
        field[nf] = p;
        const char *q = strchr(p, '-');
        if (!q) { flen[nf] = strlen(p); nf++; break; }
        flen[nf] = (size_t)(q - p);
        nf++;
        p = q + 1;
    }
    /* field[0]=foundry, [1]=family, [6]=pixelsize */
    if (nf > 1 && flen[1] > 0 &&
        !(flen[1] == 1 && field[1][0] == '*'))
        snprintf(family, famsz, "%.*s", (int)flen[1], field[1]);
    if (nf > 6 && flen[6] > 0 && !(flen[6] == 1 && field[6][0] == '*')) {
        long v = strtol(field[6], NULL, 10);
        if (v >= 1 && v <= 200) *px = (int)v;
    }
}

static FcPattern *font_match(FcPattern *pat)
{
    FcConfigSubstitute(NULL, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    FcPattern *match = FcFontMatch(NULL, pat, &res);
    FcPatternDestroy(pat);
    return match;
}

/* Build the raster font from a matched fontconfig pattern. */
static MwFont *font_from_match(FcPattern *match, int px_hint, int antialias)
{
    if (!match) return NULL;
    int px = px_hint > 0 ? px_hint : 14;

    FcChar8 *file = NULL;
    int index = 0;
    FcPatternGetString(match, FC_FILE, 0, &file);
    FcPatternGetInteger(match, FC_INDEX, 0, &index);
    double mpx = px;
    FcPatternGetDouble(match, FC_PIXEL_SIZE, 0, &mpx);

    MwFont *f = calloc(1, sizeof *f);
    for (int i = 0; i < 256; i++) f->wcache[i] = -1;
    f->pixel_size = (int)(mpx + 0.5);
    if (!file || FT_New_Face(g_ft, (const char *)file, index, &f->ft) != 0) {
        /* fallback: default sans */
        if (FT_New_Face(g_ft, "/usr/share/fonts/TTF/DejaVuSans.ttf", 0, &f->ft) != 0) {
            /* last resort: whatever fontconfig has, else give up */
        }
    }
    if (f->ft) {
        FT_Set_Pixel_Sizes(f->ft, 0, f->pixel_size);
        f->face = cairo_ft_font_face_create_for_ft_face(f->ft, 0);
        /* cairo can keep the font face (and use the FT_Face) after we drop
         * our reference -- its font caches hold faces past the destroy -- so,
         * as cairo documents, the FT_Face is freed when the face itself is,
         * not by mw_font_destroy. */
        static const cairo_user_data_key_t ft_key;
        if (f->face && cairo_font_face_set_user_data(f->face, &ft_key, f->ft,
                (cairo_destroy_func_t)FT_Done_Face) == CAIRO_STATUS_SUCCESS)
            f->ft_owned_by_cairo = true;
    } else {
        f->face = cairo_toy_font_face_create("sans", CAIRO_FONT_SLANT_NORMAL,
                                             CAIRO_FONT_WEIGHT_NORMAL);
    }
    /* A private face at exactly pixel_size, never handed to cairo, so wl_ and
     * XTextWidth metrics are stable and correct. */
    if (file) {
        if (FT_New_Face(g_ft, (const char *)file, index, &f->ft_m) != 0)
            f->ft_m = NULL;
    }
    if (!f->ft_m) f->ft_m = f->ft;   /* fallback: shared face */
    if (f->ft_m) FT_Set_Pixel_Sizes(f->ft_m, 0, f->pixel_size);
    FcPatternDestroy(match);

    /* font matrix carries the size; the CTM is identity (our canvases are
     * 1:1), otherwise the effective size becomes pixel_size * pixel_size. */
    cairo_matrix_t fm, ctm;
    cairo_matrix_init_scale(&fm, f->pixel_size, f->pixel_size);
    cairo_matrix_init_identity(&ctm);
    cairo_font_options_t *opt = cairo_font_options_create();
    /* Hinting would change glyph advances away from the unhinted metrics we
     * report to the client, causing text to drift/overlap when the client
     * positions runs using XTextWidth.  Keep them consistent. */
    cairo_font_options_set_antialias(opt,
        antialias ? CAIRO_ANTIALIAS_GRAY : CAIRO_ANTIALIAS_NONE);
    cairo_font_options_set_hint_style(opt, CAIRO_HINT_STYLE_NONE);
    cairo_font_options_set_hint_metrics(opt, CAIRO_HINT_METRICS_OFF);
    f->scaled = cairo_scaled_font_create(f->face, &fm, &ctm, opt);
    cairo_font_options_destroy(opt);

    if (f->ft_m && f->ft_m->size) {
        f->ascent  = (int)ceil(f->ft_m->size->metrics.ascender / 64.0);
        f->descent = (int)ceil(-f->ft_m->size->metrics.descender / 64.0);
        int mw = 0;
        for (unsigned ch = 32; ch < 127; ch++) {
            FT_UInt gi = FT_Get_Char_Index(f->ft_m, ch);
            if (!gi) continue;
            if (FT_Load_Glyph(f->ft_m, gi, FT_LOAD_NO_HINTING|FT_LOAD_NO_BITMAP) == 0) {
                int adv = (int)ceil(f->ft_m->glyph->metrics.horiAdvance / 64.0);
                if (adv > mw) mw = adv;
            }
        }
        f->max_width = mw > 0 ? mw : f->pixel_size;
    } else {
        cairo_font_extents_t fe;
        cairo_scaled_font_extents(f->scaled, &fe);
        f->ascent  = (int)ceil(fe.ascent);
        f->descent = (int)ceil(fe.descent);
        f->max_width = (int)ceil(fe.max_x_advance);
    }
    if (f->max_width <= 0) f->max_width = f->pixel_size;
    return f;
}

MwFont *mw_font_create(const char *pattern, int pixel_size_hint)
{
    if (!g_ft_ready) mw_raster_init();
    char family[256]; int px = 0;
    parse_xlfd(pattern, family, sizeof family, &px);
    if (px <= 0) px = pixel_size_hint > 0 ? pixel_size_hint : 14;
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: font '%s' -> family='%s' px=%d\n",
                pattern ? pattern : "(null)", family, px);
    FcPattern *pat = FcPatternCreate();
    if (family[0] && strcasecmp(family, "fixed") == 0)
        FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)"monospace");
    else if (family[0])
        FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)family);
    FcPatternAddDouble(pat, FC_PIXEL_SIZE, (double)px);
    FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
    /* Grayscale antialiasing for core fonts too.  It changes only the raster,
     * not the advances (hinting stays off on both paths), so the widths we
     * report through XTextWidth still match what we draw -- which is what the
     * no-antialias rule was protecting.  Set MW_NO_AA=1 for the old flat look. */
    return font_from_match(font_match(pat), px, getenv("MW_NO_AA") ? 0 : 1);
}

/* The Xft path: match the caller's own fontconfig pattern so weight, slant and
 * family are honoured, and keep grayscale smoothing on. */
MwFont *mw_font_create_fc(const void *fcpat, int pixel_size_hint, int antialias)
{
    if (!g_ft_ready) mw_raster_init();
    FcPattern *pat = fcpat ? FcPatternDuplicate((FcPattern *)fcpat)
                           : FcPatternCreate();
    if (!pat) return NULL;
    double have = 0;
    if (FcPatternGetDouble(pat, FC_PIXEL_SIZE, 0, &have) != FcResultMatch &&
        pixel_size_hint > 0)
        FcPatternAddDouble(pat, FC_PIXEL_SIZE, (double)pixel_size_hint);
    return font_from_match(font_match(pat), pixel_size_hint, antialias);
}

void mw_font_destroy(MwFont *f)
{
    if (!f) return;
    if (f->scaled) cairo_scaled_font_destroy(f->scaled);
    if (f->face)   cairo_font_face_destroy(f->face);
    if (f->ft_m && f->ft_m != f->ft) FT_Done_Face(f->ft_m);
    if (f->ft && !f->ft_owned_by_cairo) FT_Done_Face(f->ft);
    free(f);
}

int mw_font_ascent(const MwFont *f)    { return f ? f->ascent : 0; }
int mw_font_descent(const MwFont *f)   { return f ? f->descent : 0; }
int mw_font_max_width(const MwFont *f) { return f ? f->max_width : 0; }


/* Metrics are computed with the TrueType hinting bytecode disabled: it is a
 * significant cost (and needless) for advance widths, and some fonts run it
 * very slowly. Rendering still uses cairo/FreeType with normal hinting. */
static int ft_load_metrics(MwFont *f, unsigned int ch)
{
    if (!f || !f->ft_m) return 0;
    FT_UInt gi = FT_Get_Char_Index(f->ft_m, (FT_ULong)ch);
    if (!gi) return 0;
    return FT_Load_Glyph(f->ft_m, gi, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) == 0;
}

int mw_font_char_width(const MwFont *f, unsigned int ch)
{
    MwFont *m = (MwFont *)f;
    if (!m || !m->ft_m) return 0;
    if (ch < 256 && m->wcache[ch] >= 0) return m->wcache[ch];
    int adv = 0;
    if (ft_load_metrics(m, ch))
        adv = (int)ceil(m->ft_m->glyph->metrics.horiAdvance / 64.0);
    if (ch < 256) m->wcache[ch] = adv;
    return adv;
}

int mw_font_char_index(const MwFont *f, unsigned int ch)
{
    const MwFont *m = f;
    if (!m || !m->ft_m) return 0;
    return (int)FT_Get_Char_Index(m->ft_m, ch);
}

void *mw_font_ft_face(const MwFont *f) { return f ? (void *)f->ft_m : NULL; }

int mw_font_char_lbearing(const MwFont *f, unsigned int ch)
{
    MwFont *m = (MwFont *)f;
    if (!m || !m->ft_m || !ft_load_metrics(m, ch)) return 0;
    return (int)floor(m->ft_m->glyph->metrics.horiBearingX / 64.0);
}

int mw_font_char_rbearing(const MwFont *f, unsigned int ch)
{
    MwFont *m = (MwFont *)f;
    if (!m || !m->ft_m || !ft_load_metrics(m, ch)) return 0;
    double adv = m->ft_m->glyph->metrics.horiAdvance / 64.0;
    double bx  = m->ft_m->glyph->metrics.horiBearingX / 64.0;
    double bw  = m->ft_m->glyph->metrics.width / 64.0;
    return (int)ceil(adv - bx - bw);
}

int mw_font_text_width_utf8(const MwFont *f, const char *s, int len)
{
    MwFont *m = (MwFont *)f;
    if (!m || !s) return 0;
    if (len < 0) len = (int)strlen(s);
    int total = 0, i = 0;
    while (i < len) {
        unsigned int ch = (unsigned char)s[i++];
        if (ch >= 0xF0 && i + 2 < len) { ch = ((ch & 7) << 18) | ((s[i]&0x3F)<<12) | ((s[i+1]&0x3F)<<6) | (s[i+2]&0x3F); i += 3; }
        else if (ch >= 0xE0 && i + 1 < len) { ch = ((ch & 0xF) << 12) | ((s[i]&0x3F)<<6) | (s[i+1]&0x3F); i += 2; }
        else if (ch >= 0xC0 && i < len) { ch = ((ch & 0x1F) << 6) | (s[i]&0x3F); i += 1; }
        total += mw_font_char_width(m, ch);
    }
    return total;
}

void mw_set_font(MwCanvas *c, MwFont *f)
{
    if (!f || !f->scaled) return;
    cairo_set_scaled_font(c->cr, f->scaled);
}

/* Decode one UTF-8 character; returns its length in bytes. */
static int utf8_next(const char *s, int len, int i, unsigned int *cp)
{
    unsigned char b = (unsigned char)s[i];
    int n = 1;
    unsigned int v;

    if (b >= 0xF0) { n = 4; v = b & 0x07u; }
    else if (b >= 0xE0) { n = 3; v = b & 0x0Fu; }
    else if (b >= 0xC0) { n = 2; v = b & 0x1Fu; }
    else { *cp = b; return 1; }
    if (i + n > len) { *cp = b; return 1; }
    for (int k = 1; k < n; k++)
        v = (v << 6) | ((unsigned char)s[i + k] & 0x3Fu);
    *cp = v;
    return n;
}

void mw_show_utf8(MwCanvas *c, MwFont *f, const char *s, int len, int x, int y)
{
    if (!c || !f || !s) return;

    if (f->scaled) cairo_set_scaled_font(c->cr, f->scaled);
    else { cairo_set_font_face(c->cr, f->face); cairo_set_font_size(c->cr, f->pixel_size); }
    if (len < 0) len = (int)strlen(s);

    /* Advance one character at a time using the same width XTextWidth()
     * reports.  Letting cairo lay out the whole string itself drifts from that
     * width (cairo uses unhinted advances, XTextWidth rounds each character
     * up), and Motif places the text cursor and its erase boxes from
     * XTextWidth -- so the cursor drifted and deleting a character could leave
     * it behind on screen.
     *
     * The positions are therefore computed here and handed to cairo as one
     * glyph array.  Drawing with one cairo_show_text per character was correct
     * but far too slow: a full-window repaint (which is what a resize causes)
     * issued thousands of calls and blocked the application for seconds. */
    cairo_glyph_t stack_glyphs[256];
    cairo_glyph_t *glyphs = stack_glyphs;
    if (len > (int)(sizeof stack_glyphs / sizeof stack_glyphs[0])) {
        glyphs = malloc(sizeof *glyphs * (size_t)len);
        if (!glyphs) return;
    }
    int ng = 0;
    double px = x;
    for (int i = 0; i < len; ) {
        unsigned int cp = 0;
        int n = utf8_next(s, len, i, &cp);
        if (f->ft_m) {
            FT_UInt gi = FT_Get_Char_Index(f->ft_m, cp);
            if (gi != 0) {
                glyphs[ng].index = gi;
                glyphs[ng].x = px;
                glyphs[ng].y = y;
                ng++;
            }
        }
        px += mw_font_char_width(f, cp);
        i += n;
    }
    if (ng) cairo_show_glyphs(c->cr, glyphs, ng);
    if (glyphs != stack_glyphs) free(glyphs);
}

void mw_show_glyphs(MwCanvas *c, MwFont *f, const unsigned int *glyphs,
                    const int *xs, const int *ys, int n)
{
    if (!c || !f || !glyphs || n <= 0) return;
    if (f->scaled) cairo_set_scaled_font(c->cr, f->scaled);
    else { cairo_set_font_face(c->cr, f->face); cairo_set_font_size(c->cr, f->pixel_size); }

    cairo_glyph_t stack_glyphs[256];
    cairo_glyph_t *g = stack_glyphs;
    if (n > (int)(sizeof stack_glyphs / sizeof stack_glyphs[0])) {
        g = malloc(sizeof *g * (size_t)n);
        if (!g) return;
    }
    int ng = 0;
    for (int i = 0; i < n; i++) {
        if (glyphs[i] == 0) continue;      /* glyph 0 is .notdef */
        g[ng].index = glyphs[i];
        g[ng].x = xs ? xs[i] : 0.0;
        g[ng].y = ys ? ys[i] : 0.0;
        ng++;
    }
    if (ng) cairo_show_glyphs(c->cr, g, ng);
    if (g != stack_glyphs) free(g);
}
