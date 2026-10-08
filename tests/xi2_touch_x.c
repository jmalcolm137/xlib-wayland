/* xi2_touch_x.c — XInput2 touch, bridged from Wayland.
 *
 * The shim reflects the Wayland seat: a touch device with an XITouchClassInfo
 * appears when the seat advertises touch.  This selects XI_TouchBegin/Update/
 * End on a window and checks a Wayland touch sequence arrives as those events,
 * with the device id and coordinates from Wayland.
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
    if (!d) { fprintf(stderr, "xi2_touch_x: no display\n"); return 2; }

    int opcode = 0, evbase = 0, errbase = 0;
    XQueryExtension(d, "XInputExtension", &opcode, &evbase, &errbase);

    int maj = 0, min = 0;
    Status ver = XIQueryVersion(d, &maj, &min);
    printf("XI2:version status=%d %d.%d opcode=%d\n", ver, maj, min, opcode);

    /* The touch device comes from the Wayland seat's touch capability. */
    int ndev = 0;
    XIDeviceInfo *devs = XIQueryDevice(d, XIAllDevices, &ndev);
    int touchdev = 0;
    for (int i = 0; i < ndev; i++)
        for (int c = 0; c < devs[i].num_classes; c++)
            if (devs[i].classes[c]->type == XITouchClass)
                touchdev = devs[i].deviceid;
    printf("XI2:devices=%d touchdevice=%d\n", ndev, touchdev);

    Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 64, 64, 0, 0, 0);
    XSelectInput(d, w, ExposureMask);
    XMapWindow(d, w);
    for (;;) { XEvent e; XNextEvent(d, &e); if (e.type == Expose) break; }

    unsigned char mask[(XI_LASTEVENT + 7) / 8];
    memset(mask, 0, sizeof mask);
    XISetMask(mask, XI_TouchBegin);
    XISetMask(mask, XI_TouchUpdate);
    XISetMask(mask, XI_TouchEnd);
    XIEventMask em;
    em.deviceid = XIAllDevices;
    em.mask_len = sizeof mask;
    em.mask = mask;
    XISelectEvents(d, w, &em, 1);
    XFlush(d);

    int begin = 0, update = 0, end = 0;
    for (int i = 0; i < 300 && !(begin && update && end); i++) {
        while (XPending(d)) {
            XEvent e;
            XNextEvent(d, &e);
            if (e.type != GenericEvent || e.xcookie.extension != opcode)
                continue;
            XGenericEventCookie *c = &e.xcookie;
            if (!XGetEventData(d, c))
                continue;
            XIDeviceEvent *de = c->data;
            if (de->evtype == XI_TouchBegin) begin = 1;
            else if (de->evtype == XI_TouchUpdate) update = 1;
            else if (de->evtype == XI_TouchEnd) end = 1;
            printf("XI2:event evtype=%d device=%d source=%d id=%d x=%.0f y=%.0f\n",
                   de->evtype, de->deviceid, de->sourceid, de->detail,
                   de->event_x, de->event_y);
            XFreeEventData(d, c);
        }
        if (!(begin && update && end)) usleep(10000);
    }

    printf("XI2:RESULT begin=%d update=%d end=%d\n", begin, update, end);
    return (touchdev && begin && update && end) ? 0 : 1;
}
