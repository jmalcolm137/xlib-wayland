/* clip_x.c — an X client used to test the X<->Wayland clipboard bridge.
 *
 *   clip_x own <text>              take the CLIPBOARD selection, serve <text>
 *   clip_x own-then-convert <text> take CLIPBOARD, then (after a Wayland client
 *                                  has taken the compositor clipboard) paste
 *   clip_x convert                 ask for CLIPBOARD as STRING, print GOT:<data>
 *
 * Deliberately plain Xlib: it exercises exactly the selection protocol Motif
 * uses, without pulling in the whole toolkit.
 */
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Atom clipboard_atom(Display *d)
{
    return XInternAtom(d, "CLIPBOARD", False);
}

/* Wait up to `ms` for an event of `type` on `w`.  Selection events have no
 * event-mask bit, so they must be looked up by type, exactly as Xt does with
 * XNextEvent. */
static int wait_typed(Display *d, Window w, int type, XEvent *ev, int ms)
{
    for (int i = 0; i < ms / 20; i++) {
        if (XCheckTypedWindowEvent(d, w, type, ev)) return 1;
        XFlush(d);
        usleep(20 * 1000);
    }
    return XCheckTypedWindowEvent(d, w, type, ev);
}

/* The MIME target this client advertises/serves (own-mime); None for text. */
static Atom serve_target_atom;

/* Answer one SelectionRequest exactly the way an application would. */
static void serve(Display *d, XSelectionRequestEvent *r, const char *text)
{
    Atom prop = r->property == None ? r->target : r->property;
    Atom targets = XInternAtom(d, "TARGETS", False);
    if (r->target == targets) {
        Atom list[3];
        int n = 0;
        list[n++] = targets;
        list[n++] = XA_STRING;
        if (serve_target_atom) list[n++] = serve_target_atom;
        XChangeProperty(d, r->requestor, prop, XA_ATOM, 32, PropModeReplace,
                        (const unsigned char *)list, n);
    } else {
        /* Write with the requested target as the property type; a non-text
         * target (image/png, text/uri-list, ...) arrives the same way. */
        XChangeProperty(d, r->requestor, prop, r->target, 8, PropModeReplace,
                        (const unsigned char *)text, (int)strlen(text));
    }
    XSelectionEvent se;
    memset(&se, 0, sizeof se);
    se.type = SelectionNotify;
    se.display = d;
    se.requestor = r->requestor;
    se.selection = r->selection;
    se.target = r->target;
    se.property = prop;
    se.time = r->time;
    XSendEvent(d, r->requestor, False, 0, (XEvent *)&se);
    XFlush(d);
}

/* Pump events and serve our selection for up to `ms` milliseconds. */
static void pump_serving(Display *d, Window w, const char *text, int ms)
{
    for (int i = 0; i < ms / 20; i++) {
        XEvent ev;
        while (wait_typed(d, w, SelectionRequest, &ev, 0))
            serve(d, &ev.xselectionrequest, text);
        /* Also dispense anything else (the shim's Wayland translation happens
         * inside these calls). */
        while (XPending(d)) XNextEvent(d, &ev);
        XFlush(d);
        usleep(20 * 1000);
    }
}

/* Ask for `target` and print GOT:<data>. */
static int do_convert_target(Display *d, Window w, Atom clip, Atom target)
{
    /* Let the shim receive the compositor's clipboard offer.  A real
     * application has been dispatching events for a long time before the user
     * pastes; this test converts immediately after opening. */
    for (int i = 0; i < 50; i++) { XPending(d); XFlush(d); usleep(20 * 1000); }

    Atom prop = XInternAtom(d, "CLIP_DATA", False);
    XConvertSelection(d, clip, target, prop, w, CurrentTime);
    XFlush(d);

    XEvent ev;
    if (!wait_typed(d, w, SelectionNotify, &ev, 4000)) {
        printf("GOT:<no-notify>\n");
        return 1;
    }
    if (ev.xselection.property == None) {
        printf("GOT:<none>\n");
        return 1;
    }
    Atom type; int format; unsigned long n, ba;
    unsigned char *data = NULL;
    XGetWindowProperty(d, w, ev.xselection.property, 0, 0x7fffffff, True,
                       AnyPropertyType, &type, &format, &n, &ba, &data);
    if (!data) { printf("GOT:<no-data>\n"); return 1; }
    printf("GOT:%.*s\n", (int)n, data);
    XFree(data);
    return 0;
}

static int do_convert(Display *d, Window w, Atom clip)
{
    return do_convert_target(d, w, clip, XA_STRING);
}

int main(int argc, char **argv)
{
    setbuf(stdout, NULL);
    if (argc < 2) {
        fprintf(stderr, "usage: clip_x own|own-then-convert <text>|convert\n");
        return 2;
    }

    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "clip_x: cannot open display\n"); return 2; }
    Window root = DefaultRootWindow(d);
    Window win = XCreateSimpleWindow(d, root, 0, 0, 1, 1, 0, 0, 0);
    Atom clip = clipboard_atom(d);
    const char *text = argc > 2 ? argv[2] : "x-text";

    if (strcmp(argv[1], "own") == 0) {
        XSetSelectionOwner(d, clip, win, CurrentTime);
        XFlush(d);
        if (XGetSelectionOwner(d, clip) != win) {
            fprintf(stderr, "clip_x: could not take CLIPBOARD\n");
            return 1;
        }
        printf("OWNING\n");
        pump_serving(d, win, text, 6000);
        return 0;
    }

    if (strcmp(argv[1], "own-then-convert") == 0) {
        /* Reproduce the real sequence: an application takes CLIPBOARD (NEdit
         * after a cut), then a Wayland client copies, then the user pastes. */
        XSetSelectionOwner(d, clip, win, CurrentTime);
        XFlush(d);
        if (XGetSelectionOwner(d, clip) != win) {
            fprintf(stderr, "clip_x: could not take CLIPBOARD\n");
            return 1;
        }
        printf("OWNING\n");
        pump_serving(d, win, text, 2500);
        return do_convert(d, win, clip);
    }

    if (strcmp(argv[1], "own-mime") == 0) {
        if (argc < 4) {
            fprintf(stderr, "usage: clip_x own-mime <mime> <payload>\n");
            return 2;
        }
        serve_target_atom = XInternAtom(d, argv[2], False);
        XSetSelectionOwner(d, clip, win, CurrentTime);
        XFlush(d);
        if (XGetSelectionOwner(d, clip) != win) {
            fprintf(stderr, "clip_x: could not take CLIPBOARD\n");
            return 1;
        }
        printf("OWNING\n");
        pump_serving(d, win, argv[3], 6000);
        return 0;
    }

    if (strcmp(argv[1], "convert") == 0)
        return do_convert(d, win, clip);

    if (strcmp(argv[1], "convert-target") == 0) {
        if (argc < 3) return 2;
        return do_convert_target(d, win, clip, XInternAtom(d, argv[2], False));
    }

    if (strcmp(argv[1], "primary-own-mime") == 0) {
        if (argc < 4) {
            fprintf(stderr, "usage: clip_x primary-own-mime <mime> <payload>\n");
            return 2;
        }
        serve_target_atom = XInternAtom(d, argv[2], False);
        XSetSelectionOwner(d, XA_PRIMARY, win, CurrentTime);
        XFlush(d);
        if (XGetSelectionOwner(d, XA_PRIMARY) != win) {
            fprintf(stderr, "clip_x: could not take PRIMARY\n");
            return 1;
        }
        printf("OWNING\n");
        pump_serving(d, win, argv[3], 6000);
        return 0;
    }

    if (strcmp(argv[1], "primary-convert") == 0) {
        Atom t = argc > 2 ? XInternAtom(d, argv[2], False) : XA_STRING;
        return do_convert_target(d, win, XA_PRIMARY, t);
    }

    fprintf(stderr, "clip_x: unknown mode %s\n", argv[1]);
    return 2;
}
