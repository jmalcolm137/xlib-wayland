/* test_xm.c — a Motif (Xm) application: validates the full
 * Xlib -> Xt -> Xm stack running on the Wayland shim.
 */
#include <Xm/Xm.h>
#include <Xm/Form.h>
#include <Xm/Label.h>
#include <Xm/PushB.h>
#include <Xm/Text.h>
#include <X11/Intrinsic.h>
#include <stdio.h>
#include <stdlib.h>

static int g_activations;

static void activate(Widget w, XtPointer client, XtPointer call)
{
    (void)w; (void)call;
    g_activations++;
    printf("Xm PushButton activated (%d)\n", g_activations);
    XtAppSetExitFlag((XtAppContext)client);
}

static void quit_cb(XtPointer client, XtIntervalId *id)
{
    (void)id;
    XtAppSetExitFlag((XtAppContext)client);
}

int main(int argc, char **argv)
{
    XtAppContext app;
    Widget top = XtAppInitialize(&app, "MWXm", NULL, 0, &argc, argv,
                                 NULL, NULL, 0);
    XtVaSetValues(top, XtNwidth, (XtArgVal)420, XtNheight, (XtArgVal)320, NULL);

    Widget form = XtVaCreateManagedWidget("form", xmFormWidgetClass, top, NULL);

    XmString hello = XmStringCreateLocalized("Hello from Motif on Wayland");
    XtVaCreateManagedWidget("label", xmLabelWidgetClass, form,
        XmNlabelString, hello, XmNx, 20, XmNy, 20, NULL);

    XmString plabel = XmStringCreateLocalized("Press me");
    Widget btn = XtVaCreateManagedWidget("button", xmPushButtonWidgetClass, form,
        XmNlabelString, plabel, XmNx, 20, XmNy, 60, XmNwidth, 140, NULL);
    XtAddCallback(btn, XmNactivateCallback, activate, (XtPointer)app);

    Widget text = XtVaCreateManagedWidget("text", xmTextWidgetClass, form,
        XmNx, 20, XmNy, 110, XmNwidth, 360, XmNheight, 160, NULL);
    XmTextSetString(text, "editable text\nsecond line");

    XtRealizeWidget(top);
    printf("Xm realize OK: shell window=0x%lx\n", XtWindow(top));

    XtAppAddTimeOut(app, 3000, quit_cb, (XtPointer)app);
    XtAppMainLoop(app);

    XmStringFree(hello);
    XmStringFree(plabel);
    printf("Xm done: %d activations\n", g_activations);
    return 0;
}
