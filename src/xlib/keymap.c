/* keymap.c — X keycode/keysym mapping backed by xkbcommon. */
#define _GNU_SOURCE
#include "internal.h"

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
    return NoSymbol;
}

char *XKeysymToString(KeySym ks)
{
    for (size_t i = 0; i < sizeof(ks_table) / sizeof(ks_table[0]); i++)
        if (ks_table[i].ks == ks) return (char *)ks_table[i].name;
    for (size_t i = 0; i < sizeof(vendor_table) / sizeof(vendor_table[0]); i++)
        if (vendor_table[i].ks == ks) return (char *)vendor_table[i].name;
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
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) return;
    if (!dp->xkb_ctx) dp->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (dp->xkb_keymap) xkb_keymap_unref(dp->xkb_keymap);
    if (dp->xkb_state) xkb_state_unref(dp->xkb_state);
    dp->xkb_keymap = xkb_keymap_new_from_string(dp->xkb_ctx, map,
                        XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (dp->xkb_keymap) dp->xkb_state = xkb_state_new(dp->xkb_keymap);
    mw_compose_init(d);
    dp->keymap_inited = 1;
    munmap(map, size);
    mw_update_modmap(d);
}

void mw_keymap_fini(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (dp->xkb_state) xkb_state_unref(dp->xkb_state);
    if (dp->xkb_keymap) xkb_keymap_unref(dp->xkb_keymap);
    if (dp->xkb_ctx) xkb_context_unref(dp->xkb_ctx);
    if (dp->xkb_compose_state) { xkb_compose_state_unref(dp->xkb_compose_state); dp->xkb_compose_state = NULL; }
    if (dp->xkb_compose_table) { xkb_compose_table_unref(dp->xkb_compose_table); dp->xkb_compose_table = NULL; }
    if (dp->modmap) { free(dp->modmap); dp->modmap = NULL; }
    dp->xkb_state = NULL; dp->xkb_keymap = NULL; dp->xkb_ctx = NULL;
    dp->modmap = NULL; dp->keymap_inited = 0;
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
{ return mw_keycode_to_keysym(d, kc, index); }

KeySym XLookupKeysym(XKeyEvent *event, int index)
{ return mw_keycode_to_keysym(event->display, event->keycode, index); }

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
    Display *dd = d;
    int per = 2;
    KeySym *out = calloc((size_t)count * per, sizeof(KeySym));
    for (int i = 0; i < count; i++) {
        out[i * per + 0] = mw_keycode_to_keysym(dd, (KeyCode)(first + i), 0);
        out[i * per + 1] = mw_keycode_to_keysym(dd, (KeyCode)(first + i), 1);
    }
    if (nkeys) *nkeys = per;
    return out;
}

void mw_update_modmap(Display *d)
{
    XDisplayImpl *dp = MWD(d);
    if (dp->modmap) return;

    /* Map each modifier index to the keycodes that carry it, derived from the
     * keysyms of those keys.  Xt turns an event's state mask back into
     * keycodes through this map when it matches modifiers in a translation
     * ("Ctrl<Key>a"), so an entry holding the wrong keycode means no Ctrl+key
     * binding can ever fire -- which is how Ctrl+A/C/V (select all, copy,
     * paste) silently degraded to inserting the plain character. */
    const int per = 4;
    XModifierKeymap *m = malloc(sizeof(*m) + 8 * per * sizeof(KeyCode));
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

void XConvertCase(KeySym sym, KeySym *lower, KeySym *upper)
{
    *lower = sym; *upper = sym;
    if (sym >= XK_A && sym <= XK_Z) *lower = sym + 32;
    else if (sym >= XK_a && sym <= XK_z) { *upper = sym - 32; }
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
char *XSetLocaleModifiers(_Xconst char *mods)
{
    static char *cur;
    const char *want = mods ? mods : getenv("XMODIFIERS");
    free(cur);
    cur = strdup(want ? want : "");
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
    KeySym ks = mw_keycode_to_keysym(d, event->keycode, 0);
    if (event->state & ShiftMask) {
        KeySym s2 = mw_keycode_to_keysym(d, event->keycode, 1);
        if (s2 != NoSymbol) ks = s2;
    }
    if (keysym) *keysym = ks;

    /* Feed the keysym to the compose state: dead keys and multi-key sequences
     * resolve here, so an intl. layout's dead_acute then e yields é.  Skip it
     * while Ctrl/Alt is held (there the key means something else). */
    if (dp->xkb_compose_state && !(event->state & ControlMask) &&
        !(event->state & Mod1Mask)) {
        /* feed() only says whether the keysym was accepted into a sequence;
         * the outcome is read from get_status(). */
        xkb_compose_state_feed(dp->xkb_compose_state, ks);
        switch (xkb_compose_state_get_status(dp->xkb_compose_state)) {
        case XKB_COMPOSE_COMPOSING:
            if (getenv("MW_TRACE"))
                fprintf(stderr, "MW: composing ks=0x%lx\n", (unsigned long)ks);
            return 0;                    /* part of a sequence: no bytes yet */
        case XKB_COMPOSE_COMPOSED: {
            char cbuf[64];
            int r = xkb_compose_state_get_utf8(dp->xkb_compose_state,
                                               cbuf, sizeof cbuf);
            if (r > 1) {
                int len = r - 1;
                if (len > nbytes) len = nbytes;
                memcpy(buffer, cbuf, (size_t)len);
                xkb_compose_state_reset(dp->xkb_compose_state);
                if (getenv("MW_TRACE"))
                    fprintf(stderr, "MW: composed '%s' n=%d\n", cbuf, len);
                return len;
            }
            xkb_compose_state_reset(dp->xkb_compose_state);
            break;
        }
        case XKB_COMPOSE_CANCELLED:      /* partial sequence dropped: fall through */
        case XKB_COMPOSE_NOTHING:
        default:
            break;
        }
    }

    /* Non-printable keys still yield a byte: Xlib collapses them to their low
     * seven bits, exactly as the reference _XkbHandleSpecialSym() does.  This
     * is what makes Return deliver '\r' -- without it a terminal emulator
     * receives no bytes for Enter and a command is never submitted. */
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
    } else if (dp->xkb_state && dp->xkb_keymap) {
        /* A multi-byte character (composed or non-Latin-1); ask xkbcommon. */
        char buf[64];
        int r = xkb_state_key_get_utf8(dp->xkb_state,
                                       (xkb_keycode_t)event->keycode,
                                       buf, sizeof buf);
        if (r > 1) {
            int len = r - 1;
            if (len > nbytes) len = nbytes;
            memcpy(buffer, buf, len);
            n = len;
        }
    }
    if (getenv("MW_TRACE"))
        fprintf(stderr, "MW: XLookupString kc=%u ks=0x%lx n=%d\n",
                event->keycode, (unsigned long)ks, n);
    return n;
}

int XkbLookupKeySym(Display *d, KeyCode kc, unsigned int state,
                    unsigned int *state_out, KeySym *sym)
{
    /* Honour Shift (and CapsLock, which shifts letters) by selecting the
     * keymap level, as XkbTranslateKeyCode() does; ignoring the state made a
     * shifted key report its unshifted keysym (gtk-demo's send-shift-key). */
    int level = (state & ShiftMask) ? 1 : 0;
    if (state_out) *state_out = state;
    if (sym) *sym = mw_keycode_to_keysym(d, kc, level);
    return 1;
}

int XkbLookupKeyBinding(Display *d, KeySym ks, unsigned int st, char *buf,
                        int n, int *nret)
{ (void)d; (void)ks; (void)st; (void)buf; (void)n; if (nret) *nret = 0; return 0; }
