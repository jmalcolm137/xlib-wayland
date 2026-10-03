/* test_xm_msg.c — a modal MessageDialog, like the one NEdit shows for
 * File -> Exit ("Save changes to ...?" with Save / Don't Save / Cancel).
 *
 * Reported symptom: the dialog appears but none of its buttons can be
 * clicked.  The buttons in a Motif MessageDialog are push button *gadgets*
 * (no X windows of their own), so they are dispatched through the dialog's
 * own window by Motif's gadget code.
 *
 * Prints the button geometry so an input script can aim, and prints
 * RESULT:<n> when a button fires.  Exits 0 once one does.
 */
#include <Xm/Xm.h>
#include <Xm/MessageB.h>
#include <Xm/PushB.h>
#include <X11/Intrinsic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static XtAppContext app;
static Widget dialog;
static int answered;

static void answer(Widget w, XtPointer client, XtPointer call)
{
    (void)w; (void)call;
    printf("RESULT:%ld\n", (long)client);
    fflush(stdout);
    answered = 1;
    XtDestroyWidget(XtParent(dialog));
}

static Window grab_win;

/* Mimic what Motif leaves behind after a menu is used: an active pointer grab
 * on the menu bar and the input focus parked there.  NEdit's dialogs are all
 * opened from a menu item, so they inherit exactly this state. */
static void arm_menu_state(XtPointer c, XtIntervalId *id)
{
    Display *d = XtDisplay((Widget)c);
    (void)id;
    grab_win = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 60, 24, 0, 0, 0);
    XMapWindow(d, grab_win);
    XSync(d, False);
    XGrabPointer(d, grab_win, True,
                 ButtonPressMask | ButtonReleaseMask |
                 EnterWindowMask | LeaveWindowMask,
                 GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
    XSetInputFocus(d, grab_win, RevertToParent, CurrentTime);
    XSync(d, False);
    printf("ARMED:grab+focus set on 0x%lx\n", (unsigned long)grab_win);
    fflush(stdout);
}

static void show(XtPointer c, XtIntervalId *id)
{
    static Widget ok, cancel, help;
    Dimension w, h;
    Position x, y;
    (void)c; (void)id;
    /* Position the dialog off the origin, the way NEdit does (it centres
     * dialogs on the pointer).  Motif's _XmGetPointVisibility() then compares
     * the event's *root* coordinates against the widget's translated
     * position, so a client that reports top-level-relative root coordinates
     * sees every click land "outside" the button and never activates it. */
    XtVaSetValues(XtParent(dialog), XtNx, (XtArgVal)150, XtNy, (XtArgVal)120, NULL);
    XtManageChild(dialog);
    ok = XmMessageBoxGetChild(dialog, XmDIALOG_OK_BUTTON);
    cancel = XmMessageBoxGetChild(dialog, XmDIALOG_CANCEL_BUTTON);
    help = XmMessageBoxGetChild(dialog, XmDIALOG_HELP_BUTTON);
    XtVaGetValues(ok, XtNwidth, &w, XtNheight, &h, XtNx, &x, XtNy, &y, NULL);
    printf("GEO:OK w=%u h=%u rel=%d,%d\n", w, h, x, y);
    XtVaGetValues(cancel, XtNwidth, &w, XtNheight, &h, XtNx, &x, XtNy, &y, NULL);
    printf("GEO:CANCEL w=%u h=%u rel=%d,%d\n", w, h, x, y);
    XtVaGetValues(help, XtNwidth, &w, XtNheight, &h, XtNx, &x, XtNy, &y, NULL);
    printf("GEO:HELP w=%u h=%u rel=%d,%d\n", w, h, x, y);
    fflush(stdout);
}

static void done(XtPointer c, XtIntervalId *id)
{
    (void)c; (void)id;
    printf("MSG:answered=%d\n", answered);
    fflush(stdout);
    XtAppSetExitFlag(app);
}

int main(int argc, char **argv)
{
    Widget top = XtAppInitialize(&app, "MwMsg", NULL, 0, &argc, argv,
                                 NULL, NULL, 0);
    Arg args[4];
    Cardinal n = 0;
    XtSetArg(args[n], XmNdialogStyle, XmDIALOG_FULL_APPLICATION_MODAL); n++;
    XtSetArg(args[n], XmNdialogTitle,
             XmStringCreateLocalized("Save")); n++;
    XtSetArg(args[n], XmNmessageString,
             XmStringCreateLocalized("Save changes?")); n++;
    dialog = XmCreateMessageDialog(top, "msg", args, n);
    /* NEdit registers the dialog-level callbacks (util/DialogF.c), not
     * per-button ones. */
    XtAddCallback(dialog, XmNokCallback, (XtCallbackProc)answer, (XtPointer)1);
    XtAddCallback(dialog, XmNcancelCallback, (XtCallbackProc)answer, (XtPointer)2);
    XtAddCallback(dialog, XmNhelpCallback, (XtCallbackProc)answer, (XtPointer)3);

    Widget button = XtVaCreateManagedWidget("open", xmPushButtonWidgetClass, top,
                                            XmNlabelString,
                                            XmStringCreateLocalized("Open"),
                                            XmNwidth, (Dimension)120,
                                            XmNheight, (Dimension)40, NULL);
    (void)button;
    XtRealizeWidget(top);

    XtAppAddTimeOut(app, 600, arm_menu_state, (XtPointer)top);
    XtAppAddTimeOut(app, 1500, show, NULL);
    XtAppAddTimeOut(app, 9000, done, NULL);
    XtAppMainLoop(app);
    return answered ? 0 : 1;
}
