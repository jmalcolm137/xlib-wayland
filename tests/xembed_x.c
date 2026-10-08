/* xembed_x.c — the XEmbed primitives in one process.
 *
 * An XEmbed plug is a window reparented into its socket, carrying an
 * _XEMBED_INFO property and exchanging _XEMBED ClientMessages.  Cross-process
 * embedding is impossible under Wayland, so this exercises the shim's protocol
 * support for a same-process embedder: XReparentWindow must move the window
 * into the socket (and emit ReparentNotify, which is how a plug learns its
 * socket), and XSendEvent must deliver the _XEMBED message.
 */
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static void pump(Display *d, int *reparent, int *message,
                 Window plug, Window sock, Atom xembed)
{
    while (XPending(d)) {
        XEvent e;
        XNextEvent(d, &e);
        if (e.type == ReparentNotify && e.xreparent.window == plug &&
            e.xreparent.parent == sock)
            *reparent = 1;
        else if (e.type == ClientMessage &&
                 e.xclient.message_type == xembed &&
                 e.xclient.window == plug)
            *message = 1;
    }
}

int main(void)
{
    setbuf(stdout, NULL);
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "xembed_x: no display\n"); return 2; }
    Window root = DefaultRootWindow(d);

    Window sock = XCreateSimpleWindow(d, root, 0, 0, 100, 100, 0, 0, 0);
    Window plug = XCreateSimpleWindow(d, root, 200, 200, 50, 50, 0, 0, 0);
    XSelectInput(d, plug, StructureNotifyMask);
    XMapWindow(d, sock);
    XMapWindow(d, plug);
    XFlush(d);

    /* The plug advertises the XEmbed protocol via _XEMBED_INFO. */
    Atom info = XInternAtom(d, "_XEMBED_INFO", False);
    unsigned long infodata[2] = { 0, 0 };   /* flags, version */
    XChangeProperty(d, plug, info, info, 32, PropModeReplace,
                    (const unsigned char *)infodata, 2);

    int reparent = 0, message = 0;
    for (int i = 0; i < 50; i++) {
        pump(d, &reparent, &message, plug, sock, XInternAtom(d, "_XEMBED", False));
        if (reparent) break;
        usleep(10000);
    }

    /* Embed the plug at (5,5) in the socket. */
    XReparentWindow(d, plug, sock, 5, 5);
    XFlush(d);
    for (int i = 0; i < 100 && !reparent; i++) {
        pump(d, &reparent, &message, plug, sock, XInternAtom(d, "_XEMBED", False));
        if (!reparent) usleep(10000);
    }

    /* Geometry is now socket-relative. */
    XWindowAttributes wa;
    XGetWindowAttributes(d, plug, &wa);
    int geom = (wa.x == 5 && wa.y == 5);

    /* The embedder notifies the plug with an _XEMBED message. */
    Atom xembed = XInternAtom(d, "_XEMBED", False);
    XClientMessageEvent cm;
    memset(&cm, 0, sizeof cm);
    cm.type = ClientMessage;
    cm.window = plug;
    cm.message_type = xembed;
    cm.format = 32;
    cm.data.l[0] = 0;            /* XEMBED_EMBEDDED_NOTIFY */
    cm.data.l[1] = 0;
    cm.data.l[2] = (long)sock;
    cm.data.l[3] = 0;
    XSendEvent(d, plug, False, NoEventMask, (XEvent *)&cm);
    XFlush(d);
    for (int i = 0; i < 100 && !message; i++) {
        pump(d, &reparent, &message, plug, sock, xembed);
        if (!message) usleep(10000);
    }

    /* _XEMBED_INFO is readable back. */
    Atom type; int fmt; unsigned long n = 0, ba = 0;
    unsigned char *data = NULL;
    int have_info = 0;
    if (XGetWindowProperty(d, plug, info, 0, 2, False, info, &type, &fmt,
                           &n, &ba, &data) == Success && n == 2)
        have_info = 1;
    if (data) XFree(data);

    printf("XEMBED:reparent=%d geometry=%d message=%d info=%d\n",
           reparent, geom, message, have_info);
    return (reparent && geom && message && have_info) ? 0 : 1;
}
