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

typedef struct {
    XID        id;
    int        format;
    MwSurface **glyphs;          /* by glyph id */
    int        *gw, *gh, *gx, *gy;   /* bitmap size + xOff/yOff (advance) */
    int        *gox, *goy;           /* xGlyphInfo.x/y: pen-to-bitmap bearing */
    int         cap;
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

static void rgba_of(cairo_t *cr, const xRenderColor *c)
{
    cairo_set_source_rgba(cr, c->red / 65535.0, c->green / 65535.0,
                          c->blue / 65535.0, c->alpha / 65535.0);
}

static void xf_matrix(cairo_matrix_t *m, const xRenderTransform *t)
{
    /* Render's 3x3 fixed matrix, column vectors; cairo is row vectors. */
    m->xx = frac(t->matrix11); m->yx = frac(t->matrix21);
    m->xy = frac(t->matrix12); m->yy = frac(t->matrix22);
    m->x0 = frac(t->matrix13); m->y0 = frac(t->matrix23);
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
        cairo_pattern_set_filter(pat, cairo_filter(src->filter));
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
        cairo_matrix_t t;
        xf_matrix(&t, &src->xf);
        cairo_matrix_multiply(&m, &m, &t);
    }
    cairo_pattern_set_matrix(pat, &m);
    cairo_pattern_set_extend(pat, cairo_extend(src->repeat));
    cairo_pattern_set_filter(pat, cairo_filter(src->filter));
    cairo_set_source(cr, pat);
    cairo_pattern_destroy(pat);
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

/* The core Composite. */
static void do_composite(Display *d, MwRenderPicture *dst, MwRenderPicture *src,
                         MwRenderPicture *mask, int op,
                         int xs, int ys, int xm, int ym,
                         int xd, int yd, int w, int h)
{
    if (!dst || w <= 0 || h <= 0) return;
    if (getenv("MW_TRACE_RENDER"))
        fprintf(stderr, "MW: composite op=%d src=0x%lx(k%d drw=0x%lx fmt=%d) mask=%s op=%d "
                        "src=%d,%d mask=%d,%d dst=%d,%d %dx%d\n",
                op, src ? src->id : 0, src ? src->kind : -1,
                src ? (unsigned long)src->drawable : 0, src ? src->format : -1,
                mask ? "yes" : "no", op, xs, ys, xm, ym, xd, yd, w, h);
    MwSurface *ds = pic_surface(d, dst);
    cairo_surface_t *dcs = ds ? mw_surface_native(ds) : NULL;
    if (!dcs) return;

    cairo_t *cr = cairo_create(dcs);
    cairo_save(cr);
    cairo_set_operator(cr, cairo_op(op));

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
                cairo_pattern_set_filter(mp, cairo_filter(mask->filter));
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
    for (size_t i = 0; i < n; i++) {
        const unsigned char *q = base + i * 12;
        memcpy(&p->stops[i].color, q, 8);
        memcpy(&p->stops[i].pos, q + 8, 4);
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
    if (mask & CPClipMask)        (void)NEXT();
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

static void glyphset_reserve(MwGlyphSet *gs, int gid)
{
    if (gid < gs->cap) return;
    int ncap = gs->cap ? gs->cap : 256;
    while (ncap <= gid) ncap *= 2;
    gs->glyphs = realloc(gs->glyphs, sizeof(MwSurface *) * ncap);
    gs->gw = realloc(gs->gw, sizeof(int) * ncap);
    gs->gh = realloc(gs->gh, sizeof(int) * ncap);
    gs->gx = realloc(gs->gx, sizeof(int) * ncap);
    gs->gy = realloc(gs->gy, sizeof(int) * ncap);
    gs->gox = realloc(gs->gox, sizeof(int) * ncap);
    gs->goy = realloc(gs->goy, sizeof(int) * ncap);
    for (int i = gs->cap; i < ncap; i++) {
        gs->glyphs[i] = NULL; gs->gw[i] = gs->gh[i] = 0;
        gs->gx[i] = gs->gy[i] = gs->gox[i] = gs->goy[i] = 0;
    }
    gs->cap = ncap;
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
        if (gs->glyphs[i]) mw_surface_destroy(gs->glyphs[i]);
    free(gs->glyphs); free(gs->gw); free(gs->gh);
    free(gs->gx); free(gs->gy); free(gs->gox); free(gs->goy);
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
    for (size_t i = 0; i < n; i++) {
        int g = (int)ids[i];
        if (g >= 0 && g < gs->cap && gs->glyphs[g]) {
            mw_surface_destroy(gs->glyphs[g]);
            gs->glyphs[g] = NULL;
        }
    }
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
        glyphset_reserve(gs, (int)gid);
        if (gs->glyphs[gid]) { mw_surface_destroy(gs->glyphs[gid]); }
        if (getenv("MW_TRACE_RENDER") && i == 0)
            fprintf(stderr, "MW: addglyph gid=%u w=%u h=%u x=%d y=%d xOff=%d yOff=%d d0=%d\n",
                    (unsigned)gid, info.width, info.height,
                    (int)info.x, (int)info.y,
                    (int)info.xOff, (int)info.yOff, p[0]);
        gs->glyphs[gid] = glyph_surface(p, need, info.width, info.height, depth1);
        gs->gw[gid] = info.width; gs->gh[gid] = info.height;
        gs->gx[gid] = info.xOff; gs->gy[gid] = info.yOff;   /* advance */
        gs->gox[gid] = info.x; gs->goy[gid] = info.y;       /* bearing */
        p += need;
    }
}

/* Draw one glyph at (x,y) in dst, using `src` (usually solid) and the glyph's
 * mask surface; the bitmap's top-left is at (x - xOff, y - yOff). */
static void draw_glyph(Display *d, MwRenderPicture *dst, MwRenderPicture *src,
                       int op, MwGlyphSet *gs, int gid, int x, int y)
{
    if (gid < 0 || gid >= gs->cap || !gs->glyphs[gid]) return;
    MwSurface *mask = gs->glyphs[gid];
    int w = gs->gw[gid], h = gs->gh[gid];
    /* The pen (x,y) is the glyph origin; the bitmap's top-left sits at the
     * glyph's stored bearing (xGlyphInfo.x/y), exactly as the X server
     * composites it (destination = pen - info). */
    int bx = x - gs->gox[gid];
    int by = y - gs->goy[gid];
    /* Decode mask onto the destination directly (cairo mask pattern needs a
     * drawable; a glyph mask is an in-memory surface, so composite it here). */
    MwSurface *ds = pic_surface(d, dst);
    cairo_surface_t *dcs = ds ? mw_surface_native(ds) : NULL;
    if (!dcs) return;
    cairo_t *cr = cairo_create(dcs);
    cairo_save(cr);
    cairo_set_operator(cr, cairo_op(op));
    cairo_rectangle(cr, bx, by, w, h);
    cairo_clip(cr);
    apply_dst_clip(cr, dst);
    set_source(d, cr, src, bx, by, bx, by);
    cairo_mask_surface(cr, mw_surface_native(mask), bx, by);
    cairo_restore(cr);
    cairo_destroy(cr);
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
            if (gid < (CARD32)gs->cap) {
                px += gs->gx[gid];
                py += gs->gy[gid];
            }
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

static void fill_poly(Display *d, MwRenderPicture *dst, MwRenderPicture *src,
                      int op, double *pts, int n)
{
    if (!dst || n < 3) return;
    MwSurface *ds = pic_surface(d, dst);
    cairo_surface_t *dcs = ds ? mw_surface_native(ds) : NULL;
    if (!dcs) return;
    cairo_t *cr = cairo_create(dcs);
    cairo_save(cr);
    cairo_set_operator(cr, cairo_op(op));
    apply_dst_clip(cr, dst);
    set_source(d, cr, src, 0, 0, 0, 0);
    cairo_move_to(cr, pts[0], pts[1]);
    for (int i = 1; i < n; i++) cairo_line_to(cr, pts[i*2], pts[i*2+1]);
    cairo_close_path(cr);
    cairo_fill(cr);
    cairo_restore(cr);
    cairo_destroy(cr);
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
    for (size_t i = 0; i < n; i++) {
        xTrapezoid t; memcpy(&t, p + i*sz_xTrapezoid, sz_xTrapezoid);
        double pts[8] = {
            frac(t.left.p1.x),  t.top / 65536.0,
            frac(t.right.p1.x), t.top / 65536.0,
            frac(t.right.p2.x), t.bottom / 65536.0,
            frac(t.left.p2.x),  t.bottom / 65536.0,
        };
        fill_poly(d, dst, src, r->op, pts, 4);
    }
}

static void req_triangles(Display *d, const unsigned char *b, size_t len)
{
    const xRenderTrianglesReq *r = (const void *)b;
    if (len < sz_xRenderTrianglesReq) return;
    MwRenderPicture *dst = pic(d, r->dst), *src = pic(d, r->src);
    size_t n = (len - sz_xRenderTrianglesReq) / sz_xTriangle;
    const unsigned char *p = b + sz_xRenderTrianglesReq;
    for (size_t i = 0; i < n; i++) {
        xTriangle t; memcpy(&t, p + i*sz_xTriangle, sz_xTriangle);
        double pts[6] = { frac(t.p1.x), frac(t.p1.y), frac(t.p2.x), frac(t.p2.y),
                          frac(t.p3.x), frac(t.p3.y) };
        fill_poly(d, dst, src, r->op, pts, 3);
    }
}

/* --------------------------------------------------------------- replies */

static void build_pict_formats(Display *d,
                               xRenderQueryPictFormatsReply *rep,
                               unsigned char **data, size_t *len)
{
    size_t nformats = 4;
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
    dp->render_reply_pending = 0;
    if (dp->render_reply_data) { free(dp->render_reply_data);
        dp->render_reply_data = NULL; dp->render_reply_len = dp->render_reply_off = 0; }

    switch (minor) {
    case X_RenderQueryVersion:
    case X_RenderQueryPictFormats:
    case X_RenderQueryFilters:
    case X_RenderQueryPictIndexValues:
        dp->render_reply_pending = minor;
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

int mw_render_reply(Display *d, void *rep)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->render_reply_pending) return 0;
    int minor = dp->render_reply_pending;
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
    dp->render_reply_pending = 0;
    return 1;
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
