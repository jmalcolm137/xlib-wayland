/* resource.c — Xlib resource manager (Xrm) for the Motif/Wayland Xlib.
 *
 * This is a self-contained implementation of the public Xrm API declared in
 * <X11/Xresource.h>.  It is intentionally independent of internal.h so that it
 * builds (and links into libX11) without the Wayland/XKB/pixman dependencies.
 *
 * Representation
 * --------------
 * Quarks are interned in a process-global string table.  A resource database
 * (XrmDatabase, i.e. struct _XrmHashBucketRec) is a flat list of complete
 * resource entries.  Each entry carries its own name/class quark list, its
 * tight/loose binding list, a representation type and a copied value.
 *
 * Lookup evaluates every entry against the query name/class lists with full
 * wildcard semantics (tight '.', loose '*', '?'/XrmQANY and class fallback) and
 * picks the entry whose match is "most specific" under the classic Xrm
 * precedence rules.  See the report accompanying this file for the exact
 * behaviours implemented versus approximated.
 *
 * Standard headers only, plus Xlib.h / Xresource.h:
 *     gcc -c -std=gnu11 -Wall -Wextra -I/usr/include resource.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <X11/Xlib.h>
#include <X11/Xresource.h>

/* ================================================================== quarks */

typedef struct QuarkNode {
    struct QuarkNode *next;
    unsigned long     hash;
    XrmQuark          quark;
    char             *name;
} QuarkNode;

#define QUARK_BUCKETS 1024

static QuarkNode *quark_buckets[QUARK_BUCKETS];
static char     **quark_names = NULL;
static int        quark_count = 0;
static int        quark_cap = 0;
static XrmQuark   next_unique = -1;

/* Cached well-known quarks ("String" and "?"). */
static XrmQuark quark_String = NULLQUARK;
static XrmQuark quark_ANY = NULLQUARK;

static unsigned long
hash_string(const char *s)
{
    /* FNV-1a */
    unsigned long h = 1469598103934665603UL;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211UL;
    }
    return h;
}

static Bool
grow_quarks(int need)
{
    int ncap = quark_cap ? quark_cap : 64;
    char **nn;

    while (ncap <= need)
        ncap *= 2;
    nn = (char **)realloc(quark_names, (size_t)ncap * sizeof(char *));
    if (!nn)
        return False;
    quark_names = nn;
    quark_cap = ncap;
    return True;
}

static XrmQuark
intern(const char *s)
{
    unsigned long h;
    QuarkNode *n;
    XrmQuark q;
    char *copy;
    QuarkNode *node;

    if (!s)
        return NULLQUARK;

    h = hash_string(s);
    for (n = quark_buckets[h % QUARK_BUCKETS]; n; n = n->next)
        if (n->hash == h && strcmp(n->name, s) == 0)
            return n->quark;

    q = (XrmQuark)(quark_count + 1);
    if (q >= quark_cap && !grow_quarks((int)q))
        return NULLQUARK;

    copy = (char *)malloc(strlen(s) + 1);
    if (!copy)
        return NULLQUARK;
    strcpy(copy, s);

    node = (QuarkNode *)malloc(sizeof(*node));
    if (!node) {
        free(copy);
        return NULLQUARK;
    }
    node->hash = h;
    node->quark = q;
    node->name = copy;
    node->next = quark_buckets[h % QUARK_BUCKETS];
    quark_buckets[h % QUARK_BUCKETS] = node;
    quark_names[q] = copy;
    quark_count = (int)q;
    return q;
}

static void
xrm_init_once(void)
{
    if (quark_String == NULLQUARK)
        quark_String = intern("String");
    if (quark_ANY == NULLQUARK)
        quark_ANY = intern("?");
}

void
XrmInitialize(void)
{
    xrm_init_once();
}

XrmQuark
XrmStringToQuark(const char *name)
{
    return intern(name);
}

XrmQuark
XrmPermStringToQuark(const char *name)
{
    /* We always copy; the permanence contract only guarantees the caller's
     * string outlives the quark, which copying trivially satisfies. */
    return intern(name);
}

XrmQuark
XrmUniqueQuark(void)
{
    XrmQuark q = next_unique;
    next_unique--;
    if (next_unique == 0)      /* never hand out NULLQUARK */
        next_unique = -1;
    return q;
}

XrmString
XrmQuarkToString(XrmQuark quark)
{
    if (quark <= 0 || (int)quark > quark_count)
        return NULLSTRING;
    return quark_names[quark];
}

static int
quark_list_len(XrmQuarkList q)
{
    int n = 0;
    if (q)
        while (q[n] != NULLQUARK)
            n++;
    return n;
}

/* =========================================== string -> quark list parsing */

void
XrmStringToQuarkList(const char *name, XrmQuarkList quarks)
{
    const char *start;
    const char *p;

    if (!quarks)
        return;
    if (!name) {
        quarks[0] = NULLQUARK;
        return;
    }

    start = name;
    p = name;
    {
        int have = 0;
        for (;;) {
            char c = *p;
            if (c == '.' || c == '*' || c == '\0') {
                if (p > start) {
                    size_t len = (size_t)(p - start);
                    char *tmp = (char *)malloc(len + 1);
                    if (tmp) {
                        memcpy(tmp, start, len);
                        tmp[len] = '\0';
                        quarks[have++] = intern(tmp);
                        free(tmp);
                    } else {
                        quarks[have++] = NULLQUARK;
                    }
                }
                if (c == '\0')
                    break;
                start = p + 1;
            }
            p++;
        }
        quarks[have] = NULLQUARK;
    }
}

void
XrmStringToBindingQuarkList(const char *name, XrmBindingList bindings,
                            XrmQuarkList quarks)
{
    const char *start;
    const char *p;
    XrmBinding binding;

    if (!quarks)
        return;
    if (!bindings) {
        XrmStringToQuarkList(name, quarks);
        return;
    }
    if (!name) {
        quarks[0] = NULLQUARK;
        return;
    }

    start = name;
    p = name;
    binding = XrmBindTightly;
    {
        int have = 0;
        for (;;) {
            char c = *p;
            if (c == '.' || c == '*' || c == '\0') {
                if (p > start) {
                    size_t len = (size_t)(p - start);
                    char *tmp = (char *)malloc(len + 1);
                    if (tmp) {
                        memcpy(tmp, start, len);
                        tmp[len] = '\0';
                        bindings[have] = binding;
                        quarks[have] = intern(tmp);
                        free(tmp);
                    } else {
                        bindings[have] = binding;
                        quarks[have] = NULLQUARK;
                    }
                    have++;
                    binding = XrmBindTightly;
                }
                if (c == '\0')
                    break;
                start = p + 1;
                if (c == '*')
                    binding = XrmBindLoosely;
            }
            p++;
        }
        quarks[have] = NULLQUARK;
    }
}

/* ============================================================== database */

typedef struct RmEntry {
    struct RmEntry *next;
    int             n;          /* number of components */
    XrmBinding     *bindings;   /* [n] */
    XrmQuark       *quarks;     /* [n] */
    XrmRepresentation type;
    unsigned int    size;       /* value byte count (String values include NUL) */
    char           *value;
} RmEntry;

struct SearchCtx {
    struct SearchCtx *next;
    XrmDatabase       db;
    int               n;
    XrmQuark         *names;    /* [n] + terminator */
    XrmQuark         *classes;  /* [n] + terminator */
};

struct _XrmHashBucketRec {
    RmEntry          *entries;
    RmEntry          *tail;
    struct SearchCtx *searches;
};

static XrmDatabase
new_database(void)
{
    XrmDatabase db = (XrmDatabase)calloc(1, sizeof(*db));
    return db;
}

static void
free_entry(RmEntry *e)
{
    if (!e)
        return;
    free(e->bindings);
    free(e->quarks);
    free(e->value);
    free(e);
}

static void
destroy_database(XrmDatabase db)
{
    RmEntry *e;
    struct SearchCtx *s;

    if (!db)
        return;
    e = db->entries;
    while (e) {
        RmEntry *next = e->next;
        free_entry(e);
        e = next;
    }
    s = db->searches;
    while (s) {
        struct SearchCtx *next = s->next;
        free(s->names);
        free(s->classes);
        free(s);
        s = next;
    }
    free(db);
}

static Bool
entry_key_equal(const RmEntry *e, XrmBindingList bindings, XrmQuarkList quarks)
{
    int i;
    for (i = 0; i < e->n; i++)
        if (e->quarks[i] != quarks[i])
            return False;
    for (i = 0; i < e->n; i++) {
        XrmBinding b = bindings ? bindings[i] : XrmBindTightly;
        if (e->bindings[i] != b)
            return False;
    }
    return True;
}

static void
put_entry(XrmDatabase db, XrmBindingList bindings, XrmQuarkList quarks,
          XrmRepresentation type, XrmValue *value)
{
    int n;
    RmEntry *e;
    unsigned int sz;
    char *copy = NULL;

    if (!db || !quarks || quarks[0] == NULLQUARK)
        return;
    n = quark_list_len(quarks);
    if (n <= 0)
        return;

    for (e = db->entries; e; e = e->next) {
        if (e->n == n && entry_key_equal(e, bindings, quarks)) {
            free(e->value);
            e->value = NULL;
            sz = value ? value->size : 0;
            if (sz && value && value->addr) {
                copy = (char *)malloc(sz);
                if (!copy)
                    return;
                memcpy(copy, value->addr, sz);
            }
            e->type = type;
            e->size = sz;
            e->value = copy;
            return;
        }
    }

    e = (RmEntry *)calloc(1, sizeof(*e));
    if (!e)
        return;
    e->n = n;
    e->bindings = (XrmBinding *)malloc((size_t)n * sizeof(XrmBinding));
    e->quarks = (XrmQuark *)malloc((size_t)n * sizeof(XrmQuark));
    if (!e->bindings || !e->quarks) {
        free_entry(e);
        return;
    }
    if (bindings) {
        memcpy(e->bindings, bindings, (size_t)n * sizeof(XrmBinding));
    } else {
        int i;
        for (i = 0; i < n; i++)
            e->bindings[i] = XrmBindTightly;
    }
    memcpy(e->quarks, quarks, (size_t)n * sizeof(XrmQuark));

    sz = value ? value->size : 0;
    if (sz && value && value->addr) {
        e->value = (char *)malloc(sz);
        if (!e->value) {
            free_entry(e);
            return;
        }
        memcpy(e->value, value->addr, sz);
    }
    e->type = type;
    e->size = sz;

    if (db->tail)
        db->tail->next = e;
    else
        db->entries = e;
    db->tail = e;
}

/* =============================================================== matching */

/*
 * Match ranks, evaluated at a single query position.  Lower is better; this
 * encodes the classic Xrm preference order (name before class before '?',
 * tight before loose) and the fact that matching an earlier query position
 * always beats skipping it.
 */
#define RANK_SKIP 6

static int
match_rank(XrmQuark v, XrmQuark name, XrmQuark cls, Bool tight)
{
    if (v == name)
        return tight ? 0 : 1;
    if (v == cls)
        return tight ? 2 : 3;
    if (v == quark_ANY)
        return tight ? 4 : 5;
    return -1;
}

/*
 * Depth-first match of one entry against a complete query.  Choices are
 * explored earliest-position-first, so the first successful alignment found is
 * also the lexicographically most specific one for this entry.
 */
static Bool
match_rec(const RmEntry *e, int ei, int qi, XrmQuarkList names,
          XrmQuarkList classes, int m, int *ranks)
{
    XrmQuark v;

    if (ei == e->n)
        return qi == m;
    if (qi > m)
        return False;

    v = e->quarks[ei];
    if (e->bindings[ei] == XrmBindTightly) {
        int r;
        int old;
        if (qi >= m)
            return False;
        r = match_rank(v, names[qi], classes[qi], True);
        if (r < 0)
            return False;
        old = ranks[qi];
        ranks[qi] = r;
        if (match_rec(e, ei + 1, qi + 1, names, classes, m, ranks))
            return True;
        ranks[qi] = old;
        return False;
    } else {
        int j;
        for (j = qi; j < m; j++) {
            int r = match_rank(v, names[j], classes[j], False);
            int old;
            if (r < 0)
                continue;
            old = ranks[j];
            ranks[j] = r;
            if (match_rec(e, ei + 1, j + 1, names, classes, m, ranks))
                return True;
            ranks[j] = old;
        }
        return False;
    }
}

static int
cmp_ranks(const int *a, const int *b, int n)
{
    int i;
    for (i = 0; i < n; i++)
        if (a[i] != b[i])
            return a[i] - b[i];
    return 0;
}

static RmEntry *
best_match(XrmDatabase db, XrmQuarkList names, XrmQuarkList classes, int m)
{
    RmEntry *best = NULL;
    int *ranks;
    int *best_ranks;
    RmEntry *e;

    xrm_init_once();
    if (!db || m <= 0)
        return NULL;

    ranks = (int *)malloc((size_t)m * sizeof(int));
    best_ranks = (int *)malloc((size_t)m * sizeof(int));
    if (!ranks || !best_ranks) {
        free(ranks);
        free(best_ranks);
        return NULL;
    }

    for (e = db->entries; e; e = e->next) {
        int i;
        if (e->n < 1)
            continue;
        for (i = 0; i < m; i++)
            ranks[i] = RANK_SKIP;
        if (!match_rec(e, 0, 0, names, classes, m, ranks))
            continue;
        if (!best || cmp_ranks(ranks, best_ranks, m) < 0) {
            best = e;
            memcpy(best_ranks, ranks, (size_t)m * sizeof(int));
        }
    }

    free(ranks);
    free(best_ranks);
    return best;
}

/* ========================================================== put interface */

void
XrmQPutResource(XrmDatabase *pdb, XrmBindingList bindings, XrmQuarkList quarks,
                XrmRepresentation type, XrmValue *value)
{
    if (!pdb)
        return;
    if (!*pdb) {
        *pdb = new_database();
        if (!*pdb)
            return;
    }
    put_entry(*pdb, bindings, quarks, type, value);
}

void
XrmPutResource(XrmDatabase *pdb, const char *specifier, const char *type,
               XrmValue *value)
{
    size_t cap;
    XrmBinding *bindings;
    XrmQuark *quarks;

    if (!pdb || !specifier)
        return;
    cap = strlen(specifier) + 2;
    bindings = (XrmBinding *)malloc(cap * sizeof(XrmBinding));
    quarks = (XrmQuark *)malloc(cap * sizeof(XrmQuark));
    if (!bindings || !quarks) {
        free(bindings);
        free(quarks);
        return;
    }
    XrmStringToBindingQuarkList(specifier, bindings, quarks);
    XrmQPutResource(pdb, bindings, quarks, XrmStringToQuark(type), value);
    free(bindings);
    free(quarks);
}

void
XrmQPutStringResource(XrmDatabase *pdb, XrmBindingList bindings,
                      XrmQuarkList quarks, const char *str)
{
    XrmValue value;

    xrm_init_once();
    value.addr = (XPointer)str;
    value.size = (unsigned int)(strlen(str) + 1);
    XrmQPutResource(pdb, bindings, quarks, quark_String, &value);
}

void
XrmPutStringResource(XrmDatabase *pdb, const char *specifier, const char *str)
{
    size_t cap;
    XrmBinding *bindings;
    XrmQuark *quarks;

    if (!pdb || !specifier || !str)
        return;
    cap = strlen(specifier) + 2;
    bindings = (XrmBinding *)malloc(cap * sizeof(XrmBinding));
    quarks = (XrmQuark *)malloc(cap * sizeof(XrmQuark));
    if (!bindings || !quarks) {
        free(bindings);
        free(quarks);
        return;
    }
    XrmStringToBindingQuarkList(specifier, bindings, quarks);
    XrmQPutStringResource(pdb, bindings, quarks, str);
    free(bindings);
    free(quarks);
}

/* ======================================================= database parsing */

/* Read one logical line, honouring backslash-newline continuations and
 * stripping the leading whitespace of a continuation.  Returns a malloc'd
 * NUL-terminated string and advances *pp past the consumed input. */
static char *
read_logical_line(const char **pp)
{
    const char *p = *pp;
    size_t cap = 128;
    size_t len = 0;
    char *b = (char *)malloc(cap);

    if (!b) {
        *pp = p + strlen(p);
        return NULL;
    }

    for (;;) {
        unsigned char c = (unsigned char)*p;
        if (c == '\0')
            break;
        if (c == '\n') {
            p++;
            break;
        }
        if (c == '\r') {
            p++;
            continue;
        }
        if (c == '\\') {
            if (p[1] == '\n') {
                p += 2;
                while (*p == ' ' || *p == '\t')
                    p++;
                continue;
            }
            if (p[1] == '\r' && p[2] == '\n') {
                p += 3;
                while (*p == ' ' || *p == '\t')
                    p++;
                continue;
            }
        }
        if (len + 2 > cap) {
            size_t ncap = cap * 2;
            char *nb = (char *)realloc(b, ncap);
            if (!nb) {
                free(b);
                *pp = p;
                return NULL;
            }
            b = nb;
            cap = ncap;
        }
        b[len++] = (char)c;
        p++;
    }
    b[len] = '\0';
    *pp = p;
    return b;
}

/* Process backslash escapes in a value in place; returns the same pointer. */
static char *
unescape_inplace(char *s)
{
    char *r = s;
    char *w = s;

    while (*r) {
        if (*r == '\\') {
            r++;
            if (*r == 'n') {
                *w++ = '\n';
                r++;
            } else if (*r == '\\') {
                *w++ = '\\';
                r++;
            } else if (*r >= '0' && *r <= '7') {
                int v = 0;
                int k = 0;
                while (k < 3 && *r >= '0' && *r <= '7') {
                    v = v * 8 + (*r - '0');
                    r++;
                    k++;
                }
                *w++ = (char)v;
            } else if (*r == '\0') {
                break;
            } else {
                *w++ = *r++;
            }
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
    return s;
}

static void
parse_one_line(XrmDatabase db, char *line)
{
    char *colon;
    char *lhs;
    char *rhs;
    char *eq;
    XrmRepresentation type = NULLQUARK;
    size_t cap;
    XrmBinding *bindings;
    XrmQuark *quarks;
    XrmValue value;

    colon = strchr(line, ':');
    if (!colon)
        return;
    *colon = '\0';
    lhs = line;
    rhs = colon + 1;

    while (*lhs == ' ' || *lhs == '\t')
        lhs++;
    {
        size_t l = strlen(lhs);
        while (l > 0 && (lhs[l - 1] == ' ' || lhs[l - 1] == '\t'))
            lhs[--l] = '\0';
    }
    if (!*lhs)
        return;

    xrm_init_once();
    type = quark_String;

    eq = strchr(lhs, '=');
    if (eq) {
        char *t = eq + 1;
        size_t l;
        *eq = '\0';
        while (*t == ' ' || *t == '\t')
            t++;
        l = strlen(t);
        while (l > 0 && (t[l - 1] == ' ' || t[l - 1] == '\t'))
            t[--l] = '\0';
        if (*t)
            type = XrmStringToQuark(t);
        l = strlen(lhs);
        while (l > 0 && (lhs[l - 1] == ' ' || lhs[l - 1] == '\t'))
            lhs[--l] = '\0';
    }

    while (*rhs == ' ' || *rhs == '\t')
        rhs++;
    rhs = unescape_inplace(rhs);

    cap = strlen(lhs) + 2;
    bindings = (XrmBinding *)malloc(cap * sizeof(XrmBinding));
    quarks = (XrmQuark *)malloc(cap * sizeof(XrmQuark));
    if (!bindings || !quarks) {
        free(bindings);
        free(quarks);
        return;
    }
    XrmStringToBindingQuarkList(lhs, bindings, quarks);

    value.addr = (XPointer)rhs;
    value.size = (unsigned int)(strlen(rhs) + 1);
    put_entry(db, bindings, quarks, type, &value);

    free(bindings);
    free(quarks);
}

static void
parse_database(XrmDatabase db, const char *data, Bool doall)
{
    const char *p = data;

    if (!db || !p)
        return;

    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            p++;
        if (!*p)
            break;
        if (*p == '!') {          /* comment */
            while (*p && *p != '\n')
                p++;
            continue;
        }
        if (*p == '#') {          /* directive (#include etc.): ignored */
            while (*p && *p != '\n')
                p++;
            continue;
        }
        {
            char *line = read_logical_line(&p);
            if (!line)
                break;
            parse_one_line(db, line);
            free(line);
        }
        if (!doall)
            break;
    }
}

XrmDatabase
XrmGetStringDatabase(const char *data)
{
    XrmDatabase db;

    xrm_init_once();
    db = new_database();
    if (!db)
        return NULL;
    parse_database(db, data, True);
    return db;
}

static char *
read_in_file(const char *filename)
{
    FILE *f;
    long sz;
    char *buf;
    size_t rd;

    if (!filename)
        return NULL;
    f = fopen(filename, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    return buf;
}

XrmDatabase
XrmGetFileDatabase(const char *filename)
{
    XrmDatabase db;
    char *str = read_in_file(filename);

    if (!str)
        return NULL;
    db = new_database();
    if (!db) {
        free(str);
        return NULL;
    }
    parse_database(db, str, True);
    free(str);
    return db;
}

void
XrmPutLineResource(XrmDatabase *pdb, const char *line)
{
    XrmDatabase db;

    if (!pdb || !line)
        return;
    if (!*pdb) {
        *pdb = new_database();
        if (!*pdb)
            return;
    }
    db = *pdb;
    parse_database(db, line, False);
}

/* =========================================================== db management */

void
XrmCombineDatabase(XrmDatabase source, XrmDatabase *target, Bool override)
{
    RmEntry *e;
    XrmDatabase into;

    if (!target)
        return;
    if (!*target) {
        *target = source;
        return;
    }
    into = *target;
    if (!source || source == into)
        return;

    e = source->entries;
    while (e) {
        RmEntry *next = e->next;
        RmEntry *t;
        RmEntry *match = NULL;

        for (t = into->entries; t; t = t->next) {
            if (t->n == e->n && entry_key_equal(t, e->bindings, e->quarks)) {
                match = t;
                break;
            }
        }
        if (match) {
            if (override) {
                free(match->value);
                match->type = e->type;
                match->size = e->size;
                match->value = e->value;
                e->value = NULL;
            }
            free_entry(e);
        } else {
            e->next = NULL;
            if (into->tail)
                into->tail->next = e;
            else
                into->entries = e;
            into->tail = e;
        }
        e = next;
    }
    source->entries = NULL;
    source->tail = NULL;
    destroy_database(source);
}

void
XrmMergeDatabases(XrmDatabase source, XrmDatabase *target)
{
    XrmCombineDatabase(source, target, True);
}

Status
XrmCombineFileDatabase(const char *filename, XrmDatabase *target, Bool override)
{
    char *str;
    XrmDatabase db;

    if (!target)
        return 0;
    str = read_in_file(filename);
    if (!str)
        return 0;

    if (override) {
        db = *target;
        if (!db) {
            db = new_database();
            *target = db;
        }
    } else {
        db = new_database();
    }
    if (!db) {
        free(str);
        return 0;
    }
    parse_database(db, str, True);
    free(str);

    if (!override)
        XrmCombineDatabase(db, target, False);
    return 1;
}

/* ================================================================ lookups */

Bool
XrmQGetResource(XrmDatabase db, XrmNameList quark_name,
                XrmClassList quark_class, XrmRepresentation *quark_type_return,
                XrmValue *value_return)
{
    int m;
    RmEntry *e;
    XrmQuark *zero_classes = NULL;
    XrmClassList classes = quark_class;

    xrm_init_once();
    m = quark_list_len(quark_name);
    if (classes == NULL && m > 0) {
        zero_classes = (XrmQuark *)calloc((size_t)m, sizeof(XrmQuark));
        classes = zero_classes;
    }

    e = best_match(db, quark_name, classes, m);
    free(zero_classes);

    if (e) {
        if (quark_type_return)
            *quark_type_return = e->type;
        if (value_return) {
            value_return->addr = (XPointer)e->value;
            value_return->size = e->size;
        }
        return True;
    }
    if (quark_type_return)
        *quark_type_return = NULLQUARK;
    if (value_return) {
        value_return->addr = (XPointer)NULL;
        value_return->size = 0;
    }
    return False;
}

Bool
XrmGetResource(XrmDatabase db, const char *name_str, const char *class_str,
               char **pType_str, XrmValue *pValue)
{
    XrmQuark *names;
    XrmQuark *classes;
    XrmRepresentation type = NULLQUARK;
    Bool result;

    xrm_init_once();

    names = (XrmQuark *)calloc(strlen(name_str ? name_str : "") + 2,
                               sizeof(XrmQuark));
    classes = (XrmQuark *)calloc(strlen(class_str ? class_str : "") + 2,
                                 sizeof(XrmQuark));
    if (!names || !classes) {
        free(names);
        free(classes);
        if (pType_str)
            *pType_str = NULLSTRING;
        if (pValue) {
            pValue->addr = (XPointer)NULL;
            pValue->size = 0;
        }
        return False;
    }

    XrmStringToQuarkList(name_str, names);
    XrmStringToQuarkList(class_str, classes);

    result = XrmQGetResource(db, names, classes, &type, pValue);
    if (pType_str)
        *pType_str = XrmQuarkToString(type);

    free(names);
    free(classes);
    return result;
}

Bool
XrmQGetSearchList(XrmDatabase db, XrmNameList names, XrmClassList classes,
                  XrmSearchList list_return, int list_length)
{
    struct SearchCtx *ctx;
    int n;
    int i;

    if (!list_return || list_length < 2)
        return False;
    if (!db) {
        list_return[0] = (XrmHashTable)NULL;
        list_return[1] = (XrmHashTable)NULL;
        return True;
    }

    ctx = (struct SearchCtx *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        list_return[0] = (XrmHashTable)NULL;
        list_return[1] = (XrmHashTable)NULL;
        return True;
    }
    n = quark_list_len(names);
    ctx->db = db;
    ctx->n = n;
    ctx->names = (XrmQuark *)malloc((size_t)(n + 1) * sizeof(XrmQuark));
    ctx->classes = (XrmQuark *)malloc((size_t)(n + 1) * sizeof(XrmQuark));
    if (!ctx->names || !ctx->classes) {
        free(ctx->names);
        free(ctx->classes);
        free(ctx);
        list_return[0] = (XrmHashTable)NULL;
        list_return[1] = (XrmHashTable)NULL;
        return True;
    }
    for (i = 0; i < n; i++) {
        ctx->names[i] = names[i];
        ctx->classes[i] = classes ? classes[i] : NULLQUARK;
    }
    ctx->names[n] = NULLQUARK;
    ctx->classes[n] = NULLQUARK;
    ctx->next = db->searches;
    db->searches = ctx;

    list_return[0] = (XrmHashTable)(void *)ctx;
    list_return[1] = (XrmHashTable)NULL;
    return True;
}

Bool
XrmQGetSearchResource(XrmSearchList searchList, XrmName name, XrmClass class,
                      XrmRepresentation *pType, XrmValue *pValue)
{
    struct SearchCtx *ctx = NULL;
    XrmQuark *names;
    XrmQuark *classes;
    int n;
    int i;
    Bool result;

    if (searchList)
        ctx = (struct SearchCtx *)(void *)searchList[0];
    if (!ctx || !ctx->db) {
        if (pType)
            *pType = NULLQUARK;
        if (pValue) {
            pValue->addr = (XPointer)NULL;
            pValue->size = 0;
        }
        return False;
    }

    n = ctx->n;
    names = (XrmQuark *)malloc((size_t)(n + 2) * sizeof(XrmQuark));
    classes = (XrmQuark *)malloc((size_t)(n + 2) * sizeof(XrmQuark));
    if (!names || !classes) {
        free(names);
        free(classes);
        if (pType)
            *pType = NULLQUARK;
        if (pValue) {
            pValue->addr = (XPointer)NULL;
            pValue->size = 0;
        }
        return False;
    }
    for (i = 0; i < n; i++) {
        names[i] = ctx->names[i];
        classes[i] = ctx->classes[i];
    }
    names[n] = name;
    classes[n] = class;
    names[n + 1] = NULLQUARK;
    classes[n + 1] = NULLQUARK;

    result = XrmQGetResource(ctx->db, names, classes, pType, pValue);

    free(names);
    free(classes);
    return result;
}

/* =========================================================== enumeration */

typedef Bool (*XrmEnumProc)(XrmDatabase *, XrmBindingList, XrmQuarkList,
                            XrmRepresentation *, XrmValue *, XPointer);

/* Match a name/class prefix against the start of an entry.  Returns True when
 * the prefix is consumed and the residual length satisfies the mode. */
static Bool
enum_match_rec(const RmEntry *e, int ei, int qi, XrmQuarkList names,
               XrmQuarkList classes, int m, int mode, int *remaining)
{
    XrmQuark v;

    if (qi == m) {
        int rem = e->n - ei;
        if (mode == XrmEnumAllLevels || rem == 1) {
            *remaining = rem;
            return True;
        }
        return False;
    }
    if (ei >= e->n)
        return False;

    v = e->quarks[ei];
    if (e->bindings[ei] == XrmBindTightly) {
        if (match_rank(v, names[qi], classes[qi], True) < 0)
            return False;
        return enum_match_rec(e, ei + 1, qi + 1, names, classes, m, mode,
                              remaining);
    } else {
        int j;
        for (j = qi; j < m; j++) {
            if (match_rank(v, names[j], classes[j], False) < 0)
                continue;
            if (enum_match_rec(e, ei + 1, j + 1, names, classes, m, mode,
                               remaining))
                return True;
        }
        return False;
    }
}

Bool
XrmEnumerateDatabase(XrmDatabase db, XrmNameList name_prefix,
                     XrmClassList class_prefix, int mode, XrmEnumProc proc,
                     XPointer closure)
{
    int m;
    RmEntry *e;

    if (!db || !proc)
        return False;
    xrm_init_once();

    m = quark_list_len(name_prefix);
    for (e = db->entries; e; e = e->next) {
        int remaining = 0;
        XrmBinding *bindings;
        XrmQuark *quarks;
        XrmValue v;
        XrmRepresentation t;
        Bool stop;

        if (!enum_match_rec(e, 0, 0, name_prefix, class_prefix, m, mode,
                            &remaining))
            continue;

        bindings = (XrmBinding *)malloc((size_t)(e->n + 1) *
                                        sizeof(XrmBinding));
        quarks = (XrmQuark *)malloc((size_t)(e->n + 1) * sizeof(XrmQuark));
        if (!bindings || !quarks) {
            free(bindings);
            free(quarks);
            continue;
        }
        memcpy(bindings, e->bindings, (size_t)e->n * sizeof(XrmBinding));
        memcpy(quarks, e->quarks, (size_t)e->n * sizeof(XrmQuark));
        bindings[e->n] = XrmBindTightly;
        quarks[e->n] = NULLQUARK;

        v.addr = (XPointer)e->value;
        v.size = e->size;
        t = e->type;
        stop = (*proc)(&db, bindings, quarks, &t, &v, closure);

        free(bindings);
        free(quarks);
        if (stop)
            return True;
    }
    return False;
}

/* ========================================================== file writing */

static void
dump_binding_quarks(FILE *f, XrmBindingList bindings, XrmQuarkList quarks)

{
    Bool seen = False;
    for (; *quarks; bindings++, quarks++) {
        if (*bindings == XrmBindLoosely)
            fputc('*', f);
        else if (seen)
            fputc('.', f);
        seen = True;
        fputs(XrmQuarkToString(*quarks), f);
    }
}

static void
dump_value(FILE *f, XrmRepresentation type, XrmValue *value)
{
    unsigned int i = value->size;
    const unsigned char *s = (const unsigned char *)value->addr;

    if (!s)
        i = 0;
    if (type == quark_String) {
        if (i)
            i--;                  /* drop the trailing NUL */
    }
    if (i && (*s == ' ' || *s == '\t'))
        fputc('\\', f);           /* preserve leading whitespace */
    while (i--) {
        unsigned char c = *s++;
        if (c == '\n')
            fputs("\\n", f);
        else if (c == '\\')
            fputs("\\\\", f);
        else if ((c < ' ' && c != '\t') || (c >= 0x7f && c < 0xa0))
            fprintf(f, "\\%03o", c);
        else
            fputc(c, f);
    }
}

static Bool
dump_proc(XrmDatabase *db, XrmBindingList bindings, XrmQuarkList quarks,
          XrmRepresentation *type, XrmValue *value, XPointer closure)
{
    FILE *f = (FILE *)closure;

    (void)db;
    dump_binding_quarks(f, bindings, quarks);
    if (*type == quark_String)
        fputs(":\t", f);
    else
        fprintf(f, "=%s:\t", XrmQuarkToString(*type));
    dump_value(f, *type, value);
    fputc('\n', f);
    return ferror(f) != 0;
}

void
XrmPutFileDatabase(XrmDatabase db, const char *fileName)
{
    FILE *f;
    XrmQuark empty = NULLQUARK;

    if (!db || !fileName)
        return;
    f = fopen(fileName, "w");
    if (!f)
        return;
    xrm_init_once();
    XrmEnumerateDatabase(db, &empty, &empty, XrmEnumAllLevels, dump_proc,
                         (XPointer)f);
    fclose(f);
}

/* ========================================================= display hooks */

XrmDatabase
XrmGetDatabase(Display *display)
{
    return ((_XPrivDisplay)display)->db;
}

void
XrmSetDatabase(Display *display, XrmDatabase database)
{
    ((_XPrivDisplay)display)->db = database;
}

const char *
XrmLocaleOfDatabase(XrmDatabase database)
{
    (void)database;
    return "C";
}

void
XrmDestroyDatabase(XrmDatabase db)
{
    destroy_database(db);
}

/* ====================================================== command line parse */

static void
put_command_resource(XrmDatabase *pdb, XrmQuark prefix, const char *specifier,
                     const char *value)
{
    size_t cap = strlen(specifier ? specifier : "") + 3;
    XrmBinding *bindings = (XrmBinding *)malloc(cap * sizeof(XrmBinding));
    XrmQuark *quarks = (XrmQuark *)malloc(cap * sizeof(XrmQuark));

    if (!bindings || !quarks) {
        free(bindings);
        free(quarks);
        return;
    }
    bindings[0] = XrmBindTightly;
    quarks[0] = prefix;
    XrmStringToBindingQuarkList(specifier ? specifier : "", bindings + 1,
                                quarks + 1);
    XrmQPutStringResource(pdb, bindings, quarks, value ? value : "");
    free(bindings);
    free(quarks);
}

void
XrmParseCommand(XrmDatabase *pdb, XrmOptionDescList options, int num_options,
                const char *prefix, int *argc, char **argv)
{
    enum { DontCare, Check, NotSorted, Sorted } table_is_sorted;
    int myargc;
    char **argsave;
    char **argend;
    XrmQuark qprefix;
    int foundOption;
    int matches;
    int i;
    char *optP;
    char *argP;
    char optchar;
    char argchar;

    if (!pdb || !argc || !argv)
        return;

    xrm_init_once();
    if (!*pdb) {
        *pdb = new_database();
        if (!*pdb)
            return;
    }

    myargc = *argc;
    argend = argv + myargc;
    argsave = ++argv;             /* keep argv[0] (program name) */
    qprefix = XrmStringToName(prefix ? prefix : "");

    table_is_sorted = (myargc > 2) ? Check : DontCare;

    for (--myargc; myargc > 0; --myargc, ++argv) {
        foundOption = 0;
        matches = 0;
        argP = NULL;

        for (i = 0; i < num_options; ++i) {
            if (table_is_sorted == Check && i > 0 &&
                strcmp(options[i].option, options[i - 1].option) < 0)
                table_is_sorted = NotSorted;

            for (argP = *argv, optP = options[i].option;
                 (optchar = *optP++) && (argchar = *argP++) &&
                 argchar == optchar;)
                ;

            if (!optchar) {
                if (!*argP || options[i].argKind == XrmoptionStickyArg ||
                    options[i].argKind == XrmoptionIsArg) {
                    matches = 1;
                    foundOption = i;
                    break;
                }
            } else if (!argchar) {
                matches++;
                foundOption = i;
            } else if (table_is_sorted == Sorted && optchar > argchar) {
                break;
            }

            if (table_is_sorted == Check && i > 0 &&
                strcmp(options[i].option, options[i - 1].option) < 0)
                table_is_sorted = NotSorted;
        }
        if (table_is_sorted == Check && i >= num_options - 1)
            table_is_sorted = Sorted;

        /* The standard "-xrm resourcestring" request, honoured even when the
         * caller's option table does not list it. */
        if (matches != 1 &&
            (strcmp(*argv, "-xrm") == 0 || strcmp(*argv, "+xrm") == 0) &&
            myargc > 1) {
            ++argv;
            --myargc;
            --(*argc);
            --(*argc);
            XrmPutLineResource(pdb, *argv);
            continue;
        }

        if (matches == 1) {
            i = foundOption;
            switch (options[i].argKind) {
            case XrmoptionNoArg:
                --(*argc);
                put_command_resource(pdb, qprefix, options[i].specifier,
                                     options[i].value
                                         ? (const char *)options[i].value
                                         : "");
                break;

            case XrmoptionIsArg:
                --(*argc);
                put_command_resource(pdb, qprefix, options[i].specifier, *argv);
                break;

            case XrmoptionStickyArg:
                --(*argc);
                put_command_resource(pdb, qprefix, options[i].specifier,
                                     argP ? argP : "");
                break;

            case XrmoptionSepArg:
                if (myargc > 1) {
                    ++argv;
                    --myargc;
                    --(*argc);
                    --(*argc);
                    put_command_resource(pdb, qprefix, options[i].specifier,
                                         *argv);
                } else {
                    (*argsave++) = (*argv);
                }
                break;

            case XrmoptionResArg:
                if (myargc > 1) {
                    ++argv;
                    --myargc;
                    --(*argc);
                    --(*argc);
                    XrmPutLineResource(pdb, *argv);
                } else {
                    (*argsave++) = (*argv);
                }
                break;

            case XrmoptionSkipArg:
                if (myargc > 1) {
                    --myargc;
                    (*argsave++) = (*argv++);
                }
                (*argsave++) = (*argv);
                break;

            case XrmoptionSkipLine:
                for (; myargc > 0; myargc--)
                    (*argsave++) = (*argv++);
                break;

            case XrmoptionSkipNArgs: {
                int j = 1 + (int)(intptr_t)options[i].value;
                if (j > myargc)
                    j = myargc;
                for (; j > 0; j--) {
                    (*argsave++) = (*argv++);
                    myargc--;
                }
                argv--;           /* the loop increment steps one too far */
                myargc++;
                break;
            }

            default:
                (*argsave++) = (*argv);
                break;
            }
        } else {
            (*argsave++) = (*argv);   /* compress arglist */
        }
    }

    if (argsave < argend)
        *argsave = NULL;
}
