/* xft.c — a small Xft implementation over the shim's own font path.
 *
 * Xft normally draws through the X Render extension: it uploads glyphs to the
 * server and composites them.  The shim rasterises text with
 * FreeType/fontconfig/cairo (see src/raster/raster_cairo.c) and implements the
 * Render extension on that same backend (src/xlib/render.c), so the Xft entry
 * points Motif uses are served from the raster font path through the mw_xft_*
 * bridge and the Render-level picture entry points hand out real Render
 * pictures.  The public structures (XftFont, XftColor, XftDraw) match Xft's
 * headers, so callers see the same ABI.
 *
 * This is deliberately a subset: it covers what a Motif application calls to
 * draw and measure strings, plus the picture accessors Render-drawing clients
 * (xclock's clock face, for one) use.  The glyph-upload Render entry points are
 * still stubs.
 */
#include <X11/Xft/Xft.h>
#include <X11/extensions/Xrender.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xftbridge.h"

struct _XftDraw {
    Display    *dpy;
    Drawable    drawable;
    Visual     *visual;      /* for the Render picture format, or NULL */
    Picture     picture;     /* cached XftDrawPicture(), None until asked */
    Picture     src_picture; /* cached XftDrawSrcPicture(), None until asked */
    unsigned long src_pixel; /* colour the cached source was built for */
    XRectangle *clip;        /* rectangles in drawable coordinates, or NULL */
    int         nclip;
};

/* The public XftFont is the first member so an XftFont* can be cast to this. */
typedef struct {
    XftFont pub;
    void   *raster;
} MwXftFont;

static unsigned xft_argb(const XftColor *c)
{
    unsigned a = c->color.alpha >> 8;
    unsigned r = c->color.red   >> 8;
    unsigned g = c->color.green >> 8;
    unsigned b = c->color.blue  >> 8;
    if (!a) a = 0xff;                       /* unset alpha means opaque */
    if (!r && !g && !b) {                   /* caller set only the pixel */
        unsigned long px = c->pixel & 0xffffff;
        r = (unsigned)(px >> 16) & 0xff;
        g = (unsigned)(px >> 8) & 0xff;
        b = (unsigned)px & 0xff;
    }
    return (a << 24) | (r << 16) | (g << 8) | b;
}

/* ------------------------------------------------ character conversion */

static int utf8_put(unsigned int c, char *o)
{
    if (c < 0x80) { o[0] = (char)c; return 1; }
    if (c < 0x800) {
        o[0] = (char)(0xC0 | (c >> 6)); o[1] = (char)(0x80 | (c & 0x3F)); return 2;
    }
    if (c < 0x10000) {
        o[0] = (char)(0xE0 | (c >> 12));
        o[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        o[2] = (char)(0x80 | (c & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (c >> 18));
    o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((c >> 6) & 0x3F));
    o[3] = (char)(0x80 | (c & 0x3F));
    return 4;
}

/* Convert 8/16/32-bit characters to UTF-8.  The 8-bit form is Latin-1. */
static char *to_utf8(const void *s, int len, int bits, int *out_len)
{
    if (bits == 8) {
        char *o = malloc((size_t)len * 2 + 1);
        int n = 0;
        for (int i = 0; i < len; i++)
            n += utf8_put(((const unsigned char *)s)[i], o + n);
        o[n] = 0; *out_len = n;
        return o;
    }
    char *o = malloc((size_t)len * 4 + 1);
    int n = 0;
    for (int i = 0; i < len; i++) {
        unsigned int c = (bits == 16) ? ((const unsigned short *)s)[i]
                                      : ((const unsigned int *)s)[i];
        n += utf8_put(c, o + n);
    }
    o[n] = 0; *out_len = n;
    return o;
}

/* ------------------------------------------------------------- draws */

XftDraw *XftDrawCreate(Display *dpy, Drawable drawable, Visual *visual,
                       Colormap colormap)
{
    (void)colormap;
    XftDraw *d = calloc(1, sizeof *d);
    if (d) { d->dpy = dpy; d->drawable = drawable; d->visual = visual; }
    return d;
}

XftDraw *XftDrawCreateBitmap(Display *dpy, Pixmap bitmap)
{ return XftDrawCreate(dpy, bitmap, NULL, None); }

void XftDrawDestroy(XftDraw *draw)
{
    if (!draw) return;
    if (draw->picture)     XRenderFreePicture(draw->dpy, draw->picture);
    if (draw->src_picture) XRenderFreePicture(draw->dpy, draw->src_picture);
    free(draw->clip);
    free(draw);
}

void XftDrawRect(XftDraw *draw, _Xconst XftColor *color, int x, int y,
                 unsigned int width, unsigned int height)
{
    if (!draw || !color) return;
    mw_xft_fill_rect(draw->dpy, draw->drawable, x, y, width, height,
                     xft_argb(color), draw->clip, draw->nclip);
}

Bool XftDrawSetClip(XftDraw *draw, Region r)
{
    if (!draw) return False;
    free(draw->clip);
    draw->clip = NULL;
    draw->nclip = 0;
    if (!r) return True;
    /* A Region's rectangles are private; its bounding box is all a caller can
     * ask for portably.  Motif clips to single rectangles in practice. */
    XRectangle box;
    XClipBox(r, &box);
    draw->clip = malloc(sizeof box);
    if (draw->clip) { draw->clip[0] = box; draw->nclip = 1; }
    return True;
}

Bool XftDrawSetClipRectangles(XftDraw *draw, int xOrigin, int yOrigin,
                              _Xconst XRectangle *rects, int n)
{
    if (!draw) return False;
    free(draw->clip);
    draw->clip = NULL;
    draw->nclip = 0;
    if (!rects || n <= 0) return True;
    draw->clip = malloc(sizeof(XRectangle) * (size_t)n);
    if (!draw->clip) return False;
    for (int i = 0; i < n; i++) {
        draw->clip[i] = rects[i];
        draw->clip[i].x = (short)(draw->clip[i].x + xOrigin);
        draw->clip[i].y = (short)(draw->clip[i].y + yOrigin);
    }
    draw->nclip = n;
    return True;
}

static void draw_string(XftDraw *draw, _Xconst XftColor *color, XftFont *pub,
                        int x, int y, const void *s, int len, int bits)
{
    if (!draw || !color || !pub || len <= 0) return;
    const char *u = s;
    int n = len;
    char *conv = NULL;
    if (bits) { conv = to_utf8(s, len, bits, &n); if (!conv) return; u = conv; }
    mw_xft_draw_utf8(draw->dpy, draw->drawable, ((MwXftFont *)pub)->raster,
                     x, y, u, n, xft_argb(color), draw->clip, draw->nclip);
    free(conv);
}

void XftDrawString8(XftDraw *draw, _Xconst XftColor *color, XftFont *pub,
                    int x, int y, _Xconst XftChar8 *string, int len)
{ draw_string(draw, color, pub, x, y, string, len, 8); }

void XftDrawString16(XftDraw *draw, _Xconst XftColor *color, XftFont *pub,
                     int x, int y, _Xconst XftChar16 *string, int len)
{ draw_string(draw, color, pub, x, y, string, len, 16); }

void XftDrawString32(XftDraw *draw, _Xconst XftColor *color, XftFont *pub,
                     int x, int y, _Xconst XftChar32 *string, int len)
{ draw_string(draw, color, pub, x, y, string, len, 32); }

void XftDrawStringUtf8(XftDraw *draw, _Xconst XftColor *color, XftFont *pub,
                       int x, int y, _Xconst FcChar8 *string, int len)
{ draw_string(draw, color, pub, x, y, string, len, 0); }

/* ---------------------------------------------------------------- fonts */

FcPattern *XftFontMatch(Display *dpy, int screen, _Xconst FcPattern *pattern,
                        FcResult *result)
{
    (void)dpy; (void)screen;
    FcPattern *p = pattern ? FcPatternDuplicate(pattern) : FcPatternCreate();
    if (!p) { if (result) *result = FcResultNoMatch; return NULL; }
    FcConfigSubstitute(NULL, p, FcMatchPattern);
    FcDefaultSubstitute(p);
    FcPattern *match = FcFontMatch(NULL, p, result);
    FcPatternDestroy(p);
    return match;
}

XftFont *XftFontOpenPattern(Display *dpy, FcPattern *pattern)
{
    (void)dpy;
    if (getenv("MW_TRACE")) {
        FcChar8 *fam = NULL; double px = 0;
        if (pattern) {
            FcPatternGetString(pattern, FC_FAMILY, 0, &fam);
            FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &px);
        }
        fprintf(stderr, "MW: XftFontOpenPattern family=%s px=%.1f\n",
                fam ? (char *)fam : "(null)", px);
    }
    if (!pattern) return NULL;
    double px = 0;
    FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &px);
    MwXftFont *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->raster = mw_xft_font_open(pattern, px > 0 ? (int)(px + 0.5) : 0, 1);
    if (!f->raster) { free(f); return NULL; }
    f->pub.ascent  = mw_xft_ascent(f->raster);
    f->pub.descent = mw_xft_descent(f->raster);
    f->pub.height  = f->pub.ascent + f->pub.descent;
    f->pub.max_advance_width = mw_xft_max_advance(f->raster);
    f->pub.charset = NULL;
    f->pub.pattern = pattern;      /* Xft takes ownership of the pattern */
    return &f->pub;
}

void XftFontClose(Display *dpy, XftFont *pub)
{
    (void)dpy;
    if (!pub) return;
    MwXftFont *f = (MwXftFont *)pub;
    mw_xft_font_close(f->raster);
    if (pub->pattern) FcPatternDestroy(pub->pattern);
    free(f);
}

XftFont *XftFontOpenName(Display *dpy, int screen, _Xconst char *name)
{
    FcPattern *pat = FcNameParse((const FcChar8 *)name);
    if (!pat) return NULL;
    FcResult res;
    FcPattern *match = XftFontMatch(dpy, screen, pat, &res);
    FcPatternDestroy(pat);
    if (!match) return NULL;
    return XftFontOpenPattern(dpy, match);
}

XftFont *XftFontOpen(Display *dpy, int screen, ...)
{
    va_list ap;
    va_start(ap, screen);
    FcPattern *pat = FcPatternVaBuild(NULL, ap);
    va_end(ap);
    if (!pat) return NULL;
    FcResult res;
    FcPattern *match = XftFontMatch(dpy, screen, pat, &res);
    FcPatternDestroy(pat);
    if (!match) return NULL;
    return XftFontOpenPattern(dpy, match);
}

Bool XftCharExists(Display *dpy, XftFont *pub, FcChar32 ucs4)
{
    (void)dpy;
    if (!pub) return False;
    if (pub->pattern) {
        FcCharSet *cs = NULL;
        if (FcPatternGetCharSet(pub->pattern, FC_CHARSET, 0, &cs) == FcResultMatch)
            return FcCharSetHasChar(cs, ucs4);
    }
    return mw_xft_char_width(((MwXftFont *)pub)->raster, ucs4) != 0;
}

/* ------------------------------------------------------------ extents */

static void set_extents(XftFont *pub, const char *u, int n, XGlyphInfo *extents)
{
    memset(extents, 0, sizeof *extents);
    if (!pub) return;
    int w = mw_xft_text_width(((MwXftFont *)pub)->raster, u, n);
    extents->width  = (unsigned short)(w < 0 ? 0 : w);
    extents->height = (unsigned short)pub->height;
    extents->x      = 0;
    extents->y      = (short)pub->ascent;
}

static void text_extents(Display *dpy, XftFont *pub, const void *s, int len,
                         int bits, XGlyphInfo *extents)
{
    (void)dpy;
    if (!extents) return;
    if (!pub) { memset(extents, 0, sizeof *extents); return; }
    if (!bits) {
        set_extents(pub, s, len, extents);
        return;
    }
    int n;
    char *u = to_utf8(s, len, bits, &n);
    if (!u) { memset(extents, 0, sizeof *extents); return; }
    set_extents(pub, u, n, extents);
    free(u);
}

void XftTextExtents8(Display *dpy, XftFont *pub, _Xconst FcChar8 *string,
                     int len, XGlyphInfo *extents)
{ text_extents(dpy, pub, string, len, 8, extents); }

void XftTextExtents16(Display *dpy, XftFont *pub, _Xconst XftChar16 *string,
                      int len, XGlyphInfo *extents)
{ text_extents(dpy, pub, string, len, 16, extents); }

void XftTextExtents32(Display *dpy, XftFont *pub, _Xconst XftChar32 *string,
                      int len, XGlyphInfo *extents)
{ text_extents(dpy, pub, string, len, 32, extents); }

void XftTextExtentsUtf8(Display *dpy, XftFont *pub, _Xconst FcChar8 *string,
                        int len, XGlyphInfo *extents)
{ text_extents(dpy, pub, string, len, 0, extents); }

/* ------------------------------------------------------------- colours */

Bool XftColorAllocValue(Display *dpy, Visual *visual, Colormap cmap,
                        _Xconst XRenderColor *color, XftColor *result)
{
    (void)visual;
    if (!color || !result) return False;
    result->color = *color;
    XColor xc;
    xc.red = color->red; xc.green = color->green; xc.blue = color->blue;
    xc.flags = DoRed | DoGreen | DoBlue;
    if (XAllocColor(dpy, cmap, &xc)) {
        result->pixel = xc.pixel;
    } else {
        result->pixel = (unsigned long)((color->red >> 8) << 16 |
                                        (color->green >> 8) << 8 |
                                        (color->blue >> 8));
    }
    return True;
}

Bool XftColorAllocName(Display *dpy, _Xconst Visual *visual, Colormap cmap,
                       _Xconst char *name, XftColor *result)
{
    (void)visual;
    if (!name || !result) return False;
    XColor xc, exact;
    if (!XAllocNamedColor(dpy, cmap, name, &xc, &exact)) return False;
    result->pixel = xc.pixel;
    result->color.red = xc.red;
    result->color.green = xc.green;
    result->color.blue = xc.blue;
    result->color.alpha = 0xffff;
    return True;
}

void XftColorFree(Display *dpy, Visual *visual, Colormap cmap, XftColor *color)
{
    (void)visual;
    if (color) XFreeColors(dpy, cmap, &color->pixel, 1, 0);
}

/* ------------------------------------------------ names, glyphs, default */

FcPattern *XftNameParse(_Xconst char *name)
{ return FcNameParse((const FcChar8 *)name); }

FcBool XftNameUnparse(FcPattern *pat, char *dest, int len)
{
    FcChar8 *s = FcNameUnparse(pat);
    if (!s) return False;
    if (dest && len > 0) {
        strncpy(dest, (const char *)s, (size_t)len - 1);
        dest[len - 1] = 0;
    }
    free(s);
    return True;
}

FT_UInt XftCharIndex(Display *dpy, XftFont *pub, FcChar32 ucs4)
{
    (void)dpy;
    if (!pub) return 0;
    return (FT_UInt)mw_xft_char_index(((MwXftFont *)pub)->raster, ucs4);
}

FT_Face XftLockFace(XftFont *pub)
{
    if (!pub) return NULL;
    return (FT_Face)mw_xft_face(((MwXftFont *)pub)->raster);
}

void XftUnlockFace(XftFont *pub) { (void)pub; }

Bool XftDefaultSet(Display *dpy, FcPattern *defaults)
{
    (void)dpy;
    /* fontconfig's own defaults already apply; accept and discard the caller's
     * so applications that set defaults do not fail. */
    if (defaults) FcPatternDestroy(defaults);
    return True;
}

void XftDefaultSubstitute(Display *dpy, int screen, FcPattern *pattern)
{
    (void)dpy; (void)screen;
    if (pattern) FcDefaultSubstitute(pattern);
}

void XftDrawCharSpec(XftDraw *draw, _Xconst XftColor *color, XftFont *pub,
                     _Xconst XftCharSpec *chars, int len)
{
    if (!draw || !color || !pub || !chars) return;
    for (int i = 0; i < len; i++) {
        char u[8];
        int n = utf8_put(chars[i].ucs4, u);
        mw_xft_draw_utf8(draw->dpy, draw->drawable, ((MwXftFont *)pub)->raster,
                         chars[i].x, chars[i].y, u, n, xft_argb(color),
                         draw->clip, draw->nclip);
    }
}

/* The Render-level picture entry points.  Xft clients that draw through Render
 * (xclock's clock face, for one) call XftDrawPicture()/XftDrawSrcPicture() and
 * hand the results to XRenderComposite*.  The shim now implements the Render
 * extension on its raster backend, so these can hand out real pictures rather
 * than None -- with None every such client composited against picture 0 and
 * drew nothing. */
Picture XftDrawPicture(XftDraw *draw)
{
    if (!draw) return None;
    if (draw->picture) return draw->picture;
    if (!draw->visual) return None;
    XRenderPictFormat *fmt = XRenderFindVisualFormat(draw->dpy, draw->visual);
    if (!fmt) return None;
    draw->picture = XRenderCreatePicture(draw->dpy, draw->drawable, fmt, 0, NULL);
    return draw->picture;
}

Picture XftDrawSrcPicture(XftDraw *draw, _Xconst XftColor *color)
{
    if (!draw || !color) return None;
    if (draw->src_picture && draw->src_pixel == color->pixel)
        return draw->src_picture;
    if (draw->src_picture) {
        XRenderFreePicture(draw->dpy, draw->src_picture);
        draw->src_picture = None;
    }
    XRenderColor rc = color->color;
    if (!rc.alpha && !rc.red && !rc.green && !rc.blue) {
        /* The caller set only the pixel; split it into components. */
        unsigned long px = color->pixel;
        rc.red   = (unsigned short)((px >> 16) & 0xff) * 0x101;
        rc.green = (unsigned short)((px >> 8) & 0xff) * 0x101;
        rc.blue  = (unsigned short)(px & 0xff) * 0x101;
        rc.alpha = 0xffff;
    }
    draw->src_picture = XRenderCreateSolidFill(draw->dpy, &rc);
    draw->src_pixel = color->pixel;
    return draw->src_picture;
}

/* ---------------------------------------------------- glyph-level drawing
 *
 * Xft's glyph APIs come in two families.  The *draw-level* ones take an
 * XftDraw, an XftFont and a colour, and composite straight onto a drawable:
 * those are implemented here on the shim's cairo font path.  The *Render-level*
 * ones (XftGlyphSpecRender, XftGlyphFontSpecRender, XftCharSpecRender, ...)
 * take Render Pictures -- a source and a destination -- and are the way a
 * Render-backed Xft uploads glyphs to a server.  The shim's Render backend
 * rasterises pictures but does not yet accept uploaded glyph sets, so these
 * glyph-upload entry points are still stubs; a caller that needs them (Pango's
 * legacy Xft renderer is the main one) draws nothing rather than mis-drawing.
 */

static int ft_glyph_advance(XftFont *pub, FT_UInt glyph)
{
    if (!pub || !glyph) return 0;
    FT_Face face = (FT_Face)mw_xft_face(((MwXftFont *)pub)->raster);
    if (!face) return 0;
    if (FT_Load_Glyph(face, glyph, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0)
        return 0;
    return (int)((face->glyph->metrics.horiAdvance + 63) / 64);
}

Bool XftDefaultHasRender(Display *dpy)
{
    /* Xft uses this to decide whether it can upload glyphs through the Render
     * extension.  The shim has Render, but glyph upload is one of the stubs
     * above, so it still answers no; callers that only need the draw-level API
     * (and the picture accessors) work regardless. */
    (void)dpy;
    return False;
}

void XftGlyphExtents(Display *dpy, XftFont *pub, _Xconst FT_UInt *glyphs,
                     int nglyphs, XGlyphInfo *extents)
{
    (void)dpy;
    if (!extents) return;
    memset(extents, 0, sizeof *extents);
    if (!pub || !glyphs || nglyphs <= 0) return;
    int w = 0;
    for (int i = 0; i < nglyphs; i++)
        w += ft_glyph_advance(pub, glyphs[i]);
    extents->width  = (unsigned short)(w < 0 ? 0 : w);
    extents->height = (unsigned short)pub->height;
    extents->x      = 0;
    extents->y      = (short)pub->ascent;
}

static void draw_glyph_run(XftDraw *draw, _Xconst XftColor *color,
                           XftFont *pub, const FT_UInt *glyphs,
                           const int *xs, const int *ys, int n)
{
    if (!draw || !color || !pub || !glyphs || n <= 0) return;
    mw_xft_draw_glyphs(draw->dpy, draw->drawable,
                       ((MwXftFont *)pub)->raster, glyphs, xs, ys, n,
                       xft_argb(color), draw->clip, draw->nclip);
}

void XftDrawGlyphs(XftDraw *draw, _Xconst XftColor *color, XftFont *pub,
                   int x, int y, _Xconst FT_UInt *glyphs, int nglyphs)
{
    if (!draw || !color || !pub || !glyphs || nglyphs <= 0) return;
    int *xs = malloc(sizeof(int) * (size_t)nglyphs);
    int *ys = malloc(sizeof(int) * (size_t)nglyphs);
    if (!xs || !ys) { free(xs); free(ys); return; }
    int px = x;
    for (int i = 0; i < nglyphs; i++) {
        xs[i] = px;
        ys[i] = y;
        px += ft_glyph_advance(pub, glyphs[i]);
    }
    draw_glyph_run(draw, color, pub, glyphs, xs, ys, nglyphs);
    free(xs);
    free(ys);
}

void XftDrawGlyphSpec(XftDraw *draw, _Xconst XftColor *color, XftFont *pub,
                      _Xconst XftGlyphSpec *glyphs, int len)
{
    if (!draw || !color || !pub || !glyphs || len <= 0) return;
    unsigned int *gl = malloc(sizeof(unsigned int) * (size_t)len);
    int *xs = malloc(sizeof(int) * (size_t)len);
    int *ys = malloc(sizeof(int) * (size_t)len);
    if (!gl || !xs || !ys) { free(gl); free(xs); free(ys); return; }
    for (int i = 0; i < len; i++) {
        gl[i] = glyphs[i].glyph;
        xs[i] = glyphs[i].x;
        ys[i] = glyphs[i].y;
    }
    draw_glyph_run(draw, color, pub, gl, xs, ys, len);
    free(gl);
    free(xs);
    free(ys);
}

void XftDrawGlyphFontSpec(XftDraw *draw, _Xconst XftColor *color,
                          _Xconst XftGlyphFontSpec *glyphs, int len)
{
    if (!draw || !color || !glyphs || len <= 0) return;
    for (int i = 0; i < len; i++) {
        unsigned int g = glyphs[i].glyph;
        int x = glyphs[i].x;
        int y = glyphs[i].y;
        draw_glyph_run(draw, color, glyphs[i].font, &g, &x, &y, 1);
    }
}

void XftDrawCharFontSpec(XftDraw *draw, _Xconst XftColor *color,
                         _Xconst XftCharFontSpec *chars, int len)
{
    if (!draw || !color || !chars || len <= 0) return;
    for (int i = 0; i < len; i++) {
        XftFont *pub = chars[i].font;
        unsigned int g = XftCharIndex(draw->dpy, pub, chars[i].ucs4);
        int x = chars[i].x;
        int y = chars[i].y;
        draw_glyph_run(draw, color, pub, &g, &x, &y, 1);
    }
}

/* ---------------------------------------------------------------- init */
int XftInit(_Xconst char *config) { (void)config; return 1; }
FcBool XftInitFtLibrary(void) { return 1; }

/* ------------------------------------------------ Render-level entry points
 *
 * These composite through Render Pictures.  XftDrawPicture() hands out a real
 * picture, but these upload glyphs through Render glyph sets, which the shim
 * does not implement, so they remain stubs; pangoxft only reaches them when a
 * caller has explicitly installed a source Picture with
 * pango_xft_renderer_set_source(); its normal path draws through the draw-level
 * API above, which is implemented.  They exist so pangoxft (marco and
 * mate-panel) links.  Each says so once on stderr: a toolkit that needs one
 * should show up as "missing text" in triage, not as a silent blank.
 */
void XftGlyphSpecRender(Display *dpy, int op, Picture src, XftFont *pub,
                        Picture dst, int srcx, int srcy,
                        _Xconst XftGlyphSpec *glyphs, int nglyphs)
{
    static int warned;
    if (!warned++)
        fprintf(stderr, "MW: XftGlyphSpecRender is a stub: Render glyph-set "
                        "upload is not implemented, so its text is missing\n");
    (void)dpy; (void)op; (void)src; (void)pub; (void)dst; (void)srcx; (void)srcy; (void)glyphs; (void)nglyphs;
}

void XftCharSpecRender(Display *dpy, int op, Picture src, XftFont *pub,
                       Picture dst, int srcx, int srcy,
                       _Xconst XftCharSpec *chars, int len)
{
    static int warned;
    if (!warned++)
        fprintf(stderr, "MW: XftCharSpecRender is a stub: Render glyph-set "
                        "upload is not implemented, so its text is missing\n");
    (void)dpy; (void)op; (void)src; (void)pub; (void)dst; (void)srcx; (void)srcy; (void)chars; (void)len;
}

void XftGlyphFontSpecRender(Display *dpy, int op, Picture src, Picture dst,
                            int srcx, int srcy,
                            _Xconst XftGlyphFontSpec *glyphs, int nglyphs)
{
    static int warned;
    if (!warned++)
        fprintf(stderr, "MW: XftGlyphFontSpecRender is a stub: Render glyph-set "
                        "upload is not implemented, so its text is missing\n");
    (void)dpy; (void)op; (void)src; (void)dst; (void)srcx; (void)srcy; (void)glyphs; (void)nglyphs;
}

void XftCharFontSpecRender(Display *dpy, int op, Picture src, Picture dst,
                           int srcx, int srcy,
                           _Xconst XftCharFontSpec *chars, int len)
{
    static int warned;
    if (!warned++)
        fprintf(stderr, "MW: XftCharFontSpecRender is a stub: Render glyph-set "
                        "upload is not implemented, so its text is missing\n");
    (void)dpy; (void)op; (void)src; (void)dst; (void)srcx; (void)srcy; (void)chars; (void)len;
}
