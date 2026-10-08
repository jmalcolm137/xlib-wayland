/* xext_x.c — exercise the libXext facade (SHM and Sync) plus XShape fallback.
 *
 * The shim installs its own libXext.so.6; this links it (not libX11's XShape)
 * and checks MIT-SHM works end to end (create/attach/put), that Sync reports
 * present, and that XShape resolves to the shim's libX11 implementation.
 */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/sync.h>
#include <X11/extensions/shape.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>

int main(void)
{
    setbuf(stdout, NULL);
    Display *d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "xext_x: no display\n"); return 2; }
    int scr = DefaultScreen(d);

    int smaj = 0, smin = 0;
    Bool pixmaps = True;
    Bool shm = XShmQueryExtension(d);
    if (shm) XShmQueryVersion(d, &smaj, &smin, &pixmaps);

    int eb = 0, erb = 0;
    Bool sync = XSyncQueryExtension(d, &eb, &erb);
    int ymaj = 0, ymin = 0;
    if (sync) XSyncInitialize(d, &ymaj, &ymin);

    Bool shaped = XShapeQueryExtension(d, &eb, &erb);

    Window w = XCreateSimpleWindow(d, RootWindow(d, scr), 0, 0, 64, 64, 0, 0, 0);
    XMapWindow(d, w);
    GC gc = XCreateGC(d, w, 0, NULL);
    XSetForeground(d, gc, 0x00ff0000);
    XFillRectangle(d, w, gc, 0, 0, 64, 64);

    int put_ok = 0;
    if (shm) {
        XShmSegmentInfo info;
        memset(&info, 0, sizeof info);
        XImage *img = XShmCreateImage(d, DefaultVisual(d, scr), 24, ZPixmap,
                                      NULL, &info, 16, 16);
        if (img) {
            info.shmid = shmget(IPC_PRIVATE,
                                (size_t)img->bytes_per_line * img->height,
                                IPC_CREAT | 0600);
            info.shmaddr = info.shmid >= 0 ? shmat(info.shmid, NULL, 0)
                                           : (char *)-1;
            if (info.shmaddr != (char *)-1) {
                info.readOnly = False;
                img->data = info.shmaddr;
                memset(img->data, 0x7f, (size_t)img->bytes_per_line * img->height);
                XShmAttach(d, &info);
                XShmPutImage(d, w, gc, img, 0, 0, 0, 0, 16, 16, False);
                XFlush(d);
                put_ok = 1;
                XShmDetach(d, &info);
                shmdt(info.shmaddr);
                shmctl(info.shmid, IPC_RMID, NULL);
            }
            XDestroyImage(img);
        }
    }

    printf("XEXT:shm=%d ver=%d.%d pixmaps=%d put=%d sync=%d sver=%d.%d shape=%d\n",
           shm, smaj, smin, pixmaps, put_ok, sync, ymaj, ymin, shaped);

    XFreeGC(d, gc);
    XCloseDisplay(d);
    return (shm && put_ok && sync && shaped) ? 0 : 1;
}
