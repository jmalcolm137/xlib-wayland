/* xresources.c — build the X resource database from the standard sources and
 * publish it as the root window's RESOURCE_MANAGER property.
 *
 * X clients get their defaults from the RESOURCE_MANAGER property (XV reads it
 * directly) and from XResourceManagerString (which Xt/Motif use).  With no X
 * server and no xrdb, nothing populates it, so applications lose all their
 * resource-driven configuration.  We synthesise it from the usual files.
 */
#include "internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>

/* Append n bytes, then a newline unless they already end with one. */
static void append_str(char **buf, size_t *len, size_t *cap,
                       const char *s, size_t n)
{
    if (*len + n + 2 > *cap) {
        size_t ncap = *cap ? *cap * 2 : 4096;
        while (*len + n + 2 > ncap) ncap *= 2;
        char *nb = realloc(*buf, ncap);
        if (!nb) return;
        *buf = nb;
        *cap = ncap;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    if (n == 0 || s[n - 1] != '\n') (*buf)[(*len)++] = '\n';
    (*buf)[*len] = 0;
}

/* A resource file goes in as it is.  Reading it line by line and adding a
 * newline to each (fgets keeps its own) turned "...\\<newline>" into a
 * continuation onto an empty line, cutting every multi-line resource (Motif
 * translation tables) short, and split lines longer than the buffer. */
static void append_file(char **buf, size_t *len, size_t *cap, const char *path)
{
    if (!path || !*path) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char *text = NULL;
    size_t tl = 0, tc = 0;
    for (;;) {
        if (tl + 65536 > tc) {
            size_t nc = tc ? tc * 2 : 65536;
            char *nt = realloc(text, nc);
            if (!nt) break;
            text = nt;
            tc = nc;
        }
        size_t n = fread(text + tl, 1, tc - tl, f);
        if (n == 0) break;
        tl += n;
    }
    fclose(f);
    if (text && tl) append_str(buf, len, cap, text, tl);
    free(text);
}

static void append_dir(char **buf, size_t *len, size_t *cap, const char *dir)
{
    if (!dir) return;
    struct stat st;
    if (stat(dir, &st) != 0) return;
    if (S_ISREG(st.st_mode)) { append_file(buf, len, cap, dir); return; }
    if (!S_ISDIR(st.st_mode)) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char path[PATH_MAX];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat es;
        if (stat(path, &es) == 0 && S_ISREG(es.st_mode))
            append_file(buf, len, cap, path);
    }
    closedir(d);
}

void mw_load_resources(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    char *buf = NULL;
    size_t len = 0, cap = 0;

    char path[PATH_MAX];
    const char *home = getenv("HOME");
    const char *xenv = getenv("XENVIRONMENT");

    /* System-wide defaults first, then per-user (later entries win on
     * conflicting resources in Xrm's last-write-wins model). */
    append_dir(&buf, &len, &cap, "/etc/X11/Xresources");
    append_dir(&buf, &len, &cap, "/usr/share/X11/Xresources");

    if (home) {
        snprintf(path, sizeof path, "%s/.Xresources", home);
        append_file(&buf, &len, &cap, path);
    }
    if (xenv) {
        append_file(&buf, &len, &cap, xenv);
    } else if (home) {
        snprintf(path, sizeof path, "%s/.Xdefaults", home);
        append_file(&buf, &len, &cap, path);
    }

    /* A session resource file that does not depend on the environment of the
     * client.  Every client has its own private display/database here, so a
     * process started without XENVIRONMENT (e.g. an action launched from a
     * File Manager menu) would otherwise lose the session's defaults -- which
     * is what made DtTerm fall back to a single core font and draw only a
     * quarter of each multibyte string.  Applied last so the session wins over
     * the user's generic files. */
    append_file(&buf, &len, &cap, "/etc/xlib-wayland/Xresources");
    append_dir(&buf, &len, &cap, "/usr/share/xlib-wayland/Xresources");
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(path, sizeof path, "%s/xlib-wayland/Xresources", xdg);
        append_file(&buf, &len, &cap, path);
    } else if (home) {
        snprintf(path, sizeof path, "%s/.config/xlib-wayland/Xresources", home);
        append_file(&buf, &len, &cap, path);
    }

    if (!buf) { buf = strdup(""); len = 0; cap = 1; }

    /* The session manager's colour server (dtsession) publishes the CDE palette
     * as RESOURCE_MANAGER.  Every client here runs its own X server, so that
     * write is invisible to the others; it reaches them through the shared
     * property store (see smprops.c).  Merged after the files so the palette
     * wins over the file defaults on conflicting resources, while everything
     * the files set (fonts, per-application defaults) is kept. */
    Atom rm = mw_intern_atom(d, "RESOURCE_MANAGER", False);
    if (mw_smprop_is_shared(d, rm)) {
        Atom st = None;
        int sf = 0;
        unsigned long sn = 0;
        unsigned char *sd = NULL;
        if (mw_smprop_lookup(d, rm, &st, &sf, &sn, &sd)) {
            if (sd && sn > 0)
                append_str(&buf, &len, &cap, (const char *)sd, (size_t)sn);
            free(sd);
        }
    }

    free(dp->xdefaults);
    dp->xdefaults = buf;

    /* Publish as RESOURCE_MANAGER on the root window (what clients read). */
    Window root = MWSCR(d)->root;
    if (root) {
        Atom ra = mw_intern_atom(d, "RESOURCE_MANAGER", False);
        mw_set_prop(d, mw_window(d, root), ra, XA_STRING, 8,
                    (const unsigned char *)buf, (unsigned long)len);
    }
}
