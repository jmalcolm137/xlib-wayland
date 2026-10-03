/* test_xt.c — a genuine X Toolkit Intrinsics program, built against the
 * upstream, unmodified libXt and our libX11.  This is the M3 milestone:
 * it proves Xt's realize / event-loop / drawing path works on the shim.
 */
#include <X11/Intrinsic.h>
#include <X11/Shell.h>
#include <X11/Core.h>
#include <X11/StringDefs.h>
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GC g_gc;
static int g_exposes;

static void draw(Widget w)
{
    Display *d = XtDisplay(w);
    Window win = XtWindow(w);
    Dimension W, H;
    XtVaGetValues(w, XtNwidth, &W, XtNheight, &H, NULL);

    XSetForeground(d, g_gc, WhitePixel(d, DefaultScreen(d)));
    XFillRectangle(d, win, g_gc, 0, 0, W, H);
    XSetForeground(d, g_gc, BlackPixel(d, DefaultScreen(d)));
    XDrawRectangle(d, win, g_gc, 8, 8, W - 16, H - 16);
    XSetForeground(d, g_gc, 0x008000);
    XFillRectangle(d, win, g_gc, 24, 24, 140, 60);
    const char *msg = "Xt Intrinsics on Motif/Wayland";
    XDrawString(d, win, g_gc, 24, 120, msg, (int)strlen(msg));
    XFlush(d);
}

static void event_handler(Widget w, XtPointer client, XEvent *e, Boolean *cont)
{
    (void)client; (void)cont;
    switch (e->type) {
    case Expose:
        g_exposes++;
        printf("Xt Expose %d,%d %ux%u\n", e->xexpose.x, e->xexpose.y,
               e->xexpose.width, e->xexpose.height);
        draw(w);
        break;
    case ConfigureNotify:
        printf("Xt ConfigureNotify %dx%d\n", e->xconfigure.width, e->xconfigure.height);
        break;
    case ButtonPress:
        printf("Xt ButtonPress button=%u\n", e->xbutton.button);
        break;
    case KeyPress:
        printf("Xt KeyPress keycode=%u\n", e->xkey.keycode);
        break;
    default:
        break;
    }
}

static void quit_cb(XtPointer client, XtIntervalId *id)
{
    (void)id;
    XtAppSetExitFlag((XtAppContext)client);
}

int main(int argc, char **argv)
{
    XtAppContext app;
    Widget toplevel = XtAppInitialize(&app, "MWTest", NULL, 0, &argc, argv,
                                      NULL, NULL, 0);
    XtVaSetValues(toplevel,
                  XtNwidth, (XtArgVal)500,
                  XtNheight, (XtArgVal)300,
                  XtNtitle, (XtArgVal)"Motif/Wayland Xt test",
                  NULL);
    XtAddEventHandler(toplevel,
                      ExposureMask | StructureNotifyMask | ButtonPressMask |
                      KeyPressMask,
                      False, event_handler, NULL);
    XtRealizeWidget(toplevel);

    g_gc = XCreateGC(XtDisplay(toplevel), XtWindow(toplevel), 0, NULL);
    printf("Xt realize OK: window=0x%lx display=%p\n",
           XtWindow(toplevel), (void *)XtDisplay(toplevel));

    XtAppAddTimeOut(app, 3000, quit_cb, (XtPointer)app);
    XtAppMainLoop(app);

    XFreeGC(XtDisplay(toplevel), g_gc);
    printf("Xt done: %d exposes\n", g_exposes);
    return 0;
}
