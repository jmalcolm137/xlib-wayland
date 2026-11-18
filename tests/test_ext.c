/* test_ext.c — the extension facades (libXext, libXfixes, libXdamage) through
 * their public APIs. */
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xext.h>
/* extutil.h names Xlib's private wire types in hook signatures. */
typedef struct _MwXextEvent xEvent;
typedef struct _MwXextError xError;
#include <X11/extensions/extutil.h>
#include <X11/extensions/sync.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xdamage.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void check_sync_values(void)
{
    XSyncValue a, b, r, max, min;
    int ovf = -1;
    XSyncMaxValue(&max);
    XSyncMinValue(&min);
    XSyncIntToValue(&b, 1);
    XSyncValueAdd(&r, max, b, &ovf);
    CHECK(ovf != 0);
    XSyncValueSubtract(&r, min, b, &ovf);
    CHECK(ovf != 0);
    XSyncIntToValue(&a, -5);
    XSyncValueAdd(&r, a, b, &ovf);
    CHECK(ovf == 0 && XSyncValueHigh32(r) == -1 && XSyncValueLow32(r) == (unsigned)-4);
    XSyncValueSubtract(&r, b, a, &ovf);
    CHECK(ovf == 0 && XSyncValueHigh32(r) == 0 && XSyncValueLow32(r) == 6);
    XSyncIntsToValue(&a, 0xffffffffu, 0);
    XSyncValueAdd(&r, a, b, &ovf);
    CHECK(ovf == 0 && XSyncValueHigh32(r) == 1 && XSyncValueLow32(r) == 0);
}

static void check_regions(Display *d)
{
    XRectangle r1 = { 0, 0, 10, 10 }, r2 = { 5, 0, 10, 10 };
    XserverRegion a = XFixesCreateRegion(d, &r1, 1);
    XserverRegion b = XFixesCreateRegion(d, &r2, 1);
    XserverRegion u = XFixesCreateRegion(d, NULL, 0);
    XFixesUnionRegion(d, u, a, b);
    XRectangle bounds;
    int n = 0;
    XRectangle *rects = XFixesFetchRegionAndBounds(d, u, &n, &bounds);
    CHECK(rects && n >= 1 && bounds.x == 0 && bounds.width == 15 && bounds.height == 10);
    XFree(rects);
    XFixesSubtractRegion(d, u, u, b);
    rects = XFixesFetchRegion(d, u, &n);
    CHECK(rects && n == 1 && rects[0].x == 0 && rects[0].width == 5);
    XFree(rects);
    XFixesIntersectRegion(d, u, a, b);
    XFixesTranslateRegion(d, u, 1, 2);
    rects = XFixesFetchRegion(d, u, &n);
    CHECK(rects && n == 1 && rects[0].x == 6 && rects[0].y == 2 && rects[0].width == 5);
    XFree(rects);
    XFixesSetRegion(d, u, NULL, 0);
    rects = XFixesFetchRegion(d, u, &n);
    CHECK(n == 0);
    XFree(rects);
    XFixesDestroyRegion(d, a);
    XFixesDestroyRegion(d, b);
    XFixesDestroyRegion(d, u);
}

static int damage_base;

static int count_damage(Display *d, Damage dm, XRectangle *last)
{
    XSync(d, False);
    int n = 0;
    XEvent ev;
    while (XCheckTypedEvent(d, damage_base + XDamageNotify, &ev)) {
        XDamageNotifyEvent *de = (XDamageNotifyEvent *)&ev;
        if (de->damage != dm) continue;
        n++;
        if (last) *last = de->area;
    }
    return n;
}

static void check_damage(Display *d, Window root)
{
    int err;
    CHECK(XDamageQueryExtension(d, &damage_base, &err));
    Window w = XCreateSimpleWindow(d, root, 0, 0, 100, 50, 0, 0, 0);
    XMapWindow(d, w);
    GC gc = XCreateGC(d, w, 0, NULL);
    XSync(d, False);
    XRectangle area;

    /* NonEmpty: a notify when the damage becomes non-empty, then nothing
     * until it is repaired with XDamageSubtract. */
    Damage ne = XDamageCreate(d, w, XDamageReportNonEmpty);
    XFillRectangle(d, w, gc, 0, 0, 5, 5);
    CHECK(count_damage(d, ne, NULL) == 1);
    XFillRectangle(d, w, gc, 0, 0, 5, 5);
    CHECK(count_damage(d, ne, NULL) == 0);
    XDamageSubtract(d, ne, None, None);
    XFillRectangle(d, w, gc, 0, 0, 5, 5);
    CHECK(count_damage(d, ne, NULL) == 1);

    /* A partial repair leaves damage behind, which is reported again; the
     * parts region gets what was repaired. */
    XRectangle half = { 0, 0, 50, 50 };
    XserverRegion repair = XFixesCreateRegion(d, &half, 1);
    XserverRegion parts = XFixesCreateRegion(d, NULL, 0);
    XDamageSubtract(d, ne, repair, parts);
    CHECK(count_damage(d, ne, NULL) == 1);
    int n = 0;
    XRectangle *pr = XFixesFetchRegion(d, parts, &n);
    CHECK(n == 1 && pr && pr[0].width == 50 && pr[0].height == 50);
    XFree(pr);
    XDamageSubtract(d, ne, None, None);
    CHECK(count_damage(d, ne, NULL) == 0);
    XDamageDestroy(d, ne);

    /* Raw: every damage is reported; XDamageAdd reports the added region. */
    Damage raw = XDamageCreate(d, w, XDamageReportRawRectangles);
    XFillRectangle(d, w, gc, 0, 0, 5, 5);
    XFillRectangle(d, w, gc, 0, 0, 5, 5);
    CHECK(count_damage(d, raw, NULL) == 2);
    XRectangle add = { 7, 8, 9, 10 };
    XserverRegion ar = XFixesCreateRegion(d, &add, 1);
    XDamageAdd(d, w, ar);
    CHECK(count_damage(d, raw, &area) == 1 && area.x == 7 && area.y == 8 &&
          area.width == 9 && area.height == 10);
    XDamageDestroy(d, raw);

    /* BoundingBox: reported only when the box grows. */
    Damage bb = XDamageCreate(d, w, XDamageReportBoundingBox);
    XDamageAdd(d, w, ar);
    CHECK(count_damage(d, bb, &area) == 1 && area.x == 7 && area.width == 9);
    XDamageAdd(d, w, ar);
    CHECK(count_damage(d, bb, NULL) == 0);
    XRectangle more = { 0, 0, 2, 2 };
    XFixesSetRegion(d, ar, &more, 1);
    XDamageAdd(d, w, ar);
    CHECK(count_damage(d, bb, &area) == 1 && area.x == 0 && area.y == 0 &&
          area.width == 16 && area.height == 18);
    XDamageDestroy(d, bb);

    XFixesDestroyRegion(d, ar);
    XFixesDestroyRegion(d, repair);
    XFixesDestroyRegion(d, parts);
    XFreeGC(d, gc);

    /* A destroyed window takes its XFIXES selection registration with it. */
    int fx_base;
    CHECK(XFixesQueryExtension(d, &fx_base, &err));
    Atom sel = XInternAtom(d, "MW_TEST_SEL", False);
    XFixesSelectSelectionInput(d, w, sel, XFixesSetSelectionOwnerNotifyMask);
    XSetSelectionOwner(d, sel, w, CurrentTime);
    XSync(d, False);
    XEvent fe;
    CHECK(XCheckTypedEvent(d, fx_base + XFixesSelectionNotify, &fe) &&
          ((XFixesSelectionNotifyEvent *)&fe)->subtype == XFixesSetSelectionOwnerNotify);
    /* The owner's destruction is its own subtype, sent only to those who
     * asked for it. */
    Window watcher = XCreateSimpleWindow(d, root, 0, 0, 10, 10, 0, 0, 0);
    XFixesSelectSelectionInput(d, watcher, sel, XFixesSelectionWindowDestroyNotifyMask);
    Damage gone = XDamageCreate(d, w, XDamageReportRawRectangles);
    (void)gone;
    XDestroyWindow(d, w);
    XSync(d, False);
    CHECK(XCheckTypedEvent(d, fx_base + XFixesSelectionNotify, &fe) &&
          ((XFixesSelectionNotifyEvent *)&fe)->subtype == XFixesSelectionWindowDestroyNotify &&
          ((XFixesSelectionNotifyEvent *)&fe)->window == watcher);
    CHECK(!XCheckTypedEvent(d, fx_base + XFixesSelectionNotify, &fe));
    XDestroyWindow(d, watcher);
    Window owner = XCreateSimpleWindow(d, root, 0, 0, 10, 10, 0, 0, 0);
    XSetSelectionOwner(d, sel, owner, CurrentTime);
    XSync(d, False);
    XEvent ev;
    CHECK(!XCheckTypedEvent(d, fx_base + XFixesSelectionNotify, &ev));
    XDestroyWindow(d, owner);
}

static int closed;
static int on_close(Display *d, XExtCodes *codes) { (void)d; (void)codes; closed++; return 0; }

int main(void)
{
    check_sync_values();
    Display *d = XOpenDisplay(NULL);
    if (!d) {
        if (getenv("MW_TEST_REQUIRE_DISPLAY")) { fprintf(stderr, "no display\n"); return 1; }
        printf("test_ext: no display, skipping\n");
        return failures ? 1 : 77;
    }
    Window root = DefaultRootWindow(d);
    check_regions(d);
    check_damage(d, root);

    /* XextAddDisplay's close_display hook runs at XCloseDisplay (libXext
     * extensions free their per-display data there). */
    static XExtensionInfo info;
    XExtensionHooks hooks;
    memset(&hooks, 0, sizeof hooks);
    hooks.close_display = on_close;
    CHECK(XextAddDisplay(&info, d, "SHAPE", &hooks, 0, NULL) != NULL);
    XCloseDisplay(d);
    CHECK(closed == 1);

    if (failures) { fprintf(stderr, "test_ext: %d failure(s)\n", failures); return 1; }
    printf("test_ext: all checks passed\n");
    return 0;
}
