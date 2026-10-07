/* xdnd_src_x.c — an X XDND drag source, to test the shim's X→Wayland XDND path.
 *
 * Owns XdndSelection (what GDK does when a drag starts), advertises an
 * XdndTypeList, answers TARGETS and the text targets with a URI, and prints the
 * events it sees.  The shim should notice the selection ownership, query the
 * targets, and start a Wayland drag.
 */
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

static Atom A(Display *d, const char *n) { return XInternAtom(d, n, False); }

int main(void)
{
    setbuf(stdout, NULL);
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "xdnd_src_x: no display\n"); return 2; }
    int scr = DefaultScreen(d);
    Window w = XCreateSimpleWindow(d, RootWindow(d, scr), 0, 0, 300, 200, 0, 0, 0);
    XSelectInput(d, w, ExposureMask | StructureNotifyMask);
    XMapWindow(d, w);
    XClearWindow(d, w);
    XFlush(d);

    long types[4];
    types[0] = A(d, "text/uri-list");
    types[1] = A(d, "text/plain;charset=utf-8");
    types[2] = A(d, "UTF8_STRING");
    types[3] = A(d, "STRING");
    XChangeProperty(d, w, A(d, "XdndTypeList"), XA_ATOM, 32, PropModeReplace,
                    (const unsigned char *)types, 4);

    XSetSelectionOwner(d, A(d, "XdndSelection"), w, CurrentTime);
    XFlush(d);
    printf("SRC:DRAG %lu\n", (unsigned long)w);

    const char *payload = "file:///tmp/from-x";
    for (;;) {
        XEvent e;
        XNextEvent(d, &e);
        if (e.type == Expose) {
            if (e.xexpose.count == 0) XClearWindow(d, w);
        } else if (e.type == SelectionRequest) {
            Atom prop = e.xselectionrequest.property == None
                            ? e.xselectionrequest.target
                            : e.xselectionrequest.property;
            Atom t = e.xselectionrequest.target;
            if (t == A(d, "TARGETS")) {
                long list[4] = { A(d, "TARGETS"), A(d, "text/uri-list"),
                                 A(d, "UTF8_STRING"), A(d, "STRING") };
                XChangeProperty(d, e.xselectionrequest.requestor, prop, XA_ATOM,
                                32, PropModeReplace, (const unsigned char *)list, 4);
            } else {
                XChangeProperty(d, e.xselectionrequest.requestor, prop, t, 8,
                                PropModeReplace, (const unsigned char *)payload,
                                (int)strlen(payload));
            }
            XSelectionEvent se;
            memset(&se, 0, sizeof se);
            se.type = SelectionNotify;
            se.display = d;
            se.requestor = e.xselectionrequest.requestor;
            se.selection = e.xselectionrequest.selection;
            se.target = t;
            se.property = prop;
            se.time = e.xselectionrequest.time;
            XSendEvent(d, e.xselectionrequest.requestor, False, 0, (XEvent *)&se);
            XFlush(d);
        }
    }
}
