/* test_xm_dnd.c — a Motif XmText drop target for exercising the Wayland DnD
 * bridge (xlib/dnd.c).
 *
 * A dropped string is inserted into the text widget, so printing GOT:<text>
 * whenever the contents change makes a successful drop visible in the client
 * log without a screenshot.  Drive it with tests/dnd_wl.c as the drag source
 * and the virtual-pointer tools (press, move, release); see README.md.
 *
 * Build (against the installed shim + Motif):
 *   cc -o build/test_xm_dnd tests/test_xm_dnd.c \
 *      -I$PREFIX/include -L$PREFIX/lib -lXm -lXt -lX11 -Wl,-rpath,$PREFIX/lib
 */
#include <Xm/Xm.h>
#include <Xm/Text.h>
#include <X11/Intrinsic.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static Widget text;
static char last[8192];
static int tick;

static void poll(XtPointer c, XtIntervalId *id)
{
    (void)c; (void)id;
    char *s = XmTextGetString(text);
    if (s) {
        if (strcmp(s, last) != 0) {
            snprintf(last, sizeof last, "%s", s);
            printf("GOT:%s\n", last);
            fflush(stdout);
        } else if (++tick % 25 == 0) {
            printf("ALIVE len=%zu last=%ld\n", strlen(s),
                   (long)XmTextGetLastPosition(text));
            fflush(stdout);
        }
        XtFree(s);
    }
    XtAppAddTimeOut(XtWidgetToApplicationContext(text), 200, poll, NULL);
}

int main(int argc, char **argv)
{
    XtAppContext app;
    Widget shell = XtAppInitialize(&app, "XmDnd", NULL, 0, &argc, argv,
                                   NULL, NULL, 0);
    text = XmCreateText(shell, "text", NULL, 0);
    XtManageChild(text);
    XtRealizeWidget(shell);
    printf("READY win=0x%lx\n", (unsigned long)XtWindow(shell));
    fflush(stdout);
    XtAppAddTimeOut(app, 200, poll, NULL);
    XtAppMainLoop(app);
    return 0;
}
