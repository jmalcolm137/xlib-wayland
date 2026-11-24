/* keymap.c — X keycode/keysym mapping backed by xkbcommon. */
#define _GNU_SOURCE
#include "internal.h"

#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <locale.h>
#include <sys/mman.h>
#include <wchar.h>
#include <xkbcommon/xkbcommon-compose.h>

/* Load the compose table for the current locale, so dead keys and multi-key
 * sequences (dead_acute then e -> é, Multi_key then ... ) resolve to their
 * composed character.  Honours $XCOMPOSEFILE via xkbcommon. */
static void mw_compose_init(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (dp->xkb_compose_state || !dp->xkb_ctx) return;

    const char *loc = setlocale(LC_CTYPE, NULL);
    /* An application that never called setlocale() reports "C", which has no
     * compose sequences; fall back to the environment's locale. */
    if (!loc || !*loc || strcmp(loc, "C") == 0 || strcmp(loc, "POSIX") == 0) {
        const char *e = getenv("LC_CTYPE");
        if (!e || !*e) e = getenv("LANG");
        if (e && *e) loc = e;
    }
    if (!loc || !*loc) loc = "C";

    dp->xkb_compose_table = xkb_compose_table_new_from_locale(
        dp->xkb_ctx, loc, XKB_COMPOSE_COMPILE_NO_FLAGS);
    if (dp->xkb_compose_table)
        dp->xkb_compose_state = xkb_compose_state_new(
            dp->xkb_compose_table, XKB_COMPOSE_STATE_NO_FLAGS);
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: compose table %s (locale %s)\n",
                dp->xkb_compose_state ? "loaded" : "absent", loc);
}

static const struct { const char *name; KeySym ks; } ks_table[] = {
#include "keysym_table.h"
};

/* Vendor keysyms (X11/XKeysymDB).  The file is absent on modern systems, but
 * Motif's translation tables reference the osf* names, so we provide them. */
static const struct { const char *name; KeySym ks; } vendor_table[] = {
    { "osfCopy",         0x1004FF02 }, { "osfCut",          0x1004FF03 },
    { "osfPaste",        0x1004FF04 }, { "osfBackTab",      0x1004FF07 },
    { "osfBackSpace",    0x1004FF08 }, { "osfClear",        0x1004FF0B },
    { "osfEscape",       0x1004FF1B }, { "osfAddMode",      0x1004FF31 },
    { "osfPrimaryPaste", 0x1004FF32 }, { "osfQuickPaste",   0x1004FF33 },
    { "osfPageLeft",     0x1004FF40 }, { "osfPageUp",       0x1004FF41 },
    { "osfPageDown",     0x1004FF42 }, { "osfPageRight",    0x1004FF43 },
    { "osfActivate",     0x1004FF44 }, { "osfMenuBar",      0x1004FF45 },
    { "osfLeft",         0x1004FF51 }, { "osfUp",           0x1004FF52 },
    { "osfRight",        0x1004FF53 }, { "osfDown",         0x1004FF54 },
    { "osfEndLine",      0x1004FF57 }, { "osfBeginLine",    0x1004FF58 },
    { "osfEndData",      0x1004FF59 }, { "osfBeginData",    0x1004FF5A },
    { "osfPrevMenu",     0x1004FF5B }, { "osfNextMenu",     0x1004FF5C },
    { "osfPrevField",    0x1004FF5D }, { "osfNextField",    0x1004FF5E },
    { "osfSelect",       0x1004FF60 }, { "osfInsert",       0x1004FF63 },
    { "osfUndo",         0x1004FF65 }, { "osfMenu",         0x1004FF67 },
    { "osfCancel",       0x1004FF69 }, { "osfHelp",         0x1004FF6A },
    { "osfSelectAll",    0x1004FF71 }, { "osfDeselectAll",  0x1004FF72 },
    { "osfReselect",     0x1004FF73 }, { "osfExtend",       0x1004FF74 },
    { "osfRestore",      0x1004FF78 }, { "osfDelete",       0x1004FFFF },
};

KeySym XStringToKeysym(_Xconst char *s)
{
    if (!s) return NoSymbol;
    if (strncmp(s, "XK_", 3) == 0) s += 3;
    for (size_t i = 0; i < sizeof(ks_table) / sizeof(ks_table[0]); i++)
        if (strcmp(ks_table[i].name, s) == 0) return ks_table[i].ks;
    for (size_t i = 0; i < sizeof(vendor_table) / sizeof(vendor_table[0]); i++)
        if (strcmp(vendor_table[i].name, s) == 0) return vendor_table[i].ks;
    if (s[0] && s[1] == 0) return (KeySym)(unsigned char)s[0];  /* single char */
    /* libX11's numeric forms: "U20AC" is the Unicode keysym 0x10020AC (a
     * Latin-1 code point is its own keysym), "0x1008ff13" a raw value. */
    if (s[0] == 'U' && s[1]) {
        char *end;
        unsigned long cp = strtoul(s + 1, &end, 16);
        /* As in libX11, control characters have no Unicode keysym. */
        if (*end == 0 && cp <= 0x10ffff && isxdigit((unsigned char)s[1]) &&
            !(cp < 0x20 || (cp >= 0x7f && cp < 0xa0))) {
            if (cp >= 0x20 && cp <= 0x7e) return cp;
            if (cp >= 0xa0 && cp <= 0xff) return cp;
            return 0x01000000 | cp;
        }
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && s[2]) {
        char *end;
        unsigned long v = strtoul(s + 2, &end, 16);
        if (*end == 0 && v <= 0x1fffffff) return v;
    }
    return NoSymbol;
}

char *XKeysymToString(KeySym ks)
{
    for (size_t i = 0; i < sizeof(ks_table) / sizeof(ks_table[0]); i++)
        if (ks_table[i].ks == ks) return (char *)ks_table[i].name;
    for (size_t i = 0; i < sizeof(vendor_table) / sizeof(vendor_table[0]); i++)
        if (vendor_table[i].ks == ks) return (char *)vendor_table[i].name;
    /* An unnamed Unicode keysym prints as "U20AC", as in libX11.  Each name
     * is made once and kept for the life of the process, so the string stays
     * valid (libX11 instead leaks a fresh copy per call). */
    if (ks >= 0x01000100 && ks <= 0x0110ffff) {
        static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
        /* One allocation per name, so a returned pointer never moves. */
        struct ksname { KeySym ks; char name[12]; };
        static struct ksname **names;
        static size_t nnames, cap;
        char *out = NULL;
        pthread_mutex_lock(&lock);
        for (size_t i = 0; i < nnames && !out; i++)
            if (names[i]->ks == ks) out = names[i]->name;
        if (!out) {
            if (nnames == cap) {
                size_t ncap = cap ? cap * 2 : 16;
                void *nn = realloc(names, ncap * sizeof *names);
                if (nn) { names = nn; cap = ncap; }
            }
            struct ksname *e = nnames < cap ? malloc(sizeof *e) : NULL;
            if (e) {
                unsigned long cp = ks & 0xffffff;
                e->ks = ks;
                snprintf(e->name, sizeof e->name,
                         cp > 0xffff ? "U%06lX" : "U%04lX", cp);
                names[nnames++] = e;
                out = e->name;
            }
        }
        pthread_mutex_unlock(&lock);
        return out;
    }
    return NULL;
}

void mw_keymap_init(Display *d, const char *keymap_str)
{
    XDisplayImpl *dp = MWD(d);
    if (dp->keymap_inited) return;
    dp->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!dp->xkb_ctx) return;
    struct xkb_rule_names names = { 0 };
    if (keymap_str) {
        dp->xkb_keymap = xkb_keymap_new_from_string(
            dp->xkb_ctx, keymap_str, XKB_KEYMAP_FORMAT_TEXT_V1,
            XKB_KEYMAP_COMPILE_NO_FLAGS);
    } else {
        dp->xkb_keymap = xkb_keymap_new_from_names(dp->xkb_ctx, &names,
                                                   XKB_KEYMAP_COMPILE_NO_FLAGS);
    }
    if (dp->xkb_keymap)
        dp->xkb_state = xkb_state_new(dp->xkb_keymap);
    mw_compose_init(d);
    dp->keymap_inited = 1;
    mw_update_modmap(d);
}

void mw_keymap_init_fd(Display *d, int fd, uint32_t size, uint32_t format)
{
    XDisplayImpl *dp = MWD(d);
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || fd < 0) return;
    /* The keymap comes from the compositor; bound its size, and never hand
     * xkbcommon a mapping as a C string -- a page-aligned keymap without a
     * terminating NUL would be read past the end of the mapping. */
    if (size == 0 || size > (16u << 20)) return;
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) return;
    if (!dp->xkb_ctx) dp->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!dp->xkb_ctx) { munmap(map, size); return; }
    /* Compile before replacing: on failure keep the working keymap rather
     * than leave xkb_state pointing at a freed one. */
    struct xkb_keymap *km = xkb_keymap_new_from_buffer(dp->xkb_ctx, map,
                        strnlen(map, size),
                        XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    struct xkb_state *st = km ? xkb_state_new(km) : NULL;
    if (!st) {
        if (km) xkb_keymap_unref(km);
        return;
    }
    if (dp->xkb_keymap) xkb_keymap_unref(dp->xkb_keymap);
    if (dp->xkb_state) xkb_state_unref(dp->xkb_state);
    dp->xkb_keymap = km;
    dp->xkb_state = st;
    mw_compose_init(d);
    dp->keymap_inited = 1;
    /* The modifier map built from the default keymap at XOpenDisplay is stale
     * now (swapped Ctrl/Caps, another layout): rebuild it from this one. */
    free(dp->modmap);
    dp->modmap = NULL;
    mw_update_modmap(d);
}

void mw_keymap_fini(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (dp->xkb_state) xkb_state_unref(dp->xkb_state);
    if (dp->xkb_lookup_state) xkb_state_unref(dp->xkb_lookup_state);
    dp->xkb_lookup_state = NULL;
    dp->xkb_lookup_keymap = NULL;
    if (dp->xkb_keymap) xkb_keymap_unref(dp->xkb_keymap);
    if (dp->xkb_ctx) xkb_context_unref(dp->xkb_ctx);
    if (dp->xkb_compose_state) { xkb_compose_state_unref(dp->xkb_compose_state); dp->xkb_compose_state = NULL; }
    if (dp->xkb_compose_table) { xkb_compose_table_unref(dp->xkb_compose_table); dp->xkb_compose_table = NULL; }
    if (dp->modmap) { free(dp->modmap); dp->modmap = NULL; }
    dp->xkb_state = NULL; dp->xkb_keymap = NULL; dp->xkb_ctx = NULL;
    dp->modmap = NULL; dp->keymap_inited = 0;
}

/* ------------------------------------------------- modifier state <-> xkb
 *
 * An X key event's state is the effective modifier state: the eight core
 * modifiers (Shift, Lock, Control, Mod1-Mod5: Num Lock is usually Mod2, AltGr
 * Mod5) plus the group in bits 13-14.  xkbcommon names the core modifiers
 * "Shift" ... "Mod5", so the two map by name. */
static const char *const core_mod_names[8] = {
    XKB_MOD_NAME_SHIFT, XKB_MOD_NAME_CAPS, XKB_MOD_NAME_CTRL, "Mod1",
    "Mod2", "Mod3", "Mod4", "Mod5"
};

static void ensure_mod_indices(XDisplayImpl *dp)
{
    if (dp->xkb_mod_keymap == dp->xkb_keymap) return;
    for (int i = 0; i < 8; i++)
        dp->xkb_mod_idx[i] = dp->xkb_keymap
            ? xkb_keymap_mod_get_index(dp->xkb_keymap, core_mod_names[i])
            : XKB_MOD_INVALID;
    dp->xkb_mod_keymap = dp->xkb_keymap;
}

/* The core state (modifiers and group) of an xkb state. */
unsigned int mw_core_state(Display *d, struct xkb_state *st)
{
    XDisplayImpl *dp = MWD(d);
    if (!st || !dp->xkb_keymap) return 0;
    ensure_mod_indices(dp);
    xkb_mod_mask_t eff = xkb_state_serialize_mods(st, XKB_STATE_MODS_EFFECTIVE);
    unsigned int m = 0;
    for (int i = 0; i < 8; i++) {
        xkb_mod_index_t idx = dp->xkb_mod_idx[i];
        if (idx != XKB_MOD_INVALID && (eff & (1u << idx))) m |= 1u << i;
    }
    xkb_layout_index_t g = xkb_state_serialize_layout(st, XKB_STATE_LAYOUT_EFFECTIVE);
    return m | ((g & 3u) << 13);
}

/* A private xkb state set from a core state: what a key means under it. */
static struct xkb_state *lookup_state(Display *d, unsigned int state)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->xkb_keymap) return NULL;
    ensure_mod_indices(dp);
    if (dp->xkb_lookup_state && dp->xkb_lookup_keymap != dp->xkb_keymap) {
        xkb_state_unref(dp->xkb_lookup_state);
        dp->xkb_lookup_state = NULL;
    }
    if (!dp->xkb_lookup_state) {
        dp->xkb_lookup_state = xkb_state_new(dp->xkb_keymap);
        dp->xkb_lookup_keymap = dp->xkb_keymap;
        if (!dp->xkb_lookup_state) return NULL;
    }
    xkb_mod_mask_t mods = 0;
    for (int i = 0; i < 8; i++) {
        if (!(state & (1u << i))) continue;
        xkb_mod_index_t idx = dp->xkb_mod_idx[i];
        if (idx != XKB_MOD_INVALID) mods |= 1u << idx;
    }
    xkb_state_update_mask(dp->xkb_lookup_state, mods, 0, 0, 0, 0,
                          (state >> 13) & 3u);
    return dp->xkb_lookup_state;
}

/* The keysym a key produces under a core state, through the keymap's key
 * types (Caps Lock on letters, Num Lock on the keypad, AltGr levels). */
KeySym mw_keysym_for_state(Display *d, KeyCode kc, unsigned int state)
{
    struct xkb_state *st = lookup_state(d, state);
    if (!st) return NoSymbol;
    return (KeySym)xkb_state_key_get_one_sym(st, (xkb_keycode_t)kc);
}

/* The core modifiers a key's translation used under `state`. */
static unsigned int consumed_core(Display *d, KeyCode kc, unsigned int state)
{
    XDisplayImpl *dp = MWD(d);
    struct xkb_state *st = lookup_state(d, state);
    if (!st) return 0;
    unsigned int m = 0;
    for (int i = 0; i < 8; i++) {
        xkb_mod_index_t idx = dp->xkb_mod_idx[i];
        if (idx != XKB_MOD_INVALID &&
            xkb_state_mod_index_is_consumed(st, (xkb_keycode_t)kc, idx) == 1)
            m |= 1u << i;
    }
    return m;
}

/* One entry of the core keysym table, laid out as Xorg's X server does:
 * G1L1 G1L2 G2L1 G2L2 G1L3 G1L4 G2L3 G2L4.  A key with one group repeats it
 * for group 2. */
#define MW_CORE_SYMS_PER_KEY 8
static KeySym core_sym(Display *d, KeyCode kc, int index)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->xkb_keymap || index < 0 || index >= MW_CORE_SYMS_PER_KEY) return NoSymbol;
    static const int grp[8] = { 0, 0, 1, 1, 0, 0, 1, 1 };
    static const int lvl[8] = { 0, 1, 0, 1, 2, 3, 2, 3 };
    xkb_keycode_t key = (xkb_keycode_t)kc;
    xkb_layout_index_t ng = xkb_keymap_num_layouts_for_key(dp->xkb_keymap, key);
    if (ng == 0) return NoSymbol;
    xkb_layout_index_t g = (xkb_layout_index_t)grp[index] % ng;
    const xkb_keysym_t *syms = NULL;
    int n = xkb_keymap_key_get_syms_by_level(dp->xkb_keymap, key, g,
                                             (xkb_level_index_t)lvl[index], &syms);
    return n > 0 ? (KeySym)syms[0] : NoSymbol;
}

KeySym mw_keycode_to_keysym(Display *d, KeyCode kc, int index)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->xkb_keymap) return NoSymbol;
    xkb_keycode_t key = (xkb_keycode_t)kc;
    const xkb_keysym_t *syms = NULL;
    int n = xkb_keymap_key_get_syms_by_level(dp->xkb_keymap, key, 0, index, &syms);
    if (n <= 0) return NoSymbol;
    return (KeySym)syms[0];
}

KeySym XKeycodeToKeysym(Display *d, KeyCode kc, int index)
{ return core_sym(d, kc, index); }

KeySym XLookupKeysym(XKeyEvent *event, int index)
{ return core_sym(event->display, event->keycode, index); }

KeyCode mw_keysym_to_keycode(Display *d, KeySym ks)
{
    XDisplayImpl *dp = MWD(d);
    if (!dp->xkb_keymap) return 0;
    for (xkb_keycode_t kc = 8; kc <= 255; kc++) {
        for (int level = 0; level < 4; level++) {
            const xkb_keysym_t *syms = NULL;
            int n = xkb_keymap_key_get_syms_by_level(dp->xkb_keymap, kc, 0,
                                                     level, &syms);
            for (int i = 0; i < n; i++)
                if ((KeySym)syms[i] == ks) return (KeyCode)kc;
        }
    }
    return 0;
}

KeyCode XKeysymToKeycode(Display *d, KeySym ks)
{ return mw_keysym_to_keycode(d, ks); }

int XDisplayKeycodes(Display *d, int *min, int *max)
{
    *min = MWD(d)->min_keycode;
    *max = MWD(d)->max_keycode;
    return 1;
}

KeySym *XGetKeyboardMapping(Display *d, KeyCode first, int count, int *nkeys)
{
    if (count <= 0) { if (nkeys) *nkeys = 0; return NULL; }
    int per = MW_CORE_SYMS_PER_KEY;
    KeySym *out = calloc((size_t)count * per, sizeof(KeySym));
    if (!out) { if (nkeys) *nkeys = 0; return NULL; }
    for (int i = 0; i < count; i++) {
        int kc = first + i;
        if (kc > 255) break;
        for (int j = 0; j < per; j++)
            out[i * per + j] = core_sym(d, (KeyCode)kc, j);
    }
    if (nkeys) *nkeys = per;
    return out;
}

void mw_update_modmap(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (dp->modmap || !dp->xkb_keymap) return;

    /* Map each modifier index to the keycodes that carry it, derived from the
     * keysyms of those keys.  Xt turns an event's state mask back into
     * keycodes through this map when it matches modifiers in a translation
     * ("Ctrl<Key>a"), so an entry holding the wrong keycode means no Ctrl+key
     * binding can ever fire -- which is how Ctrl+A/C/V (select all, copy,
     * paste) silently degraded to inserting the plain character. */
    const int per = 4;
    XModifierKeymap *m = malloc(sizeof(*m) + 8 * per * sizeof(KeyCode));
    if (!m) return;
    m->max_keypermod = per;
    m->modifiermap = (KeyCode *)(m + 1);
    memset(m->modifiermap, 0, 8 * per * sizeof(KeyCode));
    int used[8] = {0};

    for (xkb_keycode_t kc = 8; kc <= 255; kc++) {
        const xkb_keysym_t *syms = NULL;
        int n = xkb_keymap_key_get_syms_by_level(dp->xkb_keymap, kc, 0, 0, &syms);
        int idx = -1;
        for (int k = 0; k < n && idx < 0; k++) {
            switch (syms[k]) {
            case XK_Shift_L: case XK_Shift_R:   idx = ShiftMapIndex;   break;
            case XK_Caps_Lock:                  idx = LockMapIndex;    break;
            case XK_Control_L: case XK_Control_R: idx = ControlMapIndex; break;
            case XK_Alt_L: case XK_Alt_R:       idx = Mod1MapIndex;    break;
            case XK_Super_L: case XK_Super_R:   idx = Mod4MapIndex;    break;
            default: break;
            }
        }
        if (idx >= 0 && used[idx] < per)
            m->modifiermap[idx * per + used[idx]++] = (KeyCode)kc;
    }
    dp->modmap = m;
}

XModifierKeymap *XGetModifierMapping(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    mw_update_modmap(d);
    if (!dp->modmap) return NULL;
    int per = dp->modmap->max_keypermod;
    XModifierKeymap *m = malloc(sizeof(*m) + 8 * per * sizeof(KeyCode));
    if (!m) return NULL;
    m->max_keypermod = per;
    m->modifiermap = (KeyCode *)(m + 1);
    memcpy(m->modifiermap, dp->modmap->modifiermap, 8 * per * sizeof(KeyCode));
    return m;
}

int XSetModifierMapping(Display *d, XModifierKeymap *m) { (void)d; (void)m; return MappingSuccess; }
int XFreeModifiermap(XModifierKeymap *m) { if (m) free(m); return 1; }

unsigned int mw_keysym_to_modmask(Display *d, KeySym ks)
{
    switch (ks) {
    case XK_Shift_L: case XK_Shift_R: return ShiftMask;
    case XK_Control_L: case XK_Control_R: return ControlMask;
    case XK_Meta_L: case XK_Meta_R: return Mod1Mask;
    case XK_Alt_L: case XK_Alt_R: return Mod1Mask;
    case XK_Super_L: case XK_Super_R: return Mod4Mask;
    case XK_Caps_Lock: return LockMask;
    default: (void)d; return 0;
    }
}

/* Case pairs for every script xkbcommon knows (Latin-1/2/3/4, Greek,
 * Cyrillic, Armenian, Unicode keysyms ...), not just ASCII. */
void XConvertCase(KeySym sym, KeySym *lower, KeySym *upper)
{
    *lower = (KeySym)xkb_keysym_to_lower((xkb_keysym_t)sym);
    *upper = (KeySym)xkb_keysym_to_upper((xkb_keysym_t)sym);
}

int XRefreshKeyboardMapping(XMappingEvent *e) { (void)e; return 1; }

/* Returns the effective X locale modifier list, or NULL only on failure.
 *
 * This used to return NULL unconditionally, which is the failure return: Xlib
 * then prints "X locale modifiers not supported, using default", and callers
 * that build a localised path from the result -- Motif's Mrm file lookup in the
 * i18n demos, for instance -- lose the locale component.  We have no input
 * method, so the honest answer is the empty string (or whatever was asked for,
 * or $XMODIFIERS when asked to use the default). */
/* As in libX11: NULL queries the current modifiers without changing them,
 * "" takes them from $XMODIFIERS, anything else sets them.  Returns the
 * current modifiers (owned here). */
char *XSetLocaleModifiers(_Xconst char *mods)
{
    static char *cur;
    if (!mods) return cur ? cur : (char *)"";
    const char *want = mods;
    if (!*mods) {
        want = getenv("XMODIFIERS");
        if (!want) want = "";
    }
    char *n = strdup(want);
    if (!n) return NULL;
    free(cur);
    cur = n;
    return cur;
}
Bool XSupportsLocale(void) { return True; }

/* Reference Xlib's XkbToControl(): fold a printable character into the
 * control code a Ctrl-modified key produces. */
static int mw_to_control(int c)
{
    if ((c >= '@' && c < 0x7f) || c == ' ')
        return c & 0x1f;
    if (c == '2') return 0x00;
    if (c >= '3' && c <= '7') return c - ('3' - 0x1b);
    if (c == '8') return 0x7f;
    if (c == '/') return '_' & 0x1f;
    return c;
}

int XLookupString(XKeyEvent *event, char *buffer, int nbytes, KeySym *keysym,
                  XComposeStatus *status)
{
    (void)status;
    Display *d = event->display;
    XDisplayImpl *dp = MWD(d);
    struct xkb_state *st = lookup_state(d, event->state);
    KeySym ks = st ? (KeySym)xkb_state_key_get_one_sym(st, (xkb_keycode_t)event->keycode)
                   : NoSymbol;
    if (keysym) *keysym = ks;

    static __thread struct {
        Display *d; unsigned long serial; Time time; unsigned int keycode;
        int status; KeySym ks; char text[64]; int len;
    } fed;
    bool same = fed.d == d && fed.serial == event->serial &&
                fed.time == event->time && fed.keycode == event->keycode;
    if (event->type == KeyPress && same && fed.status != XKB_COMPOSE_NOTHING) {
        if (fed.status == XKB_COMPOSE_COMPOSING) return 0;
        if (fed.status == XKB_COMPOSE_COMPOSED) {
            if (keysym && fed.ks != XKB_KEY_NoSymbol) *keysym = fed.ks;
            int len = fed.len < nbytes ? fed.len : nbytes;
            if (len > 0) memcpy(buffer, fed.text, (size_t)len);
            return len;
        }
    }
    if (dp->xkb_compose_state && event->type == KeyPress && !same &&
        !(event->state & ControlMask) && !(event->state & Mod1Mask)) {
        fed.d = d; fed.serial = event->serial; fed.time = event->time;
        fed.keycode = event->keycode; fed.status = XKB_COMPOSE_NOTHING;
        fed.ks = XKB_KEY_NoSymbol;
        xkb_compose_state_feed(dp->xkb_compose_state, ks);
        switch (xkb_compose_state_get_status(dp->xkb_compose_state)) {
        case XKB_COMPOSE_COMPOSING:
            fed.status = XKB_COMPOSE_COMPOSING;
            if (MW_TRACE())
                fprintf(stderr, "MW: composing ks=0x%lx\n", (unsigned long)ks);
            return 0;
        case XKB_COMPOSE_COMPOSED: {
            char cbuf[64];
            int r = xkb_compose_state_get_utf8(dp->xkb_compose_state,
                                               cbuf, sizeof cbuf);
            KeySym cks = (KeySym)xkb_compose_state_get_one_sym(dp->xkb_compose_state);
            xkb_compose_state_reset(dp->xkb_compose_state);
            if (r > 0) {
                if (r > (int)sizeof fed.text) r = (int)sizeof fed.text;
                fed.status = XKB_COMPOSE_COMPOSED;
                fed.ks = cks;
                memcpy(fed.text, cbuf, (size_t)r);
                fed.len = r;
                if (keysym && cks != XKB_KEY_NoSymbol) *keysym = cks;
                int len = r < nbytes ? r : nbytes;
                if (len > 0) memcpy(buffer, cbuf, (size_t)len);
                if (MW_TRACE())
                    fprintf(stderr, "MW: composed '%.*s' n=%d\n", r, cbuf, len);
                return len;
            }
            break;
        }
        case XKB_COMPOSE_CANCELLED:
        case XKB_COMPOSE_NOTHING:
        default:
            break;
        }
    }

    int ch = -1;
    if ((ks >= XK_BackSpace && ks <= XK_Clear) ||
        ks == XK_Return || ks == XK_Escape ||
        ks == XK_KP_Space || ks == XK_KP_Tab || ks == XK_KP_Enter ||
        (ks >= XK_KP_Multiply && ks <= XK_KP_9) ||
        ks == XK_KP_Equal || ks == XK_Delete) {
        ch = (ks == XK_KP_Space) ? (XK_space & 0x7F) : (int)(ks & 0x7F);
    } else if (ks >= 0x20 && ks < 0x100) {
        ch = (int)ks;
    }

    int n = 0;
    if (ch >= 0) {
        if (event->state & ControlMask) ch = mw_to_control(ch);
        if (nbytes > 0) { buffer[0] = (char)ch; n = 1; }
    } else if (st) {
        /* xkb_state_key_get_utf8() returns the byte length excluding NUL. */
        char buf[64];
        int r = xkb_state_key_get_utf8(st, (xkb_keycode_t)event->keycode,
                                       buf, sizeof buf);
        if (r > 0) {
            int len = r < (int)sizeof buf - 1 ? r : (int)sizeof buf - 1;
            if (len > nbytes) len = nbytes;
            if (len > 0) memcpy(buffer, buf, (size_t)len);
            n = len;
        }
    }
    if (MW_TRACE())
        fprintf(stderr, "MW: XLookupString kc=%u ks=0x%lx n=%d\n",
                event->keycode, (unsigned long)ks, n);
    return n;
}

int XkbLookupKeySym(Display *d, KeyCode kc, unsigned int state,
                    unsigned int *state_out, KeySym *sym)
{
    /* The keysym for the whole state (Shift, Caps Lock, Num Lock, AltGr,
     * group), and the modifiers that translation consumed, as libX11. */
    KeySym ks = mw_keysym_for_state(d, kc, state);
    if (state_out) *state_out = consumed_core(d, kc, state);
    if (sym) *sym = ks;
    return ks != NoSymbol;
}

int XkbLookupKeyBinding(Display *d, KeySym ks, unsigned int st, char *buf,
                        int n, int *nret)
{ (void)d; (void)ks; (void)st; (void)buf; (void)n; if (nret) *nret = 0; return 0; }
