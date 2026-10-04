/* raster.h — backend-neutral drawing interface for Motif/Wayland.
 *
 * No cairo/Skia/pixman types may leak through this header.  The shipped
 * backend is cairo (raster_cairo.c); an alternative backend (e.g. Skia) is a
 * single additional translation unit implementing exactly these symbols.
 *
 * All surfaces are 32-bit ARGB, premultiplied, native-endian (== wl_shm
 * ARGB8888) unless documented otherwise.
 */
#ifndef MW_RASTER_H
#define MW_RASTER_H

#include <stdint.h>
#include <stddef.h>
#include <X11/Xlib.h>   /* for XRectangle / XPoint in the API */

typedef struct MwSurface MwSurface;
typedef struct MwCanvas  MwCanvas;
typedef struct MwFont    MwFont;

/* ---------------------------------------------------------------- surfaces */

/* Allocate a new surface owning its pixel memory. */
MwSurface *mw_surface_create(int w, int h);

/* Wrap caller-owned memory (e.g. a wl_shm buffer).  stride is in bytes. */
MwSurface *mw_surface_create_for_data(void *data, int w, int h, int stride);

void mw_surface_destroy(MwSurface *s);

int  mw_surface_width(const MwSurface *s);
int  mw_surface_height(const MwSurface *s);
int  mw_surface_stride(const MwSurface *s);
void *mw_surface_data(const MwSurface *s);

/* Fill the whole surface with a pixel. */
void mw_surface_clear(MwSurface *s, uint32_t argb);

/* Write the surface to a PNG (debugging). */
void mw_surface_write_png(MwSurface *s, const char *path);

/* Signal that mw_surface_data() was written to directly. */
void mw_surface_mark_dirty(MwSurface *s);

/* ---------------------------------------------------------------- canvases */

/* Begin drawing.  Pushes a saved state and installs the given clip (in
 * surface coordinates).  Region is copied by the backend. */
MwCanvas *mw_canvas_begin(MwSurface *s, const XRectangle *clip, int nclip,
                          int clip_x_origin, int clip_y_origin);
void mw_canvas_end(MwCanvas *c);

/* GC state. */
void mw_set_operator(MwCanvas *c, int gx_function);
void mw_set_source_argb(MwCanvas *c, uint32_t argb);
void mw_set_line(MwCanvas *c, int width, int line_style, int cap, int join,
                 int dash_offset, const char *dashes, int ndash);
void mw_set_fill(MwCanvas *c, int fill_style, int fill_rule, int arc_mode);
/* A stipple/tile source used by FillStippled/FillTiled (ARGB, may be 1-bit). */
void mw_set_stipple(MwCanvas *c, MwSurface *stipple, int x_origin, int y_origin);

/* Clip management, in drawable coordinates. */
void mw_clip_rects(MwCanvas *c, const XRectangle *r, int n, int x_org, int y_org);
void mw_clip_mask(MwCanvas *c, MwSurface *mask, int x_org, int y_org);

/* Paint the current clip region with the source. */
void mw_paint(MwCanvas *c);

/* Fill / stroke the current path with the current source/operator. */
void mw_fill_path(MwCanvas *c);
void mw_stroke_path(MwCanvas *c);
/* Offset subsequently constructed path coordinates by half a pixel, to match
 * X's integer pixel grid for odd-width stroked lines. */
void mw_set_snap(MwCanvas *c, int on);
/* Fill the current path using `pattern` as an alpha stipple, tinted by the
 * current source colour (FillStippled / FillTiled approximation). */
void mw_fill_with_stipple(MwCanvas *c, MwSurface *pattern, int x_origin, int y_origin);

/* ------------------------------------------------------------------- paths */

void mw_rect(MwCanvas *c, double x, double y, double w, double h);
/* XArc semantics: x,y,w,h is the bounding box, angles in 1/64 degrees CCW from
 * +x axis, a2 is the sweep.  mode is ArcPieSlice/ArcChord. */
void mw_arc(MwCanvas *c, double x, double y, double w, double h,
            double a1, double a2, int mode);
/* relative != 0 => each point is relative to the previous (polyline). */
void mw_polygon(MwCanvas *c, const XPoint *pts, int n, int relative, int filled);
void mw_segments(MwCanvas *c, const XSegment *seg, int n);
void mw_rects(MwCanvas *c, const XRectangle *r, int n, int filled);

/* ------------------------------------------------------------------ pixels */

/* XCopyArea.  src is sampled at (sx,sy) and written at (dx,dy) for w*h. */
void mw_canvas_copy(MwCanvas *dst, MwSurface *src, int sx, int sy,
                    int dx, int dy, int w, int h);

/* XPutImage: blit an existing ARGB surface (img) into dst. */
void mw_canvas_put_image(MwCanvas *c, MwSurface *img, int dx, int dy,
                         int sx, int sy, int w, int h);

/* XCopyPlane: copy one bitplane of src (bit) into dst using fg/bg. */
void mw_canvas_copy_plane(MwCanvas *c, MwSurface *src, int sx, int sy,
                          int dx, int dy, int w, int h, int bit);

/* Store a clipping result (XGetImage) — copy surface pixels out. */
void mw_surface_get(MwSurface *s, int x, int y, int w, int h,
                    uint32_t *out, int out_stride_px);

/* -------------------------------------------------------------------- fonts */

/* Create a font from an XLFD/Fontconfig pattern (nullable => default). */
MwFont *mw_font_create(const char *pattern, int pixel_size_hint);
/* Create a font from a fontconfig pattern (the Xft path).  antialias enables
 * grayscale smoothing; the X core-font path leaves it off so glyph advances
 * stay consistent with the metrics reported through XTextWidth. */
MwFont *mw_font_create_fc(const void *fc_pattern, int pixel_size_hint, int antialias);
void    mw_font_destroy(MwFont *f);

/* Metrics. */
int mw_font_ascent(const MwFont *f);
int mw_font_descent(const MwFont *f);
int mw_font_max_width(const MwFont *f);
int mw_font_char_width(const MwFont *f, unsigned int ch); /* ch is a UCS-4 code */
int mw_font_char_lbearing(const MwFont *f, unsigned int ch);
int mw_font_char_rbearing(const MwFont *f, unsigned int ch);
/* FreeType glyph index for a UCS-4 code, and the underlying face (for Xft). */
int mw_font_char_index(const MwFont *f, unsigned int ch);
void *mw_font_ft_face(const MwFont *f);
/* Advance for a UTF-8 buffer. */
int mw_font_text_width_utf8(const MwFont *f, const char *s, int len);

void mw_set_font(MwCanvas *c, MwFont *f);
/* Draw UTF-8 text at baseline (x,y). */
void mw_show_utf8(MwCanvas *c, MwFont *f, const char *s, int len, int x, int y);

/* Draw a run of FreeType glyph indices at explicit baselines.  `glyphs` are
 * indices from the same face as mw_font_char_index(); `xs`/`ys` are length n.
 * This is the glyph-level path Xft's XftDrawGlyphSpec and friends need. */
void mw_show_glyphs(MwCanvas *c, MwFont *f, const unsigned int *glyphs,
                    const int *xs, const int *ys, int n);

/* Initialise/teardown any backend-global state. */
int  mw_raster_init(void);
void mw_raster_fini(void);

#endif /* MW_RASTER_H */
