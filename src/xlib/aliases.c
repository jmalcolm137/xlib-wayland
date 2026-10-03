/* aliases.c — the trivial accessor functions that libX11 exports alongside
 * its macros.  Xlib.h defines the un-prefixed forms (DisplayWidth, ...) as
 * macros, but real libX11 also exports XDisplayWidth &c. as functions, and
 * some clients (e.g. XV) call the function forms.  These are one-liners over
 * the same fields the macros read.
 */
#include "internal.h"

#include <stdlib.h>
#include <string.h>

int XDefaultScreen(Display *d) { return MWD(d)->default_screen; }
int XScreenCount(Display *d) { return MWD(d)->nscreens; }
Window XDefaultRootWindow(Display *d) { return MWSCR(d)->root; }
Window XRootWindow(Display *d, int scr) { (void)scr; return MWSCR(d)->root; }
Visual *XDefaultVisual(Display *d, int scr) { (void)scr; return &MWD(d)->visual; }
GC XDefaultGC(Display *d, int scr) { (void)scr; return MWSCR(d)->default_gc; }
Colormap XDefaultColormap(Display *d, int scr) { (void)scr; return MWSCR(d)->cmap; }
int XDefaultDepth(Display *d, int scr) { (void)scr; return MWSCR(d)->root_depth; }
unsigned long XBlackPixel(Display *d, int scr) { (void)scr; return MWSCR(d)->black_pixel; }
unsigned long XWhitePixel(Display *d, int scr) { (void)scr; return MWSCR(d)->white_pixel; }
/* The fd a client waits on: the public `fd' field, which the wakeup helper
 * points at its pipe so a client blocked in select() wakes when the Wayland
 * socket has data or a key repeat is due.  (libXt bypasses this function and
 * reads the field through the ConnectionNumber() macro, so the field itself has
 * to carry the pipe.) */
int XConnectionNumber(Display *d)
{
    return MWD(d)->fd;
}
int XProtocolVersion(Display *d) { return MWD(d)->proto_major_version; }
int XProtocolRevision(Display *d) { return MWD(d)->proto_minor_version; }
int XVendorRelease(Display *d) { return MWD(d)->release; }
char *XServerVendor(Display *d) { return MWD(d)->vendor; }
char *XDisplayString(Display *d) { return MWD(d)->display_name; }
int XQLength(Display *d) { return MWD(d)->qlen; }
int XImageByteOrder(Display *d) { return MWD(d)->byte_order; }
int XBitmapPad(Display *d) { return MWD(d)->bitmap_pad; }
int XBitmapBitOrder(Display *d) { return MWD(d)->bitmap_bit_order; }
int XBitmapUnit(Display *d) { return MWD(d)->bitmap_unit; }
int XDisplayWidth(Display *d, int scr) { (void)scr; return MWSCR(d)->width; }
int XDisplayHeight(Display *d, int scr) { (void)scr; return MWSCR(d)->height; }
int XDisplayWidthMM(Display *d, int scr) { (void)scr; return MWSCR(d)->mwidth; }
int XDisplayHeightMM(Display *d, int scr) { (void)scr; return MWSCR(d)->mheight; }
int XDisplayPlanes(Display *d, int scr) { (void)scr; return MWSCR(d)->root_depth; }
int XDisplayCells(Display *d, int scr) { (void)scr; return MWD(d)->visual.map_entries; }
Screen *XDefaultScreenOfDisplay(Display *d) { return MWSCR(d); }
int XDefaultDepthOfScreen(Screen *s) { return s->root_depth; }

int XWidthOfScreen(Screen *s) { return s->width; }
int XHeightOfScreen(Screen *s) { return s->height; }
int XWidthMMOfScreen(Screen *s) { return s->mwidth; }
int XHeightMMOfScreen(Screen *s) { return s->mheight; }
int XPlanesOfScreen(Screen *s) { return s->root_depth; }
int XCellsOfScreen(Screen *s) { return s->root_visual->map_entries; }
unsigned long XBlackPixelOfScreen(Screen *s) { return s->black_pixel; }
unsigned long XWhitePixelOfScreen(Screen *s) { return s->white_pixel; }
Colormap XDefaultColormapOfScreen(Screen *s) { return s->cmap; }
GC XDefaultGCOfScreen(Screen *s) { return s->default_gc; }
Visual *XDefaultVisualOfScreen(Screen *s) { return s->root_visual; }
Window XRootWindowOfScreen(Screen *s) { return s->root; }
int XMinCmapsOfScreen(Screen *s) { return s->min_maps; }
int XMaxCmapsOfScreen(Screen *s) { return s->max_maps; }
int XDoesBackingStore(Screen *s) { return s->backing_store; }
int XDoesSaveUnders(Screen *s) { return s->save_unders; }
long XEventMaskOfScreen(Screen *s) { return s->root_input_mask; }

/* Miscellaneous small entry points libX11 exports. */
GContext XGContextFromGC(GC gc) { return gc ? gc->gid : 0; }
void XFlushGC(Display *d, GC gc) { (void)d; (void)gc; }

int XCirculateSubwindows(Display *d, Window w, int direction)
{ (void)d; (void)w; (void)direction; return 1; }
int XCirculateSubwindowsUp(Display *d, Window w)
{
    MwWindow *win = mw_window(d, w);
    if (win) { /* raise the bottom-most child */ 
        MwWindow *c = win->children;
        if (c) XRaiseWindow(d, c->id);
    }
    return 1;
}
int XCirculateSubwindowsDown(Display *d, Window w)
{
    MwWindow *win = mw_window(d, w);
    if (win && win->last_child) XLowerWindow(d, win->last_child->id);
    return 1;
}

int XRestackWindows(Display *d, Window *windows, int n)
{
    (void)d; (void)windows; (void)n;
    return 1;
}

int XChangeKeyboardMapping(Display *d, int first_keycode, int keysyms_per_keycode,
                           KeySym *keysyms, int nkeycodes)
{ (void)d; (void)first_keycode; (void)keysyms_per_keycode; (void)keysyms; (void)nkeycodes; return 1; }

int XSetFontPath(Display *d, char **directories, int ndirs)
{ (void)d; (void)directories; (void)ndirs; return 1; }
char **XGetFontPath(Display *d, int *npaths)
{ (void)d; if (npaths) *npaths = 0; return NULL; }
int XFreeFontPath(char **list)
{
    if (list) { for (char **p = list; *p; p++) free(*p); free(list); }
    return 1;
}
int XFreeFontInfo(char **names, XFontStruct *free_info, int actual_count)
{ (void)names; (void)free_info; (void)actual_count; return 1; }

int XAddHosts(Display *d, XHostAddress *hosts, int num_hosts)
{ (void)d; (void)hosts; (void)num_hosts; return 1; }
int XRemoveHosts(Display *d, XHostAddress *hosts, int num_hosts)
{ (void)d; (void)hosts; (void)num_hosts; return 1; }

int XRebindKeysym(Display *d, KeySym keysym, KeySym *list, int mod_count,
                  _Xconst unsigned char *string, int bytes_string)
{ (void)d; (void)keysym; (void)list; (void)mod_count; (void)string; (void)bytes_string; return 1; }

Bool XPeekIfEvent(Display *d, XEvent *event, Bool (*pred)(), XPointer arg);

XStandardColormap *XAllocStandardColormap(void)
{ return calloc(1, sizeof(XStandardColormap)); }

void XSetStandardColormap(Display *d, Window w, XStandardColormap *cmap, Atom property)
{
    /* XStandardColormap is all longs (libX11's xPropStandardColormap), so it
     * can be passed straight through as format-32 long-array data. */
    XChangeProperty(d, w, property, XA_RGB_COLOR_MAP, 32, PropModeReplace,
                    (unsigned char *)cmap,
                    (int)(sizeof(XStandardColormap) / sizeof(long)));
}
Status XGetStandardColormap(Display *d, Window w, XStandardColormap *cmap, Atom property)
{
    Atom at; int af; unsigned long ni, ba; unsigned char *data = NULL;
    if (XGetWindowProperty(d, w, property, 0, (long)(sizeof(XStandardColormap)/4),
                           False, XA_RGB_COLOR_MAP, &at, &af, &ni, &ba, &data)
        != Success || !data)
        return 0;
    memcpy(cmap, data, sizeof(XStandardColormap));
    free(data);
    return 1;
}

Status XGetSizeHints(Display *d, Window w, XSizeHints *hints, Atom property)
{
    long s;
    return XGetWMSizeHints(d, w, hints, &s, property);
}

int XSetSizeHints(Display *d, Window w, XSizeHints *hints, Atom property)
{
    XSetWMSizeHints(d, w, hints, property);
    return 1;
}

Status XGetZoomHints(Display *d, Window w, XSizeHints *zhints)
{
    long s;
    return XGetWMSizeHints(d, w, zhints, &s, XA_WM_ZOOM_HINTS);
}

int XSetZoomHints(Display *d, Window w, XSizeHints *zhints)
{
    XSetWMSizeHints(d, w, zhints, XA_WM_ZOOM_HINTS);
    return 1;
}
