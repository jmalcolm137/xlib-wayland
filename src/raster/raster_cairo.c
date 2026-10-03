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
    int              w, h, stride;
};

MwSurface *mw_surface_create(int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    MwSurface *s = calloc(1, sizeof *s);
    s->cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    s->owned = true;
    s->w = w; s->h = h;
    s->stride = cairo_image_surface_get_stride(s->cs);
    s->data = cairo_image_surface_get_data(s->cs);
    return s;
}

MwSurface *mw_surface_create_for_data(void *data, int w, int h, int stride)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (stride <= 0) stride = w * 4;
    MwSurface *s = calloc(1, sizeof *s);
    s->cs = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_ARGB32,
                                                w, h, stride);
    s->owned = false;
    s->data = data;
    s->w = w; s->h = h; s->stride = stride;
    return s;
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

/* ------------------------------------------------------------- canvas */

struct MwCanvas {
    cairo_t   *cr;
    MwSurface *surface;
    int        clip_x_origin, clip_y_origin;
    int        gx;
    int        lw;              /* line width */
    double     snap;            /* 0 or 0.5 pixel-grid nudge for strokes */
    MwSurface *stipple;
    cairo_pattern_t *pattern;
};

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
    cairo_set_antialias(c->cr, CAIRO_ANTIALIAS_NONE);
    if (clip && nclip > 0)
        mw_clip_rects(c, clip, nclip, clip_x_origin, clip_y_origin);
    return c;
}

void mw_canvas_end(MwCanvas *c)
{
    if (!c) return;
    if (c->pattern) cairo_pattern_destroy(c->pattern);
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
    case 1: lj = CAIRO_LINE_JOIN_MITER; break;
    case 2: lj = CAIRO_LINE_JOIN_ROUND; break;
    case 3: lj = CAIRO_LINE_JOIN_BEVEL; break;
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

void mw_set_stipple(MwCanvas *c, MwSurface *stipple, int x_origin, int y_origin)
{
    if (c->pattern) { cairo_pattern_destroy(c->pattern); c->pattern = NULL; }
    c->stipple = stipple;
    if (!stipple) return;
    cairo_pattern_t *p = cairo_pattern_create_for_surface(stipple->cs);
    cairo_pattern_set_extend(p, CAIRO_EXTEND_REPEAT);
    cairo_matrix_t m;
    cairo_matrix_init_translate(&m, x_origin, y_origin);
    cairo_pattern_set_matrix(p, &m);
    c->pattern = p;
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
    /* Approximate a 1-bit clip mask with its bounding box (documented). */
    (void)mask; (void)x_org; (void)y_org;
}

void mw_paint(MwCanvas *c)
{
    cairo_paint(c->cr);
}

void mw_fill_path(MwCanvas *c)
{
    cairo_fill(c->cr);
}

void mw_stroke_path(MwCanvas *c)
{
    cairo_stroke(c->cr);
}

void mw_fill_with_stipple(MwCanvas *c, MwSurface *pattern, int x_origin, int y_origin)
{
    if (!c || !pattern) return;
    cairo_pattern_t *p = cairo_pattern_create_for_surface(pattern->cs);
    cairo_pattern_set_extend(p, CAIRO_EXTEND_REPEAT);
    cairo_matrix_t m;
    cairo_matrix_init_translate(&m, x_origin, y_origin);
    cairo_pattern_set_matrix(p, &m);
    cairo_save(c->cr);
    /* cairo_mask() paints the *whole clip region*, masked by the pattern; it
     * does not honour the current path.  X fill-stippled and fill-tiled
     * rectangles only affect the rectangle being filled, so clip to the path
     * first.  Without this, every stippled fill covered the entire window with
     * the GC's foreground colour -- and Motif draws the text cursor as a
     * stippled fill, so every cursor blink turned a whole text field black,
     * flickering as it blinked. */
    cairo_clip(c->cr);
    cairo_mask(c->cr, p);   /* current source tinted by the pattern's alpha */
    cairo_restore(c->cr);
    cairo_pattern_destroy(p);
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
    cairo_save(cr);
    cairo_translate(cr, cx, cy);
    cairo_scale(cr, rx, ry);
    if (a2 >= 0)
        cairo_arc(cr, 0, 0, 1.0, s, e);
    else
        cairo_arc_negative(cr, 0, 0, 1.0, s, e);
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
    if (mode == ArcPieSlice) {
        double cx = x + w / 2.0, cy = y + h / 2.0;
        cairo_move_to(c->cr, cx, cy);
        double s = -(a1) * M_PI / (64.0 * 180.0);
        double e = -(a1 + a2) * M_PI / (64.0 * 180.0);
        cairo_save(c->cr);
        cairo_translate(c->cr, cx, cy);
        cairo_scale(c->cr, w / 2.0, h / 2.0);
        if (a2 >= 0) cairo_arc(c->cr, 0, 0, 1.0, s, e);
        else         cairo_arc_negative(c->cr, 0, 0, 1.0, s, e);
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

void mw_canvas_copy(MwCanvas *dst, MwSurface *src, int sx, int sy,
                    int dx, int dy, int w, int h)
{
    if (!dst || !src || w <= 0 || h <= 0) return;

    /* Copying a surface onto itself (XCopyArea used for scrolling, which the
     * Motif text widget does constantly) is undefined in cairo.  Snapshot the
     * source region first. */
    if (src == dst->surface) {
        cairo_surface_t *tmp = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
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

void mw_canvas_copy_plane(MwCanvas *c, MwSurface *src, int sx, int sy,
                          int dx, int dy, int w, int h, int bit)
{
    /* Render the plane as an alpha mask and mask the current source over it. */
    if (!src || w <= 0 || h <= 0) return;
    cairo_surface_t *a8 = cairo_image_surface_create(CAIRO_FORMAT_A8, w, h);
    unsigned char *ad = cairo_image_surface_get_data(a8);
    int as = cairo_image_surface_get_stride(a8);
    cairo_surface_flush(src->cs);
    const uint32_t *sp = (const uint32_t *)src->data;
    int sstride = src->stride / 4;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int sxs = sx + x, sys = sy + y;
            uint32_t px = 0;
            if (sxs >= 0 && sys >= 0 && sxs < src->w && sys < src->h)
                px = sp[(size_t)sys * sstride + sxs];
            ad[(size_t)y * as + x] = (px >> bit) & 1 ? 0xff : 0x00;
        }
    }
    cairo_surface_mark_dirty(a8);
    cairo_save(c->cr);
    cairo_rectangle(c->cr, dx, dy, w, h);
    cairo_clip(c->cr);
    cairo_set_source_surface(c->cr, a8, dx, dy);
    cairo_mask_surface(c->cr, a8, dx, dy);
    cairo_restore(c->cr);
    cairo_surface_destroy(a8);
}

void mw_surface_get(MwSurface *s, int x, int y, int w, int h,
                    uint32_t *out, int out_stride_px)
{
    if (!s || !out) return;
    cairo_surface_flush(s->cs);
    const uint32_t *sp = (const uint32_t *)s->data;
    int sstride = s->stride / 4;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int sx = x + i, sy = y + j;
            uint32_t px = 0;
            if (sx >= 0 && sy >= 0 && sx < s->w && sy < s->h)
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
    return font_from_match(font_match(pat), px, 0);
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
    if (f->ft)     FT_Done_Face(f->ft);
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
