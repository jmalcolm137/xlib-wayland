/* xfixes.c — a libXfixes facade over the Wayland Xlib shim.
 *
 * Installed as libXfixes.so.3 so that GDK's XFixes calls (clipboard
 * owner-change notifications, regions, cursor changes) reach the shim's
 * in-process implementation instead of the system libXfixes, which would send
 * X protocol the shim does not speak.  The heavy lifting is in src/xlib/xfixes.c.
 */
#include <X11/Xlib.h>
#include <X11/extensions/Xfixes.h>

#include <stdlib.h>
#include <string.h>
#include <X11/Xutil.h>

/* Shim entry points (src/xlib/xfixes.c), exported from libX11.so.6. */
extern Bool   mw_xfixes_query(int *event_base, int *error_base);
extern void   mw_xfixes_select_input(Display *, Window, Atom, unsigned long);
extern unsigned long mw_xfixes_new_region(Display *, XRectangle *, int);
extern void   mw_xfixes_free_region(Display *, unsigned long);
extern Region mw_xfixes_region(Display *, unsigned long);
extern int    mw_region_rectangles(Region, XRectangle **);
extern void   mw_xfixes_change_cursor(Display *, Cursor, Cursor);

Bool XFixesQueryExtension(Display *dpy, int *event_base_return,
                          int *error_base_return)
{
    return mw_xfixes_query(event_base_return, error_base_return);
}

Status XFixesQueryVersion(Display *dpy, int *major_version_return,
                          int *minor_version_return)
{
    (void)dpy;
    if (major_version_return) *major_version_return = XFIXES_MAJOR;
    if (minor_version_return) *minor_version_return = XFIXES_MINOR;
    return 1;
}

int XFixesVersion(void) { return XFIXES_VERSION; }

void XFixesSelectSelectionInput(Display *dpy, Window win, Atom selection,
                                unsigned long eventMask)
{
    mw_xfixes_select_input(dpy, win, selection, eventMask);
}

/* The shim has no hardware cursor to query; a no-op keeps callers happy. */
void XFixesSelectCursorInput(Display *dpy, Window win, unsigned long eventMask)
{
    (void)dpy; (void)win; (void)eventMask;
}

XserverRegion XFixesCreateRegion(Display *dpy, XRectangle *rectangles,
                                 int nrectangles)
{
    return (XserverRegion)mw_xfixes_new_region(dpy, rectangles, nrectangles);
}

/* The region operations work on the Xlib regions behind the XIDs; an
 * unknown region is ignored (the server would raise BadRegion). */
static Region reg(Display *dpy, XserverRegion r) { return mw_xfixes_region(dpy, r); }

static void region_set(Region dst, Region src)
{
    XSubtractRegion(dst, dst, dst);
    if (src) XUnionRegion(dst, src, dst);
}

void XFixesSetRegion(Display *dpy, XserverRegion region, XRectangle *rectangles,
                     int nrectangles)
{
    Region r = reg(dpy, region);
    if (!r) return;
    region_set(r, NULL);
    for (int i = 0; rectangles && i < nrectangles; i++)
        XUnionRectWithRegion(&rectangles[i], r, r);
}

void XFixesCopyRegion(Display *dpy, XserverRegion dst, XserverRegion src)
{
    Region d = reg(dpy, dst), s = reg(dpy, src);
    if (d && s && d != s) region_set(d, s);
}

void XFixesUnionRegion(Display *dpy, XserverRegion dst, XserverRegion src1,
                       XserverRegion src2)
{
    Region d = reg(dpy, dst), a = reg(dpy, src1), b = reg(dpy, src2);
    if (d && a && b) XUnionRegion(a, b, d);
}

void XFixesIntersectRegion(Display *dpy, XserverRegion dst, XserverRegion src1,
                           XserverRegion src2)
{
    Region d = reg(dpy, dst), a = reg(dpy, src1), b = reg(dpy, src2);
    if (d && a && b) XIntersectRegion(a, b, d);
}

void XFixesSubtractRegion(Display *dpy, XserverRegion dst, XserverRegion src1,
                          XserverRegion src2)
{
    Region d = reg(dpy, dst), a = reg(dpy, src1), b = reg(dpy, src2);
    if (d && a && b) XSubtractRegion(a, b, d);
}

void XFixesInvertRegion(Display *dpy, XserverRegion dst, XRectangle *rect,
                        XserverRegion src)
{
    Region d = reg(dpy, dst), s = reg(dpy, src);
    if (!d || !s || !rect) return;
    Region box = XCreateRegion();
    if (!box) return;
    XUnionRectWithRegion(rect, box, box);
    XSubtractRegion(box, s, d);
    XDestroyRegion(box);
}

void XFixesTranslateRegion(Display *dpy, XserverRegion region, int dx, int dy)
{
    Region r = reg(dpy, region);
    if (r) XOffsetRegion(r, dx, dy);
}

void XFixesRegionExtents(Display *dpy, XserverRegion dst, XserverRegion src)
{
    Region d = reg(dpy, dst), s = reg(dpy, src);
    if (!d || !s) return;
    XRectangle box;
    XClipBox(s, &box);
    region_set(d, NULL);
    if (!XEmptyRegion(s)) XUnionRectWithRegion(&box, d, d);
}

XserverRegion XFixesCreateRegionFromWindow(Display *dpy, Window window, int kind)
{
    (void)kind;                       /* no shapes beyond the window's box */
    Window root;
    int x, y;
    unsigned int w = 0, h = 0, bw, depth;
    if (!XGetGeometry(dpy, window, &root, &x, &y, &w, &h, &bw, &depth))
        return (XserverRegion)mw_xfixes_new_region(dpy, NULL, 0);
    XRectangle r = { 0, 0, (unsigned short)w, (unsigned short)h };
    return (XserverRegion)mw_xfixes_new_region(dpy, &r, 1);
}

void XFixesSetGCClipRegion(Display *dpy, GC gc, int clip_x_origin,
                           int clip_y_origin, XserverRegion region)
{
    Region r = region ? reg(dpy, region) : NULL;
    if (!r) { XSetClipMask(dpy, gc, None); return; }
    XRectangle *rects = NULL;
    int n = mw_region_rectangles(r, &rects);
    if (n < 0) return;
    XSetClipRectangles(dpy, gc, clip_x_origin, clip_y_origin, rects, n, YXBanded);
    free(rects);
}

XRectangle *XFixesFetchRegionAndBounds(Display *dpy, XserverRegion region,
                                       int *nrectanglesRet, XRectangle *bounds)
{
    Region r = reg(dpy, region);
    if (nrectanglesRet) *nrectanglesRet = 0;
    if (bounds) memset(bounds, 0, sizeof *bounds);
    if (!r) return NULL;
    XRectangle *rects = NULL;
    int n = mw_region_rectangles(r, &rects);
    if (n < 0) return NULL;
    if (bounds && n > 0) XClipBox(r, bounds);
    /* The caller frees with XFree (which is free() here); return a block
     * even for an empty region, as libXfixes does. */
    if (!rects) rects = malloc(sizeof *rects);
    if (nrectanglesRet) *nrectanglesRet = n;
    return rects;
}

XRectangle *XFixesFetchRegion(Display *dpy, XserverRegion region,
                              int *nrectanglesRet)
{
    XRectangle bounds;
    return XFixesFetchRegionAndBounds(dpy, region, nrectanglesRet, &bounds);
}

void XFixesDestroyRegion(Display *dpy, XserverRegion region)
{
    mw_xfixes_free_region(dpy, (unsigned long)region);
}

void XFixesChangeCursor(Display *dpy, Cursor source, Cursor destination)
{
    mw_xfixes_change_cursor(dpy, source, destination);
}

void XFixesChangeSaveSet(Display *dpy, Window win, int mode, int target,
                         int map)
{
    (void)dpy; (void)win; (void)mode; (void)target; (void)map;
}
