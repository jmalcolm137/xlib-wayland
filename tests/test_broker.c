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
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <poll.h>
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
    pid_t parent = getppid();
    while (now_ms() < end && getppid() == parent) {   /* stop if orphaned */
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

/* ---- an owner whose requestor does not read ----
 *
 * The owner's reply used to be written with a blocking write (bounded at
 * 5 s): a requestor that connected and then did not read froze the owning
 * client for that long.  Here this process owns a large selection and a raw
 * peer asks for it, waits before reading, then reads the whole reply. */
#define BIG_SEL "MW_TEST_SHARED_BIG"
#define NBIG (1024 * 1024)

static void hex(const char *s, char *out)
{
    static const char *hx = "0123456789abcdef";
    for (; *s; s++) { *out++ = hx[(unsigned char)*s >> 4]; *out++ = hx[*s & 0xf]; }
    *out = 0;
}

static int slow_peer(pid_t owner_pid, int result_fd)
{
    char dir[256], path[300];
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && *rt) snprintf(dir, sizeof dir, "%s/xlib-wayland", rt);
    else snprintf(dir, sizeof dir, "/tmp/xlib-wayland-%u", (unsigned)getuid());
    snprintf(path, sizeof path, "%s/sel-%d.sock", dir, (int)owner_pid);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) return 2;
    char hs[64], ht[32], hp[32], req[200];
    hex(BIG_SEL, hs); hex("STRING", ht); hex("MW_TEST_P", hp);
    int l = snprintf(req, sizeof req, "C 7 %s %s %s\n", hs, ht, hp);
    if (write(fd, req, (size_t)l) != l) return 2;
    usleep(2500 * 1000);                         /* do not read for a while */
    long total = 0;
    char last = 0, buf[65536];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n <= 0) break;
        total += n;
        last = buf[n - 1];
    }
    long msg[2] = { total, last };
    if (write(result_fd, msg, sizeof msg) != (ssize_t)sizeof msg) return 2;
    return 0;
}

static int stalled_reader(Display *d)
{
    int failures = 0;
    Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 10, 10, 0, 0, 0);
    Atom sel = XInternAtom(d, BIG_SEL, False);
    XSetSelectionOwner(d, sel, w, CurrentTime);
    XSync(d, False);
    unsigned char *data = malloc(NBIG);
    for (size_t i = 0; i < NBIG; i++) data[i] = pattern(i);
    int rp[2];
    if (pipe(rp) != 0) return 1;
    pid_t me = getpid();
    pid_t peer = fork();
    if (peer == 0) { close(rp[0]); _exit(slow_peer(me, rp[1])); }
    close(rp[1]);
    fcntl(rp[0], F_SETFL, O_NONBLOCK);

    long worst = 0, t_end = now_ms() + 12000;
    long msg[2] = { -1, 0 };
    int served = 0;
    while (now_ms() < t_end) {
        long t = now_ms();
        while (XPending(d)) {
            XEvent ev;
            XNextEvent(d, &ev);
            if (ev.type != SelectionRequest) continue;
            XSelectionRequestEvent *rq = &ev.xselectionrequest;
            XChangeProperty(d, rq->requestor, rq->property, XA_STRING, 8,
                            PropModeReplace, data, NBIG);
            XSelectionEvent se = { 0 };
            se.type = SelectionNotify;
            se.requestor = rq->requestor; se.selection = rq->selection;
            se.target = rq->target; se.property = rq->property; se.time = rq->time;
            XSendEvent(d, rq->requestor, False, 0, (XEvent *)&se);
            served++;
        }
        XFlush(d);
        long dt = now_ms() - t;
        if (dt > worst) worst = dt;
        if (read(rp[0], msg, sizeof msg) == (ssize_t)sizeof msg) break;
        usleep(5000);
    }
    waitpid(peer, NULL, 0);
    close(rp[0]);
    free(data);
    if (!served) { fprintf(stderr, "FAIL: the request was never served\n"); failures++; }
    if (worst > 1000) {
        fprintf(stderr, "FAIL: the owner was blocked for %ld ms by a peer not reading\n", worst);
        failures++;
    }
    if (msg[0] < 2L * NBIG + 1 || msg[1] != '\n') {
        fprintf(stderr, "FAIL: the slow peer got %ld bytes of the reply\n", msg[0]);
        failures++;
    }
    XDestroyWindow(d, w);
    return failures;
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
    if (!d) {
        kill(child, SIGTERM);
        waitpid(child, NULL, 0);
        return getenv("MW_TEST_REQUIRE_DISPLAY") ? 1 : 77;
    }
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
        /* The old failure mode was a 5 s stall and a refusal; anything well
         * under that (even on a loaded or sanitized run) is the fixed path. */
        if (elapsed > 4500) { fprintf(stderr, "FAIL: took %ld ms\n", elapsed); failures++; }
        XFree(data);
    }
    failures += stalled_reader(d);
    XCloseDisplay(d);
    kill(child, SIGTERM);
    waitpid(child, NULL, 0);
    if (failures) { fprintf(stderr, "test_broker: %d failure(s)\n", failures); return 1; }
    printf("test_broker: all checks passed (%ld ms)\n", elapsed);
    return 0;
}
