# Compositing and RGBA (transparent) windows

GTK2's composited-window support is an **X** concept: GDK asks whether an X
compositor is running, and for RGBA windows it redirects the window and watches
damage.  Under Wayland there is no separate X compositor — the Wayland
compositor blends our `wl_surface`s — so the shim stands in for the X one.  The
pieces:

## Is the screen composited?

`gdk_screen_is_composited()` is true iff some client owns the `_NET_WM_CM_Sn`
selection.  Nothing does under Wayland, so the shim **owns `_NET_WM_CM_S0`**
(set in `display.c` after `mw_init_selection`, which resets the selection
table).  This is what makes GTK take its composited code path at all.

## An RGBA visual

GDK's `gdk_screen_get_rgba_visual()` looks for a **depth-32 TrueColor** visual
with RGB masks `0xff0000`/`0xff00`/`0xff`.  The shim now exposes a second
visual (`visualid 0x22`) alongside the 24-bit root visual:

* `display.c` sets up `visual32`, a second `Depth` (32) and a second
  `ScreenFormat` (32bpp);
* `XGetVisualInfo`/`XMatchVisualInfo`/`XListDepths`/`XListPixmapFormats`
  report both;
* the Render `QueryPictFormats` reply lists both depths/visuals, mapping
  `0x22` to the `ARGB32` picture format (alpha mask `0xff`);
* `_XVIDtoVisual` returns the depth-32 `Visual*` for its id.

That last one matters: libXrender resolves each visual id in the reply through
`_XVIDtoVisual` and then `XRenderFindVisualFormat` matches the returned
`Visual*` **pointer**.  Until it knew about `0x22`, its entry was `NULL`, cairo
got a `NULL` format and crashed in `XRenderCreatePicture` the moment GTK took
the composited path for an RGBA window.

## Alpha reaches the compositor

The Wayland buffer was already `WL_SHM_FORMAT_ARGB8888`; the repaint now keeps
alpha for a depth-32 window instead of forcing it opaque:

* with no background the toplevel starts fully transparent (a composited RGBA
  window paints its own alpha);
* with a background it uses that background's own alpha.

`XPutImage`/Render already preserve alpha for depth ≥ 32, so a client (GTK via
cairo) drawing with `CAIRO_OPERATOR_SOURCE` + `OVER` yields a premultiplied
ARGB buffer the Wayland compositor blends.  Verified end to end: a GTK2
`GtkWindow` with the RGBA colormap and an app-paintable drawing area that
clears to transparent and paints `rgba(1,0,0,0.5)` produces pixels with
`alpha == 0x80` (`0x80800000`).

## XComposite and XDamage

`libXcomposite.so.1` and `libXdamage.so.1` are installed as facades over the
host sonames (like libXfixes/libXcursor/libXext):

* `XCompositeQueryExtension`/`QueryVersion` report 0.4 (so GDK's
  `have_xcomposite` test passes); redirect/unredirect are **no-ops** — there is
  nothing to redirect into, the Wayland compositor composites directly.  The
  overlay window is the root window.
* `XDamageCreate`/`Destroy` live in the shim (`src/xlib/xdamage.c`).
  `XDamageSubtract` is a no-op (the damage is delivered as events), and the
  shim emits `XDamageNotify` (event base 131) from `mw_window_damage` for any
  damage object on the drawable, so GDK's composited-window repaint path stays
  live.

## Test

`tests/composite_x.c` (run by `scripts/run-tests.sh`) checks the depth-32
visual and its Render format, the `_NET_WM_CM_S0` owner, alpha round-tripping
through a depth-32 window, XComposite, and a delivered `XDamageNotify`.
The end-to-end GTK2 check (RGBA window, `alpha == 0x80`) is recorded above.

## Not yet

* Damage is reported per drawable at window granularity (no sub-rectangle
  accumulation); `XDamageReportNonEmpty`/`RawRectangles` levels are all treated
  the same.
* `XCompositeNameWindowPixmap` returns `None` (GDK does not use it).
