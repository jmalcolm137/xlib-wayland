/* test_xm_flow.c — NEdit's File menu pattern, reproduced as closely as
 * possible.
 *
 * Reported symptom: after a file has been loaded through the File menu, the
 * menus stop responding.  NEdit's getfiles.c does this:
 *
 *   - the File -> Open item's callback creates an XmFileSelectionBox
 *     (XmCreateFileSelectionDialog, XmDIALOG_FULL_APPLICATION_MODAL) and then
 *     busy-waits in a nested event loop:
 *
 *         while (!done_with_dialog)
 *             XtAppProcessEvent(XtWidgetToApplicationContext(newFileSB), XtIMAll);
 *
 *   - when a file is chosen it destroys the whole dialog shell:
 *
 *         XtDestroyWidget(XtParent(existFileSB));   (util/getfiles.c:461)
 *
 *   - and then loads the file and raises the document window.
 *
 * This test walks exactly that path, then uses the menu bar again.  The last
 * step is the regression: a menu used after a dialog has been created, run in
 * a nested loop and destroyed must still work.
 */
#include <Xm/Xm.h>
#include <Xm/MainW.h>
#include <Xm/RowColumn.h>
#include <Xm/CascadeB.h>
#include <Xm/PushB.h>
#include <Xm/FileSB.h>
#include <Xm/DrawingA.h>
#include <X11/Intrinsic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static XtAppContext app;
static Widget toplevel, filecascade, work_widget;
static int step;

static void timeout_cb(XtPointer c, XtIntervalId *id)
{
    (void)c; (void)id;
    printf("MENU:TIMEOUT step=%d\n", step);
    fflush(stdout);
    XtAppSetExitFlag(app);
}

/* ---- the file dialog, created and run the way getfiles.c does ---------- */

struct fsb_state {
    Boolean done;
};

static void fsb_ok_or_cancel(Widget w, XtPointer client, XtPointer call)
{
    struct fsb_state *st = (struct fsb_state *)client;
    (void)w; (void)call;
    st->done = True;
}

static void on_open(Widget w, XtPointer client, XtPointer call)
{
    struct fsb_state st;
    Widget fsb;
    Arg dargs[4];
    Cardinal dn = 0;

    (void)w; (void)client; (void)call;
    printf("MENU:OPEN step=%d\n", step);
    fflush(stdout);
    step++;

    /* Built inside the callback, just like CreateFileSelectionDialog(). */
    XtSetArg(dargs[dn], XmNdialogStyle, XmDIALOG_FULL_APPLICATION_MODAL); dn++;
    XtSetArg(dargs[dn], XmNdialogTitle,
             XmStringCreateLocalized("Open File")); dn++;
    fsb = XmCreateFileSelectionDialog(toplevel, "fileSB", dargs, dn);
    st.done = False;
    XtAddCallback(fsb, XmNokCallback, (XtCallbackProc)fsb_ok_or_cancel,
                  (XtPointer)&st);
    XtAddCallback(fsb, XmNcancelCallback, (XtCallbackProc)fsb_ok_or_cancel,
                  (XtPointer)&st);
    /* NEdit manages the dialog (ManageDialogCenteredOnPointer) before it runs
     * the nested loop. */
    XtManageChild(fsb);
    printf("FSB:SHOWN step=%d\n", step);
    fflush(stdout);

    /* NEdit's nested event loop. */
    while (!st.done)
        XtAppProcessEvent(XtWidgetToApplicationContext(fsb), XtIMAll);

    /* ...and its teardown: destroy the whole dialog shell. */
    XtDestroyWidget(XtParent(fsb));
    printf("FSB:DONE step=%d\n", step);
    fflush(stdout);
}

static void on_quit(Widget w, XtPointer client, XtPointer call)
{
    (void)w; (void)client; (void)call;
    printf("MENU:QUIT step=%d\n", step);
    fflush(stdout);
    step++;
    XtAppSetExitFlag(app);
}

static void report_geometry(XtPointer c, XtIntervalId *id)
{
    Dimension w = 0, h = 0;
    Position x = 0, y = 0;
    (void)c; (void)id;
    XtVaGetValues(filecascade, XtNwidth, &w, XtNheight, &h,
                  XtNx, &x, XtNy, &y, NULL);
    printf("GEO:CASCADE w=%u h=%u rel=%d,%d\n", w, h, x, y);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    toplevel = XtAppInitialize(&app, "MwFlow", NULL, 0, &argc, argv,
                               NULL, NULL, 0);
    Arg a[8];
    int n = 0;
    XtSetArg(a[n], XmNwidth, 320); n++;
    XtSetArg(a[n], XmNheight, 220); n++;
    Widget mainw = XmCreateMainWindow(toplevel, "main", a, n);
    XtManageChild(mainw);

    Widget menubar = XmCreateMenuBar(mainw, "menubar", NULL, 0);
    XtManageChild(menubar);

    Widget filemenu = XmCreatePulldownMenu(menubar, "filePulldown", NULL, 0);
    filecascade = XmCreateCascadeButton(menubar, "fileCascade", NULL, 0);
    XtVaSetValues(filecascade,
                  XmNlabelString, XmStringCreateLocalized("File"),
                  XmNsubMenuId, filemenu, NULL);
    XtManageChild(filecascade);

    Widget open_item = XmCreatePushButton(filemenu, "Open", NULL, 0);
    XtVaSetValues(open_item, XmNlabelString, XmStringCreateLocalized("Open"), NULL);
    XtAddCallback(open_item, XmNactivateCallback, on_open, NULL);
    XtManageChild(open_item);

    Widget quit_item = XmCreatePushButton(filemenu, "Quit", NULL, 0);
    XtVaSetValues(quit_item, XmNlabelString, XmStringCreateLocalized("Quit"), NULL);
    XtAddCallback(quit_item, XmNactivateCallback, on_quit, NULL);
    XtManageChild(quit_item);

    work_widget = XmCreateDrawingArea(mainw, "work", NULL, 0);
    XtManageChild(work_widget);
    XtRealizeWidget(toplevel);

    /* Motif creates a 10x10 InputOnly override-redirect window at (-100,-100)
     * and uses it as its modal grab window.  It has no pixels at all: mapping
     * it must not put a window on screen and must not take the input focus,
     * or every click afterwards goes to the invisible helper (which is what
     * happened, making the application look frozen once a dialog had been
     * opened from a menu). */
    {
        Display *dd = XtDisplay(toplevel);
        Window helper = XCreateWindow(dd, DefaultRootWindow(dd),
                                      -100, -100, 10, 10, 0,
                                      CopyFromParent, InputOnly, CopyFromParent,
                                      0, NULL);
        XMapWindow(dd, helper);
        XSync(dd, False);
        printf("HELPER:InputOnly mapped\n");
        fflush(stdout);
    }

    XtAppAddTimeOut(app, 1000, report_geometry, NULL);
    XtAppAddTimeOut(app, 20000, timeout_cb, NULL);
    XtAppMainLoop(app);
    return step >= 2 ? 0 : 1;
}
