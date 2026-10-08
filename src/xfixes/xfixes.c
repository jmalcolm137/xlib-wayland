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

/* Shim entry points (src/xlib/xfixes.c), exported from libX11.so.6. */
extern Bool   mw_xfixes_query(int *event_base, int *error_base);
extern void   mw_xfixes_select_input(Display *, Window, Atom, unsigned long);
extern unsigned long mw_xfixes_new_region(Display *);
extern void   mw_xfixes_free_region(Display *, unsigned long);
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
    (void)rectangles; (void)nrectangles;
    return (XserverRegion)mw_xfixes_new_region(dpy);
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
