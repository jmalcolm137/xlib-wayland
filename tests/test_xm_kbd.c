/* test_xm_kbd.c — keyboard input reaching widgets in a dialog.
 *
 * Reported symptom: text fields in dialogs (Find, Execute) do not accept
 * typing, while the main window's text area does.  This builds a main window
 * with a text area plus a dialog with a text field, so typing into each can be
 * compared:
 *
 *   1. click the main window's text area, type           -> main grows
 *   2. the dialog is opened (like Find)
 *   3. click the dialog's text field, type               -> dialog grows
 *
 * Prints "MAIN:<text>" / "DLG:<text>" whenever the contents change, and the
 * widget geometry up front so an input script can aim.  Exits 0 if both
 * received input.
 */
#include <Xm/Xm.h>
#include <Xm/MainW.h>
#include <Xm/DrawingA.h>
#include <Xm/Text.h>
#include <Xm/SelectioB.h>
#include <Xm/RowColumn.h>
#include <X11/Intrinsic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static XtAppContext app;
static Widget toplevel, main_text, dlg, dlg_text;
static int main_got, dlg_got, dlg_shown;

static void report(XtPointer c, XtIntervalId *id)
{
    static size_t pm, pd;
    char *m = XmTextGetString(main_text);
    char *s = dlg_text ? XmTextGetString(dlg_text) : NULL;
    size_t lm = m ? strlen(m) : 0, ls = s ? strlen(s) : 0;
    (void)c; (void)id;
    if (lm != pm && m) { printf("MAIN:%s\n", m); fflush(stdout); pm = lm; }
    if (ls != pd && s) { printf("DLG:%s\n", s); fflush(stdout); pd = ls; }
    if (lm > 0) main_got = 1;
    if (ls > 0) dlg_got = 1;
    XtFree(m); XtFree(s);
    XtAppAddTimeOut(app, 400, report, c);
}

static void show_dialog(XtPointer c, XtIntervalId *id)
{
    Dimension w = 0, h = 0;
    Position x = 0, y = 0;
    (void)c; (void)id;
    if (dlg_shown++) return;
    XtManageChild(dlg);
    XtVaGetValues(dlg_text, XtNwidth, &w, XtNheight, &h,
                  XtNx, &x, XtNy, &y, NULL);
    printf("GEO:DLGTEXT w=%u h=%u rel=%d,%d\n", w, h, x, y);
    XtVaGetValues(main_text, XtNwidth, &w, XtNheight, &h,
                  XtNx, &x, XtNy, &y, NULL);
    printf("GEO:MAINTEXT w=%u h=%u rel=%d,%d\n", w, h, x, y);
    fflush(stdout);
}

static void quit_cb(XtPointer c, XtIntervalId *id)
{
    (void)c; (void)id;
    printf("KBD:main=%d dlg=%d\n", main_got, dlg_got);
    fflush(stdout);
    XtAppSetExitFlag(app);
}

int main(int argc, char **argv)
{
    toplevel = XtAppInitialize(&app, "MwKbd", NULL, 0, &argc, argv,
                               NULL, NULL, 0);
    Arg a[8];
    int n = 0;
    XtSetArg(a[n], XmNwidth, 400); n++;
    XtSetArg(a[n], XmNheight, 300); n++;
    Widget mainw = XmCreateMainWindow(toplevel, "main", a, n);
    XtManageChild(mainw);

    main_text = XmCreateText(mainw, "maintext", NULL, 0);
    XtManageChild(main_text);

    XtVaSetValues(main_text, XmNy, (XtArgVal)60, XmNheight, (XtArgVal)60,
                  XmNwidth, (XtArgVal)380, NULL);

    dlg = XmCreatePromptDialog(toplevel, "findDlg", NULL, 0);
    /* Use the SelectionBox's own text field, like NEdit's Find dialog does. */
    dlg_text = XmSelectionBoxGetChild(dlg, XmDIALOG_TEXT);

    XtRealizeWidget(toplevel);

    XtAppAddTimeOut(app, 1200, show_dialog, NULL);
    XtAppAddTimeOut(app, 500, report, NULL);
    XtAppAddTimeOut(app, 12000, quit_cb, NULL);
    XtAppMainLoop(app);
    return (main_got && dlg_got) ? 0 : 1;
}
