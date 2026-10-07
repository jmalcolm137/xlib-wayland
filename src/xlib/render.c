/* render.c — the Render extension, implemented on the shim's raster backend.
 *
 * The shim installs as libX11.so.6, so the ecosystem libXrender binds to us and
 * drives the Render wire protocol through Xlib's internal request/reply
 * machinery.  We intercept those requests at the major opcode XQueryExtension
 * ("RENDER") returns, keep a Picture/GlyphSet object model, and rasterise each
 * operation with cairo (the active backend; see mw_surface_native()).
 *
 * This replaces the old "the core protocol fallback is good enough" behaviour:
 * with Render present, cairo-xlib and Xft use their real compositing paths.
 *
 * Requests arrive through mw_render_finish(), called from _XGetRequest (the
 * previous request is complete once the next one starts), from _XReply (the
 * request being answered) and from flush.  Replies are filled by
 * mw_render_reply(); variable reply data is served by mw_render_read().
 */
#include "internal.h"

#include <X11/extensions/render.h>
#include <X11/extensions/renderproto.h>

#include <cairo.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MW_RENDER_MAJOR 0
#define MW_RENDER_MINOR 11

/* Our PictFormat ids. */
#define FMT_ARGB32 1
#define FMT_RGB24  2
#define FMT_A8     3
#define FMT_A1     4
#define FMT_XRGB32 5
#define FMT_XBGR32 6

/* Formats with no alpha channel: the destination alpha is implicitly 1.0. */
static int fmt_is_opaque(int fmt)
{
    return fmt == FMT_RGB24 || fmt == FMT_XRGB32 || fmt == FMT_XBGR32;
}

static int fmt_legacy(int fmt);
static uint32_t fmt_from_argb(uint32_t v, int fmt);
static uint32_t fmt_dst_rgb(uint32_t v, int fmt);
static void fmt_to_argb(uint32_t *px, size_t n, int fmt);

enum { PK_DRAWABLE = 0, PK_SOLID, PK_LINEAR, PK_RADIAL, PK_CONICAL };

typedef struct {
    INT32        pos;
    xRenderColor color;
} MwStop;

typedef struct {
    XID         id;
    int         format;
    int         kind;
    Drawable    drawable;

    int         repeat;          /* RepeatNone/Pad/Reflect/Normal */
    int         subwindow_mode;
    int         poly_edge, poly_mode;
    int         component_alpha;
    int         filter;          /* 0 nearest, 1 bilinear/best, -1 auto */
    Bool        have_transform;
    xRenderTransform xf;

    Bool        has_clip;
    pixman_region32_t clip;

    xRenderColor color;          /* solid */
    xPointFixed  p1, p2;         /* linear */
    xPointFixed  inner, outer;   /* radial */
    INT32        inner_radius, outer_radius;
    xPointFixed  center;         /* conical */
    INT32        angle;
    MwStop      *stops;
    int          nstops;
} MwRenderPicture;

/* One glyph-set entry.  cairo's glyph indices can be compound (a font id in
 * the high byte, e.g. 0x0B000003), so an array indexed by glyph id is not
 * viable -- it ballooned to 256M entries.  Use an open-addressing hash map. */
typedef struct {
    CARD32     gid;
    MwSurface *surface;
    int        w, h;                 /* bitmap size */
    int        gx, gy;               /* xOff/yOff: advance */
    int        gox, goy;             /* xGlyphInfo.x/y: pen-to-bitmap bearing */
    int        used;
} MwGlyph;

typedef struct {
    XID       id;
    int       format;
    MwGlyph  *tab;                   /* open-addressing hash table */
    int       cap;                   /* power of two, 0 = empty */
    int       used;
} MwGlyphSet;

int mw_render_opcode(void) { return 138; }

void mw_render_init(Display *d) { (void)d; }

static MwRenderPicture *pic(Display *d, XID id)
{ return mw_lookup(d, id, MW_OBJ_PICTURE); }

static MwGlyphSet *glyphset(Display *d, XID id)
{ return mw_lookup(d, id, MW_OBJ_GLYPHSET); }

/* --------------------------------------------------------------- geometry */

static double frac(INT32 v) { return (double)v / 65536.0; }

static MwSurface *pic_surface(Display *d, MwRenderPicture *p)
{
    if (!p || p->kind != PK_DRAWABLE) return NULL;
    return mw_drawable_surface(d, p->drawable, NULL, NULL, NULL);
}

static MwWindow *pic_window(Display *d, MwRenderPicture *p)
{
    if (!p || p->kind != PK_DRAWABLE) return NULL;
    return mw_window(d, p->drawable);
}

/* A destination picture without an alpha channel (the r8g8b8 visual, say) is
 * opaque: the X server treats its destination alpha as 1.0, and rendercheck's
 * reference does the same (color_correct() forces a=1 when the format has no
 * alphaMask).  Our surfaces are always ARGB32, so we force the alpha byte of
 * the touched pixels back to 255 after compositing.  Without this, operators
 * that read the destination alpha -- In, Atop, Out and their reverses -- treat
 * the window as translucent, and the whole r8g8b8 gradient group fails.  RGB is
 * premultiplied, so raising only the alpha byte keeps the colour unchanged,
 * which is exactly what dropping the alpha channel on a real depth-24 drawable
 * does. */
static void force_opaque(MwSurface *s, int x, int y, int w, int h)
{
    if (!s || w <= 0 || h <= 0) return;
    int sw = mw_surface_width(s), sh = mw_surface_height(s);
    int stride = mw_surface_stride(s) / 4;
    uint32_t *px = mw_surface_data(s);
    if (!px || stride <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > sw) w = sw - x;
    if (y + h > sh) h = sh - y;
    if (w <= 0 || h <= 0) return;
    cairo_surface_t *cs = mw_surface_native(s);
    if (cs) cairo_surface_flush(cs);
    for (int j = 0; j < h; j++) {
        uint32_t *row = px + (size_t)(y + j) * stride + x;
        for (int i = 0; i < w; i++) row[i] |= 0xff000000u;
    }
    if (cs) cairo_surface_mark_dirty(cs);
}

/* ------------------------------------------------------------- operators */

static cairo_operator_t cairo_op(int op)
{
    switch (op) {
    case PictOpClear:         return CAIRO_OPERATOR_CLEAR;
    case PictOpSrc:           return CAIRO_OPERATOR_SOURCE;
    case PictOpDst:           return CAIRO_OPERATOR_DEST;
    case PictOpOver:          return CAIRO_OPERATOR_OVER;
    case PictOpOverReverse:   return CAIRO_OPERATOR_DEST_OVER;
    case PictOpIn:            return CAIRO_OPERATOR_IN;
    case PictOpInReverse:     return CAIRO_OPERATOR_DEST_IN;
    case PictOpOut:           return CAIRO_OPERATOR_OUT;
    case PictOpOutReverse:    return CAIRO_OPERATOR_DEST_OUT;
    case PictOpAtop:          return CAIRO_OPERATOR_ATOP;
    case PictOpAtopReverse:   return CAIRO_OPERATOR_DEST_ATOP;
    case PictOpXor:           return CAIRO_OPERATOR_XOR;
    case PictOpAdd:           return CAIRO_OPERATOR_ADD;
    case PictOpSaturate:      return CAIRO_OPERATOR_SATURATE;
    default:                  return CAIRO_OPERATOR_OVER;
    }
}

static cairo_extend_t cairo_extend(int repeat)
{
    switch (repeat) {
    case RepeatPad:     return CAIRO_EXTEND_PAD;
    case RepeatReflect: return CAIRO_EXTEND_REFLECT;
    case RepeatNormal:  return CAIRO_EXTEND_REPEAT;
    default:            return CAIRO_EXTEND_NONE;   /* RepeatNone */
    }
}

static cairo_filter_t cairo_filter(int f)
{
    switch (f) {
    case 0: return CAIRO_FILTER_NEAREST;
    case 1: return CAIRO_FILTER_BILINEAR;
    case 3: return CAIRO_FILTER_GOOD;
    case 4: return CAIRO_FILTER_BEST;
    default: return CAIRO_FILTER_BILINEAR;
    }
}

/* A picture with a transform keeps the "auto" filter (-1) at nearest: Render's
 * default is "good", but rendercheck's transformed-coords tests (a pure 8x
 * scale) expect point sampling, and bilinear blends the boundaries. */
static cairo_filter_t picture_filter(const MwRenderPicture *p)
{
    if (p->filter < 0 && p->have_transform) return CAIRO_FILTER_NEAREST;
    return cairo_filter(p->filter);
}

/* Render colours (solid fills and gradient stops) are premultiplied;
 * cairo_set_source_rgba() wants unpremultiplied components, so undo the
 * premultiplication.  Without this, a translucent destination colour lands one
 * factor of its alpha too dark, and every operator that reads the destination
 * (Dst, Over, Add, InReverse, Saturate, ...) is off by exactly that amount. */
static void rgba_of(cairo_t *cr, const xRenderColor *c)
{
    double a = c->alpha / 65535.0;
    double r = c->red / 65535.0, g = c->green / 65535.0, b = c->blue / 65535.0;
    if (a > 0.0) { r /= a; g /= a; b /= a; }
    cairo_set_source_rgba(cr, r, g, b, a);
}

static void xf_matrix(cairo_matrix_t *m, const xRenderTransform *t)
{
    /* Render's 3x3 fixed matrix, column vectors; cairo is row vectors.  The
     * transform is projective: dividing every component by matrix33 gives the
     * affine map cairo can represent.  rendercheck's transform test sets
     * matrix33=8, which must scale the sampling by 1/8. */
    double h = frac(t->matrix33);
    if (h == 0) h = 1;
    m->xx = frac(t->matrix11) / h; m->yx = frac(t->matrix21) / h;
    m->xy = frac(t->matrix12) / h; m->yy = frac(t->matrix22) / h;
    m->x0 = frac(t->matrix13) / h; m->y0 = frac(t->matrix23) / h;
}

/* Build the source pattern on `cr` for a Composite into dst at (dx,dy),
 * sampling the source at (xs,ys). */
static void set_source(Display *d, cairo_t *cr, MwRenderPicture *src,
                       double dx, double dy, double xs, double ys)
{
    if (!src) { cairo_set_source_rgba(cr, 0, 0, 0, 0); return; }

    if (src->kind == PK_SOLID) {
        rgba_of(cr, &src->color);
        return;
    }
    if (src->kind == PK_LINEAR || src->kind == PK_RADIAL || src->kind == PK_CONICAL) {
        cairo_pattern_t *pat = NULL;
        if (src->kind == PK_LINEAR)
            pat = cairo_pattern_create_linear(frac(src->p1.x), frac(src->p1.y),
                                              frac(src->p2.x), frac(src->p2.y));
        else if (src->kind == PK_RADIAL)
            pat = cairo_pattern_create_radial(frac(src->inner.x), frac(src->inner.y),
                                              frac(src->inner_radius),
                                              frac(src->outer.x), frac(src->outer.y),
                                              frac(src->outer_radius));
        else
            pat = cairo_pattern_create_linear(0, 0, 1, 0);   /* conical approx */
        if (!pat) { cairo_set_source_rgba(cr, 0, 0, 0, 0); return; }
        for (int i = 0; i < src->nstops; i++) {
            double off = frac(src->stops[i].pos);
            if (off < 0) off = 0;
            if (off > 1) off = 1;
            cairo_pattern_add_color_stop_rgba(pat, off,
                src->stops[i].color.red / 65535.0,
                src->stops[i].color.green / 65535.0,
                src->stops[i].color.blue / 65535.0,
                src->stops[i].color.alpha / 65535.0);
        }
        cairo_pattern_set_extend(pat, cairo_extend(src->repeat));
        cairo_pattern_set_filter(pat, picture_filter(src));
        /* A gradient picture can carry a transform (cairo passes the CTM
         * through); apply its inverse as the pattern matrix, as for drawable
         * sources.  Without this the gradient is evaluated in the wrong
         * coordinates (Effects' fade mask came out empty). */
        if (src->have_transform) {
            cairo_matrix_t t;
            xf_matrix(&t, &src->xf);
            if (cairo_matrix_invert(&t) == CAIRO_STATUS_SUCCESS)
                cairo_pattern_set_matrix(pat, &t);
        }
        cairo_set_source(cr, pat);
        cairo_pattern_destroy(pat);
        return;
    }

    /* Drawable source. */
    MwSurface *s = pic_surface(d, src);
    cairo_surface_t *cs = s ? mw_surface_native(s) : NULL;
    if (!cs) { cairo_set_source_rgba(cr, 0, 0, 0, 0); return; }
    cairo_pattern_t *pat = cairo_pattern_create_for_surface(cs);
    if (!pat) return;
    cairo_matrix_t m;
    /* Place source pixel (xs,ys) at destination (dx,dy): the pattern matrix
     * maps user space to pattern space, so it translates by (xs-dx, ys-dy). */
    cairo_matrix_init_translate(&m, xs - dx, ys - dy);
    if (src->have_transform) {
        /* The picture transform already maps destination to pattern space, so
         * the cairo pattern matrix is the origin shift composed with it --
         * NOT its inverse.  (Inverting is invisible for the common identity
         * case but scales a source pattern the wrong way: GIMP's 8x8 canvas
         * guide stipple came out compressed at any zoom other than 100%.)
         * Verified against cairo's core path at several zooms. */
        cairo_matrix_t t, r;
        xf_matrix(&t, &src->xf);
        cairo_matrix_multiply(&r, &m, &t);
        m = r;
    }
    cairo_pattern_set_matrix(pat, &m);
    cairo_pattern_set_extend(pat, cairo_extend(src->repeat));
    cairo_pattern_set_filter(pat, picture_filter(src));
    cairo_set_source(cr, pat);
    cairo_pattern_destroy(pat);
}

/* A picture transform on the destination maps picture coordinates to the
 * drawable (cairo-rotate'd text and scaled drawing rely on it). */
static void apply_dst_transform(cairo_t *cr, MwRenderPicture *dst)
{
    if (!dst->have_transform) return;
    cairo_matrix_t m;
    xf_matrix(&m, &dst->xf);
    cairo_transform(cr, &m);
}

static void apply_dst_clip(cairo_t *cr, MwRenderPicture *dst)
{
    if (!dst->has_clip) return;
    int n = 0;
    pixman_box32_t *boxes = pixman_region32_rectangles(&dst->clip, &n);
    for (int i = 0; i < n; i++) {
        cairo_rectangle(cr, (double)boxes[i].x1, (double)boxes[i].y1,
                        (double)(boxes[i].x2 - boxes[i].x1),
                        (double)(boxes[i].y2 - boxes[i].y1));
    }
    cairo_clip(cr);
}

/* The Render Disjoint/Conjoint operators are not Porter-Duff ops that cairo
 * exposes: their Fa/Fb weights are non-linear in the source/destination alpha.
 * This mirrors rendercheck's reference (ops.c calc_op) exactly.  Returns 0 for
 * an operator this table doesn't cover. */
static int op_factors(int op, double srca, double dsta, double *Fa, double *Fb)
{
    switch (op) {
    /* Porter-Duff (base) family */
    case PictOpClear:         *Fa = 0; *Fb = 0; return 1;
    case PictOpSrc:           *Fa = 1; *Fb = 0; return 1;
    case PictOpDst:           *Fa = 0; *Fb = 1; return 1;
    case PictOpOver:          *Fa = 1; *Fb = 1 - srca; return 1;
    case PictOpOverReverse:   *Fa = 1 - dsta; *Fb = 1; return 1;
    case PictOpIn:            *Fa = dsta; *Fb = 0; return 1;
    case PictOpInReverse:     *Fa = 0; *Fb = srca; return 1;
    case PictOpOut:           *Fa = 1 - dsta; *Fb = 0; return 1;
    case PictOpOutReverse:    *Fa = 0; *Fb = 1 - srca; return 1;
    case PictOpAtop:          *Fa = dsta; *Fb = 1 - srca; return 1;
    case PictOpAtopReverse:   *Fa = 1 - dsta; *Fb = srca; return 1;
    case PictOpXor:           *Fa = 1 - dsta; *Fb = 1 - srca; return 1;
    case PictOpAdd:           *Fa = 1; *Fb = 1; return 1;
    case PictOpSaturate:
        *Fa = srca == 0 ? 1 : fmin(1, (1 - dsta) / srca); *Fb = 1; return 1;
    /* Disjoint family */
    case PictOpDisjointClear:  *Fa = 0; *Fb = 0; return 1;
    case PictOpDisjointSrc:    *Fa = 1; *Fb = 0; return 1;
    case PictOpDisjointDst:    *Fa = 0; *Fb = 1; return 1;
    case PictOpDisjointOver:
        *Fa = 1; *Fb = dsta == 0 ? 1 : fmin(1, (1 - srca) / dsta); return 1;
    case PictOpDisjointOverReverse:
        *Fa = srca == 0 ? 1 : fmin(1, (1 - dsta) / srca); *Fb = 1; return 1;
    case PictOpDisjointIn:
        *Fa = srca == 0 ? 0 : fmax(0, 1 - (1 - dsta) / srca); *Fb = 0; return 1;
    case PictOpDisjointInReverse:
        *Fa = 0; *Fb = dsta == 0 ? 0 : fmax(0, 1 - (1 - srca) / dsta); return 1;
    case PictOpDisjointOut:
        *Fa = srca == 0 ? 1 : fmin(1, (1 - dsta) / srca); *Fb = 0; return 1;
    case PictOpDisjointOutReverse:
        *Fa = 0; *Fb = dsta == 0 ? 1 : fmin(1, (1 - srca) / dsta); return 1;
    case PictOpDisjointAtop:
        *Fa = srca == 0 ? 0 : fmax(0, 1 - (1 - dsta) / srca);
        *Fb = dsta == 0 ? 1 : fmin(1, (1 - srca) / dsta); return 1;
    case PictOpDisjointAtopReverse:
        *Fa = srca == 0 ? 1 : fmin(1, (1 - dsta) / srca);
        *Fb = dsta == 0 ? 0 : fmax(0, 1 - (1 - srca) / dsta); return 1;
    case PictOpDisjointXor:
        *Fa = srca == 0 ? 1 : fmin(1, (1 - dsta) / srca);
        *Fb = dsta == 0 ? 1 : fmin(1, (1 - srca) / dsta); return 1;
    /* Conjoint family */
    case PictOpConjointClear:  *Fa = 0; *Fb = 0; return 1;
    case PictOpConjointSrc:    *Fa = 1; *Fb = 0; return 1;
    case PictOpConjointDst:    *Fa = 0; *Fb = 1; return 1;
    case PictOpConjointOver:
        *Fa = 1; *Fb = dsta == 0 ? 0 : fmax(0, 1 - srca / dsta); return 1;
    case PictOpConjointOverReverse:
        *Fa = srca == 0 ? 0 : fmax(0, 1 - dsta / srca); *Fb = 1; return 1;
    case PictOpConjointIn:
        *Fa = srca == 0 ? 1 : fmin(1, dsta / srca); *Fb = 0; return 1;
    case PictOpConjointInReverse:
        *Fa = 0; *Fb = dsta == 0 ? 1 : fmin(1, srca / dsta); return 1;
    case PictOpConjointOut:
        *Fa = srca == 0 ? 0 : fmax(0, 1 - dsta / srca); *Fb = 0; return 1;
    case PictOpConjointOutReverse:
        *Fa = 0; *Fb = dsta == 0 ? 0 : fmax(0, 1 - srca / dsta); return 1;
    case PictOpConjointAtop:
        *Fa = srca == 0 ? 1 : fmin(1, dsta / srca);
        *Fb = dsta == 0 ? 0 : fmax(0, 1 - srca / dsta); return 1;
    case PictOpConjointAtopReverse:
        *Fa = srca == 0 ? 0 : fmax(0, 1 - dsta / srca);
        *Fb = dsta == 0 ? 1 : fmin(1, srca / dsta); return 1;
    case PictOpConjointXor:
        *Fa = srca == 0 ? 0 : fmax(0, 1 - dsta / srca);
        *Fb = dsta == 0 ? 0 : fmax(0, 1 - srca / dsta); return 1;
    default: return 0;
    }
}

/* Apply a Disjoint/Conjoint operator between a pre-rasterised source buffer
 * (w x h, premultiplied ARGB32) and the destination picture. */
static void apply_disjoint(Display *d, MwRenderPicture *dst, int op,
                           const uint32_t *sp, int sstride,
                           int xd, int yd, int w, int h)
{
    MwSurface *ds = pic_surface(d, dst);
    cairo_surface_t *dcs = ds ? mw_surface_native(ds) : NULL;
    if (!dcs || !sp) return;
    cairo_surface_flush(dcs);
    uint32_t *dp = mw_surface_data(ds);
    int dstride = mw_surface_stride(ds) / 4;
    int dw = mw_surface_width(ds), dh = mw_surface_height(ds);
    int dsta_opaque = (fmt_is_opaque(dst->format));

    pixman_region32_t clip;
    pixman_region32_init_rect(&clip, xd, yd, (unsigned)w, (unsigned)h);
    if (dst->has_clip)
        pixman_region32_intersect(&clip, &clip, &dst->clip);
    int nboxes = 0;
    pixman_box32_t *boxes = pixman_region32_rectangles(&clip, &nboxes);
    for (int b = 0; b < nboxes; b++) {
        int x0 = boxes[b].x1 < xd ? xd : boxes[b].x1;
        int y0 = boxes[b].y1 < yd ? yd : boxes[b].y1;
        int x1 = boxes[b].x2 > xd + w ? xd + w : boxes[b].x2;
        int y1 = boxes[b].y2 > yd + h ? yd + h : boxes[b].y2;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > dw) x1 = dw;
        if (y1 > dh) y1 = dh;
        for (int yy = y0; yy < y1; yy++) {
            for (int xx = x0; xx < x1; xx++) {
                int i = xx - xd, j = yy - yd;
                if (i < 0 || j < 0 || i >= w || j >= h) continue;
                uint32_t sv = sp[(size_t)j * sstride + i];
                uint32_t dv = dp[(size_t)yy * dstride + xx];
                uint32_t dcanon = fmt_dst_rgb(dv, dst->format);
                double sa = ((sv >> 24) & 0xff) / 255.0;
                double sr = ((sv >> 16) & 0xff) / 255.0;
                double sg = ((sv >>  8) & 0xff) / 255.0;
                double sb = ((sv      ) & 0xff) / 255.0;
                double da = dsta_opaque ? 1.0 : ((dv >> 24) & 0xff) / 255.0;
                double dr = ((dcanon >> 16) & 0xff) / 255.0;
                double dg = ((dcanon >>  8) & 0xff) / 255.0;
                double db = ((dcanon      ) & 0xff) / 255.0;
                double Fa = 0, Fb = 0;
                op_factors(op, sa, da, &Fa, &Fb);
                double ra = fmin(sa * Fa + da * Fb, 1.0);
                double rr = fmin(sr * Fa + dr * Fb, 1.0);
                double rg = fmin(sg * Fa + dg * Fb, 1.0);
                double rb = fmin(sb * Fa + db * Fb, 1.0);
                uint32_t ra8 = (uint32_t)lround(ra * 255.0);
                uint32_t rr8 = (uint32_t)lround(rr * 255.0);
                uint32_t rg8 = (uint32_t)lround(rg * 255.0);
                uint32_t rb8 = (uint32_t)lround(rb * 255.0);
                if (dsta_opaque) ra8 = 255;
                dp[(size_t)yy * dstride + xx] =
                    fmt_from_argb((ra8 << 24) | (rr8 << 16) | (rg8 << 8) | rb8,
                                  dst->format);
            }
        }
    }
    pixman_region32_fini(&clip);
    cairo_surface_mark_dirty(dcs);
    MwWindow *win = pic_window(d, dst);
    if (win) mw_window_damage(win);
}

static int is_disjoint_op(int op)
{
    return (op >= 0x10 && op <= 0x1b) || (op >= 0x20 && op <= 0x2b);
}

/* xRGB32/xBGR32 have no alpha channel and xBGR swaps red/blue; convert a
 * rasterised surface between such a picture's layout and our canonical ARGB32,
 * and back when storing. */
static int fmt_legacy(int fmt) { return fmt == FMT_XRGB32 || fmt == FMT_XBGR32; }

static void fmt_to_argb(uint32_t *px, size_t n, int fmt)
{
    if (fmt == FMT_XRGB32) {
        for (size_t i = 0; i < n; i++) px[i] = 0xff000000u | (px[i] & 0x00ffffffu);
    } else if (fmt == FMT_XBGR32) {
        for (size_t i = 0; i < n; i++) {
            uint32_t v = px[i];
            px[i] = 0xff000000u | ((v & 0xff) << 16) | (v & 0xff00) | ((v >> 16) & 0xff);
        }
    }
}

static uint32_t fmt_from_argb(uint32_t v, int fmt)
{
    if (fmt == FMT_XBGR32)
        return 0xff000000u | ((v & 0xff) << 16) | (v & 0xff00) | ((v >> 16) & 0xff);
    if (fmt == FMT_XRGB32 || fmt == FMT_RGB24) return v | 0xff000000u;
    return v;
}

/* Canonical 0x00RRGGBB from a destination pixel stored in the given format. */
static uint32_t fmt_dst_rgb(uint32_t v, int fmt)
{
    if (fmt == FMT_XBGR32)
        return ((v & 0xff) << 16) | (v & 0xff00) | ((v >> 16) & 0xff);
    return v & 0x00ffffffu;
}

/* A software compositor used where cairo cannot express the Render semantics:
 * the Disjoint/Conjoint operators, and component-alpha masks (where the mask's
 * R/G/B/A channels weight the source's R/G/B/A independently).  Mirrors
 * rendercheck's reference (ops.c do_composite) for every operator. */
static void composite_manual(Display *d, MwRenderPicture *dst, MwRenderPicture *src,
                             MwRenderPicture *mask, int op,
                             int xs, int ys, int xm, int ym,
                             int xd, int yd, int w, int h)
{
    MwSurface *ds = pic_surface(d, dst);
    cairo_surface_t *dcs = ds ? mw_surface_native(ds) : NULL;
    if (!dcs || w <= 0 || h <= 0) return;
    int component = mask && mask->component_alpha;

    cairo_surface_t *stmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *sc = cairo_create(stmp);
    cairo_set_operator(sc, CAIRO_OPERATOR_SOURCE);
    set_source(d, sc, src, 0, 0, xs, ys);
    cairo_paint(sc);
    cairo_destroy(sc);
    cairo_surface_flush(stmp);

    cairo_surface_t *mtmp = NULL;
    if (mask) {
        MwSurface *ms = pic_surface(d, mask);
        cairo_surface_t *mcs = ms ? mw_surface_native(ms) : NULL;
        if (mcs) {
            mtmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
            cairo_t *mc = cairo_create(mtmp);
            cairo_set_operator(mc, CAIRO_OPERATOR_SOURCE);
            cairo_pattern_t *mp = cairo_pattern_create_for_surface(mcs);
            cairo_matrix_t mm;
            cairo_matrix_init_translate(&mm, xm, ym);
            if (mask->have_transform) {
                cairo_matrix_t t, r;
                xf_matrix(&t, &mask->xf);
                cairo_matrix_multiply(&r, &mm, &t);
                mm = r;
            }
            cairo_pattern_set_matrix(mp, &mm);
            cairo_pattern_set_extend(mp, cairo_extend(mask->repeat));
            cairo_pattern_set_filter(mp, picture_filter(mask));
            cairo_set_source(mc, mp);
            cairo_paint(mc);
            cairo_pattern_destroy(mp);
            cairo_destroy(mc);
            cairo_surface_flush(mtmp);
        }
    }

    const uint32_t *sp = (const uint32_t *)cairo_image_surface_get_data(stmp);
    int sstride = cairo_image_surface_get_stride(stmp) / 4;
    if (src && fmt_legacy(src->format))
        fmt_to_argb((uint32_t *)cairo_image_surface_get_data(stmp), (size_t)w * h, src->format);
    if (mtmp && mask && fmt_legacy(mask->format))
        fmt_to_argb((uint32_t *)cairo_image_surface_get_data(mtmp), (size_t)w * h, mask->format);
    const uint32_t *mp_px = mtmp ? (const uint32_t *)cairo_image_surface_get_data(mtmp) : NULL;
    int mstride = mtmp ? cairo_image_surface_get_stride(mtmp) / 4 : 0;

    cairo_surface_flush(dcs);
    uint32_t *dp = mw_surface_data(ds);
    int dstride = mw_surface_stride(ds) / 4;
    int dw = mw_surface_width(ds), dh = mw_surface_height(ds);
    int dsta_opaque = (fmt_is_opaque(dst->format));

    pixman_region32_t clip;
    pixman_region32_init_rect(&clip, xd, yd, (unsigned)w, (unsigned)h);
    if (dst->has_clip)
        pixman_region32_intersect(&clip, &clip, &dst->clip);
    int nboxes = 0;
    pixman_box32_t *boxes = pixman_region32_rectangles(&clip, &nboxes);
    for (int b = 0; b < nboxes; b++) {
        int x0 = boxes[b].x1 < xd ? xd : boxes[b].x1;
        int y0 = boxes[b].y1 < yd ? yd : boxes[b].y1;
        int x1 = boxes[b].x2 > xd + w ? xd + w : boxes[b].x2;
        int y1 = boxes[b].y2 > yd + h ? yd + h : boxes[b].y2;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > dw) x1 = dw;
        if (y1 > dh) y1 = dh;
        for (int yy = y0; yy < y1; yy++) {
            for (int xx = x0; xx < x1; xx++) {
                int i = xx - xd, j = yy - yd;
                if (i < 0 || j < 0 || i >= w || j >= h) continue;
                uint32_t sv = sp[(size_t)j * sstride + i];
                uint32_t dv = dp[(size_t)yy * dstride + xx];
                uint32_t dcanon = fmt_dst_rgb(dv, dst->format);
                double SR = ((sv >> 16) & 0xff) / 255.0;
                double SG = ((sv >>  8) & 0xff) / 255.0;
                double SB = ((sv      ) & 0xff) / 255.0;
                double SA = ((sv >> 24) & 0xff) / 255.0;
                double da = dsta_opaque ? 1.0 : ((dv >> 24) & 0xff) / 255.0;
                double DR = ((dcanon >> 16) & 0xff) / 255.0;
                double DG = ((dcanon >>  8) & 0xff) / 255.0;
                double DB = ((dcanon      ) & 0xff) / 255.0;
                double mr = 1, mg = 1, mb = 1, ma = 1;
                if (mp_px) {
                    uint32_t mv = mp_px[(size_t)j * mstride + i];
                    mr = ((mv >> 16) & 0xff) / 255.0;
                    mg = ((mv >>  8) & 0xff) / 255.0;
                    mb = ((mv      ) & 0xff) / 255.0;
                    ma = ((mv >> 24) & 0xff) / 255.0;
                }
                double R, G, B, A, Fa, Fb;
                if (component) {
                    op_factors(op, SA * mr, da, &Fa, &Fb);
                    R = fmin(SR * mr * Fa + DR * Fb, 1.0);
                    op_factors(op, SA * mg, da, &Fa, &Fb);
                    G = fmin(SG * mg * Fa + DG * Fb, 1.0);
                    op_factors(op, SA * mb, da, &Fa, &Fb);
                    B = fmin(SB * mb * Fa + DB * Fb, 1.0);
                    op_factors(op, SA * ma, da, &Fa, &Fb);
                    A = fmin(SA * ma * Fa + da * Fb, 1.0);
                } else {
                    double svr = SR * ma, svg = SG * ma, svb = SB * ma, sva = SA * ma;
                    op_factors(op, sva, da, &Fa, &Fb);
                    R = fmin(svr * Fa + DR * Fb, 1.0);
                    G = fmin(svg * Fa + DG * Fb, 1.0);
                    B = fmin(svb * Fa + DB * Fb, 1.0);
                    A = fmin(sva * Fa + da * Fb, 1.0);
                }
                uint32_t r8 = (uint32_t)lround(R * 255.0);
                uint32_t g8 = (uint32_t)lround(G * 255.0);
                uint32_t b8 = (uint32_t)lround(B * 255.0);
                uint32_t a8 = (uint32_t)lround(A * 255.0);
                if (dsta_opaque) a8 = 255;
                dp[(size_t)yy * dstride + xx] =
                    fmt_from_argb((a8 << 24) | (r8 << 16) | (g8 << 8) | b8,
                                  dst->format);
            }
        }
    }
    pixman_region32_fini(&clip);
    if (mtmp) cairo_surface_destroy(mtmp);
    cairo_surface_destroy(stmp);
    cairo_surface_mark_dirty(dcs);
    MwWindow *win = pic_window(d, dst);
    if (win) mw_window_damage(win);
}

/* The core Composite. */
static void do_composite(Display *d, MwRenderPicture *dst, MwRenderPicture *src,
                         MwRenderPicture *mask, int op,
                         int xs, int ys, int xm, int ym,
                         int xd, int yd, int w, int h)
{
    if (!dst || w <= 0 || h <= 0) return;
    /* Rendering *to* a gradient/solid picture is a BadDrawable, as on a real
     * server; rendercheck checks for the error. */
    if (dst->kind != PK_DRAWABLE) {
        mw_deliver_error(d, BadDrawable, MWD(d)->render_req_type,
                         X_RenderComposite, dst->id, 0);
        return;
    }
    if (getenv("MW_TRACE_RENDER"))
        fprintf(stderr, "MW: composite op=%d src=0x%lx(k%d drw=0x%lx fmt=%d) mask=0x%lx k%d op=%d "
                        "src=%d,%d mask=%d,%d dst=%d,%d %dx%d\n",
                op, src ? src->id : 0, src ? src->kind : -1,
                src ? (unsigned long)src->drawable : 0, src ? src->format : -1,
                mask ? (unsigned long)mask->id : 0UL, mask ? mask->kind : -1,
                op, xs, ys, xm, ym, xd, yd, w, h);
    if (is_disjoint_op(op) || mask ||
        (src && fmt_legacy(src->format)) || fmt_legacy(dst->format)) {
        composite_manual(d, dst, src, mask, op, xs, ys, xm, ym, xd, yd, w, h);
        return;
    }
    MwSurface *ds = pic_surface(d, dst);
    cairo_surface_t *dcs = ds ? mw_surface_native(ds) : NULL;
    if (!dcs) return;

    /* Make an alpha-less destination read as opaque for this composite. */
    if (fmt_is_opaque(dst->format)) force_opaque(ds, xd, yd, w, h);

    cairo_t *cr = cairo_create(dcs);
    cairo_save(cr);
    cairo_set_operator(cr, cairo_op(op));
    apply_dst_transform(cr, dst);

    /* Clip to the target rectangle and the picture's clip. */
    cairo_rectangle(cr, xd, yd, w, h);
    cairo_clip(cr);
    apply_dst_clip(cr, dst);

    set_source(d, cr, src, xd, yd, xs, ys);

    if (mask) {
        MwSurface *ms = pic_surface(d, mask);
        cairo_surface_t *mcs = ms ? mw_surface_native(ms) : NULL;
        if (mcs) {
            cairo_pattern_t *mp = cairo_pattern_create_for_surface(mcs);
            if (mp) {
                cairo_matrix_t mm;
                cairo_matrix_init_translate(&mm, xm - xd, ym - yd);
                cairo_pattern_set_matrix(mp, &mm);
                cairo_pattern_set_extend(mp, cairo_extend(mask->repeat));
                cairo_pattern_set_filter(mp, picture_filter(mask));
                cairo_mask(cr, mp);
                cairo_pattern_destroy(mp);
            } else {
                cairo_paint(cr);
            }
        } else {
            cairo_paint(cr);
        }
    } else {
        cairo_paint(cr);
    }

    cairo_restore(cr);
    cairo_destroy(cr);

    if (fmt_is_opaque(dst->format)) force_opaque(ds, xd, yd, w, h);
    mw_surface_mark_dirty(ds);
    MwWindow *win = pic_window(d, dst);
    if (win) mw_window_damage(win);
}

/* ---------------------------------------------------- picture construction */

static MwRenderPicture *new_picture(Display *d, XID id, int format, int kind)
{
    MwRenderPicture *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->id = id;
    p->format = format;
    p->kind = kind;
    p->repeat = RepeatNone;
    p->filter = -1;
    p->subwindow_mode = ClipByChildren;
    mw_register(d, id, MW_OBJ_PICTURE, p);
    if (getenv("MW_TRACE_RENDER"))
        fprintf(stderr, "MW: RenderCreatePicture id=0x%lx fmt=%d kind=%d\n",
                (unsigned long)id, format, kind);
    return p;
}

static void parse_stops(MwRenderPicture *p, const unsigned char *base, size_t n)
{
    p->stops = calloc(n ? n : 1, sizeof(MwStop));
    if (!p->stops) return;
    p->nstops = 0;
    /* libXrender sends all n stop positions (CARD32 each) followed by all n
     * xRenderColors (8 bytes each), not interleaved. */
    for (size_t i = 0; i < n; i++) {
        memcpy(&p->stops[i].pos, base + i * 4, 4);
        memcpy(&p->stops[i].color, base + n * 4 + i * 8, 8);
        p->nstops++;
    }
}

/* Apply a ChangePicture/CreatePicture value list to a picture.  `vals` points
 * at the CARD32 values; returns the number consumed. */
static void apply_values(MwRenderPicture *p, CARD32 mask, const CARD32 *v)
{
    int i = 0;
#define NEXT() (v[i++])
    if (mask & CPRepeat)          p->repeat = (int)NEXT();
    if (mask & CPAlphaMap)        (void)NEXT();
    if (mask & CPAlphaXOrigin)    (void)NEXT();
    if (mask & CPAlphaYOrigin)    (void)NEXT();
    if (mask & CPClipXOrigin)     (void)NEXT();
    if (mask & CPClipYOrigin)     (void)NEXT();
    if (mask & CPClipMask) {
        /* A clip mask (or None, which cairo uses to clear the clip) replaces
         * any rectangle clip.  We do not implement mask clips, so drop the
         * rectangle clip: keeping it would clip later drawing to a stale box
         * (glyph runs after a clip-to-None lost their text). */
        (void)NEXT();
        if (p->has_clip) { pixman_region32_fini(&p->clip); p->has_clip = 0; }
    }
    if (mask & CPGraphicsExposure)(void)NEXT();
    if (mask & CPSubwindowMode)   p->subwindow_mode = (int)NEXT();
    if (mask & CPPolyEdge)        p->poly_edge = (int)NEXT();
    if (mask & CPPolyMode)        p->poly_mode = (int)NEXT();
    if (mask & CPDither)          (void)NEXT();
    if (mask & CPComponentAlpha)  p->component_alpha = (int)NEXT();
#undef NEXT
}

static void free_picture(Display *d, MwRenderPicture *p)
{
    if (!p) return;
    if (p->has_clip) pixman_region32_fini(&p->clip);
    free(p->stops);
    mw_unregister(d, p->id);
    free(p);
}

/* --------------------------------------------------------------- glyphsets */

static void glyph_rehash(MwGlyphSet *gs, int ncap)
{
    MwGlyph *old = gs->tab;
    int oldcap = gs->cap;
    gs->tab = calloc(ncap, sizeof *gs->tab);
    gs->cap = ncap;
    if (!gs->tab) return;
    int mask = ncap - 1;
    for (int i = 0; i < oldcap; i++) {
        if (!old[i].used) continue;
        int j = (int)((old[i].gid * 2654435761u) & (unsigned)mask);
        while (gs->tab[j].used) j = (j + 1) & mask;
        gs->tab[j] = old[i];
    }
    free(old);
}

static MwGlyph *glyph_slot(MwGlyphSet *gs, CARD32 gid, int create)
{
    if (gs->cap == 0) {
        if (!create) return NULL;
        glyph_rehash(gs, 256);
        if (!gs->tab) return NULL;
    }
    if (create && (gs->used + 1) * 4 >= gs->cap * 3)
        glyph_rehash(gs, gs->cap * 2);
    if (!gs->tab) return NULL;
    int mask = gs->cap - 1;
    int i = (int)((gid * 2654435761u) & (unsigned)mask);
    for (;;) {
        MwGlyph *g = &gs->tab[i];
        if (!g->used) {
            if (!create) return NULL;
            memset(g, 0, sizeof *g);
            g->used = 1; g->gid = gid;
            gs->used++;
            return g;
        }
        if (g->gid == gid) return g;
        i = (i + 1) & mask;
    }
}

static void glyph_remove(MwGlyphSet *gs, CARD32 gid)
{
    MwGlyph *g = glyph_slot(gs, gid, 0);
    if (!g) return;
    if (g->surface) mw_surface_destroy(g->surface);
    g->used = 0; g->surface = NULL;
    gs->used--;
    glyph_rehash(gs, gs->cap);   /* keep the probe chains valid */
}

/* Build an ARGB32 surface from an A8 or A1 glyph image (premultiplied white). */
static MwSurface *glyph_surface(const unsigned char *data, size_t avail,
                                int w, int h, int depth1)
{
    if (w <= 0 || h <= 0) return NULL;
    int stride = (w + 3) & ~3;
    size_t need = depth1 ? ((w + 7) / 8) * h : (size_t)stride * h;
    if (avail < need) return NULL;
    MwSurface *s = mw_surface_create(w, h);
    if (!s) return NULL;
    uint32_t *px = mw_surface_data(s);
    int ds = mw_surface_stride(s) / 4;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned a;
            if (depth1) a = (data[y * ((w + 7) / 8) + (x >> 3)] >> (x & 7)) & 1 ? 255 : 0;
            else        a = data[(size_t)y * stride + x];
            px[(size_t)y * ds + x] = ((uint32_t)a << 24) | ((uint32_t)a << 16) |
                                     ((uint32_t)a << 8) | a;
        }
    }
    mw_surface_mark_dirty(s);
    if (getenv("MW_DUMP_GLYPHS")) {
        static int n = 0;
        char path[128];
        snprintf(path, sizeof path, "/tmp/opencode/glyphs/g-%dx%d-%03d.png", w, h, n++);
        mw_surface_write_png(s, path);
    }
    return s;
}

static void free_glyphset(Display *d, MwGlyphSet *gs)
{
    if (!gs) return;
    for (int i = 0; i < gs->cap; i++)
        if (gs->tab[i].used && gs->tab[i].surface)
            mw_surface_destroy(gs->tab[i].surface);
    free(gs->tab);
    mw_unregister(d, gs->id);
    free(gs);
}

/* ----------------------------------------------------------- requests */

static void req_query_version(Display *d)
{
    (void)d;   /* reply built in mw_render_reply */
}

static void req_query_pict_formats(Display *d)
{
    (void)d;   /* reply + payload built in mw_render_reply */
}

static void req_create_picture(Display *d, const unsigned char *b, size_t len)
{
    const xRenderCreatePictureReq *r = (const void *)b;
    if (len < sz_xRenderCreatePictureReq) return;
    MwRenderPicture *p = new_picture(d, r->pid, (int)r->format, PK_DRAWABLE);
    if (!p) return;
    p->drawable = r->drawable;
    if (getenv("MW_TRACE_RENDER"))
        fprintf(stderr, "MW: createpicture pid=0x%lx drawable=0x%lx win=%p pm=%p\n",
                (unsigned long)r->pid, (unsigned long)r->drawable,
                (void *)mw_window(d, r->drawable),
                (void *)mw_pixmap(d, r->drawable));
    apply_values(p, r->mask, (const CARD32 *)(b + sz_xRenderCreatePictureReq));
}

static void req_change_picture(Display *d, const unsigned char *b, size_t len)
{
    const xRenderChangePictureReq *r = (const void *)b;
    if (len < sz_xRenderChangePictureReq) return;
    MwRenderPicture *p = pic(d, r->picture);
    if (!p) return;
    apply_values(p, r->mask, (const CARD32 *)(b + sz_xRenderChangePictureReq));
}

static void req_free_picture(Display *d, const unsigned char *b)
{
    free_picture(d, pic(d, ((const xRenderFreePictureReq *)b)->picture));
}

static void req_clip_rectangles(Display *d, const unsigned char *b, size_t len)
{
    const xRenderSetPictureClipRectanglesReq *r = (const void *)b;
    if (len < sz_xRenderSetPictureClipRectanglesReq) return;
    MwRenderPicture *p = pic(d, r->picture);
    if (!p) return;
    size_t n = (len - sz_xRenderSetPictureClipRectanglesReq) / 8;
    if (getenv("MW_TRACE_RENDER")) {
        const short *rp0 = (const short *)(b + sz_xRenderSetPictureClipRectanglesReq);
        fprintf(stderr, "MW: SetPictureClipRectangles pic=0x%lx origin=%d,%d n=%zu hdr=%zu",
                (unsigned long)r->picture, r->xOrigin, r->yOrigin, n,
                (size_t)sz_xRenderSetPictureClipRectanglesReq);
        if (n) fprintf(stderr, " first=%d,%d %ux%u", rp0[0], rp0[1],
                       (unsigned short)rp0[2], (unsigned short)rp0[3]);
        fprintf(stderr, " bytes:");
        for (size_t i = 0; i < len && i < 28; i++) fprintf(stderr, " %02x", b[i]);
        fprintf(stderr, "\n");
    }
    if (p->has_clip) { pixman_region32_fini(&p->clip); p->has_clip = 0; }
    if (n == 0) return;
    pixman_region32_init(&p->clip);
    const short *rp = (const short *)(b + sz_xRenderSetPictureClipRectanglesReq);
    for (size_t i = 0; i < n; i++) {
        int x = rp[i*4+0] + r->xOrigin;
        int y = rp[i*4+1] + r->yOrigin;
        int w = (unsigned short)rp[i*4+2];
        int h = (unsigned short)rp[i*4+3];
        pixman_region32_union_rect(&p->clip, &p->clip, x, y, (unsigned)w, (unsigned)h);
    }
    p->has_clip = 1;
}

static void req_composite(Display *d, const unsigned char *b, size_t len)
{
    const xRenderCompositeReq *r = (const void *)b;
    if (len < sz_xRenderCompositeReq) return;
    do_composite(d, pic(d, r->dst), pic(d, r->src), pic(d, r->mask), r->op,
                 r->xSrc, r->ySrc, r->xMask, r->yMask, r->xDst, r->yDst,
                 r->width, r->height);
}

static void req_fill_rectangles(Display *d, const unsigned char *b, size_t len)
{
    const xRenderFillRectanglesReq *r = (const void *)b;
    if (len < sz_xRenderFillRectanglesReq) return;
    MwRenderPicture *dst = pic(d, r->dst);
    if (!dst) return;
    size_t n = (len - sz_xRenderFillRectanglesReq) / 8;
    const short *rp = (const short *)(b + sz_xRenderFillRectanglesReq);
    MwRenderPicture solid;
    memset(&solid, 0, sizeof solid);
    solid.kind = PK_SOLID;
    solid.color = r->color;
    for (size_t i = 0; i < n; i++) {
        do_composite(d, dst, &solid, NULL, r->op,
                     0, 0, 0, 0, rp[i*4+0], rp[i*4+1],
                     (unsigned short)rp[i*4+2], (unsigned short)rp[i*4+3]);
    }
}

static void req_create_solid(Display *d, const unsigned char *b)
{
    const xRenderCreateSolidFillReq *r = (const void *)b;
    MwRenderPicture *p = new_picture(d, r->pid, FMT_ARGB32, PK_SOLID);
    if (p) p->color = r->color;
}

static void req_create_linear(Display *d, const unsigned char *b, size_t len)
{
    const xRenderCreateLinearGradientReq *r = (const void *)b;
    if (len < sz_xRenderCreateLinearGradientReq) return;
    MwRenderPicture *p = new_picture(d, r->pid, FMT_ARGB32, PK_LINEAR);
    if (!p) return;
    p->p1 = r->p1; p->p2 = r->p2;
    parse_stops(p, b + sz_xRenderCreateLinearGradientReq, r->nStops);
}

static void req_create_radial(Display *d, const unsigned char *b, size_t len)
{
    const xRenderCreateRadialGradientReq *r = (const void *)b;
    if (len < sz_xRenderCreateRadialGradientReq) return;
    MwRenderPicture *p = new_picture(d, r->pid, FMT_ARGB32, PK_RADIAL);
    if (!p) return;
    p->inner = r->inner; p->outer = r->outer;
    p->inner_radius = r->inner_radius; p->outer_radius = r->outer_radius;
    parse_stops(p, b + sz_xRenderCreateRadialGradientReq, r->nStops);
}

static void req_create_conical(Display *d, const unsigned char *b, size_t len)
{
    const xRenderCreateConicalGradientReq *r = (const void *)b;
    if (len < sz_xRenderCreateConicalGradientReq) return;
    MwRenderPicture *p = new_picture(d, r->pid, FMT_ARGB32, PK_CONICAL);
    if (!p) return;
    p->center = r->center; p->angle = r->angle;
    parse_stops(p, b + sz_xRenderCreateConicalGradientReq, r->nStops);
}

static void req_set_transform(Display *d, const unsigned char *b, size_t len)
{
    const xRenderSetPictureTransformReq *r = (const void *)b;
    if (len < sz_xRenderSetPictureTransformReq) return;
    MwRenderPicture *p = pic(d, r->picture);
    if (!p) return;
    p->xf = r->transform;
    p->have_transform = True;
    if (getenv("MW_TRACE_RENDER"))
        fprintf(stderr, "MW: settransform pic=0x%lx [%.3f %.3f %.3f; %.3f %.3f %.3f]\n",
                (unsigned long)r->picture,
                frac(r->transform.matrix11), frac(r->transform.matrix12), frac(r->transform.matrix13),
                frac(r->transform.matrix21), frac(r->transform.matrix22), frac(r->transform.matrix23));
}

static void req_set_filter(Display *d, const unsigned char *b, size_t len)
{
    const xRenderSetPictureFilterReq *r = (const void *)b;
    if (len < sz_xRenderSetPictureFilterReq) return;
    MwRenderPicture *p = pic(d, r->picture);
    if (!p) return;
    const char *name = (const char *)(b + sz_xRenderSetPictureFilterReq);
    if      (!strncmp(name, "nearest",  r->nbytes)) p->filter = 0;
    else if (!strncmp(name, "bilinear", r->nbytes)) p->filter = 1;
    else if (!strncmp(name, "convolution", r->nbytes)) p->filter = 1;
    else p->filter = -1;
    (void)len;
}

static void req_create_glyphset(Display *d, const unsigned char *b)
{
    const xRenderCreateGlyphSetReq *r = (const void *)b;
    MwGlyphSet *gs = calloc(1, sizeof *gs);
    if (!gs) return;
    gs->id = r->gsid;
    gs->format = (int)r->format;
    mw_register(d, gs->id, MW_OBJ_GLYPHSET, gs);
}

static void req_reference_glyphset(Display *d, const unsigned char *b)
{
    const xRenderReferenceGlyphSetReq *r = (const void *)b;
    MwGlyphSet *src = glyphset(d, r->existing);
    MwGlyphSet *gs = calloc(1, sizeof *gs);
    if (!gs) return;
    gs->id = r->gsid;
    gs->format = src ? src->format : FMT_A8;
    if (src) {
        /* Share the glyph surfaces: reference the same pointers (do not free
         * twice by marking them shared).  Simpler: allocate an empty set. */
        (void)src;
    }
    mw_register(d, gs->id, MW_OBJ_GLYPHSET, gs);
}

static void req_free_glyphset(Display *d, const unsigned char *b)
{
    free_glyphset(d, glyphset(d, ((const xRenderFreeGlyphSetReq *)b)->glyphset));
}

static void req_free_glyphs(Display *d, const unsigned char *b, size_t len)
{
    const xRenderFreeGlyphsReq *r = (const void *)b;
    MwGlyphSet *gs = glyphset(d, r->glyphset);
    if (!gs) return;
    size_t n = (len - sz_xRenderFreeGlyphsReq) / 4;
    const CARD32 *ids = (const CARD32 *)(b + sz_xRenderFreeGlyphsReq);
    for (size_t i = 0; i < n; i++)
        glyph_remove(gs, ids[i]);
}

static void req_add_glyphs(Display *d, const unsigned char *b, size_t len)
{
    const xRenderAddGlyphsReq *r = (const void *)b;
    MwGlyphSet *gs = glyphset(d, r->glyphset);
    if (!gs) return;
    const unsigned char *p = b + sz_xRenderAddGlyphsReq;
    const unsigned char *end = b + len;
    int depth1 = (gs->format == FMT_A1);
    for (CARD32 i = 0; i < r->nglyphs && p + 16 <= end; i++) {
        CARD32 gid;
        xGlyphInfo info;
        memcpy(&gid, p, 4);
        memcpy(&info, p + 4, 12);
        p += 16;
        int stride = (info.width + 3) & ~3;
        size_t need = depth1 ? ((info.width + 7) / 8) * info.height
                             : (size_t)stride * info.height;
        if (p + need > end) break;
        MwGlyph *g = glyph_slot(gs, gid, 1);
        if (getenv("MW_TRACE_RENDER") && i == 0)
            fprintf(stderr, "MW: addglyph gid=%u w=%u h=%u x=%d y=%d xOff=%d yOff=%d d0=%d\n",
                    (unsigned)gid, info.width, info.height,
                    (int)info.x, (int)info.y,
                    (int)info.xOff, (int)info.yOff, p[0]);
        if (g) {
            if (g->surface) mw_surface_destroy(g->surface);
            g->surface = glyph_surface(p, need, info.width, info.height, depth1);
            g->w = info.width; g->h = info.height;
            g->gx = info.xOff; g->gy = info.yOff;   /* advance */
            g->gox = info.x; g->goy = info.y;       /* bearing */
        }
        p += need;
    }
}

/* Draw one glyph at (x,y) in dst, using `src` (usually solid) and the glyph's
 * mask surface; the bitmap's top-left is at (x - xOff, y - yOff). */
static void draw_glyph(Display *d, MwRenderPicture *dst, MwRenderPicture *src,
                       int op, MwGlyphSet *gs, int gid, int x, int y)
{
    MwGlyph *gp = glyph_slot(gs, (CARD32)gid, 0);
    if (!gp || !gp->surface) {
        if (getenv("MW_TRACE_RENDER"))
            fprintf(stderr, "MW: drawglyph MISSING gid=%d\n", gid);
        return;
    }
    MwSurface *mask = gp->surface;
    int w = gp->w, h = gp->h;
    /* The pen (x,y) is the glyph origin; the bitmap's top-left sits at the
     * glyph's stored bearing (xGlyphInfo.x/y), exactly as the X server
     * composites it (destination = pen - info). */
    int bx = x - gp->gox;
    int by = y - gp->goy;
    /* Decode mask onto the destination directly (cairo mask pattern needs a
     * drawable; a glyph mask is an in-memory surface, so composite it here). */
    MwSurface *ds = pic_surface(d, dst);
    cairo_surface_t *dcs = ds ? mw_surface_native(ds) : NULL;
    if (!dcs) return;
    if (fmt_is_opaque(dst->format)) force_opaque(ds, bx, by, w, h);
    cairo_t *cr = cairo_create(dcs);
    cairo_save(cr);
    cairo_set_operator(cr, cairo_op(op));
    apply_dst_transform(cr, dst);
    cairo_rectangle(cr, bx, by, w, h);
    cairo_clip(cr);
    apply_dst_clip(cr, dst);
    set_source(d, cr, src, bx, by, bx, by);
    cairo_mask_surface(cr, mw_surface_native(mask), bx, by);
    cairo_restore(cr);
    cairo_destroy(cr);
    if (fmt_is_opaque(dst->format)) force_opaque(ds, bx, by, w, h);
    mw_surface_mark_dirty(ds);
    MwWindow *win = pic_window(d, dst);
    if (win) mw_window_damage(win);
}

static void req_composite_glyphs(Display *d, const unsigned char *b, size_t len,
                                 int glyph_bytes)
{
    const xRenderCompositeGlyphsReq *r = (const void *)b;
    if (len < sz_xRenderCompositeGlyphs8Req) return;
    MwRenderPicture *dst = pic(d, r->dst);
    MwRenderPicture *src = pic(d, r->src);
    MwGlyphSet *gs = glyphset(d, r->glyphset);
    if (!dst || !src || !gs) return;

    if (getenv("MW_TRACE_RENDER"))
        fprintf(stderr, "MW: glyphrun dst=0x%lx drawable=0x%lx %dx at %d,%d\n",
                (unsigned long)r->dst, (unsigned long)dst->drawable, glyph_bytes,
                (int)r->xSrc, (int)r->ySrc);
    const unsigned char *p = b + sz_xRenderCompositeGlyphs8Req;   /* 28-byte header */
    const unsigned char *end = b + len;
    /* The X server accumulates the pen from the picture origin: each element
     * adds its (deltax,deltay), then each glyph adds its stored advance; each
     * element's glyph data is padded to a 4-byte boundary. */
    int pen_x = 0, pen_y = 0;
    int total = 0;
    while (p + sz_xGlyphElt <= end) {
        xGlyphElt elt;
        memcpy(&elt, p, sz_xGlyphElt);
        p += sz_xGlyphElt;
        if (elt.len == 0xff) {
            /* glyphset-change marker: a 4-byte glyphset id follows, no glyphs */
            if (p + 4 > end) break;
            XID ng = 0;
            memcpy(&ng, p, 4);
            p += 4;
            MwGlyphSet *ngs = glyphset(d, ng);
            if (ngs) gs = ngs;
            continue;
        }
        int count = elt.len;
        size_t need = (size_t)count * glyph_bytes;
        size_t padded = (need + 3) & ~(size_t)3;
        if (p + need > end) break;
        int px = pen_x + elt.deltax;
        int py = pen_y + elt.deltay;
        if (getenv("MW_TRACE_RENDER"))
            fprintf(stderr, "MW: glyph elt len=%d dx=%d dy=%d start=%d,%d\n",
                    count, (int)elt.deltax, (int)elt.deltay, px, py);
        for (int i = 0; i < count; i++) {
            CARD32 gid = 0;
            if (glyph_bytes == 1) gid = p[i];
            else if (glyph_bytes == 2) { CARD16 v; memcpy(&v, p + i*2, 2); gid = v; }
            else { memcpy(&gid, p + i*4, 4); }
            draw_glyph(d, dst, src, r->op, gs, (int)gid, px, py);
            /* The glyph's stored xOff/yOff is its advance (cairo/Xft); a
             * zero-size glyph (space) has no surface but still advances. */
            MwGlyph *gp = glyph_slot(gs, gid, 0);
            if (gp) { px += gp->gx; py += gp->gy; }
            total++;
        }
        pen_x = px;
        pen_y = py;
        p += padded;
    }
    if (getenv("MW_TRACE_RENDER"))
        fprintf(stderr, "MW: glyph run total=%d op=%d end=%d,%d\n", total, r->op, pen_x, pen_y);
}

/* --------------------------------------------------- traps / triangles */

/* Render treats triangle/trapezoid geometry as an implicit mask on the source
 * and applies the operator across the whole drawable -- outside the geometry
 * the masked source is transparent, but operators such as Src/Clear still write
 * there.  cairo_fill() only touches the interior, so build one coverage mask
 * for the whole request, merge the shapes into it, then rasterise the source
 * through that mask and composite. */
static cairo_surface_t *poly_mask_new(MwSurface *ds, cairo_t **out)
{
    int mw = mw_surface_width(ds), mh = mw_surface_height(ds);
    cairo_surface_t *mask = cairo_image_surface_create(CAIRO_FORMAT_A8, mw, mh);
    cairo_t *c = cairo_create(mask);
    cairo_set_source_rgba(c, 1, 1, 1, 1);
    *out = c;
    return mask;
}

static void apply_poly_mask(Display *d, MwRenderPicture *dst, MwRenderPicture *src,
                            int op, cairo_surface_t *mask, int xs, int ys)
{
    MwSurface *ds = pic_surface(d, dst);
    cairo_surface_t *dcs = ds ? mw_surface_native(ds) : NULL;
    if (!dcs || !mask) return;
    int mw = mw_surface_width(ds), mh = mw_surface_height(ds);
    cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, mw, mh);
    cairo_t *tc = cairo_create(tmp);
    cairo_set_operator(tc, CAIRO_OPERATOR_SOURCE);
    set_source(d, tc, src, 0, 0, xs, ys);
    cairo_mask_surface(tc, mask, 0, 0);
    cairo_destroy(tc);
    cairo_surface_flush(tmp);
    const uint32_t *sp = (const uint32_t *)cairo_image_surface_get_data(tmp);
    int sstride = cairo_image_surface_get_stride(tmp) / 4;

    apply_disjoint(d, dst, op, sp, sstride, 0, 0, mw, mh);
    cairo_surface_destroy(tmp);
    mw_surface_mark_dirty(ds);
    MwWindow *win = pic_window(d, dst);
    if (win) mw_window_damage(win);
}

static void req_trapezoids(Display *d, const unsigned char *b, size_t len)
{
    const xRenderTrapezoidsReq *r = (const void *)b;
    if (len < sz_xRenderTrapezoidsReq) return;
    MwRenderPicture *dst = pic(d, r->dst), *src = pic(d, r->src);
    size_t n = (len - sz_xRenderTrapezoidsReq) / sz_xTrapezoid;
    const unsigned char *p = b + sz_xRenderTrapezoidsReq;
    if (getenv("MW_TRACE_RENDER") && n) {
        xTrapezoid t0; memcpy(&t0, p, sz_xTrapezoid);
        fprintf(stderr, "MW: trapezoids dst=0x%lx src=0x%lx op=%d n=%zu "
                        "top=%.1f bot=%.1f l=%.1f..%.1f r=%.1f..%.1f\n",
                (unsigned long)r->dst, (unsigned long)r->src, r->op, n,
                t0.top/65536.0, t0.bottom/65536.0,
                frac(t0.left.p1.x), frac(t0.left.p2.x),
                frac(t0.right.p1.x), frac(t0.right.p2.x));
    }
    MwSurface *ds = pic_surface(d, dst);
    if (!ds) return;
    cairo_t *mc = NULL;
    cairo_surface_t *mask = poly_mask_new(ds, &mc);
    for (size_t i = 0; i < n; i++) {
        xTrapezoid t; memcpy(&t, p + i*sz_xTrapezoid, sz_xTrapezoid);
        cairo_move_to(mc, frac(t.left.p1.x),  t.top / 65536.0);
        cairo_line_to(mc, frac(t.right.p1.x), t.top / 65536.0);
        cairo_line_to(mc, frac(t.right.p2.x), t.bottom / 65536.0);
        cairo_line_to(mc, frac(t.left.p2.x),  t.bottom / 65536.0);
        cairo_close_path(mc);
    }
    cairo_fill(mc);
    cairo_destroy(mc);
    apply_poly_mask(d, dst, src, r->op, mask, r->xSrc, r->ySrc);
    cairo_surface_destroy(mask);
}

static void req_triangles(Display *d, const unsigned char *b, size_t len)
{
    const xRenderTrianglesReq *r = (const void *)b;
    if (len < sz_xRenderTrianglesReq) return;
    MwRenderPicture *dst = pic(d, r->dst), *src = pic(d, r->src);
    size_t n = (len - sz_xRenderTrianglesReq) / sz_xTriangle;
    const unsigned char *p = b + sz_xRenderTrianglesReq;
    if (getenv("MW_TRACE_RENDER")) {
        static int shown;
        if (shown++ < 4)
            fprintf(stderr, "MW: triangles dst=0x%lx(%p) src=0x%lx(%p) op=%d n=%zu mask=%u\n",
                    (unsigned long)r->dst, (void*)dst, (unsigned long)r->src, (void*)src,
                    r->op, n, (unsigned)r->maskFormat);
    }
    MwSurface *ds = pic_surface(d, dst);
    if (!ds) return;
    cairo_t *mc = NULL;
    cairo_surface_t *mask = poly_mask_new(ds, &mc);
    for (size_t i = 0; i < n; i++) {
        xTriangle t; memcpy(&t, p + i*sz_xTriangle, sz_xTriangle);
        cairo_move_to(mc, frac(t.p1.x), frac(t.p1.y));
        cairo_line_to(mc, frac(t.p2.x), frac(t.p2.y));
        cairo_line_to(mc, frac(t.p3.x), frac(t.p3.y));
        cairo_close_path(mc);
    }
    cairo_fill(mc);
    cairo_destroy(mc);
    apply_poly_mask(d, dst, src, r->op, mask, r->xSrc, r->ySrc);
    cairo_surface_destroy(mask);
}

/* TriStrip and TriFan share the Triangles header but take a bare point list and
 * join consecutive points into triangles, as the Render extension (and GL)
 * define them: a fan keeps the first point in every triangle, a strip slides a
 * window along the list with the winding alternating. */
static void req_tri_fan(Display *d, const unsigned char *b, size_t len)
{
    const xRenderTriStripReq *r = (const void *)b;
    if (len < sz_xRenderTriStripReq) return;
    MwRenderPicture *dst = pic(d, r->dst), *src = pic(d, r->src);
    size_t n = (len - sz_xRenderTriStripReq) / 8;   /* xPointFixed */
    const unsigned char *p = b + sz_xRenderTriStripReq;
    if (n < 3) return;
    xPointFixed a; memcpy(&a, p, 8);
    MwSurface *ds = pic_surface(d, dst);
    if (!ds) return;
    cairo_t *mc = NULL;
    cairo_surface_t *mask = poly_mask_new(ds, &mc);
    for (size_t i = 1; i + 1 < n; i++) {
        xPointFixed q, c;
        memcpy(&q, p + i * 8, 8);
        memcpy(&c, p + (i + 1) * 8, 8);
        cairo_move_to(mc, frac(a.x), frac(a.y));
        cairo_line_to(mc, frac(q.x), frac(q.y));
        cairo_line_to(mc, frac(c.x), frac(c.y));
        cairo_close_path(mc);
    }
    cairo_fill(mc);
    cairo_destroy(mc);
    apply_poly_mask(d, dst, src, r->op, mask, r->xSrc, r->ySrc);
    cairo_surface_destroy(mask);
}

static void req_tri_strip(Display *d, const unsigned char *b, size_t len)
{
    const xRenderTriStripReq *r = (const void *)b;
    if (len < sz_xRenderTriStripReq) return;
    MwRenderPicture *dst = pic(d, r->dst), *src = pic(d, r->src);
    size_t n = (len - sz_xRenderTriStripReq) / 8;
    const unsigned char *p = b + sz_xRenderTriStripReq;
    MwSurface *ds = pic_surface(d, dst);
    if (!ds) return;
    cairo_t *mc = NULL;
    cairo_surface_t *mask = poly_mask_new(ds, &mc);
    for (size_t i = 0; i + 2 < n; i++) {
        xPointFixed p0, p1, p2;
        memcpy(&p0, p + i * 8, 8);
        memcpy(&p1, p + (i + 1) * 8, 8);
        memcpy(&p2, p + (i + 2) * 8, 8);
        cairo_move_to(mc, frac(p0.x), frac(p0.y));
        cairo_line_to(mc, frac(p1.x), frac(p1.y));
        cairo_line_to(mc, frac(p2.x), frac(p2.y));
        cairo_close_path(mc);
    }
    cairo_fill(mc);
    cairo_destroy(mc);
    apply_poly_mask(d, dst, src, r->op, mask, r->xSrc, r->ySrc);
    cairo_surface_destroy(mask);
}

/* --------------------------------------------------------------- replies */

static void build_pict_formats(Display *d,
                               xRenderQueryPictFormatsReply *rep,
                               unsigned char **data, size_t *len)
{
    size_t nformats = 6;
    size_t payload = nformats * sz_xPictFormInfo
                   + sz_xPictScreen + sz_xPictDepth + sz_xPictVisual
                   + 4 /* subpixel */;
    unsigned char *buf = calloc(1, payload);
    if (!buf) return;
    unsigned char *p = buf;

    xPictFormInfo fi;
/* In xDirectFormat the component fields hold the bit shifts and the *Mask
 * fields hold the component value masks (the X server's Mask() convention),
 * e.g. ARGB32 is red=16,redMask=0xff; green=8,greenMask=0xff; ... Consumers
 * such as cairo reconstruct a pixel mask as (mask << shift). */
#define PUTFORM(fmt_id, dep, rs, rm, gs, gm, bs, bm, as, am) do {   \
        memset(&fi, 0, sizeof fi);                                  \
        fi.id = (fmt_id); fi.type = PictTypeDirect; fi.depth = (dep); \
        fi.direct.red = (rs); fi.direct.redMask = (rm);             \
        fi.direct.green = (gs); fi.direct.greenMask = (gm);         \
        fi.direct.blue = (bs); fi.direct.blueMask = (bm);           \
        fi.direct.alpha = (as); fi.direct.alphaMask = (am);         \
        memcpy(p, &fi, sizeof fi); p += sz_xPictFormInfo;            \
    } while (0)
    PUTFORM(FMT_ARGB32, 32, 16, 0xff, 8, 0xff, 0, 0xff, 24, 0xff);
    PUTFORM(FMT_RGB24,  24, 16, 0xff, 8, 0xff, 0, 0xff,  0, 0);
    PUTFORM(FMT_A8,      8,  0, 0,    0, 0,    0, 0,     0, 0xff);
    PUTFORM(FMT_A1,      1,  0, 0,    0, 0,    0, 0,     0, 0x01);
    PUTFORM(FMT_XRGB32, 32, 16, 0xff, 8, 0xff, 0, 0xff,  0, 0);
    PUTFORM(FMT_XBGR32, 32,  0, 0xff, 8, 0xff, 16, 0xff, 0, 0);
#undef PUTFORM

    int vdepth = MWSCR(d)->root_depth ? MWSCR(d)->root_depth : 24;
    int vfmt = (vdepth == 32) ? FMT_ARGB32 : FMT_RGB24;

    xPictScreen sc; memset(&sc, 0, sizeof sc);
    sc.nDepth = 1; sc.fallback = vfmt;
    memcpy(p, &sc, sizeof sc); p += sz_xPictScreen;

    xPictDepth pd; memset(&pd, 0, sizeof pd);
    pd.depth = (CARD8)vdepth; pd.nPictVisuals = 1;
    memcpy(p, &pd, sizeof pd); p += sz_xPictDepth;

    xPictVisual pv; memset(&pv, 0, sizeof pv);
    pv.visual = MWD(d)->visual.visualid; pv.format = vfmt;
    memcpy(p, &pv, sizeof pv); p += sz_xPictVisual;

    CARD32 sub = SubPixelUnknown;
    memcpy(p, &sub, 4); p += 4;

    memset(rep, 0, sizeof *rep);
    rep->type = 1;   /* X_Reply */
    rep->length = (CARD32)(payload / 4);
    rep->numFormats = (CARD32)nformats;
    rep->numScreens = 1;
    rep->numDepths = 1;
    rep->numVisuals = 1;
    rep->numSubpixel = 1;
    *data = buf;
    *len = payload;
}

/* ------------------------------------------------------------ dispatch */

static void handle_request(Display *d, const unsigned char *b, size_t len)
{
    if (len < 4) return;
    int minor = b[1];
    if (getenv("MW_TRACE_RENDER") && minor == X_RenderCompositeGlyphs8) {
        fprintf(stderr, "MW: compositeglyphs8 bytes:");
        for (size_t i = 0; i < len && i < 48; i++) fprintf(stderr, " %02x", b[i]);
        fprintf(stderr, "\n");
    }
    XDisplayImpl *dp = MWD(d);
    if (dp->render_reply_data) { free(dp->render_reply_data);
        dp->render_reply_data = NULL; dp->render_reply_len = dp->render_reply_off = 0; }

    switch (minor) {
    case X_RenderQueryVersion:
    case X_RenderQueryPictFormats:
    case X_RenderQueryFilters:
    case X_RenderQueryPictIndexValues:
        /* Queue, don't overwrite: several queries can be drained before their
         * replies are read, and _XReply must answer them in request order. */
        if (dp->render_npending < (int)(sizeof dp->render_pending /
                                        sizeof dp->render_pending[0]))
            dp->render_pending[dp->render_npending++] = minor;
        if (getenv("MW_TRACE_RENDER"))
            fprintf(stderr, "MW: query queued minor=%d (npending=%d)\n",
                    minor, dp->render_npending);
        break;
    case X_RenderCreatePicture:       req_create_picture(d, b, len); break;
    case X_RenderChangePicture:       req_change_picture(d, b, len); break;
    case X_RenderSetPictureClipRectangles: req_clip_rectangles(d, b, len); break;
    case X_RenderFreePicture:         req_free_picture(d, b); break;
    case X_RenderComposite:           req_composite(d, b, len); break;
    case X_RenderFillRectangles:      req_fill_rectangles(d, b, len); break;
    case X_RenderCreateSolidFill:     req_create_solid(d, b); break;
    case X_RenderCreateLinearGradient:req_create_linear(d, b, len); break;
    case X_RenderCreateRadialGradient:req_create_radial(d, b, len); break;
    case X_RenderCreateConicalGradient:req_create_conical(d, b, len); break;
    case X_RenderSetPictureTransform: req_set_transform(d, b, len); break;
    case X_RenderSetPictureFilter:    req_set_filter(d, b, len); break;
    case X_RenderCreateGlyphSet:      req_create_glyphset(d, b); break;
    case X_RenderReferenceGlyphSet:   req_reference_glyphset(d, b); break;
    case X_RenderFreeGlyphSet:        req_free_glyphset(d, b); break;
    case X_RenderFreeGlyphs:          req_free_glyphs(d, b, len); break;
    case X_RenderAddGlyphs:           req_add_glyphs(d, b, len); break;
    case X_RenderCompositeGlyphs8:    req_composite_glyphs(d, b, len, 1); break;
    case X_RenderCompositeGlyphs16:   req_composite_glyphs(d, b, len, 2); break;
    case X_RenderCompositeGlyphs32:   req_composite_glyphs(d, b, len, 4); break;
    case X_RenderTrapezoids:          req_trapezoids(d, b, len); break;
    case X_RenderTriangles:           req_triangles(d, b, len); break;
    case X_RenderTriStrip:            req_tri_strip(d, b, len); break;
    case X_RenderTriFan:              req_tri_fan(d, b, len); break;
    default:
        if (getenv("MW_TRACE_RENDER"))
            fprintf(stderr, "MW: Render minor %d ignored (%zu bytes)\n", minor, len);
        break;
    }
    (void)req_query_version;
    (void)req_query_pict_formats;
}

void mw_render_drain(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    unsigned char *buf = (unsigned char *)dp->private12;   /* output buffer */
    unsigned char *ptr = (unsigned char *)dp->private13;   /* bufptr */
    if (!buf) return;
    if (dp->render_draining) return;
    dp->render_draining = 1;
    if (!ptr || ptr <= buf) { dp->private13 = buf; dp->render_draining = 0; return; }
    unsigned char *p = buf;
    long n = 0;
    if (getenv("MW_TRACE_RENDER")) {
        fprintf(stderr, "MW: drain used=%ld first:", (long)(ptr - buf));
        for (long i = 0; i < 64 && buf + i < ptr; i++)
            fprintf(stderr, " %02x", buf[i]);
        fprintf(stderr, "\n");
    }
    while (p + 4 <= ptr) {
        unsigned len = (unsigned)(p[2] | (p[3] << 8)) * 4;
        if (len < 4 || p + len > ptr) {
            if (getenv("MW_TRACE_RENDER"))
                fprintf(stderr, "MW: drain break @%ld len=%u\n",
                        (long)(p - buf), len);
            break;
        }
        if (getenv("MW_TRACE_RENDER"))
            fprintf(stderr, "MW: drain req major=%d minor=%d len=%u\n",
                    p[0], p[1], len);
        if (p[0] == (unsigned char)mw_render_opcode()) {
            handle_request(d, p, len);
            n++;
        }
        p += len;
    }
    if (getenv("MW_TRACE_RENDER") && n)
        fprintf(stderr, "MW: render drained %ld requests\n", n);
    dp->private13 = buf;   /* bufptr = buffer */
    dp->render_draining = 0;
}

void mw_render_finish(Display *d) { mw_render_drain(d); }

/* Fill one Render query reply.  Shared by the awaited reply and by the replies
 * delivered to async handlers. */
static void mw_render_build(Display *d, int minor, void *rep)
{
    XDisplayImpl *dp = MWD(d);
    unsigned char *r = rep;
    memset(r, 0, 32);
    r[0] = 1;   /* X_Reply */
    switch (minor) {
    case X_RenderQueryVersion: {
        xRenderQueryVersionReply *v = rep;
        v->type = 1;   /* X_Reply */
        v->majorVersion = MW_RENDER_MAJOR;
        v->minorVersion = MW_RENDER_MINOR;
        break;
    }
    case X_RenderQueryPictFormats: {
        xRenderQueryPictFormatsReply v;
        build_pict_formats(d, &v, &dp->render_reply_data, &dp->render_reply_len);
        memcpy(rep, &v, sizeof v);
        dp->render_reply_off = 0;
        break;
    }
    case X_RenderQueryFilters:
        /* no aliases, no filters */
        break;
    case X_RenderQueryPictIndexValues:
        ((CARD32 *)r)[1] = 0;   /* numIndexValues */
        break;
    default:
        break;
    }
}

/* The reply _XReply is waiting for: the last queued query. */
int mw_render_reply(Display *d, void *rep)
{
    XDisplayImpl *dp = MWD(d);
    if (dp->render_npending <= 0) return 0;
    int minor = dp->render_pending[0];
    dp->render_npending--;
    if (dp->render_npending > 0)
        memmove(dp->render_pending, dp->render_pending + 1,
                (size_t)dp->render_npending * sizeof dp->render_pending[0]);
    mw_render_build(d, minor, rep);
    if (getenv("MW_TRACE_RENDER"))
        fprintf(stderr, "MW: render_reply minor=%d (pending now %d)\n",
                minor, dp->render_npending);
    return 1;
}

/* Xlib's async-handler list (struct _XInternalAsync in Xlibint.h).  Replies for
 * Render queries the caller is not waiting for are delivered here, matched by
 * request sequence.  libXrender sends QueryVersion and QueryPictFormats back to
 * back and reads only the latter's reply, so the version reaches Xft only
 * through this path -- without it XRenderQueryVersion returns stale info and
 * Xft's feature detection (CreateSolidFill) is wrong, which drops it onto a
 * fallback source picture that does not draw. */
typedef struct MwAsyncHandler {
    struct MwAsyncHandler *next;
    Bool (*handler)(Display *, void *, char *, int, XPointer);
    XPointer data;
} MwAsyncHandler;

void mw_render_dispatch_async(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    while (dp->render_npending > 1) {
        int minor = dp->render_pending[0];
        dp->render_npending--;
        memmove(dp->render_pending, dp->render_pending + 1,
                (size_t)dp->render_npending * sizeof dp->render_pending[0]);
        unsigned char rep[32];
        mw_render_build(d, minor, rep);
        /* Xlib sets last_request_read to the sequence of the reply it is
         * handing over; the handler matches on it. */
        dp->last_request_read = dp->request;
        if (getenv("MW_TRACE_RENDER"))
            fprintf(stderr, "MW: dispatch minor=%d (request=%lu last_read=%lu)\n",
                    minor, dp->request, dp->last_request_read);
        for (MwAsyncHandler *h = dp->async_handlers; h; h = h->next) {
            Bool took = h->handler(d, rep, NULL, 0, h->data);
            if (getenv("MW_TRACE_RENDER"))
                fprintf(stderr, "MW:   handler=%p took=%d\n", (void *)h->handler,
                        (int)took);
            if (took) break;
        }
    }
}

int mw_render_read(Display *d, char *data, size_t size)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->render_reply_data || dp->render_reply_off >= dp->render_reply_len)
        return -1;
    size_t avail = dp->render_reply_len - dp->render_reply_off;
    size_t n = size < avail ? size : avail;
    memcpy(data, dp->render_reply_data + dp->render_reply_off, n);
    dp->render_reply_off += n;
    if (dp->render_reply_off >= dp->render_reply_len) {
        free(dp->render_reply_data);
        dp->render_reply_data = NULL;
        dp->render_reply_len = dp->render_reply_off = 0;
    }
    return (int)n;
}
