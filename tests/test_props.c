/* test_props.c — format-32 property semantics and mask-based event selection.
 *
 * Runs under the headless compositor (it needs a Display).  Both checks cover
 * bugs that froze Motif's clipboard: Cut used to spin forever and then fail
 * with "clipboard locked".
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* Motif's clipboard lock record (CutPaste.c).  A window followed by a long. */
typedef struct { Window windowId; long lockLevel; } LockRec;

int main(void)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) { printf("test_props: no display\n"); return 1; }
    Window root = DefaultRootWindow(d);
    Atom prop = XInternAtom(d, "TEST_LONG_PROP", False);
    Atom at; int af; unsigned long ni, ba; unsigned char *data = NULL;

    /* 1. Format-32 property data is a long array: one long per element.  The
     *    protocol carries only the low 32 bits, but the client-side buffer is
     *    longs and the returned buffer must be longs again. */
    long in[2] = { 0x11223344L, 0x4011c5L };
    XChangeProperty(d, root, prop, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)in, 2);
    at = None; af = 0; ni = 0; ba = 0; data = NULL;
    CHECK(XGetWindowProperty(d, root, prop, 0, 100, False, AnyPropertyType,
                             &at, &af, &ni, &ba, &data) == Success);
    CHECK(af == 32 && ni == 2);
    CHECK(data && ((long *)data)[0] == 0x11223344L);
    CHECK(data && ((long *)data)[1] == 0x4011c5L);
    if (data) XFree(data);

    /* 2. The Motif lock record shape: {Window, long}.  Packing raw bytes
     *    stored [windowId_low, windowId_high], so lockLevel read back as 0 and
     *    the locked window looked like window 0 -- ClipboardLock() then kept
     *    reporting the clipboard locked and Cut copied nothing. */
    LockRec rec;
    rec.windowId = 0x4011c5;
    rec.lockLevel = 1;
    XChangeProperty(d, root, prop, XA_INTEGER, 32, PropModeReplace,
                    (unsigned char *)&rec, 2);
    at = None; af = 0; ni = 0; ba = 0; data = NULL;
    CHECK(XGetWindowProperty(d, root, prop, 0, 100, False, AnyPropertyType,
                             &at, &af, &ni, &ba, &data) == Success);
    CHECK(data && ni == 2);
    if (data) {
        LockRec out;
        memcpy(&out, data, sizeof out);
        CHECK(out.windowId == 0x4011c5);
        CHECK(out.lockLevel == 1);
        XFree(data);
    }

    /* 3. XWindowEvent() must match a PropertyNotify against PropertyChangeMask.
     *    Xlib maps event types to masks through a table -- PropertyNotify is
     *    type 28 while PropertyChangeMask is bit 22 -- and shifting the type
     *    matched neither.  Motif's ClipboardGetCurrentTime() appends an empty
     *    property to the root and waits for exactly this event, so the mismatch
     *    spun at 100% CPU and Cut never returned. */
    XSelectInput(d, root, PropertyChangeMask);
    Atom timeAtom = XInternAtom(d, "TEST_CLIP_TIME", False);
    XChangeProperty(d, root, timeAtom, timeAtom, 8, PropModeAppend, NULL, 0);
    XFlush(d);
    XEvent ev;
    memset(&ev, 0, sizeof ev);
    CHECK(XWindowEvent(d, root, PropertyChangeMask, &ev) == 1);
    CHECK(ev.type == PropertyNotify);
    CHECK(ev.xproperty.window == root);

    /* 4. A SelectionClear must carry a rising, non-zero serial.  libXt only
     *    stops answering a selection itself (and forwards the paste to the real
     *    owner) once it sees a serial at least as large as the one it recorded
     *    when it took the selection.  Events carrying serial 0 were ignored, so
     *    Motif kept serving its own stale clipboard and the Wayland clipboard
     *    never reached it. */
    {
        Atom clipsel = XInternAtom(d, "CLIPBOARD", False);
        Window w1 = XCreateSimpleWindow(d, root, 0, 0, 10, 10, 0, 0, 0);
        Window w2 = XCreateSimpleWindow(d, root, 0, 0, 10, 10, 0, 0, 0);
        XEvent ev;
        memset(&ev, 0, sizeof ev);

        XSetSelectionOwner(d, clipsel, w1, CurrentTime);
        XSetSelectionOwner(d, clipsel, w2, CurrentTime);   /* clears w1 */
        CHECK(XCheckTypedWindowEvent(d, w1, SelectionClear, &ev) == 1);
        CHECK(ev.xselectionclear.serial > 0);
        unsigned long s1 = ev.xselectionclear.serial;

        XSetSelectionOwner(d, clipsel, w1, CurrentTime);   /* clears w2 */
        CHECK(XCheckTypedWindowEvent(d, w2, SelectionClear, &ev) == 1);
        CHECK(ev.xselectionclear.serial > s1);

        XSetSelectionOwner(d, clipsel, None, CurrentTime);
        XDestroyWindow(d, w1);
        XDestroyWindow(d, w2);
    }

    /* 5. Resizing must expose the window.  A real server discards the contents
     *    on a resize (ForgetGravity is the default, and what Motif's widgets
     *    ask for) and reports the whole window.  Motif resizes its scroll bar
     *    with XMoveResizeWindow and repaints it only in response to that
     *    Expose, so without it the bar stayed blank, and reporting only the
     *    newly uncovered strip left parts of the window unpainted until some
     *    unrelated event forced a redraw. */
    {
        Window p = XCreateSimpleWindow(d, root, 0, 0, 200, 200, 0, 0, 0);
        Window c = XCreateSimpleWindow(d, p, 0, 0, 40, 40, 0, 0, 0);
        XEvent ev;
        XSelectInput(d, p, ExposureMask);
        XSelectInput(d, c, ExposureMask);
        XMapWindow(d, p);
        XMapWindow(d, c);
        XSync(d, False);
        while (XCheckTypedWindowEvent(d, c, Expose, &ev)) ;
        while (XCheckTypedWindowEvent(d, p, Expose, &ev)) ;

        XMoveResizeWindow(d, c, 0, 0, 90, 70);
        XSync(d, False);
        int covered = 0;
        while (XCheckTypedWindowEvent(d, c, Expose, &ev))
            covered += (int)ev.xexpose.width * (int)ev.xexpose.height;
        CHECK(covered == 90 * 70);

        /* A move on its own keeps the contents: no Expose. */
        XMoveWindow(d, c, 10, 10);
        XSync(d, False);
        CHECK(XCheckTypedWindowEvent(d, c, Expose, &ev) == 0);

        XDestroyWindow(d, p);
    }

    XCloseDisplay(d);
    if (failures == 0) { printf("test_props: all checks passed\n"); return 0; }
    printf("test_props: %d failure(s)\n", failures);
    return 1;
}
