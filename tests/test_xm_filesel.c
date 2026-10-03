/* test_xm_filesel.c — reproduce the NEdit "File -> Open" dialog behaviour.
 *
 * NEdit's Open dialog is a Motif XmFileSelectionBox with the OK/Cancel
 * activate callbacks replaced (file.c).  The reported symptom is that
 * clicking Cancel does nothing and the application wedges.
 *
 * The dialog is a popup shell, so it becomes its own xdg_popup -- a different
 * role from the override-redirect menu popup, and the interesting case here.
 * The test prints each button's real geometry so an input script can aim at
 * it, reports every activate callback, and keeps a heartbeat timer running so
 * a wedged main loop is visible as missing ticks.
 */
#include <Xm/Xm.h>
#include <Xm/FileSB.h>
#include <Xm/DialogS.h>
#include <Xm/PushB.h>
#include <X11/Intrinsic.h>
#include <X11/Shell.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int ok_hits, cancel_hits, ticks;
static Widget toplevel, fsb;
static void repost(XtPointer closure, XtIntervalId *id);

static void on_ok(Widget w, XtPointer client, XtPointer cd)
{
    (void)w; (void)client; (void)cd;
    ok_hits++;
    printf("FSB:OK\n");
    fflush(stdout);
}

static void on_cancel(Widget w, XtPointer client, XtPointer cd)
{
    (void)w; (void)client; (void)cd;
    cancel_hits++;
    printf("FSB:CANCEL\n");
    fflush(stdout);
    /* Motif's own cancel-unmap behaviour, which is what wedges the app. */
    XtUnmanageChild(fsb);
    printf("FSB:UNMANAGED\n");
    fflush(stdout);
}

static void tick(XtPointer closure, XtIntervalId *id)
{
    (void)id;
    ticks++;
    if (ticks % 10 == 0) {
        printf("TICK:%d ok=%d cancel=%d\n", ticks, ok_hits, cancel_hits);
        fflush(stdout);
    }
    /* If the main loop is alive this keeps firing; a wedged app stops. */
    XtAppAddTimeOut((XtAppContext)closure, 200, tick, closure);
}

/* Print real widget geometry once the dialog has settled, so the input script
 * can aim at the buttons without guessing from a screenshot. */
static void report_geometry(XtPointer c, XtIntervalId *id)
{
    static int reported;
    static const char *const names[] = { "OK", "CANCEL", "FILTERTEXT", "HELP" };
    Widget ws[4];
    (void)c; (void)id;

    if (reported++)
        return;

    ws[0] = XmFileSelectionBoxGetChild(fsb, XmDIALOG_OK_BUTTON);
    ws[1] = XmFileSelectionBoxGetChild(fsb, XmDIALOG_CANCEL_BUTTON);
    ws[2] = XmFileSelectionBoxGetChild(fsb, XmDIALOG_FILTER_TEXT);
    ws[3] = XmFileSelectionBoxGetChild(fsb, XmDIALOG_HELP_BUTTON);

    for (int i = 0; i < 4; i++) {
        Dimension w = 0, h = 0;
        Position x = 0, y = 0;
        if (!ws[i]) {
            printf("GEO:%s missing\n", names[i]);
            continue;
        }
        XtVaGetValues(ws[i], XtNwidth, &w, XtNheight, &h,
                      XtNx, &x, XtNy, &y, NULL);
        printf("GEO:%s w=%u h=%u rel=%d,%d\n", names[i], w, h, x, y);
    }

    {
        Window win = XtWindow(fsb);
        Window root = 0, child = 0;
        int rx = 0, ry = 0, wx = 0, wy = 0;
        unsigned int ww = 0, wh = 0, bw = 0, dep = 0;
        if (win) {
            Display *d = XtDisplay(fsb);
            XTranslateCoordinates(d, win,
                                  RootWindowOfScreen(XtScreen(fsb)),
                                  0, 0, &rx, &ry, &child);
            XGetGeometry(d, win, &root, &wx, &wy, &ww, &wh, &bw, &dep);
            printf("GEO:DIALOG root=%d,%d size=%ux%u\n", rx, ry, ww, wh);
        } else {
            printf("GEO:DIALOG no-window\n");
        }
    }
    fflush(stdout);
}

/* NEdit's getfiles.c shape: FileSB-level ok/cancel callbacks that only flip a
 * flag, plus a nested XtAppProcessEvent loop that spins on it. */
static void on_fsb_ok(Widget w, XtPointer client_data, XtPointer cd)
{
    Boolean *done = (Boolean *)client_data;
    (void)w; (void)cd;
    ok_hits++;
    *done = True;
    printf("FSB:OK\n");
    fflush(stdout);
}

static void on_fsb_cancel(Widget w, XtPointer client_data, XtPointer cd)
{
    Boolean *done = (Boolean *)client_data;
    (void)w; (void)cd;
    cancel_hits++;
    *done = True;
    printf("FSB:CANCEL\n");
    fflush(stdout);
}

/* Re-post the dialog after a cancel, the way File -> Open does a second time.
 * The interesting case is not the first posting but the *repeat*: the popup
 * surface has been unmapped, so the next map needs a fresh commit, configure
 * and ack before a buffer can be attached. */
static void repost(XtPointer closure, XtIntervalId *id)
{
    XtAppContext app = (XtAppContext)closure;
    (void)id;
    printf("FSB:REPOST\n");
    fflush(stdout);
    XtManageChild(fsb);
    printf("FSB:REPOSTED ticks=%d cancel=%d\n", ticks, cancel_hits);
    fflush(stdout);
    XtAppAddTimeOut(app, 700, repost, closure);
}

int main(int argc, char **argv)
{
    XtAppContext app;
    Boolean done_with_dialog = False;
    int spins = 0;

    toplevel = XtAppInitialize(&app, "MWFsb", NULL, 0, &argc, argv,
                               NULL, NULL, 0);

    /* NEdit builds its file dialog with XmCreateFileSelectionDialog and
     * XmDIALOG_FULL_APPLICATION_MODAL (util/misc.c:CreateFileSelectionDialog
     * called from util/getfiles.c).  That creates an XmDialogShell, not a
     * menu/override-redirect popup shell, which is exactly the difference
     * that matters for input routing. */
    {
        Arg args[4];
        Cardinal n = 0;
        XtSetArg(args[n], XmNdialogStyle, XmDIALOG_FULL_APPLICATION_MODAL); n++;
        XtSetArg(args[n], XmNdialogTitle,
                 XmStringCreateLocalized("Open File")); n++;
        fsb = XmCreateFileSelectionDialog(toplevel, "fileSB", args, n);
    }
    /* NEdit's getfiles.c registers these on the FileSB itself, not on the
     * button, and then busy-waits in a nested XtAppProcessEvent loop. */
    XtAddCallback(fsb, XmNokCallback, (XtCallbackProc)on_fsb_ok,
                  (XtPointer)&done_with_dialog);
    XtAddCallback(fsb, XmNcancelCallback, (XtCallbackProc)on_fsb_cancel,
                  (XtPointer)&done_with_dialog);

    XtRealizeWidget(toplevel);
    XtManageChild(fsb);
    printf("FSB:REALIZED\n");
    fflush(stdout);

    XtAppAddTimeOut(app, 600, report_geometry, (XtPointer)app);

    /* Exactly NEdit's loop shape (getfiles.c:HandleCustomNewFileSB).  A
     * heartbeat timer cannot run inside a non-blocking XtAppProcessEvent
     * loop, so instead count iterations: a healthy run returns as soon as the
     * cancel callback fires, a broken one spins forever. */
    while (!done_with_dialog) {
        XtAppProcessEvent(XtWidgetToApplicationContext(fsb), XtIMAll);
        if (++spins % 2000000 == 0) {
            printf("SPIN:%d\n", spins);
            fflush(stdout);
        }
    }
    printf("FSB:LOOP-EXITED spins=%d ok=%d cancel=%d\n", spins, ok_hits, cancel_hits);
    fflush(stdout);

    XtDestroyWidget(fsb);
    printf("FSB:DONE\n");
    return cancel_hits > 0 ? 0 : 1;
}
