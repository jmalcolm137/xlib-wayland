/* test_xim.c — exercise the XIM -> zwp_text_input_v3 bridge.
 *
 * The headless compositor's text-input server (see tools/headless-compositor.c)
 * sends a canned preedit and then a canned commit once an input context is
 * focused.
 *
 * Default mode uses XIMPreeditCallbacks: the preedit arrives through the
 * XNPreedit* callbacks and the commit is read back with XmbLookupString.  The
 * runner asserts the markers.
 *
 * "position" mode uses XIMPreeditPosition: the shim draws the composing string
 * itself at the spot, so this client registers no callbacks, stays alive until
 * the compositor captures a frame, and the runner checks the pixels.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static XIC ic;
static int got_start, got_draw, got_done, got_caret;

static void preedit_start(XIM im, XPointer cd, XPointer call)
{
    (void)im; (void)cd; (void)call;
    got_start = 1;
    printf("XIM:PREEDIT-START\n");
    fflush(stdout);
}

static void preedit_done(XIM im, XPointer cd, XPointer call)
{
    (void)im; (void)cd; (void)call;
    got_done = 1;
    printf("XIM:PREEDIT-DONE\n");
    fflush(stdout);
}

static void preedit_draw(XIM im, XPointer cd, XPointer call)
{
    (void)im; (void)cd;
    XIMPreeditDrawCallbackStruct *d = (XIMPreeditDrawCallbackStruct *)call;
    if (d->text && d->text->string.multi_byte)
        printf("XIM:PREEDIT-DRAW %s\n", d->text->string.multi_byte);
    else
        printf("XIM:PREEDIT-DRAW -\n");
    fflush(stdout);
    got_draw = 1;
}

static void preedit_caret(XIM im, XPointer cd, XPointer call)
{
    (void)im; (void)cd; (void)call;
    got_caret = 1;
}

static Window make_window(Display *d)
{
    int scr = DefaultScreen(d);
    Window w = XCreateSimpleWindow(d, RootWindow(d, scr), 0, 0, 200, 80, 0,
                                   BlackPixel(d, scr), WhitePixel(d, scr));
    XSelectInput(d, w, KeyPressMask | ExposureMask | StructureNotifyMask);
    XMapWindow(d, w);
    /* Paint once so the toplevel has a buffer and the compositor considers it
     * mapped before we offer it as a text-input surface. */
    XClearWindow(d, w);
    XSync(d, False);
    return w;
}

static int run_position(Display *d)
{
    Window w = make_window(d);
    XIM im = XOpenIM(d, NULL, NULL, NULL);
    if (!im) { printf("XIM:NO-IM\n"); return 1; }

    /* The spot is in the client window; the overlay lands there. */
    XPoint spot = { 20, 40 };
    XVaNestedList pa = XVaCreateNestedList(0, XNSpotLocation, &spot, NULL);
    ic = XCreateIC(im, XNInputStyle, XIMPreeditPosition | XIMStatusNothing,
                   XNClientWindow, w, XNFocusWindow, w,
                   XNPreeditAttributes, pa, NULL);
    XFree(pa);
    if (!ic) { printf("XIM:NO-IC\n"); return 1; }

    XSetICFocus(ic);
    XFlush(d);
    printf("XIM:POSITION-HELD\n");
    fflush(stdout);

    /* Stay alive past the compositor's capture (HC_TIMEOUT seconds). */
    for (int i = 0; i < 400; i++) {
        while (XPending(d)) { XEvent ev; XNextEvent(d, &ev); }
        usleep(20000);
    }
    return 0;
}

static int run_callbacks(Display *d)
{
    Window w = make_window(d);
    XIM im = XOpenIM(d, NULL, NULL, NULL);
    if (!im) { printf("XIM:NO-IM\n"); return 1; }

    XIMCallback cb_start = { (XPointer)NULL, (XIMProc)preedit_start };
    XIMCallback cb_done  = { (XPointer)NULL, (XIMProc)preedit_done };
    XIMCallback cb_draw  = { (XPointer)NULL, (XIMProc)preedit_draw };
    XIMCallback cb_caret = { (XPointer)NULL, (XIMProc)preedit_caret };
    XVaNestedList pa = XVaCreateNestedList(0,
        XNPreeditStartCallback, &cb_start,
        XNPreeditDrawCallback,  &cb_draw,
        XNPreeditCaretCallback, &cb_caret,
        XNPreeditDoneCallback,  &cb_done,
        NULL);

    ic = XCreateIC(im,
                   XNInputStyle, XIMPreeditCallbacks | XIMStatusCallbacks,
                   XNClientWindow, w,
                   XNFocusWindow, w,
                   XNPreeditAttributes, pa,
                   NULL);
    XFree(pa);
    if (!ic) { printf("XIM:NO-IC\n"); return 1; }

    XSetICFocus(ic);
    XFlush(d);

    int commits = 0;
    for (int i = 0; i < 500; i++) {
        while (XPending(d)) {
            XEvent ev;
            XNextEvent(d, &ev);
            if (ev.type != KeyPress) continue;
            if (XFilterEvent(&ev, w)) continue;
            char buf[256];
            KeySym ks;
            Status st;
            int n = XmbLookupString(ic, &ev.xkey, buf, sizeof buf, &ks, &st);
            if (n > 0 && (st == XLookupChars || st == XLookupBoth)) {
                buf[n] = 0;
                printf("XIM:COMMIT %s\n", buf);
                fflush(stdout);
                commits++;
            }
        }
        if (got_start && got_draw && got_done && commits > 0) break;
        usleep(20000);
    }

    printf("XIM:RESULT start=%d draw=%d done=%d caret=%d commits=%d\n",
           got_start, got_draw, got_done, got_caret, commits);
    return (got_start && got_draw && got_done && commits > 0) ? 0 : 1;
}

int main(int argc, char **argv)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) { printf("XIM:NO-DISPLAY\n"); return 1; }
    if (argc > 1 && strcmp(argv[1], "position") == 0)
        return run_position(d);
    return run_callbacks(d);
}
