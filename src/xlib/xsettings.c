/* xsettings.c — provide the XSETTINGS manager the shim otherwise lacks.
 *
 * GDK's X11 backend reads many GTK settings — theme, font, icon theme, Xft
 * hinting/DPI, double-click and drag thresholds — from the XSETTINGS protocol:
 * the owner of the `_XSETTINGS_S<n>` selection exposes a `_XSETTINGS_SETTINGS`
 * property on its manager window and GDK watches that window for changes.
 *
 * There is no settings daemon inside a shim process, so without this every GTK2
 * client fell back to gtkrc/defaults.  The shim now becomes the manager itself:
 * it owns `_XSETTINGS_S<screen>` and publishes the settings read from a small
 * config file, re-publishing (with a new serial) when the file changes so GDK
 * clients update live.  A real settings daemon running in the same process can
 * still take the selection over; we only (re)assert ownership when nobody else
 * holds it.
 *
 * Config file (first match wins):
 *   $XLIB_WAYLAND_XSETTINGS
 *   $XDG_CONFIG_HOME/xlib-wayland/xsettings
 *   $HOME/.config/xlib-wayland/xsettings
 *
 * Format, one setting per line; `#` starts a comment:
 *
 *   Net/ThemeName   = TraditionalOk
 *   Gtk/FontName    = Sans 10
 *   Gtk/ToolbarStyle = 3
 *   Xft/DPI         = 98304
 *
 * A value is an integer when it is entirely a number (base 0), otherwise a
 * string; wrap it in double quotes to force a string.  Only settings present in
 * the file are published, so an empty/absent file changes nothing.
 */
#include "internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* XSETTINGS value types (from the protocol / GDK's xsettings-common.h). */
#define XS_TYPE_INT    0
#define XS_TYPE_STRING 1

#define XS_RECHECK_MS 500   /* how often the pump stats the config file */

typedef struct MwXSetting {
    char *name;
    int   type;
    long  i;
    char *s;
} MwXSetting;

typedef struct MwXSettings MwXSettings;

struct MwXSettings {
    Display    *d;
    Window      win;
    Atom        sel;
    Atom        prop;
    uint32_t    serial;

    MwXSetting *items;
    int         n, cap;

    char       *path;
    bool        have_file;
    time_t      mtime;
    off_t       size;
    uint64_t    next_check_ms;
};

/* ------------------------------------------------------------- byte buffer */

typedef struct { unsigned char *p; size_t n, cap; } XBuf;

static void xb_need(XBuf *b, size_t n)
{
    if (b->n + n <= b->cap) return;
    size_t c = b->cap ? b->cap : 256;
    while (c < b->n + n) c *= 2;
    b->p = realloc(b->p, c);
    b->cap = c;
}

static void xb8(XBuf *b, unsigned v)  { xb_need(b, 1); b->p[b->n++] = (unsigned char)v; }

static void xb16(XBuf *b, unsigned v)
{
    xb_need(b, 2);
    uint16_t x = (uint16_t)v;
    memcpy(b->p + b->n, &x, 2);
    b->n += 2;
}

static void xb32(XBuf *b, uint32_t v)
{
    xb_need(b, 4);
    memcpy(b->p + b->n, &v, 4);
    b->n += 4;
}

/* A length-prefixed byte run padded to a 4-byte boundary (the encoding both the
 * setting name and a string value use). */
static void xb_pad(XBuf *b, const char *s, size_t len)
{
    xb_need(b, len);
    memcpy(b->p + b->n, s, len);
    b->n += len;
    size_t pad = (4 - (len & 3)) & 3;
    for (size_t i = 0; i < pad; i++) xb8(b, 0);
}

static int host_little(void)
{
    uint32_t v = 1;
    return *(unsigned char *)&v == 1;
}

/* The `_XSETTINGS_SETTINGS` byte stream, in host byte order (the first byte
 * tells the reader which that is). */
static unsigned char *xsettings_build(MwXSettings *x, size_t *out_len)
{
    XBuf b;
    memset(&b, 0, sizeof b);
    xb8(&b, host_little() ? LSBFirst : MSBFirst);
    xb8(&b, 0); xb8(&b, 0); xb8(&b, 0);
    xb32(&b, x->serial);
    xb32(&b, (uint32_t)x->n);
    for (int i = 0; i < x->n; i++) {
        MwXSetting *it = &x->items[i];
        size_t nl = strlen(it->name);
        xb8(&b, (unsigned)it->type);
        xb8(&b, 0);
        xb16(&b, (unsigned)nl);
        xb_pad(&b, it->name, nl);
        xb32(&b, x->serial);                 /* last_change_serial */
        if (it->type == XS_TYPE_INT) {
            xb32(&b, (uint32_t)it->i);
        } else {
            size_t sl = it->s ? strlen(it->s) : 0;
            xb32(&b, (uint32_t)sl);
            xb_pad(&b, it->s ? it->s : "", sl);
        }
    }
    *out_len = b.n;
    return b.p;
}

/* --------------------------------------------------------------- settings */

static void xsetting_free(MwXSetting *it)
{
    free(it->name);
    free(it->s);
}

static void xsettings_clear(MwXSettings *x)
{
    for (int i = 0; i < x->n; i++) xsetting_free(&x->items[i]);
    free(x->items);
    x->items = NULL;
    x->n = x->cap = 0;
}

static void xsetting_set(MwXSettings *x, const char *name, int type,
                         long i, const char *s)
{
    MwXSetting *it;
    for (int k = 0; k < x->n; k++)
        if (strcmp(x->items[k].name, name) == 0) {
            it = &x->items[k];
            free(it->s);
            it->type = type; it->i = i; it->s = s ? strdup(s) : NULL;
            return;
        }
    if (x->n == x->cap) {
        int cap = x->cap ? x->cap * 2 : 16;
        MwXSetting *n = realloc(x->items, (size_t)cap * sizeof *n);
        if (!n) return;
        x->items = n;
        x->cap = cap;
    }
    it = &x->items[x->n++];
    memset(it, 0, sizeof *it);
    it->name = strdup(name);
    it->type = type;
    it->i = i;
    it->s = s ? strdup(s) : NULL;
}

static void xsettings_parse_line(MwXSettings *x, char *line)
{
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;

    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    char *end = p + strlen(p);
    while (end > p && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\n' || end[-1] == '\r')) *--end = 0;
    if (!*p) return;

    char *eq = strchr(p, '=');
    if (!eq) return;
    *eq = 0;
    char *name = p;
    char *ne = name + strlen(name);
    while (ne > name && (ne[-1] == ' ' || ne[-1] == '\t')) *--ne = 0;
    if (!*name) return;

    char *val = eq + 1;
    while (*val == ' ' || *val == '\t') val++;
    char *ve = val + strlen(val);
    while (ve > val && (ve[-1] == ' ' || ve[-1] == '\t')) *--ve = 0;
    if (!*val) return;

    int is_string = 0;
    if (val[0] == '"') {
        is_string = 1;
        val++;
        size_t l = strlen(val);
        if (l && val[l - 1] == '"') val[l - 1] = 0;
    }

    if (!is_string) {
        char *numend = NULL;
        errno = 0;
        long v = strtol(val, &numend, 0);
        if (numend && *numend == 0 && errno == 0) {
            xsetting_set(x, name, XS_TYPE_INT, v, NULL);
            return;
        }
    }
    xsetting_set(x, name, XS_TYPE_STRING, 0, val);
}

static void xsettings_load(MwXSettings *x)
{
    xsettings_clear(x);
    x->have_file = false;

    struct stat st;
    if (!x->path || stat(x->path, &st) != 0) return;
    x->have_file = true;
    x->mtime = st.st_mtime;
    x->size = st.st_size;

    FILE *f = fopen(x->path, "r");
    if (!f) return;
    char line[2048];
    while (fgets(line, sizeof line, f))
        xsettings_parse_line(x, line);
    fclose(f);
}

static void xsettings_publish(MwXSettings *x)
{
    size_t len = 0;
    unsigned char *b = xsettings_build(x, &len);
    if (!b) return;
    XChangeProperty(x->d, x->win, x->prop, x->prop, 8, PropModeReplace,
                    b, (int)len);
    free(b);
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: XSETTINGS serial=%u entries=%d\n", x->serial, x->n);
}

/* Re-publish only while we still own the selection: a settings daemon in this
 * process is free to take it over. */
static bool xsettings_owns(MwXSettings *x)
{
    Window owner = XGetSelectionOwner(x->d, x->sel);
    return owner == None || owner == x->win;
}

static const char *xsettings_path(void)
{
    const char *p = getenv("XLIB_WAYLAND_XSETTINGS");
    if (p && *p) return p;

    static char buf[1024];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
        snprintf(buf, sizeof buf, "%s/xlib-wayland/xsettings", xdg);
        return buf;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) return NULL;
    snprintf(buf, sizeof buf, "%s/.config/xlib-wayland/xsettings", home);
    return buf;
}

/* A high XID the sequential allocator will never reach, so creating the
 * manager window does not shift the ids of the application's own windows (the
 * golden tests reference them). */
#define XS_MANAGER_ID 0x00F00000UL

static Window xsettings_make_window(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    XID saved = dp->next_id;
    MwWindow *win = mw_create_window(d, DefaultRootWindow(d), 0, 0, 1, 1, 0, 0,
                                     InputOnly, &dp->visual, 0, NULL);
    if (!win) { dp->next_id = saved; return None; }
    mw_unregister(d, win->id);          /* keeps the object, drops the id */
    win->id = XS_MANAGER_ID;
    mw_register(d, win->id, MW_OBJ_WINDOW, win);
    dp->next_id = saved;                /* give the sequential space back */
    return win->id;
}

void mw_xsettings_init(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (dp->xsettings) return;

    MwXSettings *x = calloc(1, sizeof *x);
    if (!x) return;
    x->d = d;
    x->serial = 1;

    char name[64];
    snprintf(name, sizeof name, "_XSETTINGS_S%d", DefaultScreen(d));
    x->sel = XInternAtom(d, name, False);
    x->prop = XInternAtom(d, "_XSETTINGS_SETTINGS", False);

    x->win = xsettings_make_window(d);
    const char *path = xsettings_path();
    x->path = path ? strdup(path) : NULL;

    xsettings_load(x);
    XSetSelectionOwner(d, x->sel, x->win, CurrentTime);
    xsettings_publish(x);
    dp->xsettings = x;
}

void mw_xsettings_fini(Display *d)
{
    MwXSettings *x = MWD(d)->xsettings;
    if (!x) return;
    /* The window table is already torn down by XCloseDisplay; do not touch it. */
    xsettings_clear(x);
    free(x->path);
    free(x);
    MWD(d)->xsettings = NULL;
}

void mw_xsettings_pump(Display *d)
{
    MwXSettings *x = MWD(d)->xsettings;
    if (!x) return;

    uint64_t now = (uint64_t)mw_now();
    if (now < x->next_check_ms) return;
    x->next_check_ms = now + XS_RECHECK_MS;

    struct stat st;
    int have = x->path && stat(x->path, &st) == 0;
    bool changed;
    if (have != x->have_file) changed = true;
    else if (have && ((time_t)st.st_mtime != x->mtime || (off_t)st.st_size != x->size)) changed = true;
    else changed = false;
    if (!changed) return;

    xsettings_load(x);
    if (!xsettings_owns(x)) return;
    x->serial++;
    xsettings_publish(x);
}
