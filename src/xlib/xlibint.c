/* xlibint.c — the internal Xlib ABI that ecosystem libraries depend on.
 *
 * Because we install as libX11.so.6, libraries built against the real Xlib
 * (libXext, libXrender, libXft, libcairo's xlib backend) bind to us and
 * require XlibInt symbols.  We implement them over our own model.  Extensions
 * we do not support (XRender, SHM, XKB, XIM-proto, Xcms, locale converters)
 * will find working-enough stubs; the paths used by this project do not call
 * them, and where we do implement an extension (SHAPE, RandR) we shadow the
 * ecosystem implementation with our own symbols.
 */
#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* ------------------------------------------------------------ data syms */

int      _Xdebug = 0;
void    *_XHeadOfDisplayList = NULL;
int      _Xglobal_lock = 0;
int      _Xi18n_lock = 0;
XErrorHandler   _XErrorFunction = NULL;
XIOErrorHandler _XIOErrorFunction = NULL;
void   (*_XLockMutex_fn)(void *) = NULL;
void   (*_XUnlockMutex_fn)(void *) = NULL;
void   *(*_XCreateMutex_fn)(void) = NULL;
void   (*_XFreeMutex_fn)(void *) = NULL;
unsigned long (*_Xthread_self_fn)(void) = NULL;
void   (*_XInitDisplayLock_fn)(Display *) = NULL;
int    (*_XFreeDisplayLock_fn)(Display *) = NULL;
const char *xlocaledir = "/usr/share/X11/locale";

/* --------------------------------------------------------- display I/O */

int _XFlush(Display *d) { XFlush(d); return 1; }
void _XFlushGCCache(Display *d, GC gc) { (void)d; (void)gc; }

int _XSend(Display *d, _Xconst char *data, long size)
{
    (void)d; (void)data; (void)size;
    return 1;   /* no wire protocol: requests to unsupported extensions drop */
}

char *_XGetRequest(Display *d, unsigned char type, size_t len)
{
    (void)d; (void)type;
    static char *buf;
    static size_t cap;
    if (len + 8 > cap) {
        free(buf);
        cap = len + 1024;
        buf = calloc(1, cap);
    }
    if (buf) memset(buf, 0, cap);
    return buf;
}

int _XReply(Display *d, void *rep, int extra, Bool discard)
{
    (void)d; (void)extra; (void)discard;
    if (rep) memset(rep, 0, sizeof(long) * 8);
    return 1;
}

int _XRead(Display *d, char *data, long size)
{
    (void)d;
    if (data && size > 0) memset(data, 0, (size_t)size);
    return (int)size;
}

int _XRead32(Display *d, long *data, long len)
{
    (void)d;
    if (data && len > 0) memset(data, 0, (size_t)len * sizeof(long));
    return (int)len;
}

int _XReadPad(Display *d, char *data, long size)
{
    (void)d;
    if (data && size > 0) memset(data, 0, (size_t)size);
    return (int)size;
}

void _XEatData(Display *d, unsigned long n) { (void)d; (void)n; }
void _XEatDataWords(Display *d, unsigned long n) { (void)d; (void)n; }

void _XSetLastRequestRead(Display *d, unsigned long seq)
{ MWD(d)->last_request_read = seq; }

char *_XAllocScratch(Display *d, unsigned long nbytes)
{
    static char *buf;
    static unsigned long cap;
    if (nbytes > cap) {
        free(buf);
        buf = calloc(1, nbytes + 64);
        cap = nbytes + 64;
    }
    if (buf) memset(buf, 0, cap);
    (void)d;
    return buf;
}

char *_XAllocTemp(Display *d, int nbytes) { (void)d; return calloc(1, (size_t)nbytes); }void _XFreeTemp(Display *d, char *buf, int nbytes) { (void)d; (void)nbytes; free(buf); }

int _XGetBitsPerPixel(Display *d, int depth)
{ (void)d; return depth <= 8 ? 8 : depth <= 16 ? 16 : 32; }

int _XGetScanlinePad(Display *d, int depth)
{ (void)d; (void)depth; return 32; }

Visual *_XVIDtoVisual(Display *d, VisualID vid)
{
    if (vid == MWD(d)->visual.visualid) return &MWD(d)->visual;
    return NULL;
}

void _XInitImageFuncPtrs(XImage *image) { (void)image; }

int _XReadEvents(Display *d) { mw_process_events(d, false); return 1; }

int _XDeqAsyncHandler(Display *d, void *handler) { (void)d; (void)handler; return 0; }
int _XGetAsyncReply(Display *d, void *rep, void *buf, int len, int extra, Bool discard)
{ (void)d; (void)rep; (void)buf; (void)len; (void)extra; (void)discard; return 1; }
int _XData32(Display *d, void *src, long len) { (void)d; (void)src; (void)len; return 0; }
int _XCopyToArg(void *src, void *dst, unsigned int n) { (void)src; (void)dst; (void)n; return 0; }
unsigned long _XAllocID(Display *d) { return mw_alloc_id(d); }
void _XAllocIDs(Display *d, XID *ids, int count)
{ for (int i = 0; i < count; i++) ids[i] = mw_alloc_id(d); }
void _XProcessWindowAttributes(Display *d, void *v, unsigned long mask, void *attr)
{ (void)d; (void)v; (void)mask; (void)attr; }
int _XGetWindowAttributes(Display *d, Window w, void *attr)
{ return XGetWindowAttributes(d, w, (XWindowAttributes *)attr); }
Screen *_XScreenOfWindow(Display *d, Window w) { (void)w; return MWSCR(d); }
Bool _XTranslateKeySym(Display *d, KeySym ks, unsigned int mods, char *buf, int *n)
{ (void)d; (void)ks; (void)mods; (void)buf; if (n) *n = 0; return False; }
KeySym _XTranslateKey(Display *d, KeyCode kc, unsigned int mods)
{ (void)mods; return mw_keycode_to_keysym(d, kc, 0); }
void _XUpdateGCCache(Display *d, GC gc, unsigned long mask, void *attr)
{ (void)d; (void)gc; (void)mask; (void)attr; }
void *_XGetGCValues(Display *d, GC gc, unsigned long mask, void *v)
{ XGetGCValues(d, gc, mask, (XGCValues *)v); return v; }
void _XSetClipRectangles(Display *d, GC gc, int x, int y, void *r, int n, int ord)
{ XSetClipRectangles(d, gc, x, y, (XRectangle *)r, n, ord); }
int _XSetImage(XImage *src, XImage *dst, int sx, int sy, int dx, int dy,
               int w, int h, Bool flip)
{ (void)src; (void)dst; (void)sx; (void)sy; (void)dx; (void)dy; (void)w; (void)h; (void)flip; return 1; }

/* Events you never sent: benign defaults. */
int  _XEventToWire(Display *d, XEvent *re, void *event)
{ (void)d; (void)re; (void)event; return 1; }
int  _XWireToEvent(Display *d, XEvent *re, void *event)
{ (void)d; (void)re; (void)event; return 1; }
Bool _XIsEventCookie(Display *d, XEvent *ev) { (void)d; (void)ev; return False; }
void _XStoreEventCookie(Display *d, XEvent *ev) { (void)d; (void)ev; }
Bool _XFetchEventCookie(Display *d, void *ge) { (void)d; (void)ge; return False; }
Bool _XCopyEventCookie(Display *d, void *a, void *b) { (void)d; (void)a; (void)b; return False; }
void _XFreeEventCookies(Display *d) { (void)d; }
void _XUnknownWireEvent(Display *d, XEvent *re, void *event) { (void)d; (void)re; (void)event; }
void _XUnknownNativeEvent(Display *d, XEvent *re, void *event) { (void)d; (void)re; (void)event; }
void _XUnknownCopyEventCookie(Display *d, XEvent *a, void *b) { (void)d; (void)a; (void)b; }
Bool _XUnknownWireEventCookie(Display *d, XEvent *a, void *b) { (void)d; (void)a; (void)b; return False; }

/* Error handlers. */
int _XDefaultError(Display *d, XErrorEvent *e) { (void)d; (void)e; return 0; }
int _XDefaultIOError(Display *d) { (void)d; return 1; }
void _XDefaultIOErrorExit(Display *d, void *user) { (void)d; (void)user; }
void _XDefaultWireError(Display *d, XErrorEvent *e, void *rep)
{ (void)d; (void)e; (void)rep; }
int _XError(Display *d, void *rep) { (void)d; (void)rep; return 0; }
int _XIOError(Display *d) { return mw_io_error(d, "io error"); }

/* Display lifecycle internals. */
void _XFreeDisplayStructure(Display *d) { (void)d; }
void _XGetHostname(char *buf, int len)
{
    if (buf && len > 0) { gethostname(buf, (size_t)len); buf[len - 1] = 0; }
}

/* --------------------------------------------------- extension hooks */

void *XESetCreateGC(Display *d, int ext, void *p) { (void)d; (void)ext; (void)p; return NULL; }
void *XESetCopyGC(Display *d, int ext, void *p) { (void)d; (void)ext; (void)p; return NULL; }
void *XESetFreeGC(Display *d, int ext, void *p) { (void)d; (void)ext; (void)p; return NULL; }
void *XESetFlushGC(Display *d, int ext, void *p) { (void)d; (void)ext; (void)p; return NULL; }
void *XESetCreateFont(Display *d, int ext, void *p) { (void)d; (void)ext; (void)p; return NULL; }
void *XESetFreeFont(Display *d, int ext, void *p) { (void)d; (void)ext; (void)p; return NULL; }
void *XESetCreateImage(Display *d, int ext, void *p) { (void)d; (void)ext; (void)p; return NULL; }
void *XESetDestroyImage(Display *d, int ext, void *p) { (void)d; (void)ext; (void)p; return NULL; }
void *XESetError(Display *d, int ext, void *p) { (void)d; (void)ext; (void)p; return NULL; }
void *XESetErrorString(Display *d, int ext, void *p) { (void)d; (void)ext; (void)p; return NULL; }
void *XESetEventToWire(Display *d, int ext, void *p) { (void)d; (void)ext; (void)p; return NULL; }
void *XESetWireToEvent(Display *d, int ext, void *p) { (void)d; (void)ext; (void)p; return NULL; }
void *XESetWireToError(Display *d, int ext, void *p) { (void)d; (void)ext; (void)p; return NULL; }
void *XESetBeforeFlush(Display *d, int ext, void *p) { (void)d; (void)ext; (void)p; return NULL; }
void *XESetPrintErrorValues(Display *d, int ext, void *p) { (void)d; (void)ext; (void)p; return NULL; }
void *XESetCopyEventCookie(Display *d, int ext, void *p) { (void)d; (void)ext; (void)p; return NULL; }
void *XESetWireToEventCookie(Display *d, int ext, void *p) { (void)d; (void)ext; (void)p; return NULL; }

/* ------------------------------------------------------- threading */

Status XInitThreads(void) { return 1; }
Status XFreeThreads(void) { return 1; }
void XLockDisplay(Display *d) { (void)d; }
void XUnlockDisplay(Display *d) { (void)d; }

/* ------------------------------------------------------- public tail */

int XFree(void *data)
{
    free(data);
    return 1;
}

int XStoreName(Display *d, Window w, _Xconst char *name)
{
    return XChangeProperty(d, w, XA_WM_NAME, XA_STRING, 8, PropModeReplace,
                           (const unsigned char *)name,
                           name ? (int)strlen(name) : 0);
}

int XInitImage(XImage *image) { (void)image; return 1; }
