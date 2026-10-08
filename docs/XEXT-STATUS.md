# libXext (SHAPE, MIT-SHM, Sync, extutil)

The shim installs its own **`libXext.so.6`** (with `xext.pc`), shadowing the
host library the same way it already does for `libXrandr`, `libXft`,
`libXfixes` and `libXcursor`.  It carries two quite different things:

1. **extutil** — the machinery every other client extension library
   (`libXi`, `libXtst`, `libXv`, `libXRes`, …) uses to find its per-display
   info.  It must be faithful: if `XextAddDisplay` returned `NULL`, those
   libraries would treat their extension as missing and refuse to run.
2. **Extensions** — SHAPE, MIT-SHM, Sync, and (stubbed) the rest.

## extutil

`XextCreateExtension`, `XextDestroyExtension`, `XextAddDisplay`,
`XextRemoveDisplay`, `XextFindDisplay` implement the standard list of
`XExtDisplayInfo` blocks (from `<X11/extensions/extutil.h>`), and
`XMissingExtension` is a no-op stub.  `XextAddDisplay` calls the shim's
`XInitExtension` (libX11) to get the extension's codes, so a library whose
extension the shim does not advertise gets `NULL` and degrades exactly as it
would against the host library.

This is what keeps `xinput` (libXi) and `xdpyinfo` (libXtst) working with our
libXext loaded — both are in `scripts/run-tests.sh`.

## MIT-SHM

GDK only uses MIT-SHM for `GdkImage`/drawing, and holds no server reference, so
the implementation is client-side over the shim:

* `XShmCreateImage` builds an `XImage` whose `data` is the caller's segment
  (the buffer `XCreateImage` would have allocated is released), and installs a
  destroy hook that frees only the struct — the caller `shmdt`s the segment.
* `XShmAttach`/`XShmDetach` succeed; `XShmPutImage` blits through `XPutImage`;
  `XShmGetImage` copies through `XGetImage`.
* `XShmQueryVersion` reports **no** shm pixmaps, so GDK always takes the
  `XShmPutImage` path (which we service) rather than a server-side pixmap that
  could not track the segment.

## Sync

`XSyncQueryExtension`/`XSyncInitialize` report present (3.1); the full
`XSyncValue` arithmetic is implemented, and `XSyncCreateCounter` hands out ids
(the shim's `mw_xsync_new_counter`).  There is no server clock, so
`XSyncSetCounter`/`XSyncAwait` and the alarm/fence/priority calls are no-ops.
GDK only uses the counter to publish `_NET_WM_SYNC_REQUEST_COUNTER`; nothing
blocks on it.

## SHAPE

SHAPE is **not** defined in this facade.  With the host libXext out of the
search path, GDK's `XShape*` references resolve to the shim's own **libX11**
(`src/xlib/xshape.c`), which already tracks the shape region for
bounding/clip.  GDK uses `XShapeCombineMask` and `XShapeCombineRectangles`
(both client rectangles/masks, not server regions), which that implementation
handles, and `XShapeQueryExtension` reports SHAPE so
`gdk_display_supports_shapes()` is true.

## Stubbed extensions

DPMS, Xdbe, Xmbuf, MIT-SUNDRY and XSecurity are provided as stubs whose
`QueryExtension` reports the extension missing, so clients that probe them
(`xdpyinfo` lists them all) skip them cleanly instead of failing to link.
Every libXext symbol referenced anywhere in the environment (clients under
`/usr/bin`, the GTK2/MATE prefixes, `/usr/lib`) is defined; the only ones not
in this facade are the `XShape*` family, which libX11 provides.

## Test

`tests/xext_x.c` (run by `scripts/run-tests.sh`) does a full SHM round trip
(create → attach → put → detach), checks Sync reports present, and checks SHAPE
resolves through libX11.
