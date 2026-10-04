/* xftbridge.c — implements the Xft shim's bridge onto the raster font path.
 * See src/xft/xftbridge.h for why this is split from libXft. */
#include "internal.h"

#include "xftbridge.h"

#include <stdlib.h>

void *mw_xft_font_open(const void *fc_pattern, int pixel_size_hint, int antialias)
{ return mw_font_create_fc(fc_pattern, pixel_size_hint, antialias); }

void mw_xft_font_close(void *font) { mw_font_destroy(font); }

int mw_xft_ascent(const void *font)      { return mw_font_ascent(font); }
int mw_xft_descent(const void *font)     { return mw_font_descent(font); }
int mw_xft_max_advance(const void *font) { return mw_font_max_width(font); }
int mw_xft_char_width(const void *font, unsigned int ucs4)
{ return mw_font_char_width(font, ucs4); }
int mw_xft_text_width(const void *font, const char *utf8, int len)
{ return mw_font_text_width_utf8(font, utf8, len); }
unsigned int mw_xft_char_index(const void *font, unsigned int ucs4)
{ return (unsigned int)mw_font_char_index(font, ucs4); }
void *mw_xft_face(const void *font) { return mw_font_ft_face(font); }

static MwCanvas *xft_canvas(Display *dpy, Drawable dr,
                            const XRectangle *clip, int nclip)
{
    int w, h;
    MwCanvas *c = mw_canvas_for_drawable(dpy, dr, NULL, &w, &h);
    if (c && clip && nclip > 0) mw_clip_rects(c, clip, nclip, 0, 0);
    return c;
}

static void xft_damage(Display *dpy, Drawable dr)
{
    MwWindow *w = mw_window(dpy, dr);
    if (w) mw_window_damage(w);
}

void mw_xft_draw_utf8(Display *dpy, Drawable dr, void *font, int x, int y,
                      const char *utf8, int len, unsigned int argb,
                      const XRectangle *clip, int nclip)
{
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: xft-draw %d bytes '%.*s'\n",
                len, len > 12 ? 12 : len, utf8);
    MwCanvas *c = xft_canvas(dpy, dr, clip, nclip);
    if (!c) return;
    mw_set_source_argb(c, argb);
    mw_show_utf8(c, font, utf8, len, x, y);
    mw_canvas_end(c);
    xft_damage(dpy, dr);
}

void mw_xft_fill_rect(Display *dpy, Drawable dr, int x, int y,
                      unsigned int w, unsigned int h, unsigned int argb,
                      const XRectangle *clip, int nclip)
{
    MwCanvas *c = xft_canvas(dpy, dr, clip, nclip);
    if (!c) return;
    mw_set_operator(c, GXcopy);
    mw_set_source_argb(c, argb);
    mw_rect(c, x, y, w, h);
    mw_fill_path(c);
    mw_canvas_end(c);
    xft_damage(dpy, dr);
}

void mw_xft_draw_glyphs(Display *dpy, Drawable dr, void *font,
                        const unsigned int *glyphs, const int *xs,
                        const int *ys, int n, unsigned int argb,
                        const XRectangle *clip, int nclip)
{
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: xft-draw-glyphs n=%d\n", n);
    MwCanvas *c = xft_canvas(dpy, dr, clip, nclip);
    if (!c) return;
    mw_set_source_argb(c, argb);
    mw_show_glyphs(c, font, glyphs, xs, ys, n);
    mw_canvas_end(c);
    xft_damage(dpy, dr);
}
