# Motif/Wayland — Design Document

**Status:** draft / living document
**Target:** run unmodified Motif applications — specifically **XV** and **NEdit** — natively on
Wayland, with the same public API as [Open Motif](https://archive.opengroup.org/openmotif/).

---

## 1. Summary

"Motif on Wayland" is usually imagined as "rewrite the widget set." That is the wrong layer.

The Motif stack is three libraries layered on top of one another:

```
        applications          XV, NEdit, ...            (unmodified)
        ────────────────────────────────────────────
        libXm   (Motif widgets, ~210 .c files)          (unmodified upstream source)
        libXt   (X Toolkit Intrinsics)                  (unmodified upstream source)
        libX11  (Xlib client library)  ← the platform layer
        ────────────────────────────────────────────
        X11 protocol / socket        ← replaced by
        ────────────────────────────────────────────
        Wayland (wl_display, xdg-shell, wl_shm, wl_seat, ...)
```

Xlib is the *only* layer that is platform-specific. Its API is deliberately opaque at the
transport boundary: a `Display *`, a `Window`, a `GC`, an `XImage`, etc. are abstract handles.
If we provide a binary-compatible `libX11` whose implementation talks Wayland instead of the X11
wire protocol, then **libXt, libXm, XV and NEdit compile and run unmodified** — they cannot tell
the difference.

So this project is:

> **A Wayland-native, ABI-compatible implementation of `libX11` (Xlib), plus the build
> integration that lets the stock Xt/Motif stack and stock applications sit on top of it.**

We call the compatibility library **`libX11` (Wayland backend)**, installed as `libX11.so.6`
so that existing link lines (`-lX11`) and `dlopen("libX11.so.6")` resolve to it unchanged.

### 1.1 Goals

* G1 — Provide the Xlib C API and ABI used by libXt, libXm, XV and NEdit.
* G2 — Compile and run **XV 6.2** and **NEdit 5.7** from unmodified upstream sources.
* G3 — Run natively on Wayland: no XWayland, no X server, no socket to `:0`.
* G4 — Preserve the Open Motif API (`Xm/*.h`) and behaviour exactly; we do not fork Motif.
* G5 — Deterministic, headless-testable: a bundled headless compositor makes the stack testable
  in CI without a desktop session.

### 1.2 Non-goals (v1)

* Rendering *every* Xlib extension. We implement what the target set needs, shim the rest.
* Colour-managed / antialiased text via **Xft/XRender** — deferred (see §9, §12). Motif builds
  with `--disable-xft`; NEdit does not use Xft at all.
* XDMCP, remote displays, X authority, multi-screen, Xinerama across real monitors.
* Server-side decorations; v1 uses client-side `xdg-decoration`/titleless toplevels.
* Network transparency (a Wayland-native toolkit is inherently local).

---

## 2. Evidence base

The design is grounded in the actual required surface, extracted from upstream sources
(`xv`, `nedit`, `libxt`, `motif/lib/Xm`, `motif/lib/Mrm`).

### 2.1 Library dependency

| Consumer | Needs |
|---|---|
| `xv/src` | `libX11`, `libXt` (AppContext + signals only), `libXrandr` (optional) |
| `nedit/source` | `libXm`, `libXt`, `libX11` |
| `libXt/src` | `libX11`, `libSM`/`libICE` (session mgmt, optional) |
| `motif/lib/Xm` | `libXt` (public **and private** headers), `libX11`, `libXft` (optional), `libXmu` (editres) |

### 2.2 Required Xlib function surface

Grep over the four codebases (excluding `Xm*`, `Xt*`, `Xlt*`) yields **307 `X*` symbols**.
Grouped by subsystem:

* **Display / connection** `XOpenDisplay`, `XCloseDisplay`, `XFlush`, `XSync`,
  `XSynchronize`, `XDisplayString`, `XDefaultScreen`, `XScreenCount`, `XScreenOfDisplay`,
  `XConnectionNumber` (macro), `XMaxRequestSize`, `XExtendedMaxRequestSize`,
  `XAddConnectionWatch`, `XProcessInternalConnection`, `XLastKnownRequestProcessed`,
  `XNextRequest`, `XSetCloseDownMode`, `XKillClient`, `XSetErrorHandler`,
  `XSetIOErrorHandler`, `XESetCloseDisplay`, `XGetErrorText`, `XDisplayName`.
* **Windows** `XCreateWindow`, `XCreateSimpleWindow`, `XDestroyWindow`, `XMapWindow`,
  `XMapRaised`, `XMapSubwindows`, `XMapSubWindows`, `XUnmapWindow`, `XConfigureWindow`,
  `XMoveWindow`, `XResizeWindow`, `XMoveResizeWindow`, `XRaiseWindow`, `XLowerWindow`,
  `XReparentWindow`, `XClearWindow`, `XClearArea`, `XGetGeometry`, `XGetWindowAttributes`,
  `XChangeWindowAttributes`, `XQueryTree`, `XTranslateCoordinates`,
  `XSetWindowBackground[Pixmap]`, `XSetWindowColormap`, `XSetWMProperties`, `XSetWMNormalHints`,
  `XGetNormalHints`, `XSetWMHints`, `XGetWMHints`, `XSetIconName`, `XSetWMName`, `XSetWMIconName`,
  `XSetCommand`, `XSetStandardProperties`, `XSetTransientForHint`, `XSetClassHint`,
  `XAllocSizeHints`, `XAllocClassHint`, `XWMGeometry`, `XIconifyWindow`, `XWithdrawWindow`,
  `XSetWMProtocols`, `XGetWMColormapWindows`, `XSetWMColormapWindows`, `XSetInputFocus`,
  `XGetInputFocus`, `XKillClient`, `XGetWindowProperty`.
* **Events** `XNextEvent`, `XPeekEvent`, `XPutBackEvent`, `XPending`, `XEventsQueued`,
  `XIfEvent`, `XCheckIfEvent`, `XCheckMaskEvent`, `XCheckTypedWindowEvent`,
  `XCheckWindowEvent`, `XWindowEvent`, `XMaskEvent`, `XSendEvent`, `XAllowEvents`.
* **Drawing** `XCreateGC`, `XChangeGC`, `XGetGCValues`, `XFreeGC`,
  `XDrawPoint[s]`, `XDrawLine`, `XDrawLines`, `XDrawSegment[s]`, `XDrawRectangle[s]`,
  `XDrawArc[s]`, `XFillArc[s]`, `XFillPolygon`, `XFillRectangle[s]`, `XCopyArea`, `XCopyPlane`,
  `XSetForeground`, `XSetBackground`, `XSetFunction`, `XSetLineAttributes`, `XSetFillStyle`,
  `XSetState`, `XSetPlaneMask`, `XSetTSOrigin`, `XSetStipple`, `XSetFont`, `XSetClipRectangles`,
  `XSetRegion`, `XSetClipMask`, `XSetClipOrigin`, `XSetSubwindowMode`, `XImageDepth`,
  `XImageWidth/Height`.
* **Pixmaps / images** `XCreatePixmap`, `XFreePixmap`, `XCreateBitmapFromData`,
  `XCreatePixmapFromBitmapData`, `XCreatePixmapFromData`, `XCreatePixmapCursor`,
  `XCreateDataFromPixmap`, `XReadBitmapFileData`, `XReadPixmapFile`(xv ext), `XWritePixmapFile`,
  `XCreateImage`, `XDestroyImage`, `XGetImage`, `XPutImage`, `XGetPixel`, `XPutPixel`,
  `XGetSubImage`(XV), `XBitmapPad`, `XImageByteOrder`.
* **Colour / visual** `XAllocColor`, `XAllocColorCells`, `XAllocNamedColor`, `XLookupColor`,
  `XQueryColor`, `XQueryColors`, `XStoreColor`, `XFreeColors`, `XParseColor`, `XCreateColormap`,
  `XFreeColormap`, `XCopyColormapAndFree`, `XInstallColormap`, `XUninstallColormap`,
  `XListInstalledColormaps`, `XDefaultColormap`, `XDefaultDepth`, `XDefaultVisual`,
  `XDefaultScreen`, `XMatchVisualInfo`, `XGetVisualInfo`, `XVisualIDFromVisual`,
  `XBlackPixelOfScreen`, `XWhitePixelOfScreen`, `XListDepths`, `XDisplayHeight/Width`.
* **Font / text** `XLoadFont`, `XUnloadFont`, `XLoadQueryFont`, `XQueryFont`, `XFreeFont`,
  `XListFonts`, `XFreeFontNames`, `XGetFontProperty`, `XTextWidth`, `XTextWidth16`,
  `XTextExtents`, `XTextExtents16`, `XDrawString`, `XDrawString16`, `XDrawImageString`,
  `XDrawImageString16`, `XDrawText16`, `XCreateFontSet`, `XFreeFontSet`, `XFontsOfFontSet`,
  `XExtentsOfFontSet`, `XSupportsLocale`, `XSetLocaleModifiers`.
* **Input / keyboard** `XSelectInput`, `XGrabButton`, `XUngrabButton`, `XGrabKey`,
  `XUngrabKey`, `XGrabPointer`, `XUngrabPointer`, `XGrabKeyboard`, `XUngrabKeyboard`,
  `XGrabServer`, `XUngrabServer`, `XWarpPointer`, `XQueryPointer`, `XChangeActivePointerGrab`,
  `XLookupString`, `XLookupKeysym`, `XGetKeyboardMapping`, `XDisplayKeycodes`,
  `XGetModifierMapping`, `XFreeModifiermap`, `XKeycodeToKeysym`, `XKeysymToKeycode`,
  `XKeysymToString`, `XStringToKeysym`, `XConvertCase`, `XRefreshKeyboardMapping`.
* **Selections / cut buffers** `XSetSelectionOwner`, `XGetSelectionOwner`,
  `XConvertSelection`, `XSendEvent`, `XFetchBuffer`, `XStoreBuffer`, `XRotateBuffers`,
  `XStoreBytes`.
* **Regions** `XCreateRegion`, `XDestroyRegion`, `XUnionRegion`, `XUnionRectWithRegion`,
  `XIntersectRegion`, `XSubtractRegion`, `XOffsetRegion`, `XShrinkRegion`, `XEmptyRegion`,
  `XEqualRegion`, `XPointInRegion`, `XRectInRegion`, `XClipBox`,
  `XPolygonRegion`, `XXorRegion`.
* **Cursors** `XCreateFontCursor`, `XCreatePixmapCursor`, `XFreeCursor`, `XDefineCursor`,
  `XUndefineCursor`, `XRecolorCursor`, `XQueryBestCursor`.
* **Inter-client / misc** `XInternAtom[s]`, `XGetAtomName[s]`, `XChangeProperty`,
  `XGetWindowProperty`, `XDeleteProperty`, `XListProperties`, `XGetDefault`,
  `XResourceManagerString`, `XScreenResourceString`, `XSaveContext`, `XFindContext`,
  `XDeleteContext`, `XUniqueContext`, `XSetWMProperties`, `XGetTextProperty`,
  `XSetTextProperty`, `XGetWMName`, `XParseGeometry`, `XWMGeometry`, `XBell`.
* **XIM / XOM / IC** `XOpenIM`, `XCloseIM`, `XCreateIC`, `XDestroyIC`, `XSetICValues`,
  `XGetICValues`, `XSetICFocus`, `XUnsetICFocus`, `XIMOfIC`, `XOMOfOC`, `XmbLookupString`,
  `XFilterEvent`, `XCreateFontSet` (above).
* **Resource manager (Xrm)** — 54 `Xrm*` functions, part of libX11 and used heavily by Xm:
  `XrmInitialize`, `XrmGetStringDatabase`, `XrmGetFileDatabase`, `XrmGetDatabase`,
  `XrmCombineDatabase`, `XrmGetResource`, `XrmEnumerateDatabase`, `XrmDestroyDatabase`,
  `XrmParseCommand`, `XrmStringToQuark`, … (see `src/xlib/resource.c`).
* **Extensions** `XRR*` (XRandR: `XRRGetScreenResources`, `XRRGetCrtcInfo`,
  `XRRGetOutputInfo`, `XRRSelectInput`, `XRRUpdateConfiguration`, …), `XShape*`
  (`XShapeQueryExtension`, `XShapeCombineRectangles`), plus XKB `XkbLookupKeySym`.

Anything in this set may be called by *any* layer, so it must behave correctly, not merely link.

### 2.3 What XV actually needs from Xt

XV is **not** a widget application. It defines its own UI directly on Xlib and uses Xt only for
the application context and the OS-signal-aware event loop:

```
XOpenDisplay / XtCreateApplicationContext / XtToolkitInitialize / XtDisplayInitialize
XtAppNextEvent(context, &event)            (xvevent.c, xvgrab.c, xvpopup.c, ...)
XtAppAddSignal / XtNoticeSignal / XtRemoveSignal   (SIGINT/SIGHUP/SIGTERM notifier)
```

Consequence: **Milestone 1 (XV) needs only Xlib + a thin but genuine libXt.** It does not need
libXm. This gives us an early, meaningful end-to-end target before tackling Motif.

### 2.4 What NEdit needs

Everything: 62 `Xm/*.h` headers, 1111 distinct `Xm*` symbols, private Xt headers
(`IntrinsicP.h`, `ShellP.h`, `VendorP.h`, `ObjectP.h`), the Xrm resource manager, XIM-based
text input, and Motif's own drag-and-drop. This is the full-stack milestone (M4).

---

## 3. Architecture

### 3.1 Layering (runtime)

```
┌────────────────────────────────────────────────────────────────────────┐
│  Application (XV / NEdit) — unmodified                                  │
├────────────────────────────────────────────────────────────────────────┤
│  libXm.so  (Motif, upstream, unmodified)                                │
│  libXt.so  (Xt, upstream, unmodified)                                   │
├────────────────────────────────────────────────────────────────────────┤
│  libX11.so  ← THIS PROJECT: Xlib ABI, Wayland implementation            │
│    ┌──────────────┐ ┌──────────────┐ ┌──────────────┐ ┌──────────────┐  │
│    │ Object model │ │ Raster/text  │ │ Input/events │ │ Xrm/XIM/ext  │  │
│    │ Display,     │ │ cairo +      │ │ xkbcommon,   │ │ resource     │  │
│    │ Window tree, │ │ FreeType +   │ │ wl_seat,     │ │ manager,     │  │
│    │ GC, Pixmap,  │ │ fontconfig,  │ │ grabs, focus │ │ atoms, props │  │
│    │ XImage,      │ │ pixman-      │ │ translation  │ │ selections,  │  │
│    │ Colormap,    │ │ compatible   │ │ to XEvent    │ │ XRandR/XShape│  │
│    │ Region, Atom │ │ ARGB32       │ │              │ │ stubs, XIM   │  │
│    └──────┬───────┘ └──────┬───────┘ └──────┬───────┘ └──────┬───────┘  │
│           └────────────────┴────────────────┴────────────────┘          │
│                        Wayland backend (libwayland-client)              │
│     wl_display · wl_registry · wl_compositor · wl_shm · xdg_wm_base     │
│     wl_seat · wl_keyboard · wl_pointer · wl_data_device · wl_cursor     │
│     xdg_toplevel · xdg_popup · xdg_decoration · wl_output              │
└────────────────────────────────────────────────────────────────────────┘
```

### 3.2 The central problem: X windows → Wayland surfaces

X11 gives every window a server-side existence and a *tree*. Wayland gives an application a
small number of `wl_surface`s and makes the client responsible for everything inside them.
Motif creates many X windows (one per realized widget). The mapping is therefore:

* **Root window** (one per screen): synthetic; not a `wl_surface`. Owns global state (atoms,
  properties, resource-manager property, selection ownership).
* **Top-level non-override-redirect window**: owns an `xdg_toplevel` + `wl_surface`, a
  `wl_shm` pool and a cairo image surface ("the toplevel back buffer").
* **Override-redirect window** (menus, pulldowns, tooltips, drag icons): an `xdg_popup` with
  its parent toplevel; fall back to a plain `xdg_toplevel` if positioning is unavailable.
* **Child windows** (widget internals): *not* surfaces. They are rectangles in an in-process
  window tree and are composited into the nearest ancestor toplevel's back buffer.

Because we own the whole tree, we implement X's drawing semantics ourselves:

* Every window has a **backing cairo surface** sized to its content.
* Drawing calls target a window, with coordinates relative to that window, clipped to its
  border/clip region and to `subwindow-mode`.
* On any change, the owning toplevel is **recomposited**: paint root background, then paint the
  window tree depth-first (parent, then children in stacking order) into the toplevel surface,
  then `wl_surface_attach/commit` with damage.
* `Expose` events are synthesised when a window becomes (partially) visible per X rules or when
  its backing is invalidated (initial map, resize, sibling unmap, `XClearArea(exposures=True)`).

Using *always-on backing store* is a deliberate simplification: it is strictly more forgiving
than X's default and eliminates a large class of "lost drawing" bugs in clients that draw once
and never redraw. It costs memory proportional to the total window area, which for XV/NEdit is
acceptable.

### 3.3 Event loop integration — the linchpin

libXt's `NextEvent.c` does:

```c
fdlp->fd = ConnectionNumber(app->list[ii]);   /* the X display fd */
FD_SET(ConnectionNumber(app->list[ii]), &wf->rmask);
... select(...) ...
XEventsQueued(dpy, QueuedAfterReading);
```

(`libxt/src/NextEvent.c:293,325`; also `XtAppAddInput`, `_XtWaitForSomething`.)

Therefore:

* **`ConnectionNumber(dpy)` = `wl_display_get_fd()`.** This single fact makes unmodified Xt
  block on Wayland events exactly as it would block on X11 events.
* `XFlush` → `wl_display_flush`; `XSync` → flush + `roundtrip` (or flush + read to quiescence).
* `XPending`/`XEventsQueued` → the canonical Wayland nonblocking drain:
  `wl_display_dispatch_pending` → `wl_display_prepare_read` → `poll(fd,0)` →
  `wl_display_read_events`, then run the event translator.
* `XNextEvent` (blocking) → if the queue is empty, `wl_display_dispatch` (which blocks) then
  translate; otherwise pop from the queue.

The XEvent queue is a bounded ring with `XPutBackEvent` support (libXt uses it). Error events
(`XErrorEvent`) are delivered through the same queue with `XSetErrorHandler` semantics
(*not* invoked synchronously; Xlib defers them).

### 3.4 Rendering

* Drawables (`Window`, `Pixmap`) are **cairo image surfaces**, format `ARGB32`
  (premultiplied), which matches `wl_shm` `ARGB8888`. For `XPutImage` with `XRGB`/`ZPixmap`
  we convert through a per-`XImage` representation.
* `GC` holds: foreground/background pixels, function (`GXcopy`/`GXxor`/…), plane mask, line
  width/style/join/cap/dash, fill style, `ts_origin`, clip (rectangles/region/mask/origin),
  `subwindow mode`, font, stipple/tile, `graphics_exposures`.
* The rasteriser sits behind a small internal interface, **`mw_raster_*`** (`src/raster/raster.h`),
  so it is a *backend swap, not an architectural commitment*. The shipped backend is **cairo**:
  paths for lines/arcs/polygons/rectangles, `cairo_paint` for fills, `cairo_mask_surface` for
  stipple/tile, `cairo_set_operator` for `GX*` functions, `cairo_clip` for GC clip and window
  clipping. Region maths use **pixman** regions to mirror X region semantics precisely, then feed
  rectangles to the backend.
* `XCopyArea`/`XCopyPlane` are surface blits with source/dest offsets and clip.

**Why cairo behind an interface, and not Skia (v1).** The workload is CPU-first: Xlib drawables
are addressable (`XGetImage`, `XPutImage`, `XGetPixel`), so even a GPU library such as Skia would
keep a raster CPU surface and gain nothing from Ganesh/Vulkan; the GPU only matters for the final
composite into `wl_shm`/dmabuf, which is orthogonal to the drawing engine. cairo is C (matching the
C shim), ubiquitous, and has `cairo-ft`/fontconfig for core-font emulation. Skia is C++, not
packaged here, and would add a multi-GB source build and `libstdc++` to the link line to replace a
few hundred lines of glue. Keeping the interface narrow means a **Skia backend can be added later
as one additional `.cpp` translation unit** (`mw_raster_skia.cpp`, rasterising into
`SkSurface::MakeRasterDirect` over the same `wl_shm` memory) with no change to any Xlib semantics
or to Xt/Xm/XV/NEdit. The raster interface is designed to be the *only* place a backend is named.

The interface is deliberately small:

```c
/* src/raster/raster.h — backend-neutral, no cairo/Skia types leak out */
typedef struct MwSurface MwSurface;   /* a drawable's pixels (ARGB32, premultiplied) */
MwSurface *mw_surface_create(int w, int h);                 /* owns its memory */
MwSurface *mw_surface_create_for_data(void *p, int w, int h,
                                      int stride);          /* wraps wl_shm memory */
void       mw_surface_destroy(MwSurface *);

MwCanvas  *mw_canvas_begin(MwSurface *);                    /* push state + clip */
void       mw_canvas_end(MwCanvas *);                       /* pop state, mark damage */

void mw_set_operator(MwCanvas *, int gx_function);
void mw_set_source_rgb(MwCanvas *, uint32_t argb);          /* foreground pixel */
void mw_set_line(MwCanvas *, int width, int style, int cap, int join,
                 int dash_offset, const char *dashes, int ndash);
void mw_set_fill(MwCanvas *, int fill_style, int fill_rule, int arc_mode);
void mw_clip_rects(MwCanvas *, const XRectangle *r, int n, int x_org, int y_org);
void mw_clip_surface(MwCanvas *, MwSurface *mask, int x_org, int y_org);

void mw_paint(MwCanvas *);                                   /* fill current clip */
void mw_rect(MwCanvas *, double x, double y, double w, double h);
void mw_arc(MwCanvas *, double x, double y, double w, double h,
            double a1, double a2, int mode);                 /* XArc semantics */
void mw_polygon(MwCanvas *, const XPoint *pts, int n, int rel, int filled);
void mw_canvas_blit(MwCanvas *dst, MwSurface *src, int sx, int sy,
                    int dx, int dy, int w, int h);           /* XCopyArea */
void mw_canvas_put_image(MwCanvas *, MwSurface *img, int dx, int dy,
                         int sx, int sy, int w, int h);      /* XPutImage */

/* text */
void mw_set_font(MwCanvas *, MwFont *);
void mw_show_string(MwCanvas *, const void *s, int len, int x, int y,
                    int baseline);                           /* XDrawString */
```


### 3.5 Fonts and text

X11 core fonts are server-side objects; Motif requests them by XLFD
(`-*-helvetica-medium-r-normal--14-*`) and via font names in resource files. We emulate:

* `XLoadQueryFont` parses the XLFD (family, weight, slant, pixel size, spacing), asks
  **fontconfig** for the closest match, loads it with **FreeType**, and constructs a real
  `XFontStruct` (`min_bounds`, `max_bounds`, `per_char` table for the printable range,
  `ascent`, `descent`). A built-in bitmap "fixed" font is the guaranteed fallback so a missing
  font never fails to load.
* `XDrawString` draws through cairo with a `cairo_ft_font_face`, baseline-aligned; `XTextWidth`
  and `XTextExtents` are computed from cached per-glyph advances.
* `XCreateFontSet` returns a font set whose single physical font is the above; `XExtentsOfFontSet`
  reports the max extents. This satisfies Motif's font-list handling (`XmFontList`).
* 8-bit (`XDrawString`) and 16-bit (`XDrawString16`) variants share the path.
* **Xft is disabled** in the Motif build for v1 (see §9). NEdit never uses Xft. When Xft is
  wanted later, it can be satisfied by an `libXft` shim over our font/draw code, or by exposing
  a minimal Render extension.

### 3.6 Input

* `wl_seat` → `wl_keyboard`, `wl_pointer`.
* Keyboard: `xkbcommon`. The compositor's keymap is installed into `xkb_keymap`/`xkb_state`.
  * `X keycode = evdev scancode + 8` (X11 convention: keycodes 8–255).
  * `KeySym = xkb_state_key_get_one_sym(state, keycode - 8)`.
  * `XLookupString` = UTF-8/keysym from `xkb_state_key_get_utf8`, plus modifiers from
    `xkb_state_serialize_mods`; Latin-1 translation for legacy clients.
  * `XGetKeyboardMapping`, `XGetModifierMapping`, `XKeycodeToKeysym`, `XKeysymToKeycode`,
    `XStringToKeysym`/`XKeysymToString` are backed by xkbcommon plus the standard
    `X11/keysymdef.h` table (compiled in via the usual `XK_` macro trick).
* Pointer: `wl_pointer.enter/motion/button/axis` are mapped to `EnterNotify`, `MotionNotify`,
  `ButtonPress/Release`, `LeaveNotify`. The surface→toplevel mapping plus the in-process window
  tree resolve the deepest window under the cursor; events **propagate up** the tree until a
  window's `event_mask` selects them (core X propagation rules), with `do_not_propagate`.
* Focus: `wl_keyboard.enter/leave` + `XSetInputFocus` maintain the focus window; `KeyPress/Release`
  and `FocusIn/Out` are delivered accordingly. `Xm` menus rely on `XGrabPointer`/`XGrabKeyboard`
  — we implement a simple grab stack honoured by the router.
* Cursors: `XCreateFontCursor` maps the X cursor font glyph numbers to the
  `cursor-shape-v1` protocol when present, else to `xcursor`-style names via
  `libwayland-cursor`; `XDefineCursor` sets the surface cursor.

### 3.7 Atoms, properties, and the synthetic server

There is no server, so we are the server:

* **Atoms** live in a global intern table seeded with the predefined atoms at their canonical
  values (`XA_PRIMARY=1`, `XA_SECONDARY=2`, … from `X11/Xatom.h`), so application constants
  compare correctly. `XInternAtom(name, only_if_exists)` is deterministic.
* **Properties** are a per-window (atom → type/format/bytes) map. `XChangeProperty`,
  `XGetWindowProperty`, `XDeleteProperty`, `XListProperties`, `XGetTextProperty`/
  `XSetTextProperty` operate on it. `WM_*`, `_MOTIF_*`, `_NET_*`, `RESOURCE_MANAGER`,
  `WM_PROTOCOLS` and `WM_DELETE_WINDOW` are just properties; window-manager behaviour (map,
  unmap, iconify, delete) is implemented by us noticing the property and acting on the surface.
* **Selections** are an in-process registry (owner window + timestamp), integrated with
  `wl_data_device` so the clipboard can be shared with other Wayland clients when we own the
  selection. `XConvertSelection` resolves in-process immediately for same-client transfers and
  through the data-device otherwise. Cut buffers (`XFetchBuffer`/`XStoreBuffer`/`XRotateBuffers`)
  are an in-memory ring.
* **`XResourceManagerString`** returns the `RESOURCE_MANAGER` property of the root window, which
  we populate from, in order: `$XENVIRONMENT`, `~/.Xresources`, `~/.Xdefaults`, then
  `/etc/X11/Xresources` and app-defaults directories resolved by Xrm. This is essential: Motif
  widgets read almost all their configuration through Xrm.

### 3.8 Inter-client communication, WM hints, and what we ignore

The X11 "window manager" role is played by us:

| X11 concept | Wayland implementation |
|---|---|
| `XMapWindow` (top-level) | create/attach `xdg_toplevel`, `wl_surface_commit`, wait for `configure` |
| Window geometry/title | `xdg_toplevel.set_title`, `set_app_id`; ICCCM `WM_NAME`/`_NET_WM_NAME` read from properties |
| `XIconifyWindow` | `xdg_toplevel.set_minimized` |
| `XWithdrawWindow` | `wl_surface.attach(NULL)` / destroy toplevel |
| `WM_DELETE_WINDOW` client message | receive `xdg_toplevel.close`, synthesise `ClientMessage` |
| `XConfigureWindow` on a top-level | `xdg_toplevel.set_max_size`/`resize` + local resize; `ConfigureNotify` synthesised |
| Override-redirect popups | `xdg_popup` with `xdg_positioner` |
| `XRaiseWindow` | raise within our compositing order; `xdg_toplevel` has no raise |
| `XGrabServer`/`UngrabServer` | no-op (single client: already serialised) |
| `XWarpPointer` | no-op unless `pointer-warp` protocol is present |
| Bell | `xdg_system_bell` if present |

IXP (inter-client exchange), `_NET_WM_STATE` negotiation, and session management are out of
scope; we accept and ignore them so applications proceed.

### 3.9 Extensions

* **XRandR** (`XRR*`): report exactly one output matching the compositor's `wl_output`,
  with the mode from `wl_output.mode`; `XRRGetScreenResources` etc. return a stable fake
  description. `XRRSelectInput`/`XRRUpdateConfiguration` accept and ignore events.
  XV requests the XRandR extension and degrades gracefully if absent, but NEdit/Motif may
  query it.
* **XShape** (`XShape*`): track a per-window bounding/clip region; `XShapeCombineRectangles`
  updates it and feeds the compositor clip. `XShapeQueryExtension` reports it as present.
* **XKB**: `XkbLookupKeySym` provided from xkbcommon.
* **Render / Xft / XInput2 / XKB-ext / Xinerama / DRI**: not implemented; `XQueryExtension`
  reports absent so clients take their fallback paths. (Note: some clients call an extension
  function unconditionally after checking; we provide weak no-op stubs for the handful Motif
  and NEdit touch.)

### 3.10 XIM (input methods)

Motif text widgets use XIM/XIC. Full XIM (a client–server protocol with preedit/status
callbacks) is out of scope. We provide a **local input method** that is protocol-correct from
the client's point of view:

* `XOpenIM(dpy, ...)` returns a non-NULL opaque IM that supports the standard styles
  (`XIMPreeditNothing | XIMStatusNothing`).
* `XCreateIC(im, XNInputStyle, ..., XNClientWindow, w, XNFocusWindow, w, ...)` returns an IC
  bound to the window; `XmbLookupString` delegates to the same xkbcommon path as
  `XLookupString`; `XFilterEvent` returns `False` (no preedit interception).
* `XSetICFocus`/`XUnsetICFocus` maintain IC focus state.

This is enough for NEdit's text editing to type ASCII and UTF-8 composed characters. True
preedit/IME integration is a later milestone.

### 3.11 The Xrm resource manager

54 `Xrm*` entry points (part of libX11's API, not an extension) are required by Motif. This is a
self-contained, well-specified subsystem (quark table, resource database tree, matching with
wildcards/bindings). It is reimplemented in `src/xlib/resource.c`, including:

* `XrmGetStringDatabase` / `XrmGetFileDatabase` / `XrmCombineDatabase` / `XrmDestroyDatabase`
* `XrmGetResource` (with tight/loose binding, class/instance matching, wildcards)
* `XrmEnumerateDatabase`, `XrmParseCommand` (command-line `-xrm` and `-option` handling)
* `XrmGetDatabase`/`XrmSetDatabase` (per-display db), `XrmInitialize`, quark management.

---

## 4. ABI / API compatibility strategy

1. **Headers are upstream.** We do not invent `X11/*.h`; we compile against the standard Xorg
   Xlib headers, and build Xt/Motif from their upstream sources with their upstream headers.
   This guarantees the public struct layouts (`XEvent`, `XImage`, `XFontStruct`, `Screen`,
   `Visual`, `XSetWindowAttributes`, …) that clients access field-by-field.
2. **Only opaque handles are ours.** `Display`, `GC`, `Region`, `XIM`, `XIC`, `XOC`, `XOM`
   are declared opaque by the headers (`typedef struct _XDisplay Display;`), so we are free to
   define their internals. Everything the headers define concretely is filled in exactly.
3. **SONAME parity.** `libX11.so.6` with SONAME `libX11.so.6`; `libXt.so.6`; `libXm.so.4`.
   Link lines (`-lXm -lXt -lX11`) and `pkg-config` (`xm.pc`, `xt.pc`, `x11.pc`) point into our
   prefix, so **no source change** is needed in XV or NEdit.
4. **Predefined constants must match the wire values** where clients compare them
   (`XA_*`, `XK_*`, `KeyPress` event codes, `GXcopy`, `ZPixmap`, `ExposureMask`, …). These are
   fixed by the headers and by us seeding the atom/keymap tables accordingly.
5. **Error semantics.** `XSetErrorHandler` handlers are called asynchronously (deferred) as in
   Xlib; `XGetErrorText` has the standard messages. `XSetIOErrorHandler` fires on a fatal
   Wayland disconnect.

---

## 5. Build and integration

### 5.1 Layout

```
motif-wayland/
├── DESIGN.md                     # this document
├── README.md                     # build/run, status matrix
├── meson.build / meson_options.txt
├── include/                      # headers we add (Xt has none of its own needed)
│   └── X11/wayland_shim.h        # optional discovery header
├── src/
│   ├── xlib/                     # the Xlib ABI implementation (this project)
│   │   ├── internal.h            # Display/Screen/Window/GC/Drawable model
│   │   ├── display.c  drawer.c  window.c  image.c  pixmap.c
│   │   ├── color.c    font.c    text.c    gcontext.c
│   │   ├── event.c    input.c   keymap.c  grab.c
│   │   ├── atom.c     property.c selection.c cutbuffer.c
│   │   ├── region.c   cursor.c  resource.c (Xrm)
│   │   ├── xim.c      xrandr.c  xshape.c  xkb.c
│   │   └── init.c     compat.c
│   └── wayland/
│       ├── wl.c                  # connection, registry, globals
│       ├── surface.c             # toplevel/popup lifecycle, shm buffers
│       ├── composite.c           # window-tree → toplevel repaint
│       └── seat.c                # keyboard/pointer/data-device
├── tools/
│   └── headless-compositor.c     # CI/test compositor (wlroots-free, libwayland-server)
├── tests/
│   ├── test_core.c               # unit: atoms, XImage, regions, Xrm, colour
│   ├── test_draw.c               # Xlib app: windows, GC, text, images → PNG
│   └── run-headless.sh
└── scripts/
    ├── build-stack.sh            # build libXt + Motif against our prefix
    ├── build-xv.sh               # build XV unmodified
    └── build-nedit.sh            # build NEdit unmodified
```

### 5.2 Build system

* `meson` for the shim (Wl, Cairo, FreeType, fontconfig, xkbcommon, pixman, wayland-protocols
  dependencies; `wayland-scanner` for `xdg-shell`, `viewporter`, `cursorshape`).
* Upstream **libXt** and **Motif** build with their own Autotools, configured against our
  prefix (`PKG_CONFIG_PATH`, `--x-includes`, `--x-libraries`, `CPPFLAGS`, `LDFLAGS`). A wrapper
  `scripts/build-stack.sh` performs the whole chain reproducibly.
* **XV** uses CMake. `find_package(X11)` honours `CMAKE_PREFIX_PATH`, and our prefix ships
  `libX11.so`, `libXt.so`, the headers and `.pc` files, so `cmake -DCMAKE_PREFIX_PATH=$PREFIX`
  builds it unchanged.
* **NEdit** uses a plain Makefile (`makefiles/Makefile.linux`); we inject
  `-I$PREFIX/include -L$PREFIX/lib` and `LD_LIBRARY_PATH` at link/run time.

### 5.3 Testing

1. **Unit tests** (`tests/test_core.c`) — no compositor needed: atoms/property round-trips,
   `XImage` create/get/put/pixel, region algebra vs. known results, Xrm matching, colour
   parsing, keysym conversion.
2. **Headless compositor** (`tools/headless-compositor.c`) — a minimal `libwayland-server`
   compositor that implements `wl_compositor`, `wl_shm`, `xdg_wm_base`, `wl_seat`, `wl_output`.
   It sizes toplevels, exposes their `wl_shm` buffers, and can (a) dump a frame to PNG and
   (b) replay a scripted input sequence (pointer motion/button, key presses). This makes the
   full render+input path testable in CI with no display server.
3. **Golden-image tests** (`tests/test_draw.c` + `run-headless.sh`) — draw known primitives and
   text, capture the compositor's output, compare against a reference PNG.
4. **End-to-end** — build and launch XV and NEdit under the headless compositor; script a few
   interactions; assert exit status and frame hashes.

### 5.4 Milestones

| M | Scope | Exit criterion |
|---|---|---|
| **M0** | Skeleton, model, meson, unit tests | `meson test` green |
| **M1** | Wayland backend + window tree + drawing + XImage + events + Xt thin slice | **XV runs** under headless compositor |
| **M2** | Fonts/text, colours, cursors, selections, Xrm, XRandR/XShape, XIM-local | XV stable; text renders; clipboard works |
| **M3** | libXt built against shim, full Xt event/realize path | an Xt widget program runs unmodified |
| **M4** | Motif built against shim (`--disable-xft`), full Xm | **NEdit runs** and edits a file |
| **M5** | Xft via Render shim or libXft-over-cairo; server-side decorations; IME | NEdit with antialiased fonts |
| **M6** | Hardening: fuzz the window/event paths, leak/UB checks, perf | soak + ASan clean |

---

## 6. Risks and mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| Xlib surface larger than expected (clients call paths not in the 307) | link/run failures | ship the full Xorg Xlib header set; link-time `-Wl,--no-undefined` audit; run with `LD_BIND_NOW=1` + `RTLD_NOW` and stub loudly |
| Semantics of child windows/Expose differ from X | blank or stale UI | always-on backing store; recomposite on damage; golden-image tests |
| libXt blocks on the wrong fd / uses `_X` internals | event loop hangs | `ConnectionNumber` = wl fd is the contract; build libXt from source and test its `NextEvent` path early (M3) |
| Motif's private Xt headers assume structures we don't control | build break | we use upstream Xt headers/structs verbatim; only Xlib is ours |
| XIM insufficiency breaks text editing | keyboard dead in NEdit | local IM implements the client-visible contract; fall back further to `XLookupString` if `XCreateIC` fails |
| Font mismatch (XLFD) | ugly or wrong metrics | fontconfig match + built-in fixed fallback; never fail `XLoadQueryFont` |
| Motif requires Xft by default | build break | `configure --disable-xft`; NEdit needs no Xft |
| Popup/menu placement (override-redirect) | menus in wrong place | `xdg_positioner` with parent toplevel anchor; fallback to toplevel |
| Wayland lacks raise/position/global focus | WM-dependent behaviour | implement locally; degrade gracefully; document |
| Scope is enormous | never "done" | milestone-gated delivery; XV first (no Xm), NEdit second |

---

## 7. Known gaps / deliberate simplifications (v1)

* No true server-side backing store *policy*: we always keep backings.
* No XRender/Xft, no XInput2 (raw events, multiple pointers), no XKB extension beyond
  `XkbLookupKeySym`.
* No XIM preedit/status UI (local IM only).
* Single screen, single seat, no Xinerama/DRM-lease.
* No `_NET_WM_STATE`/EWMH negotiation, no inter-client exchange.
* Server-side titlebar decorations not requested; apps get undecorated toplevels unless the
  compositor decorates.
* `XGrabServer` is a no-op; `XWarpPointer` is best-effort.

---

## 8. Appendix — surface & protocol reference

* **Xlib functions to implement:** see §2.2 (307 names) plus the 54 `Xrm*` entry points.
* **Wayland protocols used:**
  `wl_display`, `wl_registry`, `wl_callback`, `wl_compositor`, `wl_surface`, `wl_region`,
  `wl_shm`, `wl_shm_pool`, `wl_buffer`, `wl_seat`, `wl_keyboard`, `wl_pointer`, `wl_output`,
  `wl_data_device_manager`/`wl_data_device`/`wl_data_source`/`wl_data_offer`,
  `xdg_wm_base`/`xdg_surface`/`xdg_toplevel`/`xdg_popup`/`xdg_positioner`,
  `zxdg_decoration_manager_v1`, `wp_viewporter`, `wp_cursor_shape_manager_v1`,
  `xdg_system_bell_v1`.
* **Reference implementations consulted:** Xorg `libX11`/`libXt`/`libXrandr`/`libXext`,
  Open Motif 2.3.x, XV 6.2, NEdit 5.7.

---

## 9. Implementation notes (as built)

Things that only became clear once the code ran, recorded here so they are not
re-learned:

1. **A drop-in `libX11.so.6` must also export Xlib's *internal* ABI.** Any
   library built against the real Xlib — `libXext`, `libXrender`,
   `libXft`, and `libcairo`'s xlib backend — binds to *our* `libX11.so.6` at
   runtime and needs `XlibInt` symbols: `_XSend`, `_XReply`, `_XRead`,
   `_XFlush`, `_XGetRequest`, `_XAllocScratch`, `XESetWireToEvent`,
   `_Xglobal_lock`, `XLockDisplay`, …  `src/xlib/xlibint.c` provides them.
   They are functional where easy and benign no-ops otherwise; the extensions
   they would carry (Render, SHM, XKB, proto-XIM, Xcms) are not used by this
   project, and where we *do* implement an extension (SHAPE, RandR) our symbols
   shadow the ecosystem ones in the global scope.
2. **libX11 exports function forms of names that Xlib.h defines as macros.**
   XV calls `XDisplayWidth`/`XDisplayHeight` (functions), not the
   `DisplayWidth()` macro.  `src/xlib/aliases.c` supplies the whole
   `XDisplay*`/`X*OfScreen`/`XDefault*` family.
3. **`ConnectionNumber` = the Wayland fd is sufficient for libXt.**  A stock
   libXt built against the shim realises widgets, receives `Expose`, draws, and
   fires `XtAppAddTimeOut` timers correctly; `XtAppMainLoop` exits normally.
   This is the single most important integration point and it holds.
4. **A dead compositor must be treated as a fatal IO error.**  Otherwise Xt's
   `poll`/`XEventsQueued` loop spins on the EOF fd.  The default handler now
   prints and exits, like Xlib's.
5. **Single-buffered surfaces, no `wl_buffer.release` dependency.**  Minimal
   compositors never send `release`; deferring on it deadlocks after a resize.
   We render into one buffer and re-attach.  Tearing is possible and acceptable
   for the emulated (2D, non-animated) workload; a double-buffered pool is a
   later refinement.
6. **X's integer pixel grid.**  cairo strokes on pixel centres, so odd-width
   stroked lines are half a pixel off.  `mw_set_snap()` applies the standard
   `+0.5` nudge for stroke primitives; filled primitives are unaffected.
   Verified: a horizontal line requested at `y=260` lands exactly on row 260.
7. **`XParseGeometry` stores magnitudes** and signals sign via
   `XNegative`/`YNegative`, which is what `XWMGeometry` consumes.
8. **Colour names must not assume `rgb.txt`.**  It is absent on some distros;
   a built-in table of the common X11 colour names backs it up.
9. **fontconfig/FreeType are always available**; `XLoadQueryFont` must never
   fail, so it falls back to a default family.

### 9.1 Verified XV run (unmodified)

XV 6.2 was configured with `CMAKE_PREFIX_PATH=$PREFIX` (so `find_package(X11)`
resolves to the shim), built without source changes, and run under the bundled
headless compositor.  It opened a toplevel, decoded the supplied image, and the
captured frame was a 2×-scaled render of that image (linear red/green ramps,
correct wrap points) — i.e. XV's whole Xlib image path (`XImage`, `XPutImage`,
colours, GCs, exposure) works through the shim.

### 9.2 NEdit and Open Motif (M4) — completed

Open Motif 2.3.9 was configured (`--disable-xft`, `--x-libraries=$PREFIX/lib`)
and its `libXm`/`libMrm` built and installed against the shim; NEdit 5.7 then
built and ran, rendering its menu bar, toolbar and text.  Issues that had to be
solved, in order:

1. **Motif's host tools need `-std=gnu17`.**  `makestrs.c` uses old-style
   `void (*)()` prototypes that C23 rejects.  A build-flag accommodation.
2. **Xlib's i18n text + XOM API** (`Xmb*`/`Xwc*`/`Xutf8*` drawing, escapement
   and extents, `XGetOMValues`, `XGetOCValues`, …) — `src/xlib/xmb.c`.
3. **`XVaCreateNestedList`** — an X11R5-era Xt helper that modern libXt no
   longer exports but Open Motif's `XmIm.c` still calls; provided as a
   documented compatibility shim (`src/xlib/xtcompat.c`).
4. **Vendor keysyms.**  Motif's translation tables reference `osfActivate`,
   `osfHelp`, … These come from `XKeysymDB`, which is absent on modern
   systems, so the standard table is built in (`keymap.c`).
5. **The X11 colour database.**  `rgb.txt` is likewise not installed here, so
   the standard database is embedded (`data/rgb.txt` → `src/xlib/rgb_table.h`)
   and consulted by `XParseColor`/`XAllocNamedColor`.
6. **`XAddExtension` must return a real record.**  Motif's `ColorObj.c` calls
   `XAddExtension()` and reads `xExt->extension`; returning `NULL` crashed it.
7. **`XGetIMValues`/`XGetICValues` must fill their return slots.**  Motif's
   `XmIm.c` queries `XNQueryInputStyle` and dereferences the result; a stub
   that returns `NULL` without writing the out-parameter crashed.  The local
   IM now advertises `XIMPreeditNothing|XIMStatusNothing` and answers the IC
   queries.
8. **`XOpenDisplay` must ignore the X display name.**  Xt passes `DISPLAY`
   (`:0`), which must not be forwarded to `wl_display_connect`.
9. **Two performance bugs, both real:**
   * the object registry's cluster-shifting `XID` delete could loop forever
     and realloc the table mid-scan — replaced with a clean rebuild;
   * core-font metrics ran FreeType's hinting bytecode for all 256 glyphs of
     every font loaded, which took ~20 s in NEdit's menu construction.  Metrics
     now use `FT_LOAD_NO_HINTING` with a per-font width cache, and drawing is
     still hinted.
10. **Damage batching.**  Rendering every drawing call forces an `O(n)`
    recomposite per widget operation; damage is now accumulated and flushed
    once at the event-loop wait (or an explicit `XFlush`/`XSync`).

### 9.3 Interaction and polish (found while running XV interactively)

1. **`XClearWindow` must not generate `Expose`.**  It is defined as
   `XClearArea(..., exposures=False)`.  XV does clear-then-redraw, so
   synthesising an Expose produced an infinite redraw loop (100% CPU).  Fixed,
   and `XClearArea` now clears only the requested rectangle.
2. **Never attach a buffer before the first `configure`.**  `xdg-shell`
   requires a bufferless first commit and an `ack_configure` before any
   buffer; rendering during `map` raised *"attached a buffer before configure
   event"* on a strict compositor.  Rendering is now gated on `configured`.
3. **X resources.**  Applications read defaults from the root window's
   `RESOURCE_MANAGER` property (XV reads it directly) and from
   `XResourceManagerString` (Xt/Motif).  We now synthesise the database from
   `/etc/X11/Xresources`, `/usr/share/X11/Xresources`, `~/.Xresources`, then
   `$XENVIRONMENT` or `~/.Xdefaults`, publish it, and keep `d->xdefaults` in
   sync — so XV gets `ctrlMap`/`infoMap` and Motif apps get their defaults
   without command-line flags.
4. **`XGetWindowProperty` `long_offset`/`long_length` are in 32-bit units.**
   Treating `long_length` as items truncated multi-word properties (XV's
   `RESOURCE_MANAGER` read); fixed.
5. **Client-side decorations.**  When the compositor offers no
   `zxdg_decoration_manager_v1`, we draw a titlebar (title text, close button,
   drag-to-move via `xdg_toplevel.move`) and inset the X content below it;
   `xdg_surface.set_window_geometry` marks the content area.
   A subtlety cost a real bug: if the compositor *does* advertise the
   decoration protocol, requesting `SERVER_SIDE` and then **destroying the
   decoration object before its `configure` arrives** leaves the window with
   *no* chrome at all (the compositor cancels decorations).  We now request
   `CLIENT_SIDE` explicitly, keep the object, and always draw our titlebar; if
   the compositor insists on `SERVER_SIDE` the decoration `configure` callback
   removes ours and lets it decorate.
6. **`wl_seat` capabilities must be caught at bind time.**  The compositor
   sends `wl_seat.capabilities` in response to the bind, during the next
   round-trip.  We originally installed the seat listener *after* the
   round-trips, so the event was missed and we never bound `wl_pointer` /
   `wl_keyboard` — the application received **no input at all**.  The listener
   is now added inside the registry callback, immediately after binding.

### 9.4 Menus: override-redirect popups (M4 follow-up)

NEdit's pulldown menus rendered but **no item could be selected**.  Two
independent bugs, both in the shim, found with `tests/test_popup.c` (a
Motif-free reproduction of `XmMenuShell`: override-redirect popup + item
children + `XGrabPointer(owner_events=True)`) driven by `tests/popup.input`.

1. **An override-redirect window is a top-level regardless of its parent.**
   `mw_map_window` only promoted a window to a Wayland toplevel when its
   parent was the **X root**.  But Motif parents a menu popup to the
   *application's toplevel shell*, not to the root, so the popup never became
   a `MwToplevel`: no `wl_surface`, no `xdg_popup`, no `xdg_popup.grab`.  It
   was instead composited straight into the parent window's backing store.
   That is why the symptom was so misleading — **the menu looked perfectly
   open** (it really was being drawn), but the compositor still considered the
   main toplevel to be the only surface, so it kept routing `wl_pointer` to
   that surface and `mw_deepest_at()` hit-tested the pointer in the wrong
   coordinate space.  Presses landed on whatever happened to be under the
   coordinates on the main window, never on the menu items.
   `mw_map_window` now also promotes `override_redirect` windows, which is
   what X semantics require: override-redirect bypasses the window manager
   wherever it is parented.
2. **`Enter`/`Leave` must be reported while a grab is active.**  `ptr_motion`
   suppressed crossing events whenever `ptr_grab_active || implicit_grab`.
   Real X reports them regardless — a grab only decides which window gets the
   button and motion events.  Motif arms an `XmMenuButton` on
   `EnterNotify` and disarms on `LeaveNotify`, so with the menu's
   `XGrabPointer` active **no item would ever highlight**, even once presses
   routed correctly.  (`ptr_focus` was already updated unconditionally two
   lines below, so deriving crossing events the same way also made the two
   self-consistent.)

Both are covered by `scripts/run-tests.sh`, which asserts the menu posts,
item 1 arms, the press reaches the item window with item-relative
coordinates, and the selection fires.

Still not covered by the harness: the test compositor's `popup_grab` is a
no-op (no implicit grab, no dismiss-on-outside-click) and its PNG capture
writes the toplevel buffer only, so an open menu is not visible in a golden
image.  `tests/test_xm_menu.c` — the same test driven through real Motif —
exists but is not built here, as libXm is not installed.

### 9.5 Remaining work (M5+)

Xft/Render (so Motif can stop using `--disable-xft`), server-side decorations,
a real preedit/IME, and the long tail of Xlib entry points not exercised by
XV/NEdit.  `MW_TRACE=1` enables an operation trace (`src/xlib/window.c`,
`src/wayland/surface.c`, `wl.c`) for debugging applications on the shim.


