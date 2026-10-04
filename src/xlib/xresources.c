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

static void append_str(char **buf, size_t *len, size_t *cap,
                       const char *s, size_t n)
{
    if (*len + n + 2 > *cap) {
        *cap = (*cap ? *cap * 2 : 4096);
        while (*len + n + 2 > *cap) *cap *= 2;
        *buf = realloc(*buf, *cap);
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[(*len)++] = '\n';
    (*buf)[*len] = 0;
}

static void append_file(char **buf, size_t *len, size_t *cap, const char *path)
{
    if (!path || !*path) return;
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof line, f))
        append_str(buf, len, cap, line, strlen(line));
    fclose(f);
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

    free(dp->xdefaults);
    dp->xdefaults = buf;

    /* Publish as RESOURCE_MANAGER on the root window (what clients read). */
    Window root = MWSCR(d)->root;
    if (root) {
        Atom ra = mw_intern_atom(d, "RESOURCE_MANAGER", False);
        mw_set_prop(d, mw_window(d, root), ra, XA_STRING, 8,
                    (const unsigned char *)buf, (unsigned long)len);
    }

    if (len > 0)
        dp->rdb = XrmGetStringDatabase(buf);
}
