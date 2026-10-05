# XIM — a real input method over Wayland

The shim's XIM was previously a *local* stub: it returned a non-NULL `XIM` so
Motif and libXaw would not abort, advertised only
`XIMPreeditNothing|XIMStatusNothing`, and `XmbLookupString` merely delegated to
`XLookupString`. There was no preedit and no way to type anything an IME had to
compose (accented Latin, CJK, …).

It is now a real input-method client. `src/xlib/xim.c` keeps the client-visible
XIM contract and drives it from the compositor's input method through
`zwp_text_input_v3` (bound in `src/wayland/wl.c`). There is no XIM wire protocol
and no separate XIM server process: the compositor routes keys to whatever IME
is running (ibus, fcitx5, …) and sends this process `preedit_string` /
`commit_string`.

```
CDE/Motif or GTK2 text widget
   └─ XIM API (XCreateIC, XFilterEvent, XmbLookupString, XNPreedit* callbacks)
        └─ xim.c bridge
             └─ zwp_text_input_v3  ──► River / labwc / KWin
                                          └─ input-method-v2 ──► ibus
```

## Styles

`XNQueryInputStyle` advertises, in order:

1. `XIMPreeditPosition | XIMStatusNothing` — Motif's `OverTheSpot` default.
   The shim paints the composing string itself, over the spot the client
   supplies with `XNSpotLocation` (`mw_xim_overlay`, called from
   `mw_toplevel_render` after the window tree is composited, so it survives
   every client repaint).
2. `XIMPreeditCallbacks | XIMStatusCallbacks` — what GTK2's `im-xim` wants.
   The client draws the preedit; the shim invokes `XNPreeditStart/Draw/Caret/
   Done`. (GTK2 filters out `Position` before choosing.)
3. `XIMPreeditNothing | XIMStatusNothing`
4. `XIMPreeditNone | XIMStatusNone`

Advertised order matters: Motif's `XmIm` walks `XmNpreeditType` (default
`OverTheSpot,OffTheSpot,Root`) and takes the first match, so Position must come
before Callbacks for CDE; GTK2 ignores Position and takes Callbacks.

## Focus and state

`XSetICFocus`/`XUnsetICFocus` call `zwp_text_input_v3.enable`/`disable`
(committed), set the content type and the cursor rectangle, and remember which
`_XIC` is active. After the compositor's `enter`, all text-input state is
invalidated and the shim resends the content type, cursor rectangle and commit.
`leave` ends any composition.

## Commit delivery

A commit is delivered the way libX11 delivers one: the text is queued and a
synthetic `KeyPress` with **keycode 0** is pushed onto the event queue.
`XFilterEvent` returns `False` for it (as for every event — keys the IME
consumes never reach this process at all), and `XmbLookupString` /
`Xutf8LookupString` / `XwcLookupString` return the queued UTF-8 with status
`XLookupChars`. `XmbResetIC` returns the current preedit as committed text and
resets the IME with a `disable`/`enable` cycle.

`delete_surrounding_text` has no XIM client equivalent and is currently ignored.

## Tests

`scripts/run-tests.sh` exercises both paths against the bundled headless
compositor, which now also implements a **scripted text-input server**: once an
input context is focused it sends a canned preedit then a canned commit.

* `tests/test_xim.c` (callbacks mode) asserts the preedit callbacks fire and
  that `XmbLookupString` returns the committed UTF-8 (`你好`).
* `tests/test_xim.c position` (Position mode) holds the preedit (`HC_IME_COMMIT=""`)
  and a Pillow check confirms the shim drew it at the spot.

When the compositor does not offer `zwp_text_input_manager_v3`, the shim simply
has no text-input object and XIM degrades to the old pass-through, so the
Motif/Xt tests are unaffected (the headless compositor can be told to omit it
with `HC_NO_TEXTINPUT=1`).

## Consuming sessions

Any compositor that exposes `zwp_text_input_manager_v3` works; River (wlroots)
and labwc both do. An input method must be running for composition —
`cde-wayland` starts `ibus`, whose `ibus-wayland` module registers as an
`input-method-v2` server.
