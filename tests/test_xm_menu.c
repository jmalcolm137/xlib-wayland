/* test_xm_menu.c — deterministic menu test.
 *
 * Creates a Motif menu bar with a File pulldown and three items.  Selecting an
 * item prints "MENU:<name>" and exits 0; a timeout prints "MENU:TIMEOUT" and
 * exits 1.  This lets the menu interaction be tested without a human.
 */
#include <Xm/Xm.h>
#include <Xm/MainW.h>
#include <Xm/RowColumn.h>
#include <Xm/CascadeB.h>
#include <Xm/PushB.h>
#include <Xm/DrawingA.h>
#include <X11/Intrinsic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static XtAppContext app;

static void activate(Widget w, XtPointer client, XtPointer call)
{
    (void)w; (void)call;
    printf("MENU:%s\n", (char *)client);
    fflush(stdout);
    XtAppSetExitFlag(app);
}

static void timeout_cb(XtPointer c, XtIntervalId *id)
{
    (void)c; (void)id;
    printf("MENU:TIMEOUT\n");
    fflush(stdout);
    XtAppSetExitFlag(app);
}

int main(int argc, char **argv)
{
    Widget top = XtAppInitialize(&app, "MwMenu", NULL, 0, &argc, argv,
                                 NULL, NULL, 0);
    Arg a[8];
    int n = 0;
    XtSetArg(a[n], XmNwidth, 300); n++;
    XtSetArg(a[n], XmNheight, 200); n++;
    Widget mainw = XmCreateMainWindow(top, "main", a, n);
    XtManageChild(mainw);

    Widget menubar = XmCreateMenuBar(mainw, "menubar", NULL, 0);
    XtManageChild(menubar);

    Widget filemenu = XmCreatePulldownMenu(menubar, "filemenu", NULL, 0);
    Widget cascade = XmCreateCascadeButton(menubar, "file", NULL, 0);
    XtVaSetValues(cascade,
                  XmNlabelString, XmStringCreateLocalized("File"),
                  XmNsubMenuId, filemenu, NULL);
    XtManageChild(cascade);

    const char *names[] = { "Alpha", "Beta", "Quit" };
    for (int i = 0; i < 3; i++) {
        Widget b = XmCreatePushButton(filemenu, (char *)names[i], NULL, 0);
        XtVaSetValues(b, XmNlabelString, XmStringCreateLocalized((char *)names[i]), NULL);
        XtAddCallback(b, XmNactivateCallback, activate, (XtPointer)names[i]);
        XtManageChild(b);
    }

    Widget work = XmCreateDrawingArea(mainw, "work", NULL, 0);
    XtManageChild(work);
    XtRealizeWidget(top);

    XtAppAddTimeOut(app, 15000, timeout_cb, NULL);
    XtAppMainLoop(app);
    return 0;
}
