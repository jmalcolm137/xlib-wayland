/* xext.c — a libXext facade over the Wayland Xlib shim.
 *
 * Installed as libXext.so.6.  libXext carries a grab-bag: the extutil
 * machinery every other client extension library (libXi, libXtst, libXv, ...)
 * builds on, and a set of extensions (SHAPE, MIT-SHM, Sync, ...).  We shadow
 * the host's library so GDK's SHM/Sync calls reach the shim, but XShape is left
 * undefined here on purpose: it then resolves to the shim's own libX11
 * (src/xlib/xshape.c), which is where SHAPE lives for this project.
 *
 * extutil must be faithful -- libXi and libXtst use it to find their display
 * and would refuse to run (XMissingExtension) if it returned NULL.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xext.h>
/* extutil.h wants Xlib's private xEvent/xError types for hook signatures; we
 * never use those hooks, so incomplete types are enough. */
typedef struct _MwXextEvent xEvent;
typedef struct _MwXextError xError;
#include <X11/extensions/extutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/sync.h>
#include <X11/extensions/dpms.h>
#include <X11/extensions/Xdbe.h>
#include <X11/extensions/MITMisc.h>
#include <X11/extensions/security.h>

extern void mw_queue_event(Display *d, XEvent *ev);   /* libX11 (shim) */
#include <X11/Xauth.h>

#include <stdlib.h>
#include <string.h>

/* Shim helper (src/xlib/xsync.c): allocate a counter id. */
extern unsigned long mw_xsync_new_counter(Display *);
extern int (*XESetCloseDisplay(Display *, int, int (*)(Display *, XExtCodes *)))(Display *, XExtCodes *);

/* --------------------------------------------------------------- extutil */

XExtensionInfo *XextCreateExtension(void)
{
    return calloc(1, sizeof(XExtensionInfo));
}

void XextDestroyExtension(XExtensionInfo *info)
{
    if (!info) return;
    XExtDisplayInfo *i = info->head;
    while (i) { XExtDisplayInfo *n = i->next; free(i); i = n; }
    free(info);
}

XExtDisplayInfo *XextAddDisplay(XExtensionInfo *extinfo, Display *dpy,
                                _Xconst char *ext_name, XExtensionHooks *hooks,
                                int nevents, XPointer data)
{
    (void)nevents;
    if (!extinfo) return NULL;
    XExtCodes *codes = XInitExtension(dpy, ext_name);
    if (!codes) return NULL;
    /* The extension frees its per-display data from close_display (which
     * normally calls XextRemoveDisplay); XCloseDisplay has to run it. */
    if (hooks && hooks->close_display)
        XESetCloseDisplay(dpy, codes->extension, hooks->close_display);
    XExtDisplayInfo *info = calloc(1, sizeof *info);
    if (!info) return NULL;              /* codes belong to Xlib */
    info->display = dpy;
    info->codes = codes;
    info->data = data;
    info->next = extinfo->head;
    extinfo->head = info;
    extinfo->ndisplays++;
    return info;
}

int XextRemoveDisplay(XExtensionInfo *extinfo, Display *dpy)
{
    if (!extinfo) return 0;
    XExtDisplayInfo **pp = &extinfo->head;
    while (*pp) {
        if ((*pp)->display == dpy) {
            XExtDisplayInfo *dead = *pp;
            *pp = dead->next;
            free(dead);
            extinfo->ndisplays--;
            return 1;
        }
        pp = &(*pp)->next;
    }
    return 0;
}

XExtDisplayInfo *XextFindDisplay(XExtensionInfo *extinfo, Display *dpy)
{
    if (!extinfo) return NULL;
    for (XExtDisplayInfo *i = extinfo->head; i; i = i->next)
        if (i->display == dpy) return i;
    return NULL;
}

int XMissingExtension(Display *dpy, _Xconst char *ext_name)
{
    (void)dpy; (void)ext_name;
    return 0;
}

/* ------------------------------------------------------------------ SHM */

/* The shim has no server: an XShmSegmentInfo is the client's own shared
 * segment.  "Attach" merely succeeds, and PutImage blits the caller's pixels. */
Bool XShmQueryExtension(Display *dpy) { (void)dpy; return True; }

Status XShmQueryVersion(Display *dpy, int *major, int *minor, Bool *pixmaps)
{
    (void)dpy;
    if (major) *major = 1;
    if (minor) *minor = 2;
    /* No shm pixmaps: GDK then always draws shared images with XShmPutImage,
     * which we service, instead of a server-side pixmap that would not track
     * the segment. */
    if (pixmaps) *pixmaps = False;
    return True;
}

/* Event numbers are allocated in src/xlib/internal.h (MW_XSHM_EVENT_BASE,
 * MW_XSYNC_*): they must not collide with another extension's. */
#define MW_XSHM_EVENT_BASE  95
#define MW_XSYNC_EVENT_BASE 96
#define MW_XSYNC_ERROR_BASE 156

int XShmGetEventBase(Display *dpy) { (void)dpy; return MW_XSHM_EVENT_BASE; }

/* The image struct is Xlib's, but its pixels belong to the caller's shm
 * segment, so destruction must not free them (XDestroyImage would). */
static int xshm_destroy_image(XImage *image)
{
    free(image);
    return 1;
}

XImage *XShmCreateImage(Display *dpy, Visual *visual, unsigned int depth,
                        int format, char *data, XShmSegmentInfo *shminfo,
                        unsigned int width, unsigned int height)
{
    XImage *image = XCreateImage(dpy, visual, depth, format, 0, data,
                                 width, height, 32, 0);
    if (!image) return NULL;
    image->obdata = (char *)shminfo;          /* as libXext does */
    image->f.destroy_image = xshm_destroy_image;
    if (shminfo) shminfo->shmaddr = image->data;
    return image;
}

Bool XShmAttach(Display *dpy, XShmSegmentInfo *shminfo)
{
    (void)dpy; (void)shminfo;
    return True;
}

Bool XShmDetach(Display *dpy, XShmSegmentInfo *shminfo)
{
    (void)dpy; (void)shminfo;
    return True;
}

Bool XShmPutImage(Display *dpy, Drawable d, GC gc, XImage *image,
                  int src_x, int src_y, int dest_x, int dest_y,
                  unsigned int width, unsigned int height, Bool send_event)
{
    Bool ok = XPutImage(dpy, d, gc, image, src_x, src_y, dest_x, dest_y,
                        width, height);
    /* With send_event the client waits for ShmCompletion before it reuses
     * the segment (SDL, video players): without it they waited forever. */
    if (send_event) {
        XShmCompletionEvent ce;
        memset(&ce, 0, sizeof ce);
        ce.type = MW_XSHM_EVENT_BASE + ShmCompletion;
        ce.display = dpy;
        ce.drawable = d;
        ce.major_code = 0;
        ce.minor_code = 3;                  /* X_ShmPutImage */
        XShmSegmentInfo *si = image ? (XShmSegmentInfo *)image->obdata : NULL;
        ce.shmseg = si ? si->shmseg : 0;
        ce.offset = 0;
        mw_queue_event(dpy, (XEvent *)&ce);
    }
    return ok;
}

Bool XShmGetImage(Display *dpy, Drawable d, XImage *image, int x, int y,
                  unsigned long plane_mask)
{
    XImage *src = XGetImage(dpy, d, x, y, (unsigned)image->width,
                            (unsigned)image->height, plane_mask, ZPixmap);
    if (!src) return False;
    for (int row = 0; row < image->height; row++)
        memcpy(image->data + (size_t)row * image->bytes_per_line,
               src->data + (size_t)row * src->bytes_per_line,
               (size_t)image->bytes_per_line);
    XDestroyImage(src);
    return True;
}

Pixmap XShmCreatePixmap(Display *dpy, Drawable d, char *data,
                        XShmSegmentInfo *shminfo, unsigned int width,
                        unsigned int height, unsigned int depth)
{
    (void)data; (void)shminfo;
    return XCreatePixmap(dpy, d, width, height, depth);
}

int XShmPixmapFormat(Display *dpy) { (void)dpy; return ZPixmap; }

/* ----------------------------------------------------------------- Sync */

Status XSyncQueryExtension(Display *dpy, int *event_base, int *error_base)
{
    (void)dpy;
    if (event_base) *event_base = MW_XSYNC_EVENT_BASE;
    if (error_base) *error_base = MW_XSYNC_ERROR_BASE;
    return 1;
}

Status XSyncInitialize(Display *dpy, int *major, int *minor)
{
    (void)dpy;
    if (major) *major = 3;
    if (minor) *minor = 1;
    return 1;
}

void XSyncIntToValue(XSyncValue *pv, int i)
{
    pv->hi = i < 0 ? -1 : 0;
    pv->lo = (unsigned int)i;
}

void XSyncIntsToValue(XSyncValue *pv, unsigned int l, int h)
{
    pv->lo = l;
    pv->hi = h;
}

Bool XSyncValueGreaterThan(XSyncValue a, XSyncValue b)
{ return a.hi > b.hi || (a.hi == b.hi && a.lo > b.lo); }
Bool XSyncValueLessThan(XSyncValue a, XSyncValue b)
{ return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo); }
Bool XSyncValueGreaterOrEqual(XSyncValue a, XSyncValue b)
{ return a.hi > b.hi || (a.hi == b.hi && a.lo >= b.lo); }
Bool XSyncValueLessOrEqual(XSyncValue a, XSyncValue b)
{ return a.hi < b.hi || (a.hi == b.hi && a.lo <= b.lo); }
Bool XSyncValueEqual(XSyncValue a, XSyncValue b)
{ return a.hi == b.hi && a.lo == b.lo; }
Bool XSyncValueIsNegative(XSyncValue v) { return v.hi < 0; }
Bool XSyncValueIsZero(XSyncValue a) { return a.hi == 0 && a.lo == 0; }
Bool XSyncValueIsPositive(XSyncValue v) { return v.hi >= 0; }
unsigned int XSyncValueLow32(XSyncValue v) { return v.lo; }
int XSyncValueHigh32(XSyncValue v) { return v.hi; }

/* 64-bit two's-complement arithmetic; overflow is signed overflow, as in
 * libXext's macros. */
static long long sync_ll(XSyncValue v)
{ return (long long)(((unsigned long long)(unsigned)v.hi << 32) | (unsigned)v.lo); }

static void sync_set(XSyncValue *r, unsigned long long v)
{
    r->lo = (unsigned int)v;
    r->hi = (int)(unsigned int)(v >> 32);
}

void XSyncValueAdd(XSyncValue *r, XSyncValue a, XSyncValue b, int *overflow)
{
    long long res;
    int o = __builtin_add_overflow(sync_ll(a), sync_ll(b), &res);
    sync_set(r, (unsigned long long)res);
    if (overflow) *overflow = o;
}

void XSyncValueSubtract(XSyncValue *r, XSyncValue a, XSyncValue b, int *overflow)
{
    long long res;
    int o = __builtin_sub_overflow(sync_ll(a), sync_ll(b), &res);
    sync_set(r, (unsigned long long)res);
    if (overflow) *overflow = o;
}

void XSyncMaxValue(XSyncValue *pv) { pv->hi = 0x7fffffff; pv->lo = 0xffffffffu; }
void XSyncMinValue(XSyncValue *pv) { pv->hi = (int)0x80000000; pv->lo = 0; }

XSyncCounter XSyncCreateCounter(Display *dpy, XSyncValue value)
{
    (void)value;
    return (XSyncCounter)mw_xsync_new_counter(dpy);
}

Status XSyncSetCounter(Display *dpy, XSyncCounter counter, XSyncValue value)
{ (void)dpy; (void)counter; (void)value; return 1; }
Status XSyncChangeCounter(Display *dpy, XSyncCounter counter, XSyncValue value)
{ (void)dpy; (void)counter; (void)value; return 1; }
Status XSyncDestroyCounter(Display *dpy, XSyncCounter counter)
{ (void)dpy; (void)counter; return 1; }
Status XSyncQueryCounter(Display *dpy, XSyncCounter counter, XSyncValue *value)
{ (void)dpy; (void)counter; if (value) XSyncIntToValue(value, 0); return 1; }
Status XSyncAwait(Display *dpy, XSyncWaitCondition *list, int n)
{ (void)dpy; (void)list; (void)n; return 1; }

XSyncSystemCounter *XSyncListSystemCounters(Display *dpy, int *n)
{ (void)dpy; if (n) *n = 0; return NULL; }
void XSyncFreeSystemCounterList(XSyncSystemCounter *list) { (void)list; }

XSyncAlarm XSyncCreateAlarm(Display *dpy, unsigned long mask,
                            XSyncAlarmAttributes *attr)
{ (void)dpy; (void)mask; (void)attr; return None; }
Status XSyncDestroyAlarm(Display *dpy, XSyncAlarm alarm)
{ (void)dpy; (void)alarm; return 1; }
Status XSyncQueryAlarm(Display *dpy, XSyncAlarm alarm, XSyncAlarmAttributes *attr)
{ (void)dpy; (void)alarm; (void)attr; return 0; }
Status XSyncChangeAlarm(Display *dpy, XSyncAlarm alarm, unsigned long mask,
                        XSyncAlarmAttributes *attr)
{ (void)dpy; (void)alarm; (void)mask; (void)attr; return 1; }

/* ----------------------------------------------------------------- DPMS */

/* libXext also carries DPMS.  The shim has no power management; report off. */
Status DPMSInfo(Display *dpy, unsigned short *power_level, unsigned char *state)
{
    (void)dpy;
    if (power_level) *power_level = 0;
    if (state) *state = 0;
    return 0;
}

Bool   DPMSQueryExtension(Display *dpy, int *e, int *r)
{ (void)dpy; if (e) *e = 0; if (r) *r = 0; return False; }
Status DPMSGetVersion(Display *dpy, int *maj, int *min)
{ (void)dpy; if (maj) *maj = 0; if (min) *min = 0; return 0; }
Bool   DPMSCapable(Display *dpy) { (void)dpy; return False; }
Status DPMSSetTimeouts(Display *dpy, unsigned short a, unsigned short b, unsigned short c)
{ (void)dpy; (void)a; (void)b; (void)c; return 0; }
Bool   DPMSGetTimeouts(Display *dpy, unsigned short *a, unsigned short *b, unsigned short *c)
{ (void)dpy; if (a) *a = 0; if (b) *b = 0; if (c) *c = 0; return False; }
Status DPMSEnable(Display *dpy) { (void)dpy; return 0; }
Status DPMSDisable(Display *dpy) { (void)dpy; return 0; }
Status DPMSForceLevel(Display *dpy, unsigned short level) { (void)dpy; (void)level; return 0; }

/* ------------------------------------------------------- Xdbe / Xmbuf / misc */

/* Double buffering and multi-buffering have no backend here; the Query*
 * functions report the extension missing, which is how a client skips them. */

Status XdbeQueryExtension(Display *dpy, int *e, int *r)
{ (void)dpy; if (e) *e = 0; if (r) *r = 0; return 0; }
XdbeBackBuffer XdbeAllocateBackBufferName(Display *dpy, Window w, XdbeSwapAction a)
{ (void)dpy; (void)w; (void)a; return None; }
Status XdbeDeallocateBackBufferName(Display *dpy, XdbeBackBuffer b)
{ (void)dpy; (void)b; return 0; }
Status XdbeSwapBuffers(Display *dpy, XdbeSwapInfo *i, int n)
{ (void)dpy; (void)i; (void)n; return 0; }
Status XdbeBeginIdiom(Display *dpy) { (void)dpy; return 0; }
Status XdbeEndIdiom(Display *dpy) { (void)dpy; return 0; }
XdbeScreenVisualInfo *XdbeGetVisualInfo(Display *dpy, Drawable *screens, int *n)
{ (void)dpy; (void)screens; if (n) *n = 0; return NULL; }
void XdbeFreeVisualInfo(XdbeScreenVisualInfo *v) { (void)v; }
XdbeBackBufferAttributes *XdbeGetBackBufferAttributes(Display *dpy, XdbeBackBuffer b)
{ (void)dpy; (void)b; return NULL; }

/* Xmbuf (no public header installed; the two entry points xdpyinfo probes). */
Status XmbufQueryExtension(Display *dpy, int *e, int *r)
{ (void)dpy; if (e) *e = 0; if (r) *r = 0; return 0; }
Status XmbufGetVersion(Display *dpy, int *maj, int *min)
{ (void)dpy; if (maj) *maj = 0; if (min) *min = 0; return 0; }
Status XmbufGetScreenInfo(Display *dpy, Drawable d, int *nmono, int *nstereo,
                          void **onmono, void **onstereo)
{ (void)dpy; (void)d; if (nmono) *nmono = 0; if (nstereo) *nstereo = 0;
  if (onmono) *onmono = NULL; if (onstereo) *onstereo = NULL; return 0; }

/* MIT-SUNDRY-NONSTANDARD and XSecurity. */
Bool   XMITMiscQueryExtension(Display *dpy, int *e, int *r)
{ (void)dpy; if (e) *e = 0; if (r) *r = 0; return False; }
Status XMITMiscSetBugMode(Display *dpy, int on) { (void)dpy; (void)on; return 0; }
Bool   XMITMiscGetBugMode(Display *dpy) { (void)dpy; return False; }

Status XSecurityQueryExtension(Display *dpy, int *e, int *r)
{ (void)dpy; if (e) *e = 0; if (r) *r = 0; return 0; }
Xauth *XSecurityAllocXauth(void) { return NULL; }
void   XSecurityFreeXauth(Xauth *a) { (void)a; }
Xauth *XSecurityGenerateAuthorization(Display *dpy, Xauth *auth,
                                      unsigned long mask,
                                      XSecurityAuthorizationAttributes *attributes,
                                      XSecurityAuthorization *auth_id_return)
{ (void)dpy; (void)auth; (void)mask; (void)attributes;
  if (auth_id_return) *auth_id_return = 0; return NULL; }

/* Sync fences and priority are not backed; report failure. */
XSyncFence XSyncCreateFence(Display *dpy, Drawable d, Bool initially_triggered)
{ (void)dpy; (void)d; (void)initially_triggered; return None; }
Status XSyncDestroyFence(Display *dpy, XSyncFence f) { (void)dpy; (void)f; return 0; }
Status XSyncQueryFence(Display *dpy, XSyncFence f, Bool *triggered)
{ (void)dpy; (void)f; if (triggered) *triggered = False; return 0; }
Status XSyncAwaitFence(Display *dpy, const XSyncFence *f, int n)
{ (void)dpy; (void)f; (void)n; return 0; }
Status XSyncTriggerFence(Display *dpy, XSyncFence f) { (void)dpy; (void)f; return 0; }
Status XSyncResetFence(Display *dpy, XSyncFence f) { (void)dpy; (void)f; return 0; }
Status XSyncSetPriority(Display *dpy, XID id, int priority)
{ (void)dpy; (void)id; (void)priority; return 0; }
