/* xftbridge.h — the interface between the Xft shim (libXft) and the raster
 * font/canvas machinery inside libX11.
 *
 * Xft is a separate library from Xlib in a real system, and here too: the Xft
 * shim is built as libXft.so.2 next to the shim.  It needs to rasterise text
 * with the same FreeType/fontconfig/cairo path the shim already uses, so the
 * few operations it needs are exported from libX11 under mw_xft_* names.  The
 * font handle stays opaque (void *) to keep libXft free of shim internals.
 */
#ifndef MW_XFT_BRIDGE_H
#define MW_XFT_BRIDGE_H

#include <X11/Xlib.h>

/* Open a font from a fontconfig pattern (the pattern is not consumed).
 * antialias enables grayscale smoothing. */
void *mw_xft_font_open(const void *fc_pattern, int pixel_size_hint, int antialias);
void  mw_xft_font_close(void *font);

int   mw_xft_ascent      (const void *font);
int   mw_xft_descent     (const void *font);
int   mw_xft_max_advance (const void *font);
int   mw_xft_char_width  (const void *font, unsigned int ucs4);
int   mw_xft_text_width  (const void *font, const char *utf8, int len);
unsigned int mw_xft_char_index(const void *font, unsigned int ucs4);
void *mw_xft_face        (const void *font);

/* Draw UTF-8 text at baseline (x,y) in the given ARGB colour, clipped to
 * `clip` (nclip rectangles, already in drawable coordinates) when non-NULL. */
void  mw_xft_draw_utf8(Display *dpy, Drawable dr, void *font, int x, int y,
                       const char *utf8, int len, unsigned int argb,
                       const XRectangle *clip, int nclip);

/* Draw a run of FreeType glyph indices (from the same face as
 * mw_xft_char_index) at explicit baselines xs/ys. */
void  mw_xft_draw_glyphs(Display *dpy, Drawable dr, void *font,
                         const unsigned int *glyphs, const int *xs,
                         const int *ys, int n, unsigned int argb,
                         const XRectangle *clip, int nclip);

/* Fill a rectangle in the given ARGB colour, with the same clip handling. */
void  mw_xft_fill_rect(Display *dpy, Drawable dr, int x, int y,
                       unsigned int w, unsigned int h, unsigned int argb,
                       const XRectangle *clip, int nclip);

#endif
