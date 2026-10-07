/* test_xm_menubar.c — Motif menu bar with two cascades.
 *
 * Reproduces the "use one menu, then open another" path at the Motif level
 * rather than the raw Xlib level: a menu bar with File and Edit pulldowns,
 * where an item has to be activated from each cascade in turn.  The
 * interesting part is the second cascade: posting it must anchor the new
 * popup to the application toplevel, not to the menu shell of the previous
 * (now dismissed) menu.
 *
 * Prints the cascade geometry so an input script can aim at the menu bar, and
 * prints MENU:<name> for every activation.  Exits 0 once both cascades have
 * been used; MENU:TIMEOUT and exit 1 on the deadline.
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

#define NCASCADE 2

static XtAppContext app;
static int selected;
static Widget cascades[NCASCADE];
static Widget work_widget;
static Widget menubar;
static const char *cascade_names[NCASCADE] = { "File", "Edit" };
static const char *item_names[NCASCADE][2] = {
    { "Alpha", "Beta" },
    { "Gamma", "Delta" },
};

static void activate(Widget w, XtPointer client, XtPointer call)
{
    (void)w; (void)call;
    printf("MENU:%s\n", (char *)client);
    fflush(stdout);
    if (++selected >= NCASCADE)
        XtAppSetExitFlag(app);
}

static void timeout_cb(XtPointer c, XtIntervalId *id)
{
    (void)c; (void)id;
    printf("MENU:TIMEOUT selected=%d\n", selected);
    fflush(stdout);
    XtAppSetExitFlag(app);
}

/* Report the sizes the client is actually working with, so a compositor-driven
 * resize can be checked end to end: the shell must receive a ConfigureNotify
 * and the managed children must be re-laid out to the new size. */
static void report_sizes(XtPointer c, XtIntervalId *id)
{
    Dimension ww = 0, wh = 0;
    Window wwin = 0, troot = 0;
    int wx = 0, wy = 0;
    unsigned int xw = 0, xh = 0, bw = 0, dep = 0;
    static int menubar_reported;
    (void)id;
    if (work_widget) {
        XtVaGetValues(work_widget, XtNwidth, &ww, XtNheight, &wh, NULL);
        wwin = XtWindow(work_widget);
        if (wwin)
            XGetGeometry(XtDisplay(work_widget), wwin, &troot, &wx, &wy,
                         &xw, &xh, &bw, &dep);
    }
    /* Report the menu bar height too: the work area is the toplevel less the
     * menu bar, and the bar's height follows the font, so a test cannot assert
     * an absolute work-area height without going stale when fonts change. */
    if (!menubar_reported && menubar) {
        Dimension mh = 0;
        XtVaGetValues(menubar, XtNheight, &mh, NULL);
        printf("SIZE:menubar h=%u\n", mh);
        fflush(stdout);
        menubar_reported = 1;
    }
    {
        static unsigned int pw, ph, pxw, pxh;
        if (ww != pw || wh != ph || xw != pxw || xh != pxh) {
            printf("SIZE:workarea widget=%ux%u xwindow=%ux%u\n", ww, wh, xw, xh);
            fflush(stdout);
            pw = ww; ph = wh; pxw = xw; pxh = xh;
        }
    }
    XtAppAddTimeOut(app, 500, report_sizes, c);
}

static void report_geometry(XtPointer c, XtIntervalId *id)
{
    static int reported;
    (void)c; (void)id;
    if (reported++)
        return;
    for (int i = 0; i < NCASCADE; i++) {
        Dimension w = 0, h = 0;
        Position x = 0, y = 0;
        XtVaGetValues(cascades[i], XtNwidth, &w, XtNheight, &h,
                      XtNx, &x, XtNy, &y, NULL);
        /* Menu-bar-relative position; the shell adds the titlebar. */
        printf("GEO:CASCADE%d %s w=%u h=%u rel=%d,%d\n",
               i, cascade_names[i], w, h, x, y);
    }
    fflush(stdout);
}

int main(int argc, char **argv)
{
    Widget top = XtAppInitialize(&app, "MwMenuBar", NULL, 0, &argc, argv,
                                 NULL, NULL, 0);
    Arg a[8];
    int n = 0;
    XtSetArg(a[n], XmNwidth, 320); n++;
    XtSetArg(a[n], XmNheight, 220); n++;
    Widget mainw = XmCreateMainWindow(top, "main", a, n);
    XtManageChild(mainw);

    menubar = XmCreateMenuBar(mainw, "menubar", NULL, 0);
    XtManageChild(menubar);

    for (int i = 0; i < NCASCADE; i++) {
        char name[32];
        snprintf(name, sizeof name, "%sPulldown", cascade_names[i]);
        Widget pulldown = XmCreatePulldownMenu(menubar, name, NULL, 0);

        snprintf(name, sizeof name, "%sCascade", cascade_names[i]);
        cascades[i] = XmCreateCascadeButton(menubar, name, NULL, 0);
        XtVaSetValues(cascades[i],
                      XmNlabelString,
                      XmStringCreateLocalized((char *)cascade_names[i]),
                      XmNsubMenuId, pulldown, NULL);
        XtManageChild(cascades[i]);

        for (int j = 0; j < 2; j++) {
            Widget b = XmCreatePushButton(pulldown,
                                          (char *)item_names[i][j], NULL, 0);
            XtVaSetValues(b, XmNlabelString,
                          XmStringCreateLocalized((char *)item_names[i][j]),
                          NULL);
            XtAddCallback(b, XmNactivateCallback, activate,
                          (XtPointer)item_names[i][j]);
            XtManageChild(b);
        }
    }

    work_widget = XmCreateDrawingArea(mainw, "work", NULL, 0);
    XtManageChild(work_widget);
    XtRealizeWidget(top);

    XtAppAddTimeOut(app, 1000, report_geometry, NULL);
    XtAppAddTimeOut(app, 1500, report_sizes, NULL);
    XtAppAddTimeOut(app, 20000, timeout_cb, NULL);
    XtAppMainLoop(app);
    return selected >= NCASCADE ? 0 : 1;
}
