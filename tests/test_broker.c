/* test_broker.c — a shared selection carried between two shim processes by
 * the selection broker (XLIB_WAYLAND_SHARE_SELECTIONS).  The child owns the
 * selection and answers with a large property; the parent converts it.  A
 * reply over ~8 KiB used to stall until the broker's 5 s deadline and then
 * be refused, and the owner's partial sends truncated it. */
#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SEL "MW_TEST_SHARED_SEL"
#define NBYTES (200 * 1024)

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static unsigned char pattern(size_t i) { return (unsigned char)('a' + i % 26); }

static int owner(int ready_fd)
{
    Display *d = XOpenDisplay(NULL);
    if (!d) return 2;
    Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 10, 10, 0, 0, 0);
    Atom sel = XInternAtom(d, SEL, False);
    XSetSelectionOwner(d, sel, w, CurrentTime);
    XSync(d, False);
    if (write(ready_fd, "r", 1) != 1) return 2;
    close(ready_fd);
    unsigned char *data = malloc(NBYTES);
    for (size_t i = 0; i < NBYTES; i++) data[i] = pattern(i);
    long end = now_ms() + 15000;
    while (now_ms() < end) {
        if (!XPending(d)) { usleep(5000); continue; }
        XEvent ev;
        XNextEvent(d, &ev);
        if (ev.type != SelectionRequest) continue;
        XSelectionRequestEvent *rq = &ev.xselectionrequest;
        XChangeProperty(d, rq->requestor, rq->property, XA_STRING, 8,
                        PropModeReplace, data, NBYTES);
        XSelectionEvent se = { 0 };
        se.type = SelectionNotify;
        se.requestor = rq->requestor;
        se.selection = rq->selection;
        se.target = rq->target;
        se.property = rq->property;
        se.time = rq->time;
        XSendEvent(d, rq->requestor, False, 0, (XEvent *)&se);
        XSync(d, False);
    }
    free(data);
    XCloseDisplay(d);
    return 0;
}

int main(void)
{
    setenv("XLIB_WAYLAND_SHARE_SELECTIONS", "MW_TEST_SHARED", 1);
    int p[2];
    if (pipe(p) != 0) return 1;
    pid_t child = fork();
    if (child == 0) { close(p[0]); _exit(owner(p[1])); }
    close(p[1]);
    char c;
    if (read(p[0], &c, 1) != 1) { fprintf(stderr, "owner did not start\n"); return 1; }

    Display *d = XOpenDisplay(NULL);
    if (!d) { kill(child, SIGTERM); return 77; }
    Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 10, 10, 0, 0, 0);
    Atom sel = XInternAtom(d, SEL, False);
    Atom prop = XInternAtom(d, "MW_TEST_RESULT", False);
    long t0 = now_ms();
    XConvertSelection(d, sel, XA_STRING, prop, w, CurrentTime);
    XFlush(d);
    int failures = 0;
    XEvent ev;
    int got = 0;
    while (now_ms() - t0 < 12000) {
        if (XCheckTypedWindowEvent(d, w, SelectionNotify, &ev)) { got = 1; break; }
        usleep(5000);
        XPending(d);
    }
    long elapsed = now_ms() - t0;
    if (!got) { fprintf(stderr, "FAIL: no SelectionNotify\n"); failures++; }
    else if (ev.xselection.property == None) {
        fprintf(stderr, "FAIL: conversion refused after %ld ms\n", elapsed);
        failures++;
    } else {
        Atom type; int fmt; unsigned long n, after; unsigned char *data = NULL;
        XGetWindowProperty(d, w, prop, 0, NBYTES, True, AnyPropertyType,
                           &type, &fmt, &n, &after, &data);
        int ok = data && n == NBYTES && fmt == 8;
        for (size_t i = 0; ok && i < NBYTES; i++) ok = data[i] == pattern(i);
        if (!ok) { fprintf(stderr, "FAIL: got %lu bytes (want %d)\n", n, NBYTES); failures++; }
        if (elapsed > 4000) { fprintf(stderr, "FAIL: took %ld ms\n", elapsed); failures++; }
        XFree(data);
    }
    XCloseDisplay(d);
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    if (failures) { fprintf(stderr, "test_broker: %d failure(s)\n", failures); return 1; }
    printf("test_broker: all checks passed (%ld ms)\n", elapsed);
    return 0;
}
