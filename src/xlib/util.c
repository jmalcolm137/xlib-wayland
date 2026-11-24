/* util.c — shared internal utilities (cached trace flags, monotonic clock,
 * hex codec, selection-notify helper). */
#include "internal.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------- trace flags */

static unsigned int g_trace_flags;
static int g_trace_inited;

unsigned int mw_trace_flags(void)
{
    if (__atomic_load_n(&g_trace_inited, __ATOMIC_ACQUIRE))
        return __atomic_load_n(&g_trace_flags, __ATOMIC_RELAXED);
    unsigned int f = 0;
    if (getenv("MW_TRACE"))        f |= MW_TF_TRACE;
    if (getenv("MW_TRACE_RENDER")) f |= MW_TF_RENDER;
    if (mw_secure_getenv("MW_DUMP"))         f |= MW_TF_DUMP;
    const char *gate = getenv("MW_RENDER");
    if (gate && (gate[0] == '0' || gate[0] == 'n' || gate[0] == 'N'))
        f |= MW_TF_NO_RENDER;
    __atomic_store_n(&g_trace_flags, f, __ATOMIC_RELAXED);
    __atomic_store_n(&g_trace_inited, 1, __ATOMIC_RELEASE);
    return f;
}

/* ------------------------------------------------------- monotonic clock */

uint64_t mw_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* ------------------------------------------------------------- hex codec */

void mw_hex_encode(const unsigned char *in, size_t n, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = hex[in[i] >> 4];
        out[i * 2 + 1] = hex[in[i] & 0xf];
    }
    out[n * 2] = 0;
}

static int hex_val(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

int mw_hex_decode(const char *hex, unsigned char *out, size_t max_bytes)
{
    size_t i = 0;
    while (i < max_bytes && hex[0] && hex[1]) {
        int hi = hex_val((unsigned char)hex[0]);
        int lo = hex_val((unsigned char)hex[1]);
        if (hi < 0 || lo < 0) return -1;
        out[i++] = (unsigned char)((hi << 4) | lo);
        hex += 2;
    }
    return (int)i;
}

/* ----------------------------------------------------- selection notify */

void mw_send_selection_notify(Display *d, Window requestor, Atom selection,
                              Atom target, Atom property, Time time)
{
    XEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.xselection.type = SelectionNotify;
    ev.xselection.display = d;
    ev.xselection.requestor = requestor;
    ev.xselection.selection = selection;
    ev.xselection.target = target;
    ev.xselection.property = property;
    ev.xselection.time = time;
    mw_put_event(d, &ev);
}

/* ------------------------------------------------- Wayland string sanitiser */

/* Length of the valid UTF-8 sequence at s (at most `left` bytes), or 0. */
static size_t utf8_seq(const unsigned char *s, size_t left)
{
    unsigned char c = s[0];
    size_t n;
    uint32_t cp;
    if (c < 0x80) return 1;
    if (c >= 0xc2 && c <= 0xdf) { n = 2; cp = c & 0x1f; }
    else if (c >= 0xe0 && c <= 0xef) { n = 3; cp = c & 0x0f; }
    else if (c >= 0xf0 && c <= 0xf4) { n = 4; cp = c & 0x07; }
    else return 0;
    if (n > left) return 0;
    for (size_t i = 1; i < n; i++) {
        if ((s[i] & 0xc0) != 0x80) return 0;
        cp = (cp << 6) | (s[i] & 0x3f);
    }
    if ((n == 3 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) ||
        (n == 4 && (cp < 0x10000 || cp > 0x10ffff)))
        return 0;
    return n;
}

void mw_wl_string(char *out, size_t n, const char *in, bool latin1)
{
    size_t o = 0;
    if (!out || n == 0) return;
    const unsigned char *s = (const unsigned char *)(in ? in : "");
    size_t left = strlen((const char *)s);
    while (left) {
        size_t k;
        unsigned char tmp[2];
        const unsigned char *src = s;
        if (latin1) {
            if (s[0] < 0x80) { k = 1; }
            else { tmp[0] = 0xc0 | (s[0] >> 6); tmp[1] = 0x80 | (s[0] & 0x3f);
                   src = tmp; k = 2; }
            s++; left--;
        } else {
            k = utf8_seq(s, left);
            if (k == 0) { tmp[0] = '?'; src = tmp; s++; left--; k = 1; }
            else { s += k; left -= k; }
        }
        if (o + k >= n) break;              /* never split a sequence */
        memcpy(out + o, src, k);
        o += k;
    }
    out[o] = 0;
}
