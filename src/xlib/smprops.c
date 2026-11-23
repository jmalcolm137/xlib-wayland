/* smprops.c — share the session manager's root/window properties.
 *
 * A different facet of the same problem as broker.c: each shim process is its
 * own X server, so properties the session manager puts on its root window and on
 * its top-level window are invisible to every other client.  dtstyle's Style
 * Manager reads _DT_SM_WINDOW_INFO off the root and then _DT_SM_STATE_INFO /
 * _DT_SM_SAVER_INFO off that window; with nothing there it warns that it could
 * not obtain screen-saver information from the session manager.
 *
 * When XLIB_WAYLAND_SHARE_PROPERTIES is set (a comma-separated list of property
 * name prefixes, e.g. "_DT_SM_"), a client that writes such a property also
 * publishes it to a small file under XDG_RUNTIME_DIR, and any client that reads
 * a shared property it does not have locally is served from that file.  The
 * _DT_SM_WINDOW_INFO value carries the manager's window id, which is meaningless
 * in another process, so it is replaced with a local hidden window.
 */
#define _GNU_SOURCE
#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#define SMP_MAX       65536              /* largest property value, bytes */
#define SMP_FILE_MAX  (8u << 20)         /* largest store file read */

/* The store's path in the validated runtime directory; false if none. */
static bool smp_path(char *buf, size_t n)
{
    char dir[200];
    if (!mw_runtime_dir(dir, sizeof dir)) return false;
    int w = snprintf(buf, n, "%s/smprops", dir);
    return w > 0 && (size_t)w < n;
}

/* The whole store, NUL-terminated, read under a shared lock (a writer
 * truncates and rewrites it under an exclusive one).  NULL if absent. */
static char *smp_read_all(void)
{
    char path[256];
    if (!smp_path(path, sizeof path)) return NULL;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return NULL;
    flock(fd, LOCK_SH);
    char *text = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len + 4096 + 1 > cap) {
            if (cap >= SMP_FILE_MAX) break;
            size_t ncap = cap ? cap * 2 : 16384;
            char *t = realloc(text, ncap);
            if (!t) break;
            text = t;
            cap = ncap;
        }
        ssize_t n = read(fd, text + len, cap - 1 - len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        len += (size_t)n;
    }
    flock(fd, LOCK_UN);
    close(fd);
    if (text) text[len] = 0;
    return text;
}

static bool smp_interest(const char *name)
{
    const char *env = getenv("XLIB_WAYLAND_SHARE_PROPERTIES");
    if (!env || !*env || !name) return false;
    size_t len = strlen(name);
    const char *p = env;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        const char *q = p;
        while (*q && *q != ',') q++;
        size_t l = (size_t)(q - p);
        while (l > 0 && p[l - 1] == ' ') l--;
        if (l > 0 && len >= l && strncmp(name, p, l) == 0) return true;
        p = *q ? q + 1 : q;
    }
    return false;
}

bool mw_smprop_is_shared(Display *d, Atom prop)
{
    const char *env = getenv("XLIB_WAYLAND_SHARE_PROPERTIES");
    if (!env || !*env) return false;
    const char *name = mw_atom_name(d, prop);
    return name ? smp_interest(name) : false;
}

static const char *hexc = "0123456789abcdef";
static void hexenc(const unsigned char *in, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = hexc[(in[i] >> 4) & 0xf];
        out[i * 2 + 1] = hexc[in[i] & 0xf];
    }
    out[n * 2] = 0;
}
static int hexv(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static size_t hexdec(const char *in, unsigned char *out, size_t outsz)
{
    size_t n = 0;
    while (in[0] && in[1] && n < outsz) {
        int h = hexv(in[0]), l = hexv(in[1]);
        if (h < 0 || l < 0) break;
        out[n++] = (unsigned char)((h << 4) | l);
        in += 2;
    }
    return n;
}

void mw_smprop_publish(Display *d, Atom prop, Atom type, int format,
                       const unsigned char *data, unsigned long nitems)
{
    if (!data || !mw_smprop_is_shared(d, prop)) return;
    if (format != 8 && format != 16 && format != 32) return;
    const char *pname = XGetAtomName(d, prop);
    const char *tname = XGetAtomName(d, type);
    if (!pname || strpbrk(pname, "\t\r\n") ||
        (tname && strpbrk(tname, "\t\r\n"))) {
        if (pname) XFree((char *)pname);
        if (tname) XFree((char *)tname);
        return;
    }
    size_t unit = (size_t)format / 8;
    if (nitems > (SMP_MAX / 2) / unit) { XFree((char *)pname); if (tname) XFree((char *)tname); return; }
    size_t bytes = unit * nitems;

    char  *hex = malloc(bytes * 2 + 1);
    size_t line_cap = strlen(pname) + strlen(tname ? tname : "") + bytes * 2 + 64;
    char  *line = malloc(line_cap);
    if (!hex || !line) { free(hex); free(line); XFree((char *)pname); if (tname) XFree((char *)tname); return; }
    hexenc(data, bytes, hex);
    int ll = snprintf(line, line_cap, "%s\t%s\t%d\t%lu\t%s\n", pname, tname ? tname : "",
                      format, nitems, hex);
    if (ll < 0 || (size_t)ll >= line_cap) ll = 0;

    char path[256];
    int fd = smp_path(path, sizeof path)
             ? open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600) : -1;
    if (fd >= 0) {
        flock(fd, LOCK_EX);
        char  *text = NULL;
        size_t cap = 0, len = 0;
        char   chunk[512];
        ssize_t n;
        bool oom = false;
        while ((n = read(fd, chunk, sizeof chunk)) > 0) {
            if (len + (size_t)n + 1 > SMP_FILE_MAX) { oom = true; break; }
            if (len + (size_t)n + 1 > cap) {
                cap = (len + (size_t)n + 1) * 2;
                char *t = realloc(text, cap);
                if (!t) { oom = true; break; }
                text = t;
            }
            memcpy(text + len, chunk, (size_t)n);
            len += (size_t)n;
        }
        if (oom) { free(text); text = NULL; }
        else if (!text) { text = malloc(1); }
        if (text) {
            text[len] = 0;
            size_t pn = strlen(pname);
            char  *out = malloc(len + (size_t)ll + 1);
            size_t olen = 0;
            if (out) {
                char *p = text;
                while (*p) {
                    char *nl = strchr(p, '\n');
                    size_t sl = nl ? (size_t)(nl - p) : strlen(p);
                    if (!(sl > pn && p[pn] == '\t' && strncmp(p, pname, pn) == 0)) {
                        memcpy(out + olen, p, sl);
                        olen += sl;
                        out[olen++] = '\n';
                    }
                    p = nl ? nl + 1 : p + sl;
                }
                memcpy(out + olen, line, (size_t)ll);
                olen += (size_t)ll;
                ftruncate(fd, 0);
                lseek(fd, 0, SEEK_SET);
                ssize_t wn = write(fd, out, olen);
                (void)wn;
                free(out);
            }
            free(text);
        }
        flock(fd, LOCK_UN);
        close(fd);
    }
    free(hex);
    free(line);
    XFree((char *)pname);
    if (tname) XFree((char *)tname);
}

/* Does the store text hold a value for this property name? */
static bool smp_has(const char *text, const char *name)
{
    size_t nl = strlen(name);
    for (const char *p = text; p && *p; ) {
        const char *e = strchr(p, '\n');
        size_t sl = e ? (size_t)(e - p) : strlen(p);
        if (sl > nl && p[nl] == '\t' && strncmp(p, name, nl) == 0) return true;
        p = e ? e + 1 : p + sl;
    }
    return false;
}

/* Look up a shared property.  On success the caller owns *data (XFree it). */
bool mw_smprop_lookup(Display *d, Atom prop, Atom *type, int *format,
                      unsigned long *nitems, unsigned char **data)
{
    if (!mw_smprop_is_shared(d, prop)) return false;
    const char *pname = XGetAtomName(d, prop);
    if (!pname) return false;
    char *buf = smp_read_all();
    bool found = false;
    size_t pn = strlen(pname);
    for (char *p = buf; p && *p; ) {
        char *nl = strchr(p, '\n');
        size_t sl = nl ? (size_t)(nl - p) : strlen(p);
        if (!(sl > pn && p[pn] == '\t' && strncmp(p, pname, pn) == 0)) {
            p = nl ? nl + 1 : p + sl;
            continue;
        }
        p[sl] = 0;
        char *f = p + pn + 1;
        char *tn = strsep(&f, "\t");
        char *fm = strsep(&f, "\t");
        char *ni = strsep(&f, "\t");
        char *hx = f;
        if (!tn || !fm || !ni || !hx) break;
        /* The file is input: the format must be a real one and the item
         * count must describe exactly the hex that follows (a wrapped size
         * computation used to be accepted and overflowed the reader). */
        int fmt = atoi(fm);
        char *end;
        unsigned long nit = strtoul(ni, &end, 10);
        if ((fmt != 8 && fmt != 16 && fmt != 32) || *end || nit > SMP_MAX)
            break;
        size_t bytes = (size_t)fmt / 8 * nit;
        if (bytes > SMP_MAX || strlen(hx) != bytes * 2) break;
        unsigned char *out = malloc(bytes ? bytes : 1);
        if (!out) break;
        if (hexdec(hx, out, bytes) != bytes) { free(out); break; }
        if (type) *type = tn[0] ? XInternAtom(d, tn, False) : None;
        if (format) *format = fmt;
        if (nitems) *nitems = nit;
        *data = out;
        found = true;
        break;
    }
    /* dtstyle treats a present _DT_SM_WINDOW_INFO as "the session manager is
     * here", then insists on its state and screen-saver settings and exits if
     * they are missing.  A manager that has not published those is worse than
     * none, so do not offer the window until it has. */
    if (found && strcmp(pname, "_DT_SM_WINDOW_INFO") == 0 &&
        (!smp_has(buf, "_DT_SM_STATE_INFO") || !smp_has(buf, "_DT_SM_SAVER_INFO"))) {
        free(*data);
        *data = NULL;
        found = false;
    }
    free(buf);
    XFree((char *)pname);
    return found;
}

/* A hidden local window to stand in for the session manager's window. */
Window mw_smprop_proxy_window(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (dp->sm_window == None)
        dp->sm_window = XCreateSimpleWindow(d, DefaultRootWindow(d),
                                            0, 0, 1, 1, 0, 0, 0);
    return dp->sm_window;
}
