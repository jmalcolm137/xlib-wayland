/*
 * region.c - the X11 Region API implemented on top of pixman regions.
 *
 * In Xlib a Region is a set of pixels, represented as a canonical
 * y-x-banded array of half-open rectangles (x1,y1)-(x2,y2).  This file
 * keeps the authoritative geometry in a pixman_region32_t and mirrors it
 * into the Xlib-visible `rects` array after every mutation so that code
 * which inspects a Region sees sane data.
 *
 * Xlib stores region coordinates as signed 16-bit quantities.  We keep the
 * same range and clip any result that would leave [-32768, 32767], rather
 * than letting 16-bit arithmetic wrap.
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <pixman.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ model */

/* The rectangle mirror must match Xlib's own REGION/BOX layout, not XRectangle:
 * client libraries read it directly.  libXrender's
 * XRenderSetPictureClipRegion walks numRects/rects as BOX { x1, x2, y1, y2 },
 * and storing XRectangle { x, y, width, height } there turned a full-window
 * clip into a degenerate (0, height, 0, 0) one -- which blanked every Xft
 * client that clips through Render (upstream libXft does; ours does not). */
typedef struct {
    short x1, x2, y1, y2;
} MwRegionBox;

struct _XRegion {
    long              size;      /* capacity of rects, in boxes            */
    long              numRects;  /* number of valid entries in rects       */
    MwRegionBox      *rects;     /* mirror of pix, in Xlib REGION layout    */
    MwRegionBox       extents;   /* mirror of pix's bounding box            */
    pixman_region32_t pix;       /* authoritative geometry                  */
};

#define MW_INT16_MIN (-32768)
#define MW_INT16_MAX (32767)

#define MW_MIN(a, b) ((a) < (b) ? (a) : (b))
#define MW_MAX(a, b) ((a) > (b) ? (a) : (b))

/* ---------------------------------------------------------- pixman bridge */

/*
 * Move a freshly computed pixman region into r->pix.  The source is left
 * owning nothing, so the caller must not fini it afterwards.
 */
static void mw_store(Region r, pixman_region32_t *res)
{
    pixman_region32_fini(&r->pix);
    r->pix = *res;
}

/*
 * Clip every rectangle of r to the signed 16-bit coordinate range and
 * rebuild the (normalised) pixman region if anything was out of range.
 */
static void mw_clamp16(Region r)
{
    int n = 0;
    pixman_box32_t *boxes;
    pixman_box32_t *tmp;
    pixman_region32_t res;
    int i;
    int m;
    int changed = 0;

    boxes = pixman_region32_rectangles(&r->pix, &n);
    if (n <= 0)
        return;

    for (i = 0; i < n; i++) {
        if (boxes[i].x1 < MW_INT16_MIN || boxes[i].x1 > MW_INT16_MAX ||
            boxes[i].y1 < MW_INT16_MIN || boxes[i].y1 > MW_INT16_MAX ||
            boxes[i].x2 < MW_INT16_MIN || boxes[i].x2 > MW_INT16_MAX ||
            boxes[i].y2 < MW_INT16_MIN || boxes[i].y2 > MW_INT16_MAX) {
            changed = 1;
            break;
        }
    }
    if (!changed)
        return;

    tmp = malloc((size_t)n * sizeof *tmp);
    if (!tmp)
        return;

    m = 0;
    for (i = 0; i < n; i++) {
        int x1 = boxes[i].x1;
        int y1 = boxes[i].y1;
        int x2 = boxes[i].x2;
        int y2 = boxes[i].y2;

        if (x1 < MW_INT16_MIN) x1 = MW_INT16_MIN;
        if (x1 > MW_INT16_MAX) x1 = MW_INT16_MAX;
        if (y1 < MW_INT16_MIN) y1 = MW_INT16_MIN;
        if (y1 > MW_INT16_MAX) y1 = MW_INT16_MAX;
        if (x2 < MW_INT16_MIN) x2 = MW_INT16_MIN;
        if (x2 > MW_INT16_MAX) x2 = MW_INT16_MAX;
        if (y2 < MW_INT16_MIN) y2 = MW_INT16_MIN;
        if (y2 > MW_INT16_MAX) y2 = MW_INT16_MAX;

        if (x1 < x2 && y1 < y2) {
            tmp[m].x1 = x1;
            tmp[m].y1 = y1;
            tmp[m].x2 = x2;
            tmp[m].y2 = y2;
            m++;
        }
    }

    pixman_region32_init(&res);
    if (m > 0)
        (void)pixman_region32_init_rects(&res, tmp, m);
    free(tmp);
    mw_store(r, &res);
}

/* Refresh the Xlib-visible rectangle cache from the pixman region. */
static void mw_sync_rects(Region r)
{
    int n = 0;
    pixman_box32_t *boxes;
    int i;

    boxes = pixman_region32_rectangles(&r->pix, &n);
    if (n < 0)
        n = 0;

    if ((long)n > r->size) {
        MwRegionBox *nr = realloc(r->rects, (size_t)n * sizeof *nr);

        if (!nr) {
            r->numRects = 0;
            return;
        }
        r->rects = nr;
        r->size = n;
    }

    for (i = 0; i < n; i++) {
        r->rects[i].x1 = (short)boxes[i].x1;
        r->rects[i].x2 = (short)boxes[i].x2;
        r->rects[i].y1 = (short)boxes[i].y1;
        r->rects[i].y2 = (short)boxes[i].y2;
    }
    r->numRects = n;
    {
        pixman_box32_t *ext = pixman_region32_extents(&r->pix);
        r->extents.x1 = (short)ext->x1;
        r->extents.x2 = (short)ext->x2;
        r->extents.y1 = (short)ext->y1;
        r->extents.y2 = (short)ext->y2;
    }
}

/* Install a computed region, clip it and refresh the cache. */
static int mw_commit(Region r, pixman_region32_t *res)
{
    mw_store(r, res);
    mw_clamp16(r);
    mw_sync_rects(r);
    return 1;
}

/* Build the region contents from an arbitrary (possibly unsorted) box list. */
static int mw_set_boxes(Region r, const pixman_box32_t *boxes, size_t count)
{
    pixman_region32_t res;

    pixman_region32_init(&res);
    if (count > 0) {
        if (!pixman_region32_init_rects(&res, boxes, (int)count)) {
            pixman_region32_fini(&res);
            return 0;
        }
    }
    return mw_commit(r, &res);
}

/* --------------------------------------------------------------- creation */

Region XCreateRegion(void)
{
    Region r = malloc(sizeof *r);

    if (!r)
        return (Region)NULL;

    r->rects = malloc(sizeof *r->rects);
    if (!r->rects) {
        free(r);
        return (Region)NULL;
    }
    r->size = 1;
    r->numRects = 0;
    pixman_region32_init(&r->pix);
    return r;
}

int XDestroyRegion(Region r)
{
    if (!r)
        return 1;
    pixman_region32_fini(&r->pix);
    free(r->rects);
    free(r);
    return 1;
}

/* ------------------------------------------------------------ predicates */

Bool XEmptyRegion(Region r)
{
    return pixman_region32_not_empty(&r->pix) ? False : True;
}

Bool XEqualRegion(Region r1, Region r2)
{
    return pixman_region32_equal(&r1->pix, &r2->pix) ? True : False;
}

Bool XPointInRegion(Region r, int x, int y)
{
    if (r->numRects == 0)
        return False;
    return pixman_region32_contains_point(&r->pix, x, y, (pixman_box32_t *)NULL)
               ? True
               : False;
}

/* Clip an integer to the signed 16-bit coordinate range Xlib uses. */
static int mw_clamp16_i32(long long v)
{
    if (v < MW_INT16_MIN)
        return MW_INT16_MIN;
    if (v > MW_INT16_MAX)
        return MW_INT16_MAX;
    return (int)v;
}

/*
 * Xlib's own XRectInRegion sweep.  pixman's contains_rectangle() is close
 * but takes a fast path for single-rectangle regions that answers
 * differently from Xlib for zero-area query rectangles, so we reproduce
 * the classic algorithm against the (identically y-x-banded) pixman boxes.
 */
int XRectInRegion(Region region, int rx, int ry, unsigned int rwidth,
                  unsigned int rheight)
{
    pixman_box32_t *pbox;
    pixman_box32_t *extents;
    int n;
    int i;
    int x1, y1, x2, y2;
    int cx, cy;
    int partIn, partOut;

    if (region->numRects == 0)
        return RectangleOut;

    x1 = mw_clamp16_i32((long long)rx);
    y1 = mw_clamp16_i32((long long)ry);
    x2 = mw_clamp16_i32((long long)rx + (long long)rwidth);
    y2 = mw_clamp16_i32((long long)ry + (long long)rheight);

    pbox = pixman_region32_rectangles(&region->pix, &n);
    if (n <= 0)
        return RectangleOut;

    /* useful optimization: !EXTENTCHECK */
    extents = pixman_region32_extents(&region->pix);
    if (!(extents->x1 < x2 && x1 < extents->x2 &&
          extents->y1 < y2 && y1 < extents->y2))
        return RectangleOut;

    partIn = 0;
    partOut = 0;
    cx = x1;
    cy = y1;

    for (i = 0; i < n; i++) {
        if (pbox[i].y2 <= cy)
            continue;
        if (pbox[i].y1 > cy) {
            partOut = 1;
            if (partIn || (pbox[i].y1 >= y2))
                break;
            cy = pbox[i].y1;
        }
        if (pbox[i].x2 <= cx)
            continue;
        if (pbox[i].x1 > cx) {
            partOut = 1;
            if (partIn)
                break;
        }
        if (pbox[i].x1 < x2) {
            partIn = 1;
            if (partOut)
                break;
        }
        if (pbox[i].x2 >= x2) {
            cy = pbox[i].y2;
            if (cy >= y2)
                break;
            cx = x1;
        } else {
            break;
        }
    }

    return partIn ? (cy < y2 ? RectanglePart : RectangleIn) : RectangleOut;
}

/* --------------------------------------------------------------- set math */

int XIntersectRegion(Region sra, Region srb, Region dr)
{
    pixman_region32_t res;

    pixman_region32_init(&res);
    if (!pixman_region32_intersect(&res, &sra->pix, &srb->pix)) {
        pixman_region32_fini(&res);
        return 0;
    }
    return mw_commit(dr, &res);
}

int XUnionRegion(Region sra, Region srb, Region dr)
{
    pixman_region32_t res;

    pixman_region32_init(&res);
    if (!pixman_region32_union(&res, &sra->pix, &srb->pix)) {
        pixman_region32_fini(&res);
        return 0;
    }
    return mw_commit(dr, &res);
}

int XUnionRectWithRegion(XRectangle *rectangle, Region src_region,
                         Region dest_region_return)
{
    pixman_region32_t res;

    if (!rectangle->width || !rectangle->height)
        return 0;

    pixman_region32_init(&res);
    if (!pixman_region32_union_rect(&res, &src_region->pix, rectangle->x,
                                    rectangle->y, rectangle->width,
                                    rectangle->height)) {
        pixman_region32_fini(&res);
        return 0;
    }
    return mw_commit(dest_region_return, &res);
}

int XSubtractRegion(Region sra, Region srb, Region dr)
{
    pixman_region32_t res;

    pixman_region32_init(&res);
    if (!pixman_region32_subtract(&res, &sra->pix, &srb->pix)) {
        pixman_region32_fini(&res);
        return 0;
    }
    return mw_commit(dr, &res);
}

int XXorRegion(Region sra, Region srb, Region dr)
{
    pixman_region32_t ta;
    pixman_region32_t tb;
    pixman_region32_t res;
    int ok;

    pixman_region32_init(&ta);
    pixman_region32_init(&tb);
    pixman_region32_init(&res);

    ok = pixman_region32_subtract(&ta, &sra->pix, &srb->pix) &&
         pixman_region32_subtract(&tb, &srb->pix, &sra->pix) &&
         pixman_region32_union(&res, &ta, &tb);

    pixman_region32_fini(&ta);
    pixman_region32_fini(&tb);
    if (!ok) {
        pixman_region32_fini(&res);
        return 0;
    }
    return mw_commit(dr, &res);
}

/* ------------------------------------------------------- affine / shrink */

int XOffsetRegion(Region r, int dx, int dy)
{
    pixman_region32_translate(&r->pix, dx, dy);
    mw_clamp16(r);
    mw_sync_rects(r);
    return 1;
}

/*
 * Xlib's morphological shrink/expand helper.  `r' is replaced by the set of
 * points p for which p, p+1, ... p+dx are all in r (grow == FALSE), or for
 * which at least one of them is in r (grow == TRUE).
 */
static void mw_copy_region(Region dst, Region src)
{
    pixman_region32_t res;

    pixman_region32_init(&res);
    if (pixman_region32_copy(&res, &src->pix))
        (void)mw_commit(dst, &res);
    else
        pixman_region32_fini(&res);
}

static void mw_compress(Region r, Region s, Region t, unsigned int dx,
                        int xdir, int grow)
{
    unsigned int shift = 1;

    mw_copy_region(s, r);
    while (dx) {
        if (dx & shift) {
            if (xdir)
                XOffsetRegion(r, -(int)shift, 0);
            else
                XOffsetRegion(r, 0, -(int)shift);
            if (grow)
                XUnionRegion(r, s, r);
            else
                XIntersectRegion(r, s, r);
            dx -= shift;
            if (!dx)
                break;
        }
        mw_copy_region(t, s);
        if (xdir)
            XOffsetRegion(s, -(int)shift, 0);
        else
            XOffsetRegion(s, 0, -(int)shift);
        if (grow)
            XUnionRegion(s, t, s);
        else
            XIntersectRegion(s, t, s);
        shift <<= 1;
    }
}

int XShrinkRegion(Region r, int dx, int dy)
{
    Region s;
    Region t;
    int grow;

    if (!dx && !dy)
        return 1;

    s = XCreateRegion();
    if (!s)
        return 0;
    t = XCreateRegion();
    if (!t) {
        XDestroyRegion(s);
        return 0;
    }

    if ((grow = (dx < 0)))
        dx = -dx;
    if (dx)
        mw_compress(r, s, t, 2u * (unsigned int)dx, 1, grow);

    if ((grow = (dy < 0)))
        dy = -dy;
    if (dy)
        mw_compress(r, s, t, 2u * (unsigned int)dy, 0, grow);

    XOffsetRegion(r, dx, dy);

    XDestroyRegion(s);
    XDestroyRegion(t);
    return 1;
}

int XClipBox(Region r, XRectangle *rect_return)
{
    pixman_box32_t *ext = pixman_region32_extents(&r->pix);

    if (getenv("MW_TRACE_REGION")) {
        int n = 0;
        pixman_region32_rectangles(&r->pix, &n);
        fprintf(stderr, "MW: XClipBox nrects=%d -> %d,%d %dx%d\n", n,
                ext->x1, ext->y1, ext->x2 - ext->x1, ext->y2 - ext->y1);
    }
    rect_return->x = (short)ext->x1;
    rect_return->y = (short)ext->y1;
    rect_return->width = (unsigned short)(ext->x2 - ext->x1);
    rect_return->height = (unsigned short)(ext->y2 - ext->y1);
    return 1;
}

/* ---------------------------------------------------- polygon scan fill */

#ifdef EvenOddRule
#define MW_RULE_EVENODD EvenOddRule
#else
#define MW_RULE_EVENODD 0
#endif

#define MW_LARGE_COORD 1000000
#define MW_SMALL_COORD (-1000000)
#define MW_SLLSPERBLOCK 25

typedef struct {
    int minor_axis;
    int d;
    int m;
    int m1;
    int incr1;
    int incr2;
} MwBresInfo;

typedef struct MwEdge {
    int ymax;
    MwBresInfo bres;
    struct MwEdge *next;
    struct MwEdge *back;
    struct MwEdge *nextWETE;
    int ClockWise;
} MwEdge;

typedef struct MwScanLine {
    int scanline;
    MwEdge *edgelist;
    struct MwScanLine *next;
} MwScanLine;

typedef struct {
    int ymax;
    int ymin;
    MwScanLine scanlines;
} MwEdgeTable;

typedef struct MwScanLineBlock {
    MwScanLine SLLs[MW_SLLSPERBLOCK];
    struct MwScanLineBlock *next;
} MwScanLineBlock;

/* Bresenham edge setup, after Xlib's poly.h. */
#define MW_BRESINITPGON(dy, x1, x2, xStart, d, m, m1, incr1, incr2)          \
    {                                                                        \
        int mw_dx;                                                           \
        if ((dy) != 0) {                                                     \
            xStart = (x1);                                                   \
            mw_dx = (x2) - xStart;                                           \
            if (mw_dx < 0) {                                                 \
                m = mw_dx / (dy);                                            \
                m1 = m - 1;                                                  \
                incr1 = -2 * mw_dx + 2 * (dy) * m1;                          \
                incr2 = -2 * mw_dx + 2 * (dy) * m;                           \
                d = 2 * m * (dy) - 2 * mw_dx - 2 * (dy);                     \
            } else {                                                         \
                m = mw_dx / (dy);                                            \
                m1 = m + 1;                                                  \
                incr1 = 2 * mw_dx - 2 * (dy) * m1;                           \
                incr2 = 2 * mw_dx - 2 * (dy) * m;                            \
                d = -2 * m * (dy) + 2 * mw_dx;                               \
            }                                                                \
        }                                                                    \
    }

#define MW_BRESINCRPGON(d, minval, m, m1, incr1, incr2)                       \
    {                                                                        \
        if (m1 > 0) {                                                        \
            if (d > 0) {                                                     \
                minval += m1;                                                \
                d += incr1;                                                  \
            } else {                                                         \
                minval += m;                                                 \
                d += incr2;                                                  \
            }                                                                \
        } else {                                                             \
            if (d >= 0) {                                                    \
                minval += m1;                                                \
                d += incr1;                                                  \
            } else {                                                         \
                minval += m;                                                 \
                d += incr2;                                                  \
            }                                                                \
        }                                                                    \
    }

#define MW_BRESINITPGONSTRUCT(dmaj, min1, min2, bres)                         \
    MW_BRESINITPGON(dmaj, min1, min2, bres.minor_axis, bres.d, bres.m,       \
                    bres.m1, bres.incr1, bres.incr2)

#define MW_BRESINCRPGONSTRUCT(bres)                                          \
    MW_BRESINCRPGON(bres.d, bres.minor_axis, bres.m, bres.m1, bres.incr1,    \
                    bres.incr2)

#define MW_EVAL_EVENODD(pAET, pPrevAET, y)                                   \
    {                                                                        \
        if (pAET->ymax == y) {                                               \
            pPrevAET->next = pAET->next;                                     \
            pAET = pPrevAET->next;                                           \
            if (pAET)                                                        \
                pAET->back = pPrevAET;                                       \
        } else {                                                             \
            MW_BRESINCRPGONSTRUCT(pAET->bres);                               \
            pPrevAET = pAET;                                                 \
            pAET = pAET->next;                                               \
        }                                                                    \
    }

#define MW_EVAL_WINDING(pAET, pPrevAET, y, fixWAET)                          \
    {                                                                        \
        if (pAET->ymax == y) {                                               \
            pPrevAET->next = pAET->next;                                     \
            pAET = pPrevAET->next;                                           \
            fixWAET = 1;                                                     \
            if (pAET)                                                        \
                pAET->back = pPrevAET;                                       \
        } else {                                                             \
            MW_BRESINCRPGONSTRUCT(pAET->bres);                               \
            pPrevAET = pAET;                                                 \
            pAET = pAET->next;                                               \
        }                                                                    \
    }

static void mw_free_sll(MwScanLineBlock *b)
{
    while (b) {
        MwScanLineBlock *next = b->next;
        free(b);
        b = next;
    }
}

static int mw_insert_edge(MwEdgeTable *et, MwEdge *ete, int scanline,
                          MwScanLineBlock **sll_block, int *i_sll_block)
{
    MwEdge *start;
    MwEdge *prev;
    MwScanLine *psll;
    MwScanLine *pprevsll;
    MwScanLineBlock *tmp;

    pprevsll = &et->scanlines;
    psll = pprevsll->next;
    while (psll && (psll->scanline < scanline)) {
        pprevsll = psll;
        psll = psll->next;
    }

    if ((!psll) || (psll->scanline > scanline)) {
        if (*i_sll_block > MW_SLLSPERBLOCK - 1) {
            tmp = malloc(sizeof *tmp);
            if (!tmp)
                return 0;
            (*sll_block)->next = tmp;
            tmp->next = (MwScanLineBlock *)NULL;
            *sll_block = tmp;
            *i_sll_block = 0;
        }
        psll = &((*sll_block)->SLLs[(*i_sll_block)++]);
        psll->next = pprevsll->next;
        psll->edgelist = (MwEdge *)NULL;
        pprevsll->next = psll;
    }
    psll->scanline = scanline;

    prev = (MwEdge *)NULL;
    start = psll->edgelist;
    while (start && (start->bres.minor_axis < ete->bres.minor_axis)) {
        prev = start;
        start = start->next;
    }
    ete->next = start;
    if (prev)
        prev->next = ete;
    else
        psll->edgelist = ete;
    return 1;
}

static int mw_create_et(int count, XPoint *pts, MwEdgeTable *et, MwEdge *aet,
                        MwEdge *etes, MwScanLineBlock *sll_block)
{
    XPoint *top;
    XPoint *bottom;
    XPoint *prevpt;
    XPoint *currpt;
    int i_sll_block = 0;

    if (count < 2)
        return 1;

    aet->next = (MwEdge *)NULL;
    aet->back = (MwEdge *)NULL;
    aet->nextWETE = (MwEdge *)NULL;
    aet->bres.minor_axis = MW_SMALL_COORD;

    et->scanlines.next = (MwScanLine *)NULL;
    et->ymax = MW_SMALL_COORD;
    et->ymin = MW_LARGE_COORD;
    sll_block->next = (MwScanLineBlock *)NULL;

    prevpt = &pts[count - 1];
    while (count--) {
        currpt = pts++;

        if (prevpt->y > currpt->y) {
            bottom = prevpt;
            top = currpt;
            etes->ClockWise = 0;
        } else {
            bottom = currpt;
            top = prevpt;
            etes->ClockWise = 1;
        }

        if (bottom->y != top->y) {
            int dy;

            etes->ymax = bottom->y - 1;
            dy = bottom->y - top->y;
            MW_BRESINITPGONSTRUCT(dy, top->x, bottom->x, etes->bres);

            if (!mw_insert_edge(et, etes, top->y, &sll_block, &i_sll_block))
                return 0;

            if (prevpt->y > et->ymax)
                et->ymax = prevpt->y;
            if (prevpt->y < et->ymin)
                et->ymin = prevpt->y;
            etes++;
        }
        prevpt = currpt;
    }
    return 1;
}

static void mw_load_aet(MwEdge *aet, MwEdge *etes)
{
    MwEdge *prev = aet;
    MwEdge *tmp;

    aet = aet->next;
    while (etes) {
        while (aet && (aet->bres.minor_axis < etes->bres.minor_axis)) {
            prev = aet;
            aet = aet->next;
        }
        tmp = etes->next;
        etes->next = aet;
        if (aet)
            aet->back = etes;
        etes->back = prev;
        prev->next = etes;
        prev = etes;
        etes = tmp;
    }
}

static void mw_compute_waet(MwEdge *aet)
{
    MwEdge *pwete;
    int inside = 1;
    int isinside = 0;

    aet->nextWETE = (MwEdge *)NULL;
    pwete = aet;
    aet = aet->next;
    while (aet) {
        if (aet->ClockWise)
            isinside++;
        else
            isinside--;

        if ((!inside && !isinside) || (inside && isinside)) {
            pwete->nextWETE = aet;
            pwete = aet;
            inside = !inside;
        }
        aet = aet->next;
    }
    pwete->nextWETE = (MwEdge *)NULL;
}

static int mw_insertion_sort(MwEdge *aet)
{
    MwEdge *pchase;
    MwEdge *pinsert;
    MwEdge *backup;
    int changed = 0;

    aet = aet->next;
    while (aet) {
        pinsert = aet;
        pchase = aet;
        while (pchase->back->bres.minor_axis > aet->bres.minor_axis)
            pchase = pchase->back;

        aet = aet->next;
        if (pchase != pinsert) {
            backup = pchase->back;
            pinsert->back->next = aet;
            if (aet)
                aet->back = pinsert->back;
            pinsert->next = pchase;
            pchase->back->next = pinsert;
            pchase->back = pinsert;
            pinsert->back = backup;
            changed = 1;
        }
    }
    return changed;
}

typedef struct {
    int x;
    int y;
} MwPt;

typedef struct {
    MwPt *pts;
    size_t n;
    size_t cap;
} MwPtBuf;

static int mw_ptbuf_push(MwPtBuf *b, int x, int y)
{
    if (b->n == b->cap) {
        size_t ncap = b->cap ? b->cap * 2 : 64;
        MwPt *np = realloc(b->pts, ncap * sizeof *np);

        if (!np)
            return 0;
        b->pts = np;
        b->cap = ncap;
    }
    b->pts[b->n].x = x;
    b->pts[b->n].y = y;
    b->n++;
    return 1;
}

Region XPolygonRegion(XPoint *points, int n, int fill_rule)
{
    Region region;
    MwEdgeTable et;
    MwEdge aet;
    MwEdge *etes;
    MwScanLineBlock sll;
    MwPtBuf buf;
    pixman_box32_t *boxes;
    size_t nspans;
    size_t m;
    size_t i;
    int y;
    int ok;
    int fix_waet;

    region = XCreateRegion();
    if (!region)
        return (Region)NULL;

    /* Fast path for axis-aligned rectangles, as in Xlib. */
    if (((n == 4) ||
         ((n == 5) && (points[4].x == points[0].x) &&
          (points[4].y == points[0].y))) &&
        (((points[0].y == points[1].y) &&
          (points[1].x == points[2].x) &&
          (points[2].y == points[3].y) &&
          (points[3].x == points[0].x)) ||
         ((points[0].x == points[1].x) &&
          (points[1].y == points[2].y) &&
          (points[2].x == points[3].x) &&
          (points[3].y == points[0].y)))) {
        int x1 = MW_MIN(points[0].x, points[2].x);
        int y1 = MW_MIN(points[0].y, points[2].y);
        int x2 = MW_MAX(points[0].x, points[2].x);
        int y2 = MW_MAX(points[0].y, points[2].y);

        if (x1 != x2 && y1 != y2) {
            pixman_box32_t box;

            box.x1 = x1;
            box.y1 = y1;
            box.x2 = x2;
            box.y2 = y2;
            (void)mw_set_boxes(region, &box, 1);
        } else {
            (void)mw_set_boxes(region, (const pixman_box32_t *)NULL, 0);
        }
        return region;
    }

    if (n < 2) {
        (void)mw_set_boxes(region, (const pixman_box32_t *)NULL, 0);
        return region;
    }

    etes = malloc((size_t)n * sizeof *etes);
    if (!etes) {
        XDestroyRegion(region);
        return (Region)NULL;
    }

    sll.next = (MwScanLineBlock *)NULL;
    if (!mw_create_et(n, points, &et, &aet, etes, &sll)) {
        free(etes);
        mw_free_sll(sll.next);
        XDestroyRegion(region);
        return (Region)NULL;
    }

    buf.pts = (MwPt *)NULL;
    buf.n = 0;
    buf.cap = 0;
    ok = 1;
    fix_waet = 0;

    if (fill_rule == MW_RULE_EVENODD) {
        MwScanLine *psll = et.scanlines.next;

        for (y = et.ymin; y < et.ymax; y++) {
            MwEdge *paet;
            MwEdge *pprevaet;

            if (psll && y == psll->scanline) {
                mw_load_aet(&aet, psll->edgelist);
                psll = psll->next;
            }
            pprevaet = &aet;
            paet = aet.next;
            while (paet) {
                if (!mw_ptbuf_push(&buf, paet->bres.minor_axis, y)) {
                    ok = 0;
                    break;
                }
                MW_EVAL_EVENODD(paet, pprevaet, y);
            }
            if (!ok)
                break;
            (void)mw_insertion_sort(&aet);
        }
    } else {
        MwScanLine *psll = et.scanlines.next;

        for (y = et.ymin; y < et.ymax; y++) {
            MwEdge *paet;
            MwEdge *pprevaet;
            MwEdge *pwete;

            if (psll && y == psll->scanline) {
                mw_load_aet(&aet, psll->edgelist);
                mw_compute_waet(&aet);
                psll = psll->next;
            }
            pprevaet = &aet;
            paet = aet.next;
            pwete = paet;
            while (paet) {
                if (pwete == paet) {
                    if (!mw_ptbuf_push(&buf, paet->bres.minor_axis, y)) {
                        ok = 0;
                        break;
                    }
                    pwete = pwete->nextWETE;
                }
                MW_EVAL_WINDING(paet, pprevaet, y, fix_waet);
            }
            if (!ok)
                break;
            if (mw_insertion_sort(&aet) || fix_waet) {
                mw_compute_waet(&aet);
                fix_waet = 0;
            }
        }
    }

    if (!ok) {
        free(buf.pts);
        free(etes);
        mw_free_sll(sll.next);
        XDestroyRegion(region);
        return (Region)NULL;
    }

    nspans = buf.n / 2;
    boxes = (pixman_box32_t *)NULL;
    if (nspans > 0) {
        boxes = malloc(nspans * sizeof *boxes);
        if (!boxes) {
            free(buf.pts);
            free(etes);
            mw_free_sll(sll.next);
            XDestroyRegion(region);
            return (Region)NULL;
        }
    }

    m = 0;
    for (i = 0; i + 1 < buf.n; i += 2) {
        int xl = buf.pts[i].x;
        int xr = buf.pts[i + 1].x;
        int yy = buf.pts[i].y;

        if (xl == xr)
            continue;
        boxes[m].x1 = xl;
        boxes[m].y1 = yy;
        boxes[m].x2 = xr;
        boxes[m].y2 = yy + 1;
        m++;
    }

    (void)mw_set_boxes(region, boxes, m);
    free(boxes);
    free(buf.pts);
    free(etes);
    mw_free_sll(sll.next);
    return region;
}
