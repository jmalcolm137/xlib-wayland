/* test_popup_repost.c — post, unpost and re-post an override-redirect popup.
 *
 * NEdit re-posts its menu shells the same way, and a compositor is not
 * obliged to send a second xdg_popup.configure when an already-configured
 * popup is mapped again (KWin does not).  A client that waits for one never
 * paints the re-posted menu, so it is there and clickable but invisible.
 *
 * This client needs no input: it maps the popup, unmaps it, maps it again and
 * exits, so it can be pointed at a live compositor to check the re-post path.
 */
#include <X11/Xlib.h>
#include <stdio.h>
#include <unistd.h>

int main(void)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "cannot open display\n"); return 2; }

    Window root = DefaultRootWindow(d);
    Window main_w = XCreateSimpleWindow(d, root, 0, 0, 400, 300, 1, 0, 0xffffff);
    XStoreName(d, main_w, "repost test");
    XMapWindow(d, main_w);
    XSync(d, False);
    usleep(700000);

    XSetWindowAttributes a;
    a.override_redirect = True;
    a.background_pixel = 0xf0f0e8;
    Window popup = XCreateWindow(d, main_w, 16, 24, 140, 96, 0,
                                 CopyFromParent, InputOutput, CopyFromParent,
                                 CWOverrideRedirect | CWBackPixel, &a);
    Window item[3];
    for (int i = 0; i < 3; i++) {
        XSetWindowAttributes ia;
        ia.background_pixel = 0xd8d8d0;
        item[i] = XCreateWindow(d, popup, 4, 4 + i * 30, 132, 26, 0,
                                CopyFromParent, InputOutput, CopyFromParent,
                                CWBackPixel, &ia);
        XMapWindow(d, item[i]);
    }

    printf("POST1\n"); fflush(stdout);
    XMapRaised(d, popup);
    XSync(d, False);
    usleep(1200000);

    printf("UNPOST\n"); fflush(stdout);
    XUnmapWindow(d, popup);
    XSync(d, False);
    usleep(1200000);

    printf("POST2\n"); fflush(stdout);
    XMapRaised(d, popup);
    XSync(d, False);
    usleep(1500000);

    printf("UNPOST2\n"); fflush(stdout);
    XUnmapWindow(d, popup);
    XSync(d, False);
    usleep(1200000);

    printf("POST3\n"); fflush(stdout);
    XMapRaised(d, popup);
    XSync(d, False);
    usleep(2000000);

    printf("DONE\n"); fflush(stdout);
    XCloseDisplay(d);
    return 0;
}
