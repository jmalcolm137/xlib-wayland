/* broker.c — share X selections between shim processes over a Unix socket.
 *
 * Every shim process is its own X server (see DESIGN.md §3), so an X selection
 * owned by one client is invisible to every other client.  CDE relies on this in
 * a few places: the colour server (dtsession) owns "Customize Data:<screen>" and
 * dtstyle asks for it -- getting no answer, it concludes the colour server is
 * not running and disables the Style Manager's Color module.
 *
 * When enabled (XLIB_WAYLAND_SHARE_SELECTIONS), a shim advertises its ownership
 * of the shared selections in a file under XDG_RUNTIME_DIR and listens on a
 * per-process Unix socket.  A client that wants one of those selections with no
 * local owner connects to the owner's socket and asks it to convert; the owner
 * services the request through its own SelectionRequest path (the converter
 * writes the property and sends the SelectionNotify), captures the result, and
 * returns it, and the requestor posts the SelectionNotify locally -- exactly as
 * if the owner had been in this process.
 *
 * The protocol is a one-shot text exchange (selection/target/property names are
 * hex-encoded because they may contain spaces, e.g. "Customize Data:0"):
 *
 *   requestor -> owner   C <sel> <target> <prop>\n
 *   owner -> requestor   D <sel> <target> <prop> <type> <format> <nitems>
 *                          <nbytes> <hexdata>\n
 *   owner -> requestor   R <sel> <target> <prop>\n
 */
#define _GNU_SOURCE
#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MWB_DEADLINE_MS 5000

static int mwb_trace(void)
{
    static int t = -1;
    if (t < 0) t = getenv("MW_TRACE") != NULL || getenv("MW_DND_TRACE") != NULL;
    return t;
}
#define TR(...) do { if (mwb_trace()) { fprintf(stderr, "MWB[%d] ", (int)getpid()); fprintf(stderr, __VA_ARGS__); } } while (0)

typedef struct MwXferReq {
    Atom     selection, target, property;
    Window   requestor;
    Time     time;
    struct MwXferReq *next;
} MwXferReq;

typedef struct MwBroker {
    int      listen_fd;
    volatile int xfer_fd;    /* outbound connection we are waiting on, or -1 */
    int      stop_fd;        /* eventfd: stop the thread */
    int      cmd_fd;         /* eventfd: an fd in the set changed */
    pthread_t thread;
    bool     running;
    char     dir[192];
    char     sock_path[224];
    char     reg_path[224];
    int      mypid;
    bool     enabled;
    char   **sets;
    int      nsets;
    Atom     proxy_prop;

    /* A connection accepted whose request has not fully arrived yet.  Kept
     * rather than dropped, so the peer's write does not fail. */
    int      accept_fd;
    char     accept_buf[1700];
    size_t   accept_len;

    /* Conversions waiting for the one in-flight slot.  Only one can be in
     * flight at a time, but a client may have several outstanding (the colour
     * server palette and a drag's file list, say); queueing them keeps a
     * request from being refused just because another was in progress. */
    MwXferReq *xfer_queue;

    /* inbound: we asked a remote owner and are waiting for the reply */
    struct {
        bool     active;
        int      fd;
        int      retries;
        Atom     selection, target, property;
        Window   requestor;
        Time     time;
        uint64_t deadline_ms;
        char     buf[16384];
        size_t   len;
    } xfer;

    /* outbound: we own the selection and are servicing a remote request */
    struct {
        bool           active;
        int            fd;
        Atom           selection, target, property;
        Window         proxy_win;
        bool           got_prop;
        Atom           ptype;
        int            pformat;
        unsigned long  pnitems;
        unsigned char *pdata;
        size_t         pnbytes;
        uint64_t       deadline_ms;
    } serve;
} MwBroker;

/* ------------------------------------------------------------------ utils */

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static const char *hexchars = "0123456789abcdef";

static void hexenc(const char *s, char *out, size_t outsz)
{
    size_t i = 0;
    for (; s && *s && i + 2 < outsz; s++) {
        out[i++] = hexchars[((unsigned char)*s) >> 4];
        out[i++] = hexchars[((unsigned char)*s) & 0xf];
    }
    out[i] = 0;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode "in" into out (may contain NULs); returns length. */
static size_t hexdec(const char *in, unsigned char *out, size_t outsz)
{
    size_t n = 0;
    while (in[0] && in[1] && n < outsz) {
        int h = hexval(in[0]), l = hexval(in[1]);
        if (h < 0 || l < 0) break;
        out[n++] = (unsigned char)((h << 4) | l);
        in += 2;
    }
    return n;
}

/* ---------------------------------------------------------- shared registry */

/* The registry maps a shared selection name to the pid of the process whose
 * shim owns it.  It is a small line-oriented file written under an flock. */
static void reg_set(MwBroker *b, const char *name, int pid)
{
    int fd = open(b->reg_path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return;
    flock(fd, LOCK_EX);

    /* Read the whole file. */
    char  *text = NULL;
    size_t cap = 0, len = 0;
    char   chunk[512];
    ssize_t n;
    while ((n = read(fd, chunk, sizeof chunk)) > 0) {
        if (len + (size_t)n + 1 > cap) {
            cap = (len + (size_t)n + 1) * 2;
            text = realloc(text, cap);
            if (!text) { close(fd); return; }
        }
        memcpy(text + len, chunk, (size_t)n);
        len += (size_t)n;
    }
    if (!text) { text = malloc(1); if (!text) { close(fd); return; } }
    text[len] = 0;

    /* Rewrite it without any existing entry for this name. */
    size_t nlen = strlen(name);
    size_t ocap = len + nlen + 32;
    char  *out = malloc(ocap);
    if (!out) { free(text); close(fd); return; }
    size_t olen = 0;
    char  *p = text;
    while (*p) {
        char  *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        if (!(ll > nlen && p[nlen] == '\t' && strncmp(p, name, nlen) == 0)) {
            memcpy(out + olen, p, ll);
            olen += ll;
            out[olen++] = '\n';
        }
        p = nl ? nl + 1 : p + ll;
    }
    if (pid > 0) {
        int w = snprintf(out + olen, ocap - olen, "%s\t%d\n", name, pid);
        if (w > 0 && (size_t)w < ocap - olen) olen += (size_t)w;
    }
    if (ftruncate(fd, 0) != 0) { /* best effort */ }
    lseek(fd, 0, SEEK_SET);
    ssize_t wn = write(fd, out, olen);
    (void)wn;
    free(out);
    free(text);
    flock(fd, LOCK_UN);
    close(fd);
}

static int reg_get(MwBroker *b, const char *name)
{
    int fd = open(b->reg_path, O_RDONLY, 0600);
    if (fd < 0) return 0;
    FILE *f = fdopen(fd, "r");
    if (!f) { close(fd); return 0; }
    char line[512];
    int  pid = 0;
    size_t nlen = strlen(name);
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, name, nlen) == 0 && line[nlen] == '\t') {
            pid = atoi(line + nlen + 1);
            break;
        }
    }
    fclose(f);
    return pid;
}

/* --------------------------------------------------------------- helpers */

static bool atom_is_shared(MwBroker *b, Display *d, Atom selection)
{
    if (!b->enabled) return false;
    const char *name = XGetAtomName(d, selection);
    if (!name) return false;
    bool share = false;
    for (int i = 0; i < b->nsets && !share; i++) {
        size_t l = strlen(b->sets[i]);
        if (l && strncmp(name, b->sets[i], l) == 0) share = true;
    }
    XFree((char *)name);
    return share;
}

/* ------------------------------------------------------------- the thread */

static void broker_wake(XDisplayImpl *dp)
{
    if (dp->wake_running && dp->wake_pipe[1] >= 0) {
        ssize_t n = write(dp->wake_pipe[1], "b", 1);
        (void)n;
    }
}

static void *broker_thread(void *arg)
{
    struct { XDisplayImpl *dp; MwBroker *b; } *a = arg;
    XDisplayImpl *dp = a->dp;
    MwBroker     *b  = a->b;
    free(a);

    for (;;) {
        int conn = b->xfer_fd;
        struct pollfd pfd[4] = {
            { b->listen_fd, POLLIN, 0 },
            { b->cmd_fd,    POLLIN, 0 },
            { b->stop_fd,   POLLIN, 0 },
            { conn,         POLLIN, 0 },
        };
        int r = poll(pfd, conn >= 0 ? 4 : 3, -1);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pfd[2].revents) break;                       /* stop */
        if (pfd[1].revents) {                            /* fd set changed */
            uint64_t v; ssize_t n = read(b->cmd_fd, &v, sizeof v); (void)n;
        }
        if (pfd[0].revents || (conn >= 0 && pfd[3].revents))
            broker_wake(dp);
    }
    return NULL;
}

/* ----------------------------------------------------------- lifecycle */

void mw_broker_init(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    const char *env = getenv("XLIB_WAYLAND_SHARE_SELECTIONS");
    if (!env || !*env) return;              /* sharing off by default */

    MwBroker *b = calloc(1, sizeof *b);
    if (!b) return;
    b->listen_fd = b->xfer_fd = b->stop_fd = b->cmd_fd = -1;
    b->accept_fd = -1;
    b->enabled = true;
    b->mypid = (int)getpid();
    b->xfer.fd = -1;
    b->serve.fd = -1;

    /* parse the comma-separated prefix list */
    char *list = strdup(env);
    for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
        while (*tok == ' ') tok++;
        if (!*tok) continue;
        b->sets = realloc(b->sets, sizeof(char *) * (size_t)(b->nsets + 1));
        b->sets[b->nsets++] = strdup(tok);
    }
    free(list);

    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !*rt) rt = "/tmp";
    snprintf(b->dir, sizeof b->dir, "%s/xlib-wayland", rt);
    mkdir(b->dir, 0700);
    snprintf(b->sock_path, sizeof b->sock_path, "%s/sel-%d.sock", b->dir, b->mypid);
    snprintf(b->reg_path, sizeof b->reg_path, "%s/selections", b->dir);

    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", b->sock_path);
    unlink(b->sock_path);
    b->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (b->listen_fd < 0 ||
        bind(b->listen_fd, (struct sockaddr *)&sa, sizeof sa) != 0 ||
        listen(b->listen_fd, 8) != 0) {
        if (b->listen_fd >= 0) close(b->listen_fd);
        b->listen_fd = -1;
    }

    b->stop_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    b->cmd_fd  = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    TR("init pid=%d sock=%s sets=%d\n", b->mypid, b->sock_path, b->nsets);
    dp->broker = b;

    struct { XDisplayImpl *dp; MwBroker *b; } *arg = malloc(sizeof *arg);
    if (arg) {
        arg->dp = dp; arg->b = b;
        if (pthread_create(&b->thread, NULL, broker_thread, arg) == 0)
            b->running = true;
        else
            free(arg);
    }
}

void mw_broker_fini(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    MwBroker *b = dp->broker;
    if (!b) return;
    if (b->running) {
        uint64_t one = 1; ssize_t n = write(b->stop_fd, &one, sizeof one); (void)n;
        pthread_join(b->thread, NULL);
    }
    if (b->listen_fd >= 0) close(b->listen_fd);
    if (b->xfer_fd >= 0)   close(b->xfer_fd);
    if (b->accept_fd >= 0) close(b->accept_fd);
    if (b->xfer.fd >= 0)   close(b->xfer.fd);
    if (b->serve.fd >= 0)  close(b->serve.fd);
    if (b->stop_fd >= 0)   close(b->stop_fd);
    if (b->cmd_fd >= 0)    close(b->cmd_fd);
    free(b->serve.pdata);
    while (b->xfer_queue) {
        MwXferReq *r = b->xfer_queue;
        b->xfer_queue = r->next;
        free(r);
    }
    unlink(b->sock_path);
    for (int i = 0; i < b->nsets; i++) free(b->sets[i]);
    free(b->sets);
    free(b);
    dp->broker = NULL;
}

static void poke(MwBroker *b)
{
    if (b->cmd_fd >= 0) { uint64_t one = 1; ssize_t n = write(b->cmd_fd, &one, sizeof one); (void)n; }
}

/* ------------------------------------------------------------- ownership */

void mw_broker_owner_changed(Display *d, Atom selection, Window owner)
{
    MwBroker *b = MWD(d)->broker;
    if (!b || !atom_is_shared(b, d, selection)) return;
    /* The DnD bridge owns a remote initiator's icc handle locally as a proxy so
     * the drop site's converts reach it; that must not overwrite the real
     * owner's registry entry. */
    if (mw_dnd_owns_window(d, owner)) return;
    const char *name = XGetAtomName(d, selection);
    if (!name) return;
    TR("owner %s -> %s\n", name, owner != None ? "us" : "none");
    reg_set(b, name, owner != None ? b->mypid : 0);
    XFree((char *)name);
}

/* ----------------------------------------------------------- requestor */

/* Start the conversion for one request.  Returns true when it is in flight (or
 * was handed off); false when it cannot be served at all (no local/registered
 * owner). */
static bool xfer_start(Display *d, Atom selection, Atom target, Atom property,
                       Window requestor, Time time)
{
    MwBroker *b = MWD(d)->broker;
    if (!b || b->xfer.active) return false;

    const char *selname = XGetAtomName(d, selection);
    if (!selname) return false;
    int owner_pid = reg_get(b, selname);
    TR("convert begin %s shared=%d owner_pid=%d mypid=%d\n", selname,
       atom_is_shared(b, d, selection), owner_pid, b->mypid);
    if (owner_pid <= 0 || owner_pid == b->mypid) { XFree((char *)selname); return false; }

    char path[224];
    snprintf(path, sizeof path, "%s/sel-%d.sock", b->dir, owner_pid);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { XFree((char *)selname); return false; }
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        XFree((char *)selname);
        return false;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);

    TR("convert %s: prev active=%d fd=%d newfd=%d\n", selname,
       b->xfer.active, b->xfer.fd, fd);

    const char *tname = XGetAtomName(d, target);
    const char *pname = XGetAtomName(d, property == None ? target : property);
    char hs[512], ht[512], hp[512];
    hexenc(selname, hs, sizeof hs);
    hexenc(tname ? tname : "", ht, sizeof ht);
    hexenc(pname ? pname : "", hp, sizeof hp);
    char msg[1700];
    int  mlen = snprintf(msg, sizeof msg, "C %s %s %s\n", hs, ht, hp);
    ssize_t wn = send(fd, msg, (size_t)mlen, MSG_NOSIGNAL);
    TR("convert %s -> pid %d\n", selname, owner_pid);
    XFree((char *)selname);
    if (tname) XFree((char *)tname);
    if (pname) XFree((char *)pname);
    if (wn != mlen) { close(fd); return false; }

    b->xfer.active = true;
    b->xfer.fd = fd;
    b->xfer_fd = fd;
    b->xfer.retries = 0;
    b->xfer.selection = selection;
    b->xfer.target = target;
    b->xfer.property = property;
    b->xfer.requestor = requestor;
    b->xfer.time = time;
    b->xfer.len = 0;
    b->xfer.deadline_ms = now_ms() + MWB_DEADLINE_MS;
    poke(b);
    return true;
}

/* Start the next queued conversion, or refuse it so the waiting client is not
 * left without a SelectionNotify. */
static void xfer_next(Display *d)
{
    MwBroker *b = MWD(d)->broker;
    if (!b) return;
    while (b->xfer_queue && !b->xfer.active) {
        MwXferReq *r = b->xfer_queue;
        b->xfer_queue = r->next;
        if (xfer_start(d, r->selection, r->target, r->property, r->requestor,
                       r->time)) {
            free(r);
            return;
        }
        XSelectionEvent se;
        memset(&se, 0, sizeof se);
        se.type = SelectionNotify;
        se.display = d;
        se.requestor = r->requestor;
        se.selection = r->selection;
        se.target = r->target;
        se.property = None;
        se.time = r->time;
        mw_put_event(d, (XEvent *)&se);
        free(r);
    }
}

bool mw_broker_convert(Display *d, Atom selection, Atom target, Atom property,
                       Window requestor, Time time)
{
    MwBroker *b = MWD(d)->broker;
    if (!b || !atom_is_shared(b, d, selection)) return false;
    if (!b->xfer.active)
        return xfer_start(d, selection, target, property, requestor, time);

    /* One conversion at a time, but a client can have several outstanding --
     * the colour palette and a drag's file list, say.  Queue rather than drop:
     * a request that never gets a reply stalls the client's transfer (and a
     * failed file transfer is fatal to some CDE applications). */
    MwXferReq *r = calloc(1, sizeof *r);
    if (!r) return false;
    r->selection = selection;
    r->target = target;
    r->property = property;
    r->requestor = requestor;
    r->time = time;
    MwXferReq **pp = &b->xfer_queue;
    while (*pp) pp = &(*pp)->next;
    *pp = r;
    TR("queued convert %lu (xfer busy)\n", (unsigned long)selection);
    return true;
}

/* -------------------------------------------------------------- server */

static void serve_finish(Display *d, bool refuse)
{
    XDisplayImpl *dp = MWD(d);
    MwBroker *b = dp->broker;
    if (!b || !b->serve.active) return;

    /* A conversion for a Motif drag's icc handle: when it is done the
     * destination has the data, so the initiator's own drag can be ended (the
     * source shim defers that until the relay completes). */
    bool motif = false;
    {
        const char *sn = XGetAtomName(d, b->serve.selection);
        if (sn && strncmp(sn, "_MOTIF_ATOM_", 12) == 0) motif = true;
        if (sn) XFree((char *)sn);
    }

    if (b->serve.fd >= 0) {
        if (refuse || !b->serve.got_prop) {
            char msg[1700];
            const char *sn = XGetAtomName(d, b->serve.selection);
            const char *tn = XGetAtomName(d, b->serve.target);
            const char *pn = XGetAtomName(d, b->serve.property);
            char hs[512], ht[512], hp[512];
            hexenc(sn ? sn : "", hs, sizeof hs);
            hexenc(tn ? tn : "", ht, sizeof ht);
            hexenc(pn ? pn : "", hp, sizeof hp);
            if (sn) XFree((char *)sn);
            if (tn) XFree((char *)tn);
            if (pn) XFree((char *)pn);
            int l = snprintf(msg, sizeof msg, "R %s %s %s\n", hs, ht, hp);
            ssize_t n = send(b->serve.fd, msg, (size_t)l, MSG_NOSIGNAL); (void)n;
        } else {
            const char *sn = XGetAtomName(d, b->serve.selection);
            const char *tn = XGetAtomName(d, b->serve.target);
            const char *pn = XGetAtomName(d, b->serve.property);
            const char *yn = XGetAtomName(d, b->serve.ptype);
            char hs[512], ht[512], hp[512], hy[512];
            hexenc(sn ? sn : "", hs, sizeof hs);
            hexenc(tn ? tn : "", ht, sizeof ht);
            hexenc(pn ? pn : "", hp, sizeof hp);
            hexenc(yn ? yn : "", hy, sizeof hy);
            if (sn) XFree((char *)sn);
            if (tn) XFree((char *)tn);
            if (pn) XFree((char *)pn);
            if (yn) XFree((char *)yn);
            char head[1700];
            int hl = snprintf(head, sizeof head, "D %s %s %s %s %d %lu %zu\n",
                              hs, ht, hp, hy, b->serve.pformat,
                              b->serve.pnitems, b->serve.pnbytes);
            ssize_t n1 = send(b->serve.fd, head, (size_t)hl, MSG_NOSIGNAL);
            char *hex = malloc(b->serve.pnbytes * 2 + 2);
            if (hex) {
                static const char *hx = "0123456789abcdef";
                for (size_t i = 0; i < b->serve.pnbytes; i++) {
                    hex[i * 2] = hx[(b->serve.pdata[i] >> 4) & 0xf];
                    hex[i * 2 + 1] = hx[b->serve.pdata[i] & 0xf];
                }
                hex[b->serve.pnbytes * 2] = '\n';
                ssize_t n2 = send(b->serve.fd, hex, b->serve.pnbytes * 2 + 1, MSG_NOSIGNAL);
                (void)n2;
                free(hex);
            }
            (void)n1;
        }
        close(b->serve.fd);
    }
    b->serve.fd = -1;
    b->serve.active = false;
    b->serve.got_prop = false;
    free(b->serve.pdata);
    b->serve.pdata = NULL;
    b->serve.pnbytes = b->serve.pnitems = 0;
    if (motif) mw_dnd_serve_done(d);
}

static void serve_read(Display *d);

/* Accept a connection and start servicing a CONVERT. */
static void serve_accept(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    MwBroker *b = dp->broker;
    if (b->serve.active || b->accept_fd >= 0) return;   /* one at a time */
    int fd = accept4(b->listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) return;
    TR("serve_accept fd=%d\n", fd);
    b->accept_fd = fd;
    b->accept_len = 0;
    serve_read(d);
}

/* Read a request off an accepted connection.  The request may not have arrived
 * yet (the peer connects and then writes): keep the fd and finish on a later
 * pump rather than closing it, which would make the peer's write fail with
 * EPIPE/SIGPIPE and kill it. */
static void serve_read(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    MwBroker *b = dp->broker;
    int fd = b->accept_fd;
    if (fd < 0) return;

    bool complete = false;
    for (;;) {
        ssize_t n = read(fd, b->accept_buf + b->accept_len,
                         sizeof b->accept_buf - 1 - b->accept_len);
        if (n > 0) {
            b->accept_len += (size_t)n;
            b->accept_buf[b->accept_len] = 0;
            if (strchr(b->accept_buf, '\n')) { complete = true; break; }
            if (b->accept_len >= sizeof b->accept_buf - 1) { complete = true; break; }
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;  /* wait */
        break;   /* EOF or error */
    }

    if (!complete) { close(fd); b->accept_fd = -1; b->accept_len = 0; return; }

    char hs[512] = "", ht[512] = "", hp[512] = "";
    if (b->accept_buf[0] != 'C' || b->accept_buf[1] != ' ' ||
        sscanf(b->accept_buf + 2, "%511s %511s %511s", hs, ht, hp) != 3) {
        close(fd);
        b->accept_fd = -1;
        b->accept_len = 0;
        return;
    }

    char selname[256] = "", tname[256] = "", pname[256] = "";
    hexdec(hs, (unsigned char *)selname, sizeof selname - 1);
    hexdec(ht, (unsigned char *)tname, sizeof tname - 1);
    hexdec(hp, (unsigned char *)pname, sizeof pname - 1);

    Atom sel = XInternAtom(d, selname, False);
    Atom tgt = XInternAtom(d, tname, False);
    Atom prop = pname[0] ? XInternAtom(d, pname, False) : None;
    Window owner = XGetSelectionOwner(d, sel);
    if (owner == None || (dp->clip_window != None && owner == dp->clip_window)) {
        close(fd);
        b->accept_fd = -1;
        b->accept_len = 0;
        return;
    }

    if (b->serve.proxy_win == None)
        b->serve.proxy_win = XCreateSimpleWindow(d, DefaultRootWindow(d),
                                                 0, 0, 1, 1, 0, 0, 0);
    if (!b->proxy_prop) {
        char pn[64];
        snprintf(pn, sizeof pn, "_XLIB_WAYLAND_SEL_%d", b->mypid);
        b->proxy_prop = XInternAtom(d, pn, False);
    }

    b->serve.active = true;
    b->serve.fd = fd;
    b->accept_fd = -1;          /* ownership moved to serve */
    b->accept_len = 0;
    b->serve.selection = sel;
    b->serve.target = tgt;
    b->serve.property = prop;
    b->serve.got_prop = false;
    b->serve.deadline_ms = now_ms() + MWB_DEADLINE_MS;

    XSelectionRequestEvent re;
    memset(&re, 0, sizeof re);
    re.type = SelectionRequest;
    re.display = d;
    re.owner = owner;
    re.requestor = b->serve.proxy_win;
    re.selection = sel;
    re.target = tgt;
    re.property = b->proxy_prop;
    re.time = CurrentTime;
    TR("posting SelectionRequest sel=%lu target=%lu\n", sel, tgt);
    mw_put_event(d, (XEvent *)&re);
}

/* The owner's converter wrote the property: capture it. */
bool mw_broker_capture_prop(Display *d, Window w, Atom property, Atom type,
                            int format, const unsigned char *data,
                            unsigned long nitems)
{
    MwBroker *b = MWD(d)->broker;
    if (!b || !b->serve.active) return false;
    if (w != b->serve.proxy_win || property != b->proxy_prop) return false;
    free(b->serve.pdata);
    b->serve.pdata = NULL;
    b->serve.pnbytes = 0;
    if (data) {
        size_t bytes = (size_t)format / 8 * nitems;
        b->serve.pdata = malloc(bytes ? bytes : 1);
        if (b->serve.pdata) { memcpy(b->serve.pdata, data, bytes); b->serve.pnbytes = bytes; }
    }
    b->serve.ptype = type;
    b->serve.pformat = format;
    b->serve.pnitems = nitems;
    b->serve.got_prop = true;
    TR("captured prop type=%lu fmt=%d n=%lu\n", type, format, nitems);
    return true;
}

/* The owner's converter sent the SelectionNotify: finish the transfer. */
bool mw_broker_serve_notify(Display *d, XSelectionEvent *se)
{
    MwBroker *b = MWD(d)->broker;
    if (!b || !b->serve.active) return false;
    if (se->requestor != b->serve.proxy_win) return false;
    bool refuse = (se->property == None);
    TR("serve notify refuse=%d\n", refuse);
    serve_finish(d, refuse);
    return true;
}

/* A reply (DATA/REFUSE) arrived for our convert request. */
static void xfer_read(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    MwBroker *b = dp->broker;
    char *buf = b->xfer.buf;
    size_t *len = &b->xfer.len;
    TR("xfer_read fd=%d len=%zu\n", b->xfer.fd, *len);
    for (;;) {
        ssize_t n = read(b->xfer.fd, buf + *len, sizeof b->xfer.buf - 1 - *len);
        if (n > 0) {
            *len += (size_t)n;
            buf[*len] = 0;
            char *h = strchr(buf, '\n');
            /* REFUSE is one line; DATA is a header line plus a hex data line,
             * so wait for the second newline before parsing. */
            if (h && (buf[0] == 'R' || strchr(h + 1, '\n')))
                break;
            if (*len >= sizeof b->xfer.buf - 1) break;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        break;              /* EAGAIN/EOF: wait for the next pump */
    }

    char *nl = strchr(buf, '\n');
    bool complete = nl && (buf[0] == 'R' || strchr(nl + 1, '\n'));
    if (!complete) {
        if (now_ms() > b->xfer.deadline_ms) {
            /* give up */
            close(b->xfer.fd);
            b->xfer_fd = -1; b->xfer.fd = -1; b->xfer.active = false;
            XSelectionEvent se; memset(&se, 0, sizeof se);
            se.type = SelectionNotify; se.display = d;
            se.requestor = b->xfer.requestor; se.selection = b->xfer.selection;
            se.target = b->xfer.target; se.property = None; se.time = b->xfer.time;
            mw_put_event(d, (XEvent *)&se);
            b->xfer.len = 0;
            xfer_next(d);
        }
        return;
    }

    Atom sel = b->xfer.selection, tgt = b->xfer.target, prop = b->xfer.property;
    Window req = b->xfer.requestor;
    Time t = b->xfer.time;

    if (buf[0] == 'R') {
        /* refused */
    } else if (buf[0] == 'D') {
        char hs[512], ht[512], hp[512], hy[512];
        int  fmt = 8;
        unsigned long nit = 0;
        size_t nbytes = 0;
        char *p = buf + 2;
        /* D <sel> <target> <prop> <type> <format> <nitems> <nbytes> <hex>\n */
        if (sscanf(p, "%511s %511s %511s %511s %d %lu %zu",
                   hs, ht, hp, hy, &fmt, &nit, &nbytes) == 7) {
            char *hx = strchr(nl, '\n');
            (void)hx;
            char *hex = nl + 1;
            unsigned char *data = malloc(nbytes ? nbytes : 1);
            char tname[256] = "";
            hexdec(hy, (unsigned char *)tname, sizeof tname - 1);
            char tgname[256] = "";
            hexdec(ht, (unsigned char *)tgname, sizeof tgname - 1);
            TR("xfer D target=%s type=%s fmt=%d nitems=%lu nbytes=%zu hexlen=%zu\n",
               tgname, tname, fmt, nit, nbytes, strlen(hex));

            /* Replies echo the request's target in the first field of the
             * header.  If it does not match what we asked for, this is a stale
             * reply (an earlier conversion's) that arrived on this socket;
             * ignore it and ask the owner again rather than handing the wrong
             * bytes to the application. */
            const char *rn = XGetAtomName(d, b->xfer.target);
            char reqname[128] = "";
            if (rn) { snprintf(reqname, sizeof reqname, "%s", rn); XFree((char *)rn); }
            if (reqname[0] && strcmp(reqname, tgname) != 0) {
                TR("xfer stale reply (want %s); retry %d\n", reqname, b->xfer.retries);
                free(data);
                Atom s = b->xfer.selection, tg = b->xfer.target;
                Atom pr = b->xfer.property;
                Window rq = b->xfer.requestor;
                Time tm = b->xfer.time;
                int tries = ++b->xfer.retries;
                close(b->xfer.fd);
                b->xfer_fd = -1; b->xfer.fd = -1; b->xfer.active = false;
                b->xfer.len = 0;
                if (tries <= 3 &&
                    (xfer_start(d, s, tg, pr, rq, tm),
                     b->xfer.active)) {
                    b->xfer.retries = tries;   /* xfer_start resets it */
                    return;
                }
                XSelectionEvent se;
                memset(&se, 0, sizeof se);
                se.type = SelectionNotify; se.display = d;
                se.requestor = rq; se.selection = s; se.target = tg;
                se.property = None; se.time = tm;
                mw_put_event(d, (XEvent *)&se);
                xfer_next(d);
                return;
            }
            if (data) {
                size_t got = hexdec(hex, data, nbytes);
                TR("xfer data target=%s type=%s fmt=%d nitems=%lu nbytes=%zu got=%zu\n",
                   tgname, tname, fmt, nit, nbytes, got);
                Atom type = XInternAtom(d, tname[0] ? tname : "STRING", False);
                if (got > 0 && fmt == 32) {
                    /* Xlib's format-32 interface takes an array of longs. */
                    long *l = malloc(sizeof(long) * (nit ? nit : 1));
                    if (l) {
                        for (unsigned long i = 0; i < nit; i++) {
                            uint32_t v; memcpy(&v, data + i * 4, 4);
                            l[i] = (long)v;
                        }
                        XChangeProperty(d, req, prop, type, 32, PropModeReplace,
                                        (const unsigned char *)l, (int)nit);
                        free(l);
                    }
                } else if (got > 0) {
                    XChangeProperty(d, req, prop, type, fmt, PropModeReplace,
                                    data, (int)nit);
                }
                free(data);
            }
        }
    }
    /* post the SelectionNotify */
    bool ok = (buf[0] == 'D');
    XSelectionEvent se;
    memset(&se, 0, sizeof se);
    se.type = SelectionNotify;
    se.display = d;
    se.requestor = req;
    se.selection = sel;
    se.target = tgt;
    se.property = ok ? prop : None;
    se.time = t;
    mw_put_event(d, (XEvent *)&se);
    TR("xfer posted notify req=%lu prop=%lu qcount=%d\n", req, prop, dp->qcount);

    TR("xfer reply %c ok=%d sel=%lu target=%lu\n", buf[0], ok,
       (unsigned long)sel, (unsigned long)tgt);
    close(b->xfer.fd);
    b->xfer_fd = -1;
    b->xfer.fd = -1;
    b->xfer.active = false;
    b->xfer.len = 0;
    xfer_next(d);
}

int mw_broker_poll_fd(Display *d)
{
    MwBroker *b = MWD(d)->broker;
    if (!b) return -1;
    if (b->accept_fd >= 0) return b->accept_fd;
    if (b->xfer.active) return b->xfer.fd;
    return b->listen_fd;
}

void mw_broker_handle_ready(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    MwBroker *b = dp->broker;
    if (!b) return;
    if (b->xfer.active || b->serve.active || b->accept_fd >= 0)
        TR("handle_ready xfer=%d serve=%d\n", b->xfer.active, b->serve.active);
    if (b->xfer.active) xfer_read(d);
    if (b->accept_fd >= 0) serve_read(d);
    /* Drop a stalled serve before accepting anything new: otherwise one
     * conversion whose owner never answers blocks every later request (the
     * accept is refused and the client waits forever). */
    if (b->serve.active && now_ms() > b->serve.deadline_ms)
        serve_finish(d, true);
    if (b->listen_fd >= 0) serve_accept(d);
}
