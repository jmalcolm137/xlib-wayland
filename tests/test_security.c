/* test_security.c — regression tests for the security fixes: malformed
 * requests, hostile property contents, out-of-range API arguments and the
 * object lifetimes that used to be exploitable.  Each check would crash,
 * hang, or fail on the code before its fix; a few (marked ASan) only show
 * their old out-of-bounds access under -Db_sanitize=address.  Needs a
 * compositor (run by `meson test` through tests/with-compositor.sh). */
#define _GNU_SOURCE
#include "internal.h"

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/Xresource.h>
#include <X11/Xlib-xcb.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/renderproto.h>
#include <xcb/xcb.h>

#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int failures;

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

char *_XGetRequest(Display *d, unsigned char type, size_t len);

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

static unsigned long pixel_at(Display *d, Drawable dr, int x, int y)
{
    XImage *im = XGetImage(d, dr, x, y, 1, 1, AllPlanes, ZPixmap);
    if (!im) return 0xdeadbeef;
    unsigned long v = XGetPixel(im, 0, 0) & 0xffffff;
    XDestroyImage(im);
    return v;
}

/* ------------------------------------------------------------ pure helpers */

static void check_wl_string(void)
{
    char out[MW_WL_STRING_MAX];

    mw_wl_string(out, sizeof out, "caf\xe9", true);           /* Latin-1 */
    CHECK(strcmp(out, "caf\xc3\xa9") == 0);
    mw_wl_string(out, sizeof out, "a\xff" "b\xc3", false);     /* invalid UTF-8 */
    CHECK(strcmp(out, "a?b?") == 0);
    mw_wl_string(out, sizeof out, "\xed\xa0\x80", false);      /* surrogate */
    CHECK(strcmp(out, "???") == 0);
    mw_wl_string(out, sizeof out, NULL, false);
    CHECK(out[0] == 0);

    /* Long titles are cut below libwayland's message limit, and never in the
     * middle of a multi-byte sequence. */
    size_t n = 3000;
    char *big = malloc(n * 2 + 1);
    for (size_t i = 0; i < n; i++) memcpy(big + i * 2, "\xc3\xa9", 2);
    big[n * 2] = 0;
    mw_wl_string(out, sizeof out, big, false);
    size_t len = strlen(out);
    CHECK(len < MW_WL_STRING_MAX && len % 2 == 0 && len > 1000);
    free(big);
}

static void check_owned_files(void)
{
    char path[] = "/tmp/opencode-sec-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0 && mw_fd_owned_regular(fd));
    if (fd >= 0) { close(fd); unlink(path); }
    int dn = open("/dev/null", O_RDONLY);
    CHECK(dn >= 0 && !mw_fd_owned_regular(dn));      /* not a regular file */
    if (dn >= 0) close(dn);
    CHECK(!mw_fd_owned_regular(-1));

    setenv("MW_SEC_TEST", "x", 1);                   /* not setuid: visible */
    CHECK(mw_secure_getenv("MW_SEC_TEST") &&
          strcmp(mw_secure_getenv("MW_SEC_TEST"), "x") == 0);
}

static void check_create_image(Display *d)
{
    /* A pitch shorter than a row, an impossible pad or depth, oversized
     * geometry and a negative offset are all refused. */
    CHECK(!XCreateImage(d, NULL, 24, ZPixmap, 0, NULL, 100, 10, 32, 10));
    CHECK(!XCreateImage(d, NULL, 24, ZPixmap, 0, NULL, 10, 10, 7, 0));
    CHECK(!XCreateImage(d, NULL, 0, ZPixmap, 0, NULL, 10, 10, 32, 0));
    CHECK(!XCreateImage(d, NULL, 33, ZPixmap, 0, NULL, 10, 10, 32, 0));
    CHECK(!XCreateImage(d, NULL, 24, ZPixmap, 0, NULL, 70000, 1, 32, 0));
    CHECK(!XCreateImage(d, NULL, 1, XYBitmap, -1, NULL, 8, 8, 8, 0));
    /* An XY row must also hold the leading `offset` bits. */
    CHECK(!XCreateImage(d, NULL, 1, XYBitmap, 32, NULL, 8, 8, 8, 1));
    XImage *ok = XCreateImage(d, NULL, 1, XYBitmap, 32, NULL, 8, 8, 8, 5);
    CHECK(ok != NULL);
    if (ok) XDestroyImage(ok);

    /* Pixel accessors ignore coordinates outside the image (ASan). */
    char *pix = calloc(4 * 4, 4);
    XImage *im = XCreateImage(d, NULL, 24, ZPixmap, 0, pix, 4, 4, 32, 0);
    CHECK(im != NULL);
    if (im) {
        XPutPixel(im, 1000, 1000, 0xffffff);
        XPutPixel(im, -1, 0, 0xffffff);
        CHECK(XGetPixel(im, 1000, 1000) == 0);
        CHECK(XGetPixel(im, 0, -5) == 0);
        XPutPixel(im, 3, 3, 0x123456);
        CHECK((XGetPixel(im, 3, 3) & 0xffffff) == 0x123456);
        XDestroyImage(im);                           /* frees pix */
    }
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(text, f);
    fclose(f);
}

static void check_xrm_files(void)
{
    XrmInitialize();

    /* A file including itself twice used to expand 2^100 times. */
    char dir[] = "/tmp/opencode-xrm-XXXXXX";
    if (!mkdtemp(dir)) { CHECK(!"mkdtemp"); return; }
    char path[256], fifo[256];
    snprintf(path, sizeof path, "%s/self.ad", dir);
    write_file(path, "#include \"self.ad\"\n#include \"self.ad\"\nsec.value: ok\n");
    double t0 = now_s();
    XrmDatabase db = XrmGetFileDatabase(path);
    CHECK(now_s() - t0 < 5.0);
    CHECK(db != NULL);
    char *type = NULL;
    XrmValue v = { 0, NULL };
    CHECK(db && XrmGetResource(db, "sec.value", "Sec.Value", &type, &v) &&
          v.addr && strcmp(v.addr, "ok") == 0);
    if (db) XrmDestroyDatabase(db);

    /* A FIFO (here, or named by an #include) must not block the client. */
    snprintf(fifo, sizeof fifo, "%s/fifo", dir);
    CHECK(mkfifo(fifo, 0600) == 0);
    t0 = now_s();
    CHECK(XrmGetFileDatabase(fifo) == NULL);
    snprintf(path, sizeof path, "%s/inc.ad", dir);
    write_file(path, "#include \"fifo\"\nsec.after: yes\n");
    db = XrmGetFileDatabase(path);
    CHECK(now_s() - t0 < 5.0);
    CHECK(db && XrmGetResource(db, "sec.after", "Sec.After", &type, &v));
    if (db) XrmDestroyDatabase(db);

    unlink(fifo);
    unlink(path);
    snprintf(path, sizeof path, "%s/self.ad", dir);
    unlink(path);
    rmdir(dir);
}

/* -------------------------------------------------------- display-backed */

static void check_error_text(Display *d)
{
    char buf[16];
    memset(buf, 'Z', sizeof buf);
    XGetErrorText(d, BadValue, buf, -1);             /* used to be SIZE_MAX */
    CHECK(buf[0] == 'Z');
    XGetErrorDatabaseText(d, "x", "y", "default", buf, 0);
    CHECK(buf[0] == 'Z');
    XGetErrorText(d, BadValue, buf, sizeof buf);
    CHECK(buf[sizeof buf - 1] == 0 || strlen(buf) < sizeof buf);
}

/* Properties set with the wrong format hold bytes, not longs. */
static void check_property_formats(Display *d, Window w)
{
    unsigned char three[3] = { 1, 2, 3 };
    XChangeProperty(d, w, XA_WM_HINTS, XA_WM_HINTS, 8, PropModeReplace, three, 3);
    XWMHints *h = XGetWMHints(d, w);
    CHECK(h == NULL);
    if (h) XFree(h);

    XChangeProperty(d, w, XA_WM_TRANSIENT_FOR, XA_WINDOW, 8, PropModeReplace,
                    three, 3);
    Window tf = 0;
    CHECK(XGetTransientForHint(d, w, &tf) == 0);

    XChangeProperty(d, w, XA_WM_NORMAL_HINTS, XA_WM_SIZE_HINTS, 16,
                    PropModeReplace, three, 1);
    XSizeHints sh;
    long supplied = 0;
    CHECK(XGetWMNormalHints(d, w, &sh, &supplied) == 0);

    Atom proto = XInternAtom(d, "WM_PROTOCOLS", False);
    XChangeProperty(d, w, proto, XA_ATOM, 16, PropModeReplace, three, 1);
    Atom *protos = NULL;
    int np = 0;
    CHECK(XGetWMProtocols(d, w, &protos, &np) == 0 && protos == NULL && np == 0);

    /* The right format still works. */
    long th[9] = { InputHint, True };
    XChangeProperty(d, w, XA_WM_HINTS, XA_WM_HINTS, 32, PropModeReplace,
                    (unsigned char *)th, 9);
    h = XGetWMHints(d, w);
    CHECK(h && (h->flags & InputHint) && h->input == True);
    if (h) XFree(h);
}

/* Taking an event, putting it back and taking it again must hand out the
 * cookie data exactly once (it used to be freed, then claimed, then freed
 * again). */
static void check_cookie_put_back(Display *d, Window w)
{
    XSync(d, False);
    XEvent ev;
    while (XPending(d)) XNextEvent(d, &ev);

    memset(&ev, 0, sizeof ev);
    ev.xcookie.type = GenericEvent;
    ev.xcookie.display = d;
    ev.xcookie.extension = 131;
    ev.xcookie.evtype = 1;
    void *data = calloc(1, 64);
    ev.xcookie.data = data;
    (void)w;
    mw_put_event(d, &ev);

    XEvent a, b;
    XNextEvent(d, &a);
    CHECK(a.type == GenericEvent && a.xcookie.data == data);
    XPutBackEvent(d, &a);
    XNextEvent(d, &b);
    CHECK(b.type == GenericEvent && b.xcookie.data == data);
    CHECK(XGetEventData(d, &b.xcookie));
    CHECK(b.xcookie.data == data);
    XFreeEventData(d, &b.xcookie);                   /* the only free */
    /* Taking another event must not free it again. */
    {
        XEvent cm;
        memset(&cm, 0, sizeof cm);
        cm.xclient.type = ClientMessage;
        cm.xclient.window = w;
        cm.xclient.format = 32;
        mw_put_event(d, &cm);
        XNextEvent(d, &cm);
        CHECK(cm.type == ClientMessage);
    }
}

/* XSendEvent must not queue the sender's cookie pointer (the queue would free
 * it), nor read past a struct smaller than XEvent (ASan). */
static void check_send_event(Display *d, Window w)
{
    XEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.xcookie.type = GenericEvent;
    ev.xcookie.extension = 131;
    ev.xcookie.evtype = 1;
    char not_heap[8];
    ev.xcookie.data = not_heap;
    XSendEvent(d, w, False, 0, &ev);
    XSync(d, False);
    XEvent out;
    while (XCheckTypedEvent(d, GenericEvent, &out))
        CHECK(out.xcookie.data == NULL);
    XEvent drop;
    while (XPending(d)) XNextEvent(d, &drop);          /* would free not_heap */

    XAnyEvent *small = malloc(sizeof *small);
    memset(small, 0, sizeof *small);
    small->type = 120;                                 /* unknown extension */
    small->window = w;
    XSendEvent(d, w, False, 0, (XEvent *)small);
    free(small);
    XSync(d, False);
    while (XPending(d)) XNextEvent(d, &drop);
}

/* Offsets near INT_MAX used to wrap the self-copy clamps into a write far
 * outside the pixmap, and the CopyPlane clip into an out-of-bounds read. */
static void check_copy_overflow(Display *d, Window root)
{
    Pixmap pm = XCreatePixmap(d, root, 64, 64, 24);
    GC gc = XCreateGC(d, pm, 0, NULL);
    XSetForeground(d, gc, 0xff0000);
    XFillRectangle(d, pm, gc, 0, 0, 64, 64);

    XCopyArea(d, pm, pm, gc, 0, 0, 10, 10, INT_MAX - 5, 0);
    XCopyArea(d, pm, pm, gc, 0, 0, 10, 10, 0, INT_MAX - 5);
    XCopyArea(d, pm, pm, gc, 0, 0, 10, 10, INT_MIN, INT_MIN);
    XCopyArea(d, pm, pm, gc, INT_MAX - 5, INT_MAX - 5, 10, 10, 0, 0);
    XCopyArea(d, pm, pm, gc, 0, 0, UINT_MAX, UINT_MAX, 1, 1);

    Pixmap bm = XCreatePixmap(d, root, 64, 64, 1);
    GC bgc = XCreateGC(d, bm, 0, NULL);
    XSetForeground(d, bgc, 1);
    XFillRectangle(d, bm, bgc, 0, 0, 64, 64);
    XCopyPlane(d, bm, pm, gc, INT_MAX - 5, 0, 10, 10, 0, 0, 1);
    XCopyPlane(d, bm, pm, gc, 0, INT_MAX - 5, 10, 10, 0, 0, 1);
    XSync(d, False);

    CHECK(pixel_at(d, pm, 0, 0) == 0xff0000);
    CHECK(pixel_at(d, pm, 63, 63) == 0xff0000);

    /* An ordinary scroll still works. */
    XSetForeground(d, gc, 0x0000ff);
    XFillRectangle(d, pm, gc, 0, 0, 8, 8);
    XCopyArea(d, pm, pm, gc, 0, 0, 8, 8, 20, 20);
    CHECK(pixel_at(d, pm, 24, 24) == 0x0000ff);

    XFreeGC(d, bgc);
    XFreePixmap(d, bm);
    XFreeGC(d, gc);
    XFreePixmap(d, pm);
}

/* XClearArea with a huge width used to wrap past its clamp. */
static void check_clear_area(Display *d, Window root)
{
    Window w = XCreateSimpleWindow(d, root, 0, 0, 20, 20, 0, 0, 0xffffff);
    XClearArea(d, w, 5, 5, UINT_MAX, UINT_MAX, False);
    XClearArea(d, w, INT_MAX, INT_MAX, 10, 10, False);
    XClearArea(d, w, INT_MIN, INT_MIN, UINT_MAX, 10, False);
    XSync(d, False);
    XDestroyWindow(d, w);
}

static void check_clip_rects(Display *d, Window root)
{
    GC gc = XCreateGC(d, root, 0, NULL);
    XSetClipRectangles(d, gc, 0, 0, NULL, 5, Unsorted);
    CHECK(gc->nclip == 0);                 /* not 5 uninitialised rectangles */
    XFreeGC(d, gc);
}

/* XkbAlloc* used to report Success without the room asked for. */
static void check_xkb_alloc(Display *d)
{
    XkbDescPtr xkb = XkbGetMap(d, XkbAllClientInfoMask, XkbUseCoreKbd);
    CHECK(xkb != NULL);
    if (!xkb) return;
    CHECK(XkbAllocClientMap(xkb, XkbKeyTypesMask, 8) == Success);
    CHECK(xkb->map && xkb->map->size_types >= 8);
    if (xkb->map && xkb->map->size_types >= 8)
        memset(&xkb->map->types[7], 0, sizeof xkb->map->types[7]);   /* ASan */
    CHECK(xkb->map->num_types == 2);      /* existing types kept */

    CHECK(XkbAllocServerMap(xkb, XkbKeyActionsMask, 4) == Success);
    CHECK(xkb->server && xkb->server->acts && xkb->server->size_acts >= 5 &&
          xkb->server->key_acts);

    CHECK(XkbGetNames(d, XkbKeyNamesMask, xkb) == Success);
    CHECK(xkb->names && xkb->names->keys);
    if (xkb->names && xkb->names->keys)
        xkb->names->keys[255].name[0] = 'A';

    CHECK(XkbAllocCompatMap(xkb, XkbSymInterpMask, 3) == Success);
    CHECK(xkb->compat && xkb->compat->sym_interpret &&
          xkb->compat->size_si >= 3);
    XkbFreeKeyboard(xkb, 0, True);

    XkbDescPtr fresh = XkbAllocKeyboard();
    CHECK(fresh && XkbAllocClientMap(fresh, XkbKeySymsMask | XkbModifierMapMask,
                                     0) == Success);
    CHECK(fresh && fresh->map && fresh->map->key_sym_map &&
          fresh->map->syms && fresh->map->modmap);
    if (fresh) XkbFreeKeyboard(fresh, 0, True);
}

/* Malformed Render requests: a stop count the request cannot hold used to be
 * read straight past the end of it. */
static void check_render_requests(Display *d, Window root)
{
    int op = mw_render_opcode();
    Pixmap pm = XCreatePixmap(d, root, 8, 8, 32);
    Picture pic = XRenderCreatePicture(d, pm,
                      XRenderFindStandardFormat(d, PictStandardARGB32), 0, NULL);
    XSync(d, False);

    xRenderCreateLinearGradientReq *lg = (void *)_XGetRequest(d, (unsigned char)op,
                                            sz_xRenderCreateLinearGradientReq);
    CHECK(lg != NULL);
    if (lg) {
        lg->renderReqType = X_RenderCreateLinearGradient;
        lg->pid = (CARD32)mw_alloc_id(d);
        lg->nStops = 1u << 20;              /* 12 MiB of stops, none sent */
    }
    xRenderCreateRadialGradientReq *rg = (void *)_XGetRequest(d, (unsigned char)op,
                                            sz_xRenderCreateRadialGradientReq);
    if (rg) {
        rg->renderReqType = X_RenderCreateRadialGradient;
        rg->pid = (CARD32)mw_alloc_id(d);
        rg->nStops = 0xffffffffu;
    }
    /* Every value bit set, no values carried. */
    xRenderChangePictureReq *cp = (void *)_XGetRequest(d, (unsigned char)op,
                                     sz_xRenderChangePictureReq);
    if (cp) {
        cp->renderReqType = X_RenderChangePicture;
        cp->picture = (CARD32)pic;
        cp->mask = 0xffffffffu;
    }
    /* A filter name longer than the request. */
    xRenderSetPictureFilterReq *sf = (void *)_XGetRequest(d, (unsigned char)op,
                                        sz_xRenderSetPictureFilterReq);
    if (sf) {
        sf->renderReqType = X_RenderSetPictureFilter;
        sf->picture = (CARD32)pic;
        sf->nbytes = 0xffff;
    }
    XSync(d, False);

    /* The picture and the connection still work. */
    XRenderColor green = { 0, 0xffff, 0, 0xffff };
    XRenderFillRectangle(d, PictOpSrc, pic, &green, 0, 0, 8, 8);
    XSync(d, False);
    CHECK(pixel_at(d, pm, 4, 4) == 0x00ff00);
    XRenderFreePicture(d, pic);
    XFreePixmap(d, pm);
}

/* A title larger than libwayland's message buffer used to abort the whole
 * connection (xterm titles are settable by remote output). */
static void check_long_title(Display *d, Window root)
{
    Window w = XCreateSimpleWindow(d, root, 0, 0, 40, 40, 0, 0, 0xffffff);
    XMapWindow(d, w);
    XSync(d, False);
    size_t n = 200000;
    char *title = malloc(n + 1);
    memset(title, 'T', n);
    title[n] = 0;
    title[10] = (char)0xe9;                   /* Latin-1 in a STRING property */
    XStoreName(d, w, title);
    XSync(d, False);
    XSync(d, False);
    MwWindow *mw = mw_window(d, w);
    CHECK(mw && mw->tl && mw->tl->title &&
          strlen(mw->tl->title) < MW_WL_STRING_MAX);
    CHECK(mw && mw->tl && mw->tl->title &&
          memcmp(mw->tl->title + 10, "\xc3\xa9", 2) == 0);
    free(title);
    XDestroyWindow(d, w);
    XSync(d, False);
}

/* XReparentWindow into a child used to leave popups anchored to the old
 * toplevel pointing at it once it was destroyed. */
static void check_reparent_popup(Display *d, Window root)
{
    Window top = XCreateSimpleWindow(d, root, 0, 0, 100, 100, 0, 0, 0xffffff);
    XMapWindow(d, top);
    XSync(d, False);
    MwWindow *tw = mw_window(d, top);
    XSetWindowAttributes a;
    a.override_redirect = True;
    Window pop = XCreateWindow(d, root, 10, 10, 30, 30, 0, CopyFromParent,
                               InputOutput, CopyFromParent,
                               CWOverrideRedirect, &a);
    XMapWindow(d, pop);
    XSync(d, False);
    MwWindow *pw = mw_window(d, pop);

    Window holder = XCreateSimpleWindow(d, root, 0, 0, 200, 200, 0, 0, 0);
    XMapWindow(d, holder);
    XSync(d, False);
    XReparentWindow(d, top, holder, 0, 0);
    XSync(d, False);
    CHECK(pw && pw->popup_parent != tw);
    CHECK(pw && (!pw->tl || pw->tl->parent_tl != tw));
    XDestroyWindow(d, top);
    XSync(d, False);
    /* Re-posting the popup must not touch the destroyed anchor (ASan). */
    XUnmapWindow(d, pop);
    XMapWindow(d, pop);
    XSync(d, False);
    XDestroyWindow(d, pop);
    XDestroyWindow(d, holder);
    XSync(d, False);
}

/* The XCB connection is a dead one: no display name is resolved (the old
 * "mw-invalid:0" went to DNS and could reach a remote X server) and no socket
 * opened.  This executable's getaddrinfo interposes libc's, so a lookup by
 * libxcb is counted rather than sent. */
static int getaddrinfo_calls;

int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res)
{
    (void)node; (void)service; (void)hints;
    getaddrinfo_calls++;
    if (res) *res = NULL;
    return EAI_NONAME;
}

static void check_xcb(Display *d)
{
    xcb_connection_t *c = XGetXCBConnection(d);
    CHECK(c != NULL && xcb_connection_has_error(c) != 0);
    CHECK(XGetXCBConnection(d) == c);
    CHECK(getaddrinfo_calls == 0);
}

int main(void)
{
    check_wl_string();
    check_owned_files();
    check_xrm_files();

    Display *d = XOpenDisplay(NULL);
    if (!d) {
        fprintf(stderr, "test_security: no display\n");
        return getenv("MW_TEST_REQUIRE_DISPLAY") ? 1 : 77;
    }
    Window root = DefaultRootWindow(d);
    Window w = XCreateSimpleWindow(d, root, 0, 0, 50, 50, 0, 0, 0);

    check_create_image(d);
    check_error_text(d);
    check_property_formats(d, w);
    check_cookie_put_back(d, w);
    check_send_event(d, w);
    check_copy_overflow(d, root);
    check_clear_area(d, root);
    check_clip_rects(d, root);
    check_xkb_alloc(d);
    check_render_requests(d, root);
    check_long_title(d, root);
    check_reparent_popup(d, root);
    check_xcb(d);

    XDestroyWindow(d, w);
    XCloseDisplay(d);

    if (failures == 0) { printf("test_security: all checks passed\n"); return 0; }
    fprintf(stderr, "test_security: %d failure(s)\n", failures);
    return 1;
}
