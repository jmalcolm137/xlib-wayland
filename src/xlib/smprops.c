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

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#define SMP_MAX 65536

static const char *smp_path(char *buf, size_t n)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !*rt) rt = "/tmp";
    snprintf(buf, n, "%s/xlib-wayland/smprops", rt);
    return buf;
}

static bool smp_interest(const char *name)
{
    const char *env = getenv("XLIB_WAYLAND_SHARE_PROPERTIES");
    if (!env || !*env || !name) return false;
    size_t len = strlen(name);
    char  *list = strdup(env);
    bool   ok = false;
    for (char *t = strtok(list, ","); t && !ok; t = strtok(NULL, ",")) {
        while (*t == ' ') t++;
        size_t l = strlen(t);
        if (l && len >= l && strncmp(name, t, l) == 0) ok = true;
    }
    free(list);
    return ok;
}

bool mw_smprop_is_shared(Display *d, Atom prop)
{
    const char *name = XGetAtomName(d, prop);
    if (!name) return false;
    bool ok = smp_interest(name);
    XFree((char *)name);
    return ok;
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
    const char *pname = XGetAtomName(d, prop);
    const char *tname = XGetAtomName(d, type);
    if (!pname) return;
    size_t bytes = (size_t)format / 8 * nitems;
    if (bytes > SMP_MAX / 2) { XFree((char *)pname); if (tname) XFree((char *)tname); return; }

    char  *hex = malloc(bytes * 2 + 1);
    char  *line = malloc(strlen(pname) + strlen(tname ? tname : "") + bytes * 2 + 64);
    if (!hex || !line) { free(hex); free(line); XFree((char *)pname); if (tname) XFree((char *)tname); return; }
    hexenc(data, bytes, hex);
    int ll = sprintf(line, "%s\t%s\t%d\t%lu\t%s\n", pname, tname ? tname : "",
                     format, nitems, hex);
    if (ll < 0) ll = 0;

    char path[256];
    smp_path(path, sizeof path);
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd >= 0) {
        flock(fd, LOCK_EX);
        char  *text = NULL;
        size_t cap = 0, len = 0;
        char   chunk[512];
        ssize_t n;
        while ((n = read(fd, chunk, sizeof chunk)) > 0) {
            if (len + (size_t)n + 1 > cap) {
                cap = (len + (size_t)n + 1) * 2;
                char *t = realloc(text, cap);
                if (!t) break;
                text = t;
            }
            memcpy(text + len, chunk, (size_t)n);
            len += (size_t)n;
        }
        if (!text) { text = malloc(1); }
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

/* Look up a shared property.  On success the caller owns *data (XFree it). */
bool mw_smprop_lookup(Display *d, Atom prop, Atom *type, int *format,
                      unsigned long *nitems, unsigned char **data)
{
    if (!mw_smprop_is_shared(d, prop)) return false;
    const char *pname = XGetAtomName(d, prop);
    if (!pname) return false;

    char path[256];
    smp_path(path, sizeof path);
    int fd = open(path, O_RDONLY, 0600);
    bool found = false;
    if (fd >= 0) {
        char   buf[SMP_MAX];
        ssize_t n = read(fd, buf, sizeof buf - 1);
        close(fd);
        if (n > 0) {
            buf[n] = 0;
            size_t pn = strlen(pname);
            char  *p = buf;
            while (*p) {
                char *nl = strchr(p, '\n');
                size_t sl = nl ? (size_t)(nl - p) : strlen(p);
                if (sl > pn && p[pn] == '\t' && strncmp(p, pname, pn) == 0) {
                    p[sl] = 0;
                    char *f = p + pn + 1;
                    char *tn = strsep(&f, "\t");
                    char *fm = strsep(&f, "\t");
                    char *ni = strsep(&f, "\t");
                    char *hx = f;
                    if (tn && fm && ni && hx) {
                        int fmt = atoi(fm);
                        unsigned long nit = strtoul(ni, NULL, 10);
                        Atom t = XInternAtom(d, tn, False);
                        size_t bytes = (size_t)fmt / 8 * nit;
                        unsigned char *out = malloc(bytes ? bytes : 1);
                        if (out) {
                            size_t got = hexdec(hx, out, bytes);
                            if (got == bytes || bytes == 0) {
                                if (type) *type = t;
                                if (format) *format = fmt;
                                if (nitems) *nitems = nit;
                                *data = out;
                                found = true;
                            } else {
                                free(out);
                            }
                        }
                    }
                    break;
                }
                p = nl ? nl + 1 : p + sl;
            }
        }
    }
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
