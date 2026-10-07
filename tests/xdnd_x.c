/* xdnd_x.c — an X XDND drop target for the shim's XDND bridge test.
 *
 * Advertises XdndAware, accepts an XDND drag (XdndStatus), and on XdndDrop
 * converts XdndSelection for text/uri-list and prints the bytes.  The shim is
 * the XDND *source* here (it presents a Wayland drag as XDND), so this mirrors
 * what a GTK2 text widget does when a file is dropped on it.
 */
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static Atom A(Display *d, const char *n) { return XInternAtom(d, n, False); }

int main(void)
{
    setbuf(stdout, NULL);
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "xdnd_x: no display\n"); return 2; }

    int scr = DefaultScreen(d);
    Window w = XCreateSimpleWindow(d, RootWindow(d, scr), 0, 0, 300, 200, 0, 0, 0);
    XSelectInput(d, w, ExposureMask | StructureNotifyMask);

    long ver = 5;
    XChangeProperty(d, w, A(d, "XdndAware"), XA_ATOM, 32, PropModeReplace,
                    (const unsigned char *)&ver, 1);
    XMapWindow(d, w);
    XClearWindow(d, w);
    XFlush(d);
    printf("XDND:READY\n");

    Window source = None;
    for (;;) {
        XEvent e;
        XNextEvent(d, &e);
        if (e.type == Expose) {
            if (e.xexpose.count == 0) XClearWindow(d, w);
            continue;
        }
        if (e.type == ClientMessage) {
            Atom mt = e.xclient.message_type;
            if (mt == A(d, "XdndEnter")) {
                source = (Window)e.xclient.data.l[0];
            } else if (mt == A(d, "XdndPosition")) {
                source = (Window)e.xclient.data.l[0];
                XClientMessageEvent st;
                memset(&st, 0, sizeof st);
                st.type = ClientMessage;
                st.display = d;
                st.window = source;
                st.message_type = A(d, "XdndStatus");
                st.format = 32;
                st.data.l[0] = w;
                st.data.l[1] = 1;                 /* accept */
                st.data.l[4] = A(d, "XdndActionCopy");
                XSendEvent(d, source, False, 0, (XEvent *)&st);
                XFlush(d);
            } else if (mt == A(d, "XdndDrop")) {
                source = (Window)e.xclient.data.l[0];
                XConvertSelection(d, A(d, "XdndSelection"), A(d, "text/uri-list"),
                                  A(d, "XDND_DATA"), w, CurrentTime);
                XFlush(d);
            }
        } else if (e.type == SelectionNotify) {
            if (e.xselection.property != None) {
                Atom type; int fmt; unsigned long n, ba;
                unsigned char *data = NULL;
                XGetWindowProperty(d, w, e.xselection.property, 0, 0x7fffffff,
                                   True, AnyPropertyType, &type, &fmt, &n, &ba,
                                   &data);
                printf("XDND:GOT %.*s\n", (int)n, data ? (char *)data : "");
                if (data) XFree(data);
            } else {
                printf("XDND:NONE\n");
            }
            if (source != None) {
                XClientMessageEvent fi;
                memset(&fi, 0, sizeof fi);
                fi.type = ClientMessage;
                fi.display = d;
                fi.window = source;
                fi.message_type = A(d, "XdndFinished");
                fi.format = 32;
                fi.data.l[0] = w;
                fi.data.l[1] = 1;             /* success */
                fi.data.l[2] = A(d, "XdndActionCopy");
                XSendEvent(d, source, False, 0, (XEvent *)&fi);
                XFlush(d);
            }
            return 0;
        }
    }
}
