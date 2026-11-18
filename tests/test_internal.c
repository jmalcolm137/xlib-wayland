/* test_internal.c — unit tests that reach into the shim's internals
 * (internal.h): the event queue, window destruction, keymap updates and the
 * wire helpers that extension libraries call.  Needs a compositor (run by
 * `meson test` through tests/with-compositor.sh). */
#define _GNU_SOURCE
#include "internal.h"

#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <wayland-cursor.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>

static int failures;

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int _XRead32(Display *d, long *data, long len);

static Bool any_event(Display *d, XEvent *e, XPointer a)
{ (void)d; (void)e; (void)a; return True; }

static void drain(Display *d)
{
    XEvent ev;
    XSync(d, False);
    while (XCheckIfEvent(d, &ev, any_event, NULL)) {}
}

static void put_client_message(Display *d, Window w, long tag)
{
    XEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.xclient.type = ClientMessage;
    ev.xclient.window = w;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = tag;
    mw_put_event(d, &ev);
}

static void put_expose(Display *d, Window w, int x, int y, int wd, int ht,
                       int count, Bool send)
{
    XEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.xexpose.type = Expose;
    ev.xexpose.window = w;
    ev.xexpose.x = x; ev.xexpose.y = y;
    ev.xexpose.width = wd; ev.xexpose.height = ht;
    ev.xexpose.count = count;
    ev.xexpose.send_event = send;
    mw_put_event(d, &ev);
}

static Bool is_tag(Display *d, XEvent *e, XPointer a)
{
    (void)d;
    return e->type == ClientMessage && e->xclient.data.l[0] == (long)a;
}

/* QLength() reads dp->qlen directly; every way of taking an event out of the
 * queue has to keep it equal to the real count. */
static void check_queue_length(Display *d, Window w)
{
    XDisplayImpl *dp = MWD(d);
    drain(d);
    for (long i = 0; i < 6; i++) put_client_message(d, w, i);
    CHECK(QLength(d) == 6);
    XEvent ev;
    CHECK(XCheckIfEvent(d, &ev, is_tag, (XPointer)3) && ev.xclient.data.l[0] == 3);
    CHECK(QLength(d) == 5 && dp->qcount == 5);
    CHECK(XCheckTypedEvent(d, ClientMessage, &ev) && ev.xclient.data.l[0] == 0);
    CHECK(QLength(d) == 4);
    CHECK(XCheckTypedWindowEvent(d, w, ClientMessage, &ev));
    CHECK(QLength(d) == 3);
    CHECK(XIfEvent(d, &ev, is_tag, (XPointer)5) && ev.xclient.data.l[0] == 5);
    CHECK(QLength(d) == 2);
    /* The remaining order is preserved: 2, 4. */
    XNextEvent(d, &ev);
    CHECK(ev.xclient.data.l[0] == 2 && QLength(d) == 1);
    XNextEvent(d, &ev);
    CHECK(ev.xclient.data.l[0] == 4 && QLength(d) == 0);

    /* A mask of 0 matches nothing (libX11), so it must not take events. */
    put_client_message(d, w, 9);
    put_expose(d, w, 0, 0, 1, 1, 0, False);
    CHECK(!XCheckMaskEvent(d, 0, &ev));
    CHECK(XCheckMaskEvent(d, ExposureMask, &ev) && ev.type == Expose);
    CHECK(QLength(d) == 1);
    drain(d);
}

/* XPutBackEvent on a full queue: the event goes to the head exactly once. */
static void check_put_back(Display *d, Window w)
{
    XDisplayImpl *dp = MWD(d);
    drain(d);
    int cap = dp->qcap;
    for (long i = 0; i < cap; i++) put_client_message(d, w, i);
    CHECK(dp->qcount == cap);
    XEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.xclient.type = ClientMessage;
    ev.xclient.window = w;
    ev.xclient.data.l[0] = -1;
    XPutBackEvent(d, &ev);
    CHECK(dp->qcount == cap + 1 && QLength(d) == cap + 1);
    XNextEvent(d, &ev);
    CHECK(ev.xclient.data.l[0] == -1);
    int dup = 0, n = 0;
    long last = -1;
    bool ordered = true;
    while (XCheckIfEvent(d, &ev, any_event, NULL)) {
        if (ev.type != ClientMessage) continue;
        if (ev.xclient.data.l[0] == -1) dup++;
        if (ev.xclient.data.l[0] != last + 1) ordered = false;
        last = ev.xclient.data.l[0];
        n++;
    }
    CHECK(dup == 0 && n == cap && ordered);
}

/* Expose coalescing must keep every area: the merged event covers the union
 * (bounding box) of what was queued.  Exposes sent by a client are not
 * merged. */
static void check_expose_coalescing(Display *d, Window w)
{
    drain(d);
    put_expose(d, w, 0, 0, 10, 10, 1, False);
    put_expose(d, w, 50, 60, 10, 10, 0, False);   /* disjoint, same size */
    put_expose(d, w, 5, 5, 2, 2, 0, False);       /* smaller, inside */
    XEvent ev;
    int n = 0;
    while (XCheckTypedWindowEvent(d, w, Expose, &ev)) {
        n++;
        CHECK(ev.xexpose.x == 0 && ev.xexpose.y == 0);
        CHECK(ev.xexpose.width == 60 && ev.xexpose.height == 70);
        CHECK(ev.xexpose.count == 0);
    }
    CHECK(n == 1);

    put_expose(d, w, 0, 0, 10, 10, 0, True);
    put_expose(d, w, 20, 20, 10, 10, 0, True);
    n = 0;
    while (XCheckTypedWindowEvent(d, w, Expose, &ev)) n++;
    CHECK(n == 2);
}

/* A parent selecting SubstructureNotify gets its own copy of a child's
 * ConfigureNotify (event = parent); it must not be merged into the child's. */
static void check_configure_notify(Display *d, Window root)
{
    Window p = XCreateSimpleWindow(d, root, 0, 0, 100, 100, 0, 0, 0);
    Window c = XCreateSimpleWindow(d, p, 0, 0, 10, 10, 0, 0, 0);
    XSelectInput(d, p, SubstructureNotifyMask);
    XSelectInput(d, c, StructureNotifyMask);
    drain(d);
    XMoveResizeWindow(d, c, 3, 4, 20, 30);
    XMoveResizeWindow(d, c, 5, 6, 25, 35);
    XEvent ev;
    int to_parent = 0, to_child = 0;
    while (XCheckTypedEvent(d, ConfigureNotify, &ev)) {
        CHECK(ev.xconfigure.window == c);
        if (ev.xconfigure.event == p) to_parent++;
        if (ev.xconfigure.event == c) to_child++;
        CHECK(ev.xconfigure.width == 25 && ev.xconfigure.x == 5);
    }
    CHECK(to_parent == 1 && to_child == 1);
    XDestroyWindow(d, p);
    drain(d);
}

static void check_window_tree(Display *d, Window root)
{
    /* Destroying the root is a no-op in X. */
    XDestroyWindow(d, root);
    CHECK(mw_window(d, root) != NULL);

    /* A mapped window under an unmapped parent is IsUnviewable. */
    Window p = XCreateSimpleWindow(d, root, 0, 0, 100, 100, 0, 0, 0);
    Window c = XCreateSimpleWindow(d, p, 10, 20, 30, 30, 0, 0, 0);
    XMapWindow(d, c);
    XWindowAttributes wa;
    XGetWindowAttributes(d, c, &wa);
    CHECK(wa.map_state == IsUnviewable);
    XGetWindowAttributes(d, p, &wa);
    CHECK(wa.map_state == IsUnmapped);
    /* ... and viewable once the parent is mapped too (the root, which is
     * never mapped itself, must not make everything look unviewable). */
    XMapWindow(d, p);
    XGetWindowAttributes(d, c, &wa);
    CHECK(wa.map_state == IsViewable);
    XGetWindowAttributes(d, p, &wa);
    CHECK(wa.map_state == IsViewable);
    XGetWindowAttributes(d, root, &wa);
    CHECK(wa.map_state == IsViewable);
    XUnmapWindow(d, p);

    /* XTranslateCoordinates names the child of dest under the point. */
    int dx, dy; Window child = 0;
    CHECK(XTranslateCoordinates(d, p, p, 15, 25, &dx, &dy, &child));
    CHECK(dx == 15 && dy == 25 && child == c);
    CHECK(XTranslateCoordinates(d, p, p, 90, 90, &dx, &dy, &child));
    CHECK(child == None);

    /* Destroying a selection owner gives the selection up. */
    Atom sel = XInternAtom(d, "MW_TEST_SELECTION", False);
    XSetSelectionOwner(d, sel, c, CurrentTime);
    CHECK(XGetSelectionOwner(d, sel) == c);

    /* A popup anchored to a window must not keep pointing at it. */
    Window menu = XCreateSimpleWindow(d, root, 0, 0, 10, 10, 0, 0, 0);
    MwWindow *mw = mw_window(d, menu);
    mw->popup_parent = mw_window(d, p);

    XDestroyWindow(d, p);
    CHECK(XGetSelectionOwner(d, sel) == None);
    CHECK(mw->popup_parent == NULL);
    XDestroyWindow(d, menu);
    drain(d);
}

/* XSetInputFocus reports the change with FocusOut / FocusIn. */
static void check_focus_events(Display *d, Window root)
{
    Window top = XCreateSimpleWindow(d, root, 0, 0, 100, 100, 0, 0, 0);
    Window a = XCreateSimpleWindow(d, top, 0, 0, 10, 10, 0, 0, 0);
    Window b = XCreateSimpleWindow(d, top, 20, 0, 10, 10, 0, 0, 0);
    XSelectInput(d, a, FocusChangeMask);
    XSelectInput(d, b, FocusChangeMask);
    XMapSubwindows(d, top);
    XMapWindow(d, top);
    XSetInputFocus(d, a, RevertToParent, CurrentTime);
    drain(d);
    XSetInputFocus(d, b, RevertToParent, CurrentTime);
    XEvent ev;
    CHECK(XCheckTypedWindowEvent(d, a, FocusOut, &ev));
    CHECK(XCheckTypedWindowEvent(d, b, FocusIn, &ev));
    Window f; int rev;
    XGetInputFocus(d, &f, &rev);
    CHECK(f == b);
    XDestroyWindow(d, top);
    drain(d);
}

/* _XRead32's length is in wire bytes (libXi/libXrandr pass nCrtcs << 2); the
 * buffer holds len/4 longs.  It used to write len longs. */
static void check_read32(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    uint32_t wire[3] = { 1, 0xfffffffeu, 0x12345678 };
    dp->xi2_data = malloc(sizeof wire);
    memcpy(dp->xi2_data, wire, sizeof wire);
    dp->xi2_data_len = sizeof wire;
    dp->xi2_data_off = 0;
    long out[5] = { 0, 0, 0, 0x5a5a, 0x5a5a };
    _XRead32(d, out, 12);
    CHECK(out[0] == 1 && out[1] == -2 && out[2] == 0x12345678);
    CHECK(out[3] == 0x5a5a && out[4] == 0x5a5a);
    CHECK(dp->xi2_data == NULL);
}

/* XISelectEvents' window is a CARD32 on the wire. */
static void check_xi2_select(Display *d, Window w)
{
    unsigned char req[20];
    memset(req, 0xee, sizeof req);           /* garbage around the fields */
    req[0] = 131;                            /* MW_XI_OPCODE */
    req[1] = 46;                             /* X_XISelectEvents */
    uint32_t w32 = (uint32_t)w;
    memcpy(req + 4, &w32, 4);
    unsigned short num = 1, dev = 0, words = 1;
    memcpy(req + 8, &num, 2);
    memcpy(req + 12, &dev, 2);
    memcpy(req + 14, &words, 2);
    memset(req + 16, 0, 4);
    req[16] = 1u << 4;                       /* XI_ButtonPress = 4 */
    mw_xi2_request(d, req, sizeof req);
    CHECK(mw_xi2_selected(d, w, 4));
    CHECK(!mw_xi2_selected(d, w, 5));
}

static void check_buttons(void)
{
    CHECK(mw_wl_button_to_x(0x110) == 1);    /* BTN_LEFT */
    CHECK(mw_wl_button_to_x(0x112) == 2);    /* BTN_MIDDLE */
    CHECK(mw_wl_button_to_x(0x111) == 3);    /* BTN_RIGHT */
    CHECK(mw_wl_button_to_x(0x113) == 8);    /* BTN_SIDE: past the wheel */
    CHECK(mw_wl_button_to_x(0x114) == 9);    /* BTN_EXTRA */
    CHECK(mw_wl_button_to_x(0x100) == 0);    /* BTN_0: not a mouse button */
}

static int keymap_fd(const char *layout)
{
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_rule_names names = { .rules = "evdev", .model = "pc105",
                                    .layout = layout };
    struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, &names, 0);
    if (!km) { xkb_context_unref(ctx); return -1; }
    char *str = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
    int fd = memfd_create("mw-keymap", MFD_CLOEXEC);
    size_t len = strlen(str) + 1;
    if (fd >= 0 && write(fd, str, len) != (ssize_t)len) { close(fd); fd = -1; }
    free(str);
    xkb_keymap_unref(km);
    xkb_context_unref(ctx);
    return fd;
}

static void check_keymap(Display *d, Window w)
{
    XDisplayImpl *dp = MWD(d);

    /* A keymap that fails to compile leaves the working one in place (it used
     * to leave xkb_state pointing at the freed keymap). */
    struct xkb_keymap *before = dp->xkb_keymap;
    struct xkb_state *before_state = dp->xkb_state;
    int bad = memfd_create("mw-bad-keymap", MFD_CLOEXEC);
    const char junk[] = "this is not a keymap";
    CHECK(write(bad, junk, sizeof junk) == (ssize_t)sizeof junk);
    mw_keymap_init_fd(d, bad, sizeof junk, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1);
    close(bad);
    CHECK(dp->xkb_keymap == before && dp->xkb_state == before_state);

    /* A French keymap: é is a Latin-1 keysym. */
    int fd = keymap_fd("fr");
    if (fd < 0) { printf("note: no 'fr' layout; skipping keymap checks\n"); return; }
    off_t size = lseek(fd, 0, SEEK_END);
    mw_keymap_init_fd(d, fd, (uint32_t)size, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1);
    close(fd);
    CHECK(dp->xkb_keymap != before);

    /* The modifier map follows the new keymap (it used to keep the one built
     * from the default keymap at XOpenDisplay). */
    XModifierKeymap *mm = XGetModifierMapping(d);
    CHECK(mm != NULL);
    if (mm) {
        KeyCode ctrl = XKeysymToKeycode(d, XK_Control_L);
        bool found = false;
        for (int i = 0; i < mm->max_keypermod; i++)
            if (mm->modifiermap[ControlMapIndex * mm->max_keypermod + i] == ctrl)
                found = true;
        CHECK(found);
        XFreeModifiermap(mm);
    }

    KeyCode kc = XKeysymToKeycode(d, XK_eacute);
    CHECK(kc != 0);
    if (kc) {
        XKeyEvent ke;
        memset(&ke, 0, sizeof ke);
        ke.type = KeyPress; ke.display = d; ke.window = w; ke.keycode = kc;
        char buf[8]; KeySym ks; Status st;
        /* XLookupString is Latin-1 ... */
        int n = XLookupString(&ke, buf, sizeof buf, &ks, NULL);
        CHECK(n == 1 && (unsigned char)buf[0] == 0xe9 && ks == XK_eacute);
        /* ... Xutf8LookupString must hand back UTF-8. */
        n = Xutf8LookupString(NULL, &ke, buf, sizeof buf, &ks, &st);
        CHECK(n == 2 && (unsigned char)buf[0] == 0xc3 &&
              (unsigned char)buf[1] == 0xa9 && st == XLookupBoth);
        wchar_t wb[2];
        n = XwcLookupString(NULL, &ke, wb, 2, &ks, &st);
        CHECK(n == 1 && wb[0] == 0xe9);
    }

    /* XkbGetMap walks every keycode up to 255; a KeyCode loop counter used
     * to wrap and never finish.  alarm() turns a hang into a failure. */
    alarm(10);
    XkbDescPtr xkb = XkbGetMap(d, XkbAllClientInfoMask, XkbUseCoreKbd);
    alarm(0);
    CHECK(xkb != NULL);
    if (xkb) {
        CHECK(xkb->max_key_code == 255 && xkb->min_key_code >= 8);
        XkbFreeKeyboard(xkb, 0, True);
    }
}

static void check_input_matching(Display *d, Window w)
{
    /* Passive grab modifiers: exact key modifiers, locks ignored. */
    CHECK(mw_grab_mods_match(ControlMask, ControlMask));
    CHECK(!mw_grab_mods_match(ControlMask, 0));
    CHECK(!mw_grab_mods_match(0, ControlMask));
    CHECK(mw_grab_mods_match(ControlMask, ControlMask | LockMask | Mod2Mask));
    CHECK(!mw_grab_mods_match(ControlMask, ControlMask | ShiftMask));
    CHECK(mw_grab_mods_match(AnyModifier, ShiftMask | Mod1Mask));
    CHECK(mw_grab_mods_match(0, Button1Mask));       /* buttons are not mods */

    /* Motion selection. */
    CHECK(mw_motion_selected(PointerMotionMask, 0));
    CHECK(!mw_motion_selected(ButtonMotionMask, 0));
    CHECK(mw_motion_selected(ButtonMotionMask, Button2Mask));
    CHECK(mw_motion_selected(Button1MotionMask, Button1Mask));
    CHECK(!mw_motion_selected(Button1MotionMask, Button3Mask));
    CHECK(!mw_motion_selected(ButtonPressMask, Button1Mask));

    /* Grabbing the same combination twice keeps one entry, and ungrabbing
     * honours the modifiers. */
    XDisplayImpl *dp = MWD(d);
    int before = 0;
    for (MwBtnGrab *g = dp->btn_grabs; g; g = g->next) before++;
    XGrabButton(d, Button1, ControlMask, w, False, ButtonPressMask,
                GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(d, Button1, ControlMask, w, False, ButtonPressMask,
                GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(d, Button1, ShiftMask, w, False, ButtonPressMask,
                GrabModeAsync, GrabModeAsync, None, None);
    int n = 0;
    for (MwBtnGrab *g = dp->btn_grabs; g; g = g->next) n++;
    CHECK(n == before + 2);
    XUngrabButton(d, Button1, ShiftMask, w);
    n = 0;
    bool ctrl_left = false;
    for (MwBtnGrab *g = dp->btn_grabs; g; g = g->next) {
        n++;
        if (g->window == w && g->modifiers == ControlMask) ctrl_left = true;
    }
    CHECK(n == before + 1 && ctrl_left);
    XUngrabButton(d, AnyButton, AnyModifier, w);
    n = 0;
    for (MwBtnGrab *g = dp->btn_grabs; g; g = g->next) n++;
    CHECK(n == before);
}

/* Event propagation: the first ancestor that selected the event, unless a
 * window on the way blocks it with do_not_propagate_mask. */
static void check_propagation(Display *d, Window root)
{
    Window top = XCreateSimpleWindow(d, root, 0, 0, 100, 100, 0, 0, 0);
    Window mid = XCreateSimpleWindow(d, top, 10, 10, 80, 80, 0, 0, 0);
    Window leaf = XCreateSimpleWindow(d, mid, 5, 5, 20, 20, 0, 0, 0);
    MwWindow *t = mw_window(d, top), *m = mw_window(d, mid), *l = mw_window(d, leaf);
    XSelectInput(d, top, ButtonPressMask | KeyPressMask);
    XSelectInput(d, mid, Button1MotionMask);
    Window sub = 1;
    CHECK(mw_propagate(l, ButtonPressMask, 0, &sub) == t && sub == mid);
    CHECK(mw_propagate(t, ButtonPressMask, 0, &sub) == t && sub == None);
    /* Motion: Button1MotionMask on mid applies only with button 1 held. */
    long motion = PointerMotionMask | ButtonMotionMask | Button1MotionMask |
                  Button2MotionMask | Button3MotionMask | Button4MotionMask |
                  Button5MotionMask;
    CHECK(mw_propagate(l, motion, Button1Mask, &sub) == m && sub == leaf);
    CHECK(mw_propagate(l, motion, 0, &sub) == NULL);
    /* do_not_propagate on mid stops the press there. */
    XSetWindowAttributes a;
    a.do_not_propagate_mask = ButtonPressMask;
    XChangeWindowAttributes(d, mid, CWDontPropagate, &a);
    CHECK(mw_propagate(l, ButtonPressMask, 0, &sub) == NULL);
    CHECK(mw_propagate(l, KeyPressMask, 0, &sub) == t);
    XDestroyWindow(d, top);
    drain(d);
}

typedef struct { int type; Window w; int detail; Window sub; } Cross;

static int collect_crossings(Display *d, Cross *out, int max)
{
    int n = 0;
    XEvent ev;
    while (n < max && XCheckIfEvent(d, &ev, any_event, NULL)) {
        if (ev.type != EnterNotify && ev.type != LeaveNotify) continue;
        out[n].type = ev.type;
        out[n].w = ev.xcrossing.window;
        out[n].detail = ev.xcrossing.detail;
        out[n].sub = ev.xcrossing.subwindow;
        n++;
    }
    return n;
}

/* Crossing events go to the windows that selected them, never to an
 * ancestor by propagation, with X's details for the windows in between. */
static void check_crossings(Display *d, Window root)
{
    Window top = XCreateSimpleWindow(d, root, 0, 0, 100, 100, 0, 0, 0);
    Window a = XCreateSimpleWindow(d, top, 0, 0, 40, 40, 0, 0, 0);
    Window a1 = XCreateSimpleWindow(d, a, 5, 5, 10, 10, 0, 0, 0);
    Window b = XCreateSimpleWindow(d, top, 50, 0, 40, 40, 0, 0, 0);
    long cm = EnterWindowMask | LeaveWindowMask;
    XSelectInput(d, top, cm);
    XSelectInput(d, a, cm);
    XSelectInput(d, a1, cm);
    /* b does not select crossings: nothing may reach top on its behalf. */
    MwWindow *T = mw_window(d, top), *A = mw_window(d, a),
             *A1 = mw_window(d, a1), *B = mw_window(d, b);
    mw_pointer_move_to(d, A);
    drain(d);

    Cross c[8];
    /* Into an inferior: a gets Leave(Inferior, subwindow a1), a1 Enter(Ancestor). */
    mw_pointer_move_to(d, A1);
    int n = collect_crossings(d, c, 8);
    CHECK(n == 2);
    if (n == 2) {
        CHECK(c[0].type == LeaveNotify && c[0].w == a && c[0].detail == NotifyInferior &&
              c[0].sub == a1);
        CHECK(c[1].type == EnterNotify && c[1].w == a1 && c[1].detail == NotifyAncestor);
    }

    /* Across to a sibling's subtree: a1 Leave(Nonlinear), a Leave
     * (NonlinearVirtual); b selected nothing, and top (the common ancestor)
     * gets nothing. */
    mw_pointer_move_to(d, B);
    n = collect_crossings(d, c, 8);
    CHECK(n == 2);
    if (n == 2) {
        CHECK(c[0].type == LeaveNotify && c[0].w == a1 && c[0].detail == NotifyNonlinear);
        CHECK(c[1].type == LeaveNotify && c[1].w == a && c[1].detail == NotifyNonlinearVirtual);
    }

    /* Out to the parent: b would get Leave(Ancestor) (not selected), top
     * Enter(Inferior). */
    mw_pointer_move_to(d, T);
    n = collect_crossings(d, c, 8);
    CHECK(n == 1 && c[0].type == EnterNotify && c[0].w == top &&
          c[0].detail == NotifyInferior && c[0].sub == b);
    (void)A1;

    /* XI2 gets one Enter for the window entered -- not one more for each
     * virtual crossing of the windows in between (an XI2-selecting ancestor
     * used to receive them all). */
    unsigned char req[20];
    memset(req, 0, sizeof req);
    req[0] = 131; req[1] = 46;
    uint32_t w32 = (uint32_t)top;
    memcpy(req + 4, &w32, 4);
    unsigned short one = 1;
    memcpy(req + 8, &one, 2);
    memcpy(req + 14, &one, 2);
    req[16] = 1u << 7;                        /* XI_Enter */
    mw_xi2_request(d, req, sizeof req);
    mw_pointer_move_to(d, NULL);
    drain(d);
    mw_pointer_move_to(d, A1);
    int xi_enters = 0;
    XEvent ev;
    while (XCheckTypedEvent(d, GenericEvent, &ev)) {
        xi_enters++;
        if (xi_enters == 1 && XGetEventData(d, &ev.xcookie))
            XFreeEventData(d, &ev.xcookie);   /* the others are freed for us */
    }
    CHECK(xi_enters == 1);
    mw_pointer_move_to(d, NULL);
    XDestroyWindow(d, top);
    drain(d);
}

static void check_xim(Display *d, Window w)
{
    XIM im = XOpenIM(d, NULL, NULL, NULL);
    CHECK(im != NULL);
    if (!im) return;
    XIC ic = XCreateIC(im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                       XNClientWindow, w, XNFocusWindow, w, NULL);
    CHECK(ic != NULL);
    if (!ic) { XCloseIM(im); return; }

    /* A nested list in XGetICValues names places to fill; it used to be
     * applied as values, overwriting the IC with the caller's pointers. */
    XIMStyle style = 0;
    Window client = 0;
    XVaNestedList nl = XVaCreateNestedList(0, XNInputStyle, &style,
                                           XNClientWindow, &client, NULL);
    CHECK(XGetICValues(ic, XNVaNestedList, nl, NULL) == NULL);
    XFree(nl);
    CHECK(style == (XIMPreeditNothing | XIMStatusNothing) && client == w);
    Window again = 0;
    XGetICValues(ic, XNClientWindow, &again, NULL);
    CHECK(again == w);

    /* Text input: events take effect at done, delete before commit, and
     * delete_surrounding's byte counts become characters (BackSpaces). */
    XWindowAttributes wa;
    XGetWindowAttributes(d, w, &wa);
    XSelectInput(d, w, wa.your_event_mask | KeyPressMask);
    XSetICFocus(ic);
    XSetInputFocus(d, w, RevertToParent, CurrentTime);
    drain(d);
    const struct zwp_text_input_v3_listener *l = mw_xim_text_input_listener();
    KeyCode bs = XKeysymToKeycode(d, XK_BackSpace);
    XEvent ev;
    char buf[64];
    KeySym ks;
    Status st;
    l->commit_string(d, NULL, "h\xc3\xa9");                  /* "hé" */
    CHECK(QLength(d) == 0);                                   /* not before done */
    l->done(d, NULL, 1);
    CHECK(XCheckTypedWindowEvent(d, w, KeyPress, &ev) && ev.xkey.keycode == 0);
    int n = Xutf8LookupString(ic, &ev.xkey, buf, sizeof buf, &ks, &st);
    CHECK(n == 3 && memcmp(buf, "h\xc3\xa9", 3) == 0);
    drain(d);
    /* Replace the é (2 bytes, one character) with e. */
    l->commit_string(d, NULL, "e");
    l->delete_surrounding_text(d, NULL, 2, 0);
    l->done(d, NULL, 2);
    int backs = 0, commits = 0, order_ok = 1;
    while (XCheckMaskEvent(d, KeyPressMask, &ev)) {
        if (ev.xkey.keycode == bs) { backs++; if (commits) order_ok = 0; }
        else if (ev.xkey.keycode == 0) {
            commits++;
            n = Xutf8LookupString(ic, &ev.xkey, buf, sizeof buf, &ks, &st);
            CHECK(n == 1 && buf[0] == 'e');
        }
    }
    CHECK(backs == 1 && commits == 1 && order_ok);
    /* Beyond what was committed here, a byte counts as a character. */
    l->delete_surrounding_text(d, NULL, 4, 0);       /* "he" + 2 unknown */
    l->done(d, NULL, 3);
    backs = 0;
    while (XCheckMaskEvent(d, KeyPressMask, &ev)) if (ev.xkey.keycode == bs) backs++;
    CHECK(backs == 4);
    drain(d);
    XSelectInput(d, w, wa.your_event_mask);

    /* Closing the IM destroys its ICs and detaches text input. */
    XCloseIM(im);
    CHECK(MWD(d)->text_input_ic == NULL);
}

/* Reloading the cursor theme (XcursorSetDefaultSize/SetTheme, which GDK
 * calls on an XSETTINGS change) frees the old theme's wl_cursors; existing
 * cursors must be re-resolved, not left pointing into it. */
static void check_cursor_reload(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    Cursor c = XCreateFontCursor(d, XC_xterm);
    MwCursor *mc = mw_cursor(d, c);
    CHECK(mc != NULL);
    mw_cursor_size_set(d, 32);
    if (mc) {
        struct wl_cursor *now = dp->cursor_theme
            ? wl_cursor_theme_get_cursor(dp->cursor_theme, "xterm") : NULL;
        CHECK(mc->wl == now);
    }
    XFreeCursor(d, c);
    mw_cursor_size_set(d, 24);
}

/* A runtime output-scale change must reallocate the toplevel's buffers
 * before the next frame wraps them at the new scale (AddressSanitizer
 * catches the overflow if it does not). */
static void check_scale_change(Display *d, Window root)
{
    XDisplayImpl *dp = MWD(d);
    Window w = XCreateSimpleWindow(d, root, 0, 0, 120, 80, 0, 0, 0xffffff);
    XSelectInput(d, w, ExposureMask);
    XMapWindow(d, w);
    XEvent ev;
    for (int i = 0; i < 50 && !XCheckTypedWindowEvent(d, w, Expose, &ev); i++)
        XSync(d, False);
    MwWindow *mw = mw_window(d, w);
    CHECK(mw && mw->tl);
    GC gc = XCreateGC(d, w, 0, NULL);
    XFillRectangle(d, w, gc, 0, 0, 120, 80);
    XSync(d, False);
    mw_flush_damage(d);
    int before = dp->output_scale;
    dp->output_scale = 3;
    if (mw && mw->tl) {
        mw_window_damage(mw);
        mw_toplevel_render(mw->tl);
        CHECK(mw->tl->buf_scale == 3);
    }
    dp->output_scale = before;
    if (mw && mw->tl) {
        mw_window_damage(mw);
        mw_toplevel_render(mw->tl);
    }
    XFreeGC(d, gc);
    XDestroyWindow(d, w);
    drain(d);
}

/* Writing to a Wayland peer's pipe: a closed reader must not kill the
 * client with SIGPIPE, and a reader that never reads must not hang it. */
static void check_write_all(void)
{
    int p[2];
    CHECK(pipe(p) == 0);
    close(p[0]);
    char buf[16] = "hello";
    CHECK(mw_write_all(p[1], buf, sizeof buf, 1000) == -1);   /* still alive */
    sigset_t pend;
    sigpending(&pend);
    CHECK(!sigismember(&pend, SIGPIPE));     /* consumed, not left pending */
    close(p[1]);

    CHECK(pipe(p) == 0);
    size_t big = 1 << 20;                    /* far more than a pipe holds */
    char *data = calloc(1, big);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    CHECK(mw_write_all(p[1], data, big, 200) == -1);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    CHECK(ms < 2000);
    /* The descriptor's blocking mode is left as it was. */
    CHECK(!(fcntl(p[1], F_GETFL) & O_NONBLOCK));
    close(p[0]); close(p[1]);

    CHECK(pipe(p) == 0);
    CHECK(mw_write_all(p[1], "abc", 3, 1000) == 0);
    char r[4] = { 0 };
    CHECK(read(p[0], r, 3) == 3 && !strcmp(r, "abc"));
    close(p[0]); close(p[1]);
    free(data);
}

static bool conn_ready(Display *d, int ms)
{
    struct pollfd pfd = { .fd = ConnectionNumber(d), .events = POLLIN };
    return poll(&pfd, 1, ms) == 1;
}

/* Let compositor traffic settle so the fd starts out quiet. */
static bool settle(Display *d)
{
    for (int i = 0; i < 20; i++) {
        XSync(d, False);
        mw_wakeup_drain(MWD(d));
        if (!conn_ready(d, 50)) return true;
    }
    return false;
}

/* The fd Xt selects on (ConnectionNumber) must wake for a paste's data and
 * for a transfer's timeout -- and a ready fd nobody reads must be signalled
 * once, not in a loop. */
static void check_wakeup(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->wake_running) { printf("note: no wakeup thread\n"); return; }
    int p[2];
    CHECK(pipe(p) == 0);
    mw_wakeup_watch(dp, p[0], -1, 0);
    CHECK(settle(d));
    CHECK(write(p[1], "x", 1) == 1);
    CHECK(conn_ready(d, 1000));

    /* Not read by anyone: the helper must not keep signalling. */
    usleep(200 * 1000);
    int pending = 0;
    ioctl(dp->wake_pipe[0], FIONREAD, &pending);
    CHECK(pending <= 2);

    mw_wakeup_watch(dp, -1, -1, 0);
    CHECK(settle(d));
    mw_wakeup_watch(dp, -1, -1, mw_now() + 150);
    CHECK(conn_ready(d, 1000));
    mw_wakeup_watch(dp, -1, -1, 0);
    mw_wakeup_drain(dp);
    close(p[0]); close(p[1]);
}

/* A serve to a peer that has not read yet is queued and written as the pipe
 * drains -- on the event loop, so a reader in the same thread (a drag
 * between two of our own windows) gets everything, without a stall. */
static void check_async_serve(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    int p[2];
    CHECK(pipe(p) == 0);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    size_t total = 1 << 20;
    unsigned char *data = malloc(total);
    for (size_t i = 0; i < total; i++) data[i] = (unsigned char)(i * 7);
    MwClipServe *sv = &dp->clip_serve;
    CHECK(!sv->active);
    sv->active = true;
    sv->fd = p[1];
    sv->pend = data;
    sv->pend_len = total;
    sv->pend_off = 0;
    sv->deadline_ms = mw_now() + 5000;
    size_t got = 0;
    bool same = true;
    unsigned char buf[65536];
    uint64_t t0 = mw_now();
    while (got < total && mw_now() - t0 < 3000) {
        mw_clipboard_handle_ready(d);              /* writes what fits */
        ssize_t n;
        while ((n = read(p[0], buf, sizeof buf)) > 0) {
            for (ssize_t i = 0; i < n; i++)
                if (buf[i] != (unsigned char)((got + (size_t)i) * 7)) same = false;
            got += (size_t)n;
        }
    }
    CHECK(got == total && same);
    CHECK(mw_now() - t0 < 2000);
    CHECK(!sv->active && sv->pend == NULL);       /* finished, fd closed */
    close(p[0]);
}

/* With SIGPIPE blocked by the application, the EPIPE's SIGPIPE stays
 * pending for it, as a plain write would leave it. */
static void check_sigpipe_blocked(void)
{
    sigset_t set, old;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, &old);
    int p[2];
    CHECK(pipe(p) == 0);
    close(p[0]);
    CHECK(mw_write_some(p[1], "x", 1) == -1);
    sigset_t pend;
    sigpending(&pend);
    CHECK(sigismember(&pend, SIGPIPE));
    struct timespec zero = { 0, 0 };
    sigtimedwait(&set, NULL, &zero);
    close(p[1]);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
}

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

/* The shared property store is input from other processes: malformed
 * records are rejected (a wrapped size used to overflow the reader), values
 * are served on the root only, with the usual offset/length/type rules, and
 * an append republishes the whole property. */
static void check_shared_store(Display *d, Window w)
{
    char dir[256], path[300];
    CHECK(mw_runtime_dir(dir, sizeof dir));
    snprintf(path, sizeof path, "%s/smprops", dir);
    setenv("XLIB_WAYLAND_SHARE_PROPERTIES", "MW_SHARED_", 1);
    write_text(path,
        "MW_SHARED_BIG\tSTRING\t32\t4611686018427387905\t00000000\n"
        "MW_SHARED_ODD\tSTRING\t7\t3\t616263\n"
        "MW_SHARED_SHORT\tSTRING\t8\t5\t616263\n"
        "MW_SHARED_OK\tSTRING\t8\t5\t68656c6c6f\n");
    Window root = DefaultRootWindow(d);
    Atom at; int af; unsigned long ni, ba; unsigned char *data = NULL;
    const char *bad[] = { "MW_SHARED_BIG", "MW_SHARED_ODD", "MW_SHARED_SHORT" };
    for (int i = 0; i < 3; i++) {
        XGetWindowProperty(d, root, XInternAtom(d, bad[i], False), 0, 100, False,
                           AnyPropertyType, &at, &af, &ni, &ba, &data);
        CHECK(at == None && ni == 0 && data == NULL);
        XFree(data); data = NULL;
    }
    Atom ok = XInternAtom(d, "MW_SHARED_OK", False);
    XGetWindowProperty(d, root, ok, 0, 100, False, AnyPropertyType,
                       &at, &af, &ni, &ba, &data);
    CHECK(at == XA_STRING && ni == 5 && data && !memcmp(data, "hello", 5));
    XFree(data); data = NULL;
    /* offset/length and type apply as to a local property */
    XGetWindowProperty(d, root, ok, 1, 1, False, AnyPropertyType,
                       &at, &af, &ni, &ba, &data);
    CHECK(ni == 1 && ba == 0 && data && data[0] == 'o');
    XFree(data); data = NULL;
    XGetWindowProperty(d, root, ok, 0, 100, False, XA_ATOM,
                       &at, &af, &ni, &ba, &data);
    CHECK(ni == 0 && data == NULL && at == XA_STRING && ba == 5);
    /* not on an ordinary window */
    XGetWindowProperty(d, w, ok, 0, 100, False, AnyPropertyType,
                       &at, &af, &ni, &ba, &data);
    CHECK(at == None && data == NULL);

    /* An append publishes the merged value. */
    Atom ap = XInternAtom(d, "MW_SHARED_APPEND", False);
    XChangeProperty(d, root, ap, XA_STRING, 8, PropModeReplace,
                    (const unsigned char *)"ab", 2);
    XChangeProperty(d, root, ap, XA_STRING, 8, PropModeAppend,
                    (const unsigned char *)"cd", 2);
    FILE *f = fopen(path, "r");
    char buf[4096] = "";
    if (f) { size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f); }
    CHECK(strstr(buf, "MW_SHARED_APPEND\tSTRING\t8\t4\t61626364") != NULL);
    unsetenv("XLIB_WAYLAND_SHARE_PROPERTIES");
    unlink(path);

    /* A runtime directory open to others is not used. */
    chmod(dir, 0755);
    CHECK(!mw_runtime_dir(dir, sizeof dir));
    chmod(dir, 0700);
    CHECK(mw_runtime_dir(dir, sizeof dir));
}

/* Extension event numbers are distinct and below 128 (bit 7 of a type is
 * the send_event flag). */
static void check_extension_numbers(void)
{
    int ev[] = { MW_XFIXES_EVENT_BASE, MW_XFIXES_EVENT_BASE + 1,
                 MW_XDAMAGE_EVENT_BASE, MW_XSHM_EVENT_BASE,
                 MW_XSYNC_EVENT_BASE, MW_XSYNC_EVENT_BASE + 1,
                 64 /* SHAPE */, 90, 91 /* RANDR */ };
    int n = (int)(sizeof ev / sizeof ev[0]);
    for (int i = 0; i < n; i++) {
        CHECK(ev[i] >= LASTEvent && ev[i] < 128);
        for (int j = i + 1; j < n; j++) CHECK(ev[i] != ev[j]);
        CHECK(ev[i] < 70 || ev[i] > 86);      /* XInput's range */
    }
    CHECK(MW_XDAMAGE_ERROR_BASE != MW_XSYNC_ERROR_BASE);
}

static bool dp_compose(Display *d) { return MWD(d)->xkb_compose_state != NULL; }

static void load_layout(Display *d, const char *layout)
{
    int fd = keymap_fd(layout);
    if (fd < 0) return;
    off_t size = lseek(fd, 0, SEEK_END);
    mw_keymap_init_fd(d, fd, (uint32_t)size, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1);
    close(fd);
}

static int lookup(Display *d, Window w, KeyCode kc, unsigned int state, int type,
                  char *buf, int n, KeySym *ks)
{
    XKeyEvent ke;
    memset(&ke, 0, sizeof ke);
    ke.type = type; ke.display = d; ke.window = w; ke.keycode = kc;
    ke.state = state;
    ke.serial = ++MWD(d)->serial;
    int r = XLookupString(&ke, buf, n, ks, NULL);
    if (r >= 0 && r < n) buf[r] = 0;
    return r;
}

/* The keysym comes from the key's type and the full modifier state, as on
 * X: Caps Lock, Num Lock and AltGr choose levels too (only Shift did). */
static void check_key_levels(Display *d, Window w)
{
    char buf[16];
    KeySym ks;
    load_layout(d, "us");
    KeyCode a = XKeysymToKeycode(d, XK_a);
    CHECK(lookup(d, w, a, LockMask, KeyPress, buf, sizeof buf, &ks) == 1 &&
          ks == XK_A && buf[0] == 'A');
    CHECK(lookup(d, w, a, LockMask | ShiftMask, KeyPress, buf, sizeof buf, &ks) == 1 &&
          ks == XK_a);
    KeyCode kp1 = XKeysymToKeycode(d, XK_KP_End);
    CHECK(kp1 != 0);
    CHECK(lookup(d, w, kp1, 0, KeyPress, buf, sizeof buf, &ks) == 0 && ks == XK_KP_End);
    CHECK(lookup(d, w, kp1, Mod2Mask, KeyPress, buf, sizeof buf, &ks) == 1 &&
          ks == XK_KP_1 && buf[0] == '1');

    load_layout(d, "de");
    KeyCode q = XKeysymToKeycode(d, XK_q);
    CHECK(q != 0);
    CHECK(lookup(d, w, q, Mod5Mask, KeyPress, buf, sizeof buf, &ks) == 1 &&
          ks == XK_at && buf[0] == '@');
    /* The core keysym table has the AltGr level where Xorg puts it:
     * G1L1 G1L2 G2L1 G2L2 G1L3 ... */
    int per = 0;
    KeySym *map = XGetKeyboardMapping(d, q, 1, &per);
    CHECK(map && per >= 6 && map[0] == XK_q && map[1] == XK_Q && map[4] == XK_at);
    XFree(map);
    CHECK(XKeycodeToKeysym(d, q, 4) == XK_at && XKeycodeToKeysym(d, q, 1) == XK_Q);

    /* The XkbLookupKeySym and XkbTranslateKeyCode (GTK) paths agree. */
    KeySym xs = 0;
    unsigned int consumed = 0;
    XkbLookupKeySym(d, q, Mod5Mask, &consumed, &xs);
    CHECK(xs == XK_at && (consumed & Mod5Mask));
    XkbDescPtr desc = XkbGetMap(d, XkbAllClientInfoMask, XkbUseCoreKbd);
    xs = 0;
    CHECK(desc && XkbTranslateKeyCode(desc, q, Mod5Mask, &consumed, &xs) &&
          xs == XK_at);
    if (desc) XkbFreeKeyboard(desc, 0, True);

    /* Compose is fed key presses only, once each: Xt looks a key release up
     * too, and a dead key's release used to complete the sequence. */
    if (dp_compose(d)) {
        load_layout(d, "fr");
        KeyCode dead = XKeysymToKeycode(d, XK_dead_circumflex);
        KeyCode e = XKeysymToKeycode(d, XK_e);
        if (dead && e) {
            CHECK(lookup(d, w, dead, 0, KeyPress, buf, sizeof buf, &ks) == 0);
            CHECK(lookup(d, w, dead, 0, KeyRelease, buf, sizeof buf, &ks) == 0);
            int n = lookup(d, w, e, 0, KeyPress, buf, sizeof buf, &ks);
            CHECK(n == 2 && (unsigned char)buf[0] == 0xc3 &&
                  (unsigned char)buf[1] == 0xaa);          /* ê in UTF-8 */
        }
    } else {
        printf("note: no compose table; skipping the compose check\n");
    }
    load_layout(d, "us");
}

static void check_case_and_modifiers(void)
{
    KeySym lo, up;
    XConvertCase(XK_agrave, &lo, &up);
    CHECK(lo == XK_agrave && up == XK_Agrave);
    XConvertCase(XK_Cyrillic_IE, &lo, &up);
    CHECK(lo == XK_Cyrillic_ie && up == XK_Cyrillic_IE);
    XConvertCase(0x1000101, &lo, &up);            /* U+0101 a-macron */
    CHECK(up == 0x1000100);
    /* XSetLocaleModifiers: NULL queries, "" takes $XMODIFIERS. */
    setenv("XMODIFIERS", "@im=test", 1);
    char *m = XSetLocaleModifiers("");
    CHECK(m && strstr(m, "im=test"));
    m = XSetLocaleModifiers(NULL);
    CHECK(m && strstr(m, "im=test"));
    m = XSetLocaleModifiers("@im=other");
    CHECK(m && strstr(m, "im=other"));
    m = XSetLocaleModifiers(NULL);
    CHECK(m && strstr(m, "im=other"));
    unsetenv("XMODIFIERS");
}

static int last_error;
static int on_error(Display *d, XErrorEvent *e) { (void)d; last_error = e->error_code; return 0; }

static void check_core_odds(Display *d, Window root)
{
    XDisplayImpl *dp = MWD(d);
    Window top = XCreateSimpleWindow(d, root, 0, 0, 100, 100, 0, 0, 0);
    Window kid = XCreateSimpleWindow(d, top, 10, 10, 20, 20, 0, 0, 0);
    drain(d);

    /* XSendEvent to InputFocus / PointerWindow reaches those windows, and
     * the caller's event is not modified. */
    XSetInputFocus(d, kid, RevertToParent, CurrentTime);
    drain(d);
    XEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.xclient.type = ClientMessage;
    ev.xclient.format = 32;
    ev.xclient.window = 0;
    CHECK(XSendEvent(d, InputFocus, False, NoEventMask, &ev));
    CHECK(ev.xclient.send_event == False);
    XEvent got;
    CHECK(XCheckTypedEvent(d, ClientMessage, &got) && got.xclient.send_event);
    mw_pointer_move_to(d, mw_window(d, top));
    drain(d);
    CHECK(XSendEvent(d, PointerWindow, False, NoEventMask, &ev));
    CHECK(XCheckTypedEvent(d, ClientMessage, &got));
    /* Callers pass the specific event structure, smaller than an XEvent;
     * only that much is read (ASan caught a whole-XEvent copy). */
    XClientMessageEvent *small = malloc(sizeof *small);
    memset(small, 0, sizeof *small);
    small->type = ClientMessage; small->format = 32; small->window = top;
    small->data.l[0] = 42;
    CHECK(XSendEvent(d, top, False, NoEventMask, (XEvent *)small));
    CHECK(XCheckTypedEvent(d, ClientMessage, &got) && got.xclient.data.l[0] == 42);
    free(small);
    mw_pointer_move_to(d, NULL);

    /* XSync(True) discards the queue; XNextRequest only looks. */
    put_client_message(d, top, 1);
    XSync(d, True);
    CHECK(QLength(d) == 0 && dp->qcount == 0);
    unsigned long r1 = XNextRequest(d), r2 = XNextRequest(d);
    CHECK(r1 == r2);

    /* Reparenting a window into its own descendant is BadMatch (it used to
     * make a cycle, and the tree walks looped forever). */
    XErrorHandler old = XSetErrorHandler(on_error);
    last_error = 0;
    XReparentWindow(d, top, kid, 0, 0);
    XSync(d, False);
    CHECK(last_error == BadMatch);
    CHECK(mw_window(d, kid)->parent == mw_window(d, top));

    /* XClearArea is clipped to the window: an area outside it exposes
     * nothing (a negative width used to be sent as an Expose). */
    XSelectInput(d, top, ExposureMask);
    XMapWindow(d, top);
    drain(d);
    XClearArea(d, top, 150, 0, 0, 0, True);
    CHECK(!XCheckTypedWindowEvent(d, top, Expose, &got));
    XClearArea(d, top, 90, 90, 20, 20, True);
    CHECK(XCheckTypedWindowEvent(d, top, Expose, &got) &&
          got.xexpose.width == 10 && got.xexpose.height == 10);

    /* Unmapping the grab window releases the grab, as X does when a grab
     * window becomes unviewable. */
    XGrabPointer(d, kid, False, ButtonPressMask, GrabModeAsync, GrabModeAsync,
                 None, None, CurrentTime);
    CHECK(dp->ptr_grab_active);
    XUnmapWindow(d, top);
    CHECK(!dp->ptr_grab_active && dp->ptr_grab_window == NULL);
    XSetErrorHandler(old);
    XDestroyWindow(d, top);
    drain(d);
}

int main(void)
{
    check_write_all();
    check_case_and_modifiers();
    check_extension_numbers();
    check_sigpipe_blocked();
    check_buttons();

    Display *d = XOpenDisplay(NULL);
    if (!d) {
        fprintf(stderr, "test_internal: no display\n");
        /* skipped, unless the wrapper promised a compositor */
        return getenv("MW_TEST_REQUIRE_DISPLAY") ? 1 : 77;
    }
    Window root = DefaultRootWindow(d);
    Window w = XCreateSimpleWindow(d, root, 0, 0, 50, 50, 0, 0, 0);

    check_queue_length(d, w);
    check_put_back(d, w);
    check_expose_coalescing(d, w);
    check_configure_notify(d, root);
    check_window_tree(d, root);
    check_focus_events(d, root);
    check_read32(d);
    check_xi2_select(d, w);
    check_input_matching(d, w);
    check_propagation(d, root);
    check_crossings(d, root);
    check_xim(d, w);
    check_cursor_reload(d);
    check_scale_change(d, root);
    check_wakeup(d);
    check_async_serve(d);
    check_shared_store(d, w);
    check_core_odds(d, root);
    check_keymap(d, w);
    check_key_levels(d, w);

    XDestroyWindow(d, w);
    XCloseDisplay(d);

    if (failures == 0) { printf("test_internal: all checks passed\n"); return 0; }
    fprintf(stderr, "test_internal: %d failure(s)\n", failures);
    return 1;
}
