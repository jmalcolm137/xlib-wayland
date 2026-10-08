# XFIXES and Xcursor

GDK's X11 backend links two extension libraries that are not part of libX11:

* **libXfixes** — `XFixesSelectSelectionInput` and `XFixesSelectionNotify` are
  how GDK learns that the CLIPBOARD/PRIMARY owner changed (and turns it into a
  `GDK_OWNER_CHANGE` event); `XFixesCreateRegion`/`XFixesChangeCursor` are used
  by the (inactive) Composite/Damage path and for cursor theme changes.
* **libXcursor** — themed cursor loading (`XcursorLibraryLoadCursor`,
  `XcursorShapeLoadCursor`, `XcursorImageLoadCursor`) and the theme/size
  settings.

Against the host libraries these would issue X extension protocol (an opcode
that carries the request on the wire), which the shim does not speak.  The shim
therefore installs **its own facades over the host sonames**
(`libXfixes.so.3`, `libXcursor.so.1`, with matching `.pc` files), the same way
it already shadows `libXrandr` and `libXft`.  Each facade is a thin layer that
calls `mw_*` helpers exported from `libX11.so.6`.

## XFIXES

`src/xlib/xfixes.c` keeps the selection-input registrations and creates region
handles; `src/xfixes/xfixes.c` is the public API.

* `XFixesSelectSelectionInput(dpy, window, selection, mask)` records the
  interest.
* Whenever `XSetSelectionOwner` changes an owner, an `XFixesSelectionNotify`
  event (event base **128**, `subtype` = `XFixesSetSelectionOwnerNotify`) is
  queued to every registered window whose selection matches.

Because `XSetSelectionOwner` is *also* how the clipboard bridge publishes a
cross-process owner (`sync_x_owner` points the X selection at the shim's proxy
window when a Wayland client owns the clipboard), a copy in another shim process
reaches this process's GDK leader window and the paste UI updates.  This is the
fix that makes `gdk_display_request_selection_notification()` return `TRUE` for
GTK2 instead of falling back.

`XserverRegion` values from `XFixesCreateRegion` are opaque handles; the shim
allocates and frees them (the only consumer, the Composite/Damage path, is not
active).  `XFixesChangeCursor(image, target)` records an alias so cursors
already set on windows follow a theme change.

## Xcursor

`src/xlib/cursor.c` gains the loaders; `src/xcursor/xcursor.c` is the public
API.

* `XcursorLibraryLoadCursor` / `XcursorShapeLoadCursor` load a cursor from the
  Wayland cursor theme (`wl_cursor_theme`); the shape path maps GDK's
  `GdkCursorType` (an X cursor-font glyph) to a freedesktop cursor name.
* `XcursorImageLoadCursor` builds an ARGB Wayland cursor buffer from the
  supplied pixels.
* `XcursorGet/SetTheme` and `XcursorGet/SetDefaultSize` drive the shim's
  `wl_cursor_theme`; `XcursorSupportsARGB` is true.
* Reading a themed cursor's pixels back out (`XcursorLibraryLoadImages` /
  `XcursorShapeLoadImages`) is **not** implemented, so `gdk_cursor_get_image()`
  returns `NULL` (its callers handle that).

## Not yet

* XDamage / XComposite / XSync / XShm — separate libraries (mostly in
  `libXext`), still absent; GDK keeps its non-damage code paths.
* `XFixesGetCursorImage`, pointer barriers and the rest of the Xcursor animate/
  file API are not provided.

## Test

`tests/xfixes_x.c` (run by `scripts/run-tests.sh` under the headless
compositor) links the facades, registers a selection, changes its owner and
checks the `XFixesSelectionNotify` arrives with the right fields, then loads a
themed cursor and an image cursor.
