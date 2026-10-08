/* xcursor.c — a libXcursor facade over the Wayland Xlib shim.
 *
 * Installed as libXcursor.so.1.  GDK loads themed cursors through libXcursor;
 * with the system library it would upload cursor images to an X server we do
 * not have.  Here the named/shape lookups become Wayland cursor-theme lookups
 * (via libX11's mw_cursor_* helpers) and image cursors become ARGB Wayland
 * cursor buffers.  Loading a cursor *image* back out (Xcursor*LoadImages) is not
 * supported: GDK degrades to a NULL image, which its callers handle.
 */
#include <X11/Xcursor/Xcursor.h>
#include <X11/Xlib.h>

#include <stdlib.h>
#include <string.h>

/* Shim entry points (src/xlib/cursor.c), exported from libX11.so.6. */
extern Cursor mw_cursor_load_named(Display *, const char *);
extern Cursor mw_cursor_load_shape(Display *, unsigned int);
extern Cursor mw_cursor_from_image(Display *, int, int, int, int, const uint32_t *);
extern char  *mw_cursor_theme_get(Display *);
extern int    mw_cursor_size_get(Display *);
extern void   mw_cursor_theme_set(Display *, const char *);
extern void   mw_cursor_size_set(Display *, int);

XcursorImage *XcursorImageCreate(int width, int height)
{
    if (width <= 0 || height <= 0) return NULL;
    XcursorImage *image = calloc(1, sizeof *image);
    if (!image) return NULL;
    image->version = XCURSOR_IMAGE_VERSION;
    image->size = width > height ? (XcursorDim)width : (XcursorDim)height;
    image->width = (XcursorDim)width;
    image->height = (XcursorDim)height;
    image->pixels = calloc((size_t)width * height, sizeof(XcursorPixel));
    if (!image->pixels) { free(image); return NULL; }
    return image;
}

void XcursorImageDestroy(XcursorImage *image)
{
    if (!image) return;
    free(image->pixels);
    free(image);
}

void XcursorImagesDestroy(XcursorImages *images)
{
    if (!images) return;
    for (int i = 0; i < images->nimage; i++)
        XcursorImageDestroy(images->images[i]);
    free(images->images);
    free(images->name);
    free(images);
}

/* Reading a themed cursor back as pixels is not implemented; GDK's
 * gdk_cursor_get_image() then returns NULL, as documented. */
XcursorImages *XcursorLibraryLoadImages(const char *library, const char *theme,
                                        int size)
{
    (void)library; (void)theme; (void)size;
    return NULL;
}

XcursorImages *XcursorShapeLoadImages(unsigned int shape, const char *theme,
                                      int size)
{
    (void)shape; (void)theme; (void)size;
    return NULL;
}

Cursor XcursorImageLoadCursor(Display *dpy, const XcursorImage *image)
{
    if (!image || !image->pixels) return None;
    return mw_cursor_from_image(dpy, (int)image->width, (int)image->height,
                                (int)image->xhot, (int)image->yhot,
                                image->pixels);
}

Cursor XcursorLibraryLoadCursor(Display *dpy, const char *file)
{
    return mw_cursor_load_named(dpy, file);
}

Cursor XcursorShapeLoadCursor(Display *dpy, unsigned int shape)
{
    return mw_cursor_load_shape(dpy, shape);
}

XcursorBool XcursorSupportsARGB(Display *dpy)
{
    (void)dpy;
    return XcursorTrue;
}

XcursorBool XcursorSetDefaultSize(Display *dpy, int size)
{
    mw_cursor_size_set(dpy, size);
    return XcursorTrue;
}

int XcursorGetDefaultSize(Display *dpy)
{
    return mw_cursor_size_get(dpy);
}

XcursorBool XcursorSetTheme(Display *dpy, const char *theme)
{
    mw_cursor_theme_set(dpy, theme ? theme : "");
    return XcursorTrue;
}

char *XcursorGetTheme(Display *dpy)
{
    char *t = mw_cursor_theme_get(dpy);
    if (t) return t;
    t = strdup("");
    return t;
}
