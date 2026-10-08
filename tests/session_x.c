/* session_x.c — an X client that opts into the session protocol.
 *
 * Lists WM_SAVE_YOURSELF and WM_DELETE_WINDOW in WM_PROTOCOLS and reports the
 * ClientMessages the shim relays from a session manager in another process.
 */
#include <X11/Xlib.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    setbuf(stdout, NULL);
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "session_x: no display\n"); return 2; }

    Window root = DefaultRootWindow(d);
    Window w = XCreateSimpleWindow(d, root, 0, 0, 64, 64, 0, 0, 0);
    Atom wmproto = XInternAtom(d, "WM_PROTOCOLS", False);
    Atom del = XInternAtom(d, "WM_DELETE_WINDOW", False);
    Atom save = XInternAtom(d, "WM_SAVE_YOURSELF", False);
    Atom protos[2] = { del, save };
    XSetWMProtocols(d, w, protos, 2);
    XMapWindow(d, w);
    XFlush(d);
    printf("SESSION:READY\n");

    int save_got = 0, del_got = 0;
    for (int i = 0; i < 600 && !(save_got && del_got); i++) {
        while (XPending(d)) {
            XEvent e;
            XNextEvent(d, &e);
            if (e.type != ClientMessage || e.xclient.message_type != wmproto)
                continue;
            if (e.xclient.data.l[0] == (long)save) {
                save_got = 1;
                printf("SESSION:SAVE\n");
            } else if (e.xclient.data.l[0] == (long)del) {
                del_got = 1;
                printf("SESSION:CLOSE\n");
            }
        }
        if (!(save_got && del_got)) usleep(20000);
    }
    printf("SESSION:RESULT save=%d close=%d\n", save_got, del_got);
    return (save_got && del_got) ? 0 : 1;
}
