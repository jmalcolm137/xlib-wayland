# Code review (November 2026)

Three rounds: a first review of `src/` (16 Nov), the larger changes it
left open (16 Nov), and a second review of those changes plus the files
the first pass covered least (17 Nov).

A full review of `src/` and `tools/headless-compositor.c`, done by reading the
code and by running the test suite under AddressSanitizer/UBSan
(`meson setup -Db_sanitize=address,undefined`).

## Tests

`meson test` runs four programs, each against a private headless
compositor (`tests/with-compositor.sh`), never the desktop session's:

- `tests/test_core.c` uses the public API: regions, geometry, keysyms,
  colours, Xrm (search lists, `#include`, `XGetDefault`), XImage, properties,
  WM hints, fonts, text extents, shape, lookup strings, and pixel-checked
  core drawing (raster ops, plane masks, copies, clipping, stipples and tiles,
  Latin-1 text).
- `tests/test_internal.c` is a white-box test (`internal.h`): the event
  queue, window destruction, focus, propagation and crossing events, grab
  matching, keymap updates, XIM, cursor theme reloads, output-scale changes,
  the wakeup thread, `mw_write_all`, `_XRead32`, and XI2 selection.
- `tests/test_render.c` drives Render through the real libXrender encoders:
  batched and A1/ARGB32 glyph uploads, referenced glyph sets, solid and
  gradient masks, CPClipMask pixmaps, and trapezoid source origins.
- `tests/test_broker.c` carries a 200 KiB shared selection between two shim
  processes.

`scripts/run-tests.sh` still drives the integration tests (Xt, Motif, NEdit,
XV, clipboard, DnD, IME, menus).

## Fixed

Memory safety:
- `XSubImage` returned a view into its parent; `XDestroyImage` freed an
  interior pointer.  It now returns a copy, and works for 1-bit images.
- `XShapeGetRectangles` freed the window's live shape region.
- `XSetClassHint` overflowed a stack buffer.  `XGetClassHint` and
  `XGetCommand` read past a final string with no NUL.
- `XwcLookupString` overflowed a stack buffer on a long IME commit.
- `XmbTextPerCharExtents`/`XwcTextPerCharExtents` wrote the single overall
  rectangle per character.
- `XListFonts` returned a list without the terminating NULL that
  `XFreeFontNames` walks to.
- `_XRead32` took its byte count for a count of longs and wrote 8x too much.
  libXi, libXrandr and libXfixes call it.
- `XkbGetMap` looped forever (a `KeyCode` loop counter can't reach past 255).
- A keymap that failed to compile left `xkb_state` pointing at a freed keymap.
  With no keymap at all, `mw_update_modmap` crashed.
- `XCloseDisplay` double-freed the DnD/XDND source windows, left dangling
  table entries, and leaked properties, the output buffer, the seat and a
  never-used Xrm database.
- Window properties are stored NUL-terminated; the CSD title read past
  `WM_NAME`.
- A destroyed window left popups anchored to it (`popup_parent`,
  `parent_tl`) and the deferred-enter pointers dangling.
- GCs kept raw pointers to fonts that `XUnloadFont`/`XFreeFont` freed.
  Fonts are now reference-counted, and GCs share one default font instead
  of loading "fixed" for every GC (which also leaked).
- The test compositor freed surfaces under live viewports and popups, and
  aborted when a client destroyed its text-input or primary-selection
  manager.

Behaviour:
- Arcs ran clockwise.  A full circle drew nothing and a partial arc drew its
  complement.  Outline arcs also drew pie-slice radii, consecutive arcs
  were joined by a line, and JoinRound/JoinBevel were swapped.
- `Xutf8LookupString` returned Latin-1 bytes for é, ü, £ and the like, and
  the lookup functions now report `XBufferOverflow` instead of truncating.
- The right and middle mouse buttons were swapped, and the side buttons
  collided with wheel buttons 4/5.
- Key repeat kept running after keyboard focus left the window.
- The modifier map was not rebuilt when the compositor's keymap arrived.
- `XGetWindowProperty` returned everything for `long_length == 0`, and
  deleted on a partial read without reporting `PropertyDelete`.
  `PropModeAppend` with a different format read past the old data.
- `XSetCommand` stored the `argv` pointers instead of the strings.
- `XParseGeometry`/`XWMGeometry` are ported from libX11: signed offsets with
  `XValue`, sizes in resize increments, and gravity.
- Event queue: QLength drifted, `XPutBackEvent` duplicated events when the
  queue was full, a zero mask matched everything, Expose merging lost areas,
  and the parent's SubstructureNotify ConfigureNotify was merged away.
- Window tree: destroying the root destroyed everything; a destroyed
  selection owner kept the selection; `map_state` never reported
  `IsUnviewable`; `XTranslateCoordinates` never returned a child;
  `XSetInputFocus` sent no FocusIn/FocusOut.
- Grabs: passive grab modifiers were ignored (AnyModifier never matched),
  `XUngrab*` ignored modifiers, and re-grabbing added duplicates.  Motion
  ignored `ButtonMotionMask`/`ButtonNMotionMask`.
- XI2 read a 64-bit `Window` from a 32-bit field.
- The clipboard leaked the previous offer on every selection change.
- The Xft fallback colour was white, not black; `XftFontOpen*` leaked the
  match on failure.

### Second round: the larger changes

- **Event propagation** (`input.c`): button, key and motion events now
  propagate to the first ancestor that selected them (respecting
  `do_not_propagate_mask`), with `subwindow` set.  Active grabs honour
  owner_events, and the implicit grab is the window that received the press.
  Keys start at the window under the pointer when it is inside the focus
  window.  Enter/Leave no longer propagate: the full X crossing sequence is
  generated (Inferior/Ancestor/Virtual/Nonlinear/NonlinearVirtual).  Crossing
  events carry `focus`/`state`.  `XQueryPointer` reports root coordinates.
  XI2 is still mirrored when only XI2 was selected.
- **Core drawing**: non-copy GX functions and partial plane masks are exact
  bitwise operations on the covered pixels (fills, strokes and copies).
  `XCopyPlane` uses the requested plane, with foreground and background.
  Stipples read bit 0, opaque stipples paint the background, tiles paint
  their pixels, and the tile/stipple origin had the wrong sign.  `XCopyArea`
  and `XPutImage` clip to the source (`XPutImage` read past the image).
  Empty clip lists disable output, `XSetRegion` uses the region's
  rectangles, and clip rectangles and masks replace each other.
  `XDrawString` is Latin-1, and `XwcDrawString` uses the fontset's font.
- **Render**: AddGlyphs parses libXrender's layout (all ids, then infos,
  then images).  A1 rows are padded to 32 bits, and ARGB32 glyphs are
  supported.  ReferenceGlyphSet shares the glyphs.  Solid and gradient
  pictures work as masks.  CPClipMask accepts a pixmap (depth 1: bit 0) at
  the clip origin, and clip rectangles replace it.  Trapezoids and triangles
  take the source relative to the first vertex and only touch their bounding
  box.
- **Clipboard and broker I/O**: writes to peers go through `mw_write_all`
  (no SIGPIPE, non-blocking with a deadline, short writes retried).  Format
  16/32 data is served with the right length.  The broker's receive buffer
  grows (transfers over ~8 KiB stalled and were refused), and the item count
  is checked against the bytes received.  Drags accept non-text types for
  XDND targets and call `finish` only when valid.  The wakeup thread watches
  the clipboard fetch fd and transfer deadlines, and neither helper thread
  spins on a ready fd any more.  The test compositor now enforces accept,
  action and finish, as real compositors do.
- **Surfaces**: buffers are reallocated when the output scale changes, and
  `xdg_popup.grab` is sent on any xdg_wm_base version.
- **Xrm/text**: `XGetDefault` is a real Xrm lookup, with results owned by
  the database.  `XrmQGetSearchList` keeps the query in the caller's array
  instead of allocating per call.  `#include` works in resource files.
  `XStringToKeysym`/`XKeysymToString` handle `U+XXXX` and hex forms.
  `strtok` is no longer used in library code.
- **XIM**: `XCloseIM` destroys its input contexts; nested lists in
  `XGetICValues` are filled, not applied.
- **Cursors**: a theme reload re-resolves existing cursors (they pointed
  into the freed theme); `XFreeCursor` frees the cursor's surface.

### Third round: second review

A fresh review of the two rounds' 3,000 changed lines found regressions in
them, which are fixed here, each with a test:

- `map_state` reported nearly every mapped window as `IsUnviewable` (the
  root is never mapped); destroying a window unmapped every inferior (extra
  UnmapNotify, O(n^2) exposes) and scanned the object table per window.
- XI2 got one Enter per virtual crossing, and button/motion were mirrored
  from the core target instead of the source window; unclaimed cookie data
  leaked (it is now freed with the next event, as in libX11).
- Raster ops now apply to stippled/tiled fills; `XDrawImageString` always
  uses GXcopy and paints exactly the text's cells; `XCopyGC` keeps an empty
  clip; key events carry root coordinates; `XQueryFont` accepts a GContext.
- Render: trapezoid edges are evaluated at top/bottom (they were taken from
  the line endpoints), polygon masks cover only the bounding box, software
  paths index HiDPI surfaces correctly, glyph runs honour the source origin
  and CPClipMask, a clip mask replaces clip rectangles, and A1 pictures keep
  the same bit-0 form core drawing uses.
- I/O: the paste-timeout off-by-one between the threads, the broker's 5 s
  spin on EOF (it now refuses at once, and the owner says `R`), broker
  deadlines now driven by the wakeup thread, accepted connections watched,
  and the clipboard serve is non-blocking (a same-process drag stalled and
  truncated); `mw_write_all` leaves SIGPIPE pending if the caller blocks it.
- The test compositor sends `dnd_finished` only after the target finishes,
  as real compositors do (the shim now frees its source on it).

And in the code the first pass covered least:

- **Shared property store** (`smprops.c`): records are validated (a wrapped
  size overflowed the reader); the files live in a validated per-user
  runtime directory, opened `O_NOFOLLOW` (the `/tmp/xlib-wayland` fallback
  could be pre-created or symlinked by another user); readers lock and read
  the whole file; values are served on the root only, through the normal
  offset/length/type logic; appends publish the merged value.
- **Resources**: files are loaded as they are (a newline was added per line,
  cutting every continued value, such as translation tables).
- **Session**: a command is not lost when the connection is accepted before
  it is written; `mw-session` cannot be killed by SIGPIPE.
- **Text properties**: the last string is kept without a trailing NUL;
  encodings follow the style and the encoding atom; 4-byte UTF-8.
- **Extensions**: event numbers are unique and below 128 (XShm 129 equalled
  XFIXES' cursor event, Sync's alarm the Damage event); `XShmPutImage` sends
  ShmCompletion; `XcursorGetTheme` no longer leaks; RandR modes have names.
- **Also**: font metrics (rbearing, bounds, run extents, 16-bit extents),
  font sets (freed; accessors return owned storage), `XCreateImage` leaves
  data to the caller as libX11 does, depth-1 and XYPixmap layouts, colour
  parsing matches libX11 (`#` left-justified, `rgb:` scaled),
  `XAllocColorCells` fails on TrueColor, the FT_Face is freed with cairo's
  face, Wayland request version guards, no zombie helper processes, the
  XDND source leak, the Motif payload size limit, `XGetStandardColormap`
  bounds, `XMatchVisualInfo` class 0, keysym `U` control characters, the
  overflow-retry compose double feed, and an offset past the end of a
  property is BadValue.
- **Tests**: the wrapper fails when the compositor does not start (a test
  used to pass by skipping), cleans up after itself, and isolates the
  environment (HOME, resource files, `XLIB_WAYLAND_*`).

## Still open

- **Keyboard**: `XLookupString` picks only Shift levels (CapsLock, NumLock
  and AltGr are ignored); compose is fed on KeyRelease too;
  `XGetKeyboardMapping` reports two keysyms per key; `XConvertCase` is ASCII
  only; `XSetLocaleModifiers` treats NULL and "" the wrong way round.
- **Clipboard**: large X-to-Wayland copies (INCR) paste empty; the broker
  still serves with a blocking write (bounded at 5 s).
- **Render**: glyph runs do not accumulate into a mask (overlapping glyphs
  with non-Over operators); core text under a raster op uses cairo
  operators; a raster-op line across a huge HiDPI window allocates a large
  coverage mask.
- **Core odds and ends**: `XSendEvent` to PointerWindow/InputFocus,
  `XSync(discard)`, `XNextRequest`, `XReparentWindow` into a descendant,
  `XClearArea` clipping, unmapping a grab window, the default error handler.
- **Extensions**: XFIXES/DAMAGE registrations outlive their window, DAMAGE
  report levels, `XDamageSubtract`, `XextAddDisplay` close hooks,
  `XSyncValue` overflow flags.
- **XIM**: `delete_surrounding_text` counts bytes as characters.
