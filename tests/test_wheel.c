/* test_wheel.c — the mouse wheel arrives as X buttons.
 *
 * Wayland reports wheel motion on wl_pointer.axis; X clients instead expect
 * button 4/5 (vertical) and 6/7 (horizontal) presses.  The shim used to ignore
 * the direction, so every scroll went one way.  Run under the headless
 * compositor with tests/wheel.input, which sends two down, one up and one
 * right and then screenshots.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <sys/time.h>
#include <unistd.h>

static long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000L + tv.tv_usec / 1000;
}

int main(void)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) { printf("test_wheel: no display\n"); return 1; }
    int screen = DefaultScreen(d);
    Window w = XCreateSimpleWindow(d, RootWindow(d, screen), 0, 0, 360, 260, 0,
                                   BlackPixel(d, screen), WhitePixel(d, screen));
    XSelectInput(d, w, ButtonPressMask);
    XMapWindow(d, w);
    XFlush(d);

    int up = 0, down = 0, left = 0, right = 0, other = 0;
    long deadline = now_ms() + 4000;
    while (now_ms() < deadline) {
        while (XPending(d)) {
            XEvent e;
            XNextEvent(d, &e);
            if (e.type != ButtonPress)
                continue;
            switch (e.xbutton.button) {
            case 4: up++;    break;
            case 5: down++;  break;
            case 6: left++;  break;
            case 7: right++; break;
            default: other++; break;
            }
        }
        usleep(10000);
    }

    if (down == 2 && up == 1 && right == 1 && left == 0 && other == 0) {
        printf("all checks passed (up=%d down=%d left=%d right=%d)\n",
               up, down, left, right);
        return 0;
    }
    printf("FAIL: up=%d down=%d left=%d right=%d other=%d\n",
           up, down, left, right, other);
    return 1;
}
