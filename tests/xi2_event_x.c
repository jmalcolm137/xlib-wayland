/* xi2_event_x.c — XInput2 pointer/keyboard events from Wayland input.
 *
 * Selects XI_Motion / XI_ButtonPress / XI_ButtonRelease / XI_KeyPress /
 * XI_KeyRelease / XI_Enter on a window and checks the compositor's scripted
 * motion/button/key arrive as XI2 events (GenericEvent cookies), independent of
 * the core event mask.
 */
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    setbuf(stdout, NULL);
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "xi2_event_x: no display\n"); return 2; }

    int opcode = 0, evbase = 0, errbase = 0;
    XQueryExtension(d, "XInputExtension", &opcode, &evbase, &errbase);
    int maj = 0, min = 0;
    XIQueryVersion(d, &maj, &min);

    Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 64, 64, 0, 0, 0);
    XSelectInput(d, w, ExposureMask);
    XMapWindow(d, w);
    for (;;) { XEvent e; XNextEvent(d, &e); if (e.type == Expose) break; }

    unsigned char mask[(XI_LASTEVENT + 7) / 8];
    memset(mask, 0, sizeof mask);
    XISetMask(mask, XI_Motion);
    XISetMask(mask, XI_ButtonPress);
    XISetMask(mask, XI_ButtonRelease);
    XISetMask(mask, XI_KeyPress);
    XISetMask(mask, XI_KeyRelease);
    XISetMask(mask, XI_Enter);
    XIEventMask em;
    em.deviceid = XIAllDevices;
    em.mask_len = sizeof mask;
    em.mask = mask;
    XISelectEvents(d, w, &em, 1);
    XFlush(d);

    int motion = 0, bpress = 0, brelease = 0, kpress = 0, enter = 0;
    for (int i = 0; i < 400; i++) {
        while (XPending(d)) {
            XEvent e;
            XNextEvent(d, &e);
            if (e.type != GenericEvent || e.xcookie.extension != opcode)
                continue;
            XGenericEventCookie *c = &e.xcookie;
            if (!XGetEventData(d, c))
                continue;
            int et = c->evtype;
            if (et == XI_Enter) {
                XIEnterEvent *xe = c->data;
                enter = 1;
                printf("XI2EV:enter mode=%d detail=%d x=%.0f y=%.0f\n",
                       xe->mode, xe->detail, xe->event_x, xe->event_y);
            } else {
                XIDeviceEvent *de = c->data;
                printf("XI2EV:evtype=%d device=%d detail=%d x=%.0f y=%.0f\n",
                       et, de->deviceid, de->detail, de->event_x, de->event_y);
                if (et == XI_Motion) motion = 1;
                else if (et == XI_ButtonPress) bpress = 1;
                else if (et == XI_ButtonRelease) brelease = 1;
                else if (et == XI_KeyPress) kpress = 1;
            }
            XFreeEventData(d, c);
        }
        if (motion && bpress && kpress) break;
        usleep(10000);
    }

    printf("XI2EV:RESULT motion=%d button=%d release=%d key=%d enter=%d\n",
           motion, bpress, brelease, kpress, enter);
    return (motion && bpress && kpress) ? 0 : 1;
}
