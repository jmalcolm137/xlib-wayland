# XInput2 (touch and device capabilities)

GTK+ 2.24 does not consume XInput2 (it has only the XInput1 backend, and the
build uses `--with-xinput=no`), so XI2 here is for libXi clients and for apps
that use XI2 directly.  The shim **defers to Wayland**: the device set comes
from the `wl_seat` capabilities and the events come from Wayland input.

## The query surface

XI2 is answered by synthesising replies in `src/xlib/xi2.c`, because libXi
builds requests through Xlib's internal machinery and the shim speaks no wire
protocol (`_XReply` recognises the XInputExtension opcode).  Implemented:
`XIGetExtensionVersion`, `XIQueryVersion` (2.4), `XIQueryDevice`,
`XIListProperties`, `XIGetProperty`.

`XIQueryDevice` reports a device set built from the seat:

* **pointer** (master + XTEST slave): two absolute valuators (x, y over the
  screen) and a three-button `XIButtonClass`;
* **keyboard** (master + XTEST slave): an `XIKeyClass` (keycodes 8–255);
* **touch**, only when the seat advertises `WL_SEAT_CAPABILITY_TOUCH`: a slave
  pointer device ("Wayland touch") with an `XITouchClassInfo` (direct mode) and
  normalised x/y valuators.

`xinput list --long` therefore shows the real classes, exactly as a server
would.

## Touch events

* `XISelectEvents` (minor 46) is parsed as the request drains
  (`mw_render_drain` → `mw_xi2_request`) and the per-window event masks are
  stored.
* The shim binds `wl_touch` when the seat has touch (`input.c`).  A Wayland
  `down`/`motion`/`up` becomes **XI_TouchBegin / XI_TouchUpdate /
  XI_TouchEnd**, delivered to the window under the touch that selected them,
  searching up the window hierarchy.

XI2 events are `GenericEvent` **cookies**: libXi normally converts the wire
event with an `XESetWireToEventCookie` handler, but the shim builds the client
`XIDeviceEvent` directly and hands it to `XGetEventData`.  For that to work,
`mw_event_size` copies the whole `XGenericEventCookie` (not just the
`XGenericEvent` prefix), so the `data` pointer survives the queue.  The device
id, `sourceid` and coordinates all come from Wayland.

## Test infrastructure

The headless compositor now advertises `WL_SEAT_CAPABILITY_TOUCH` and has a
`touch x y` script action (down, a small motion, up).
`tests/xi2_touch_x.c` (run by `scripts/run-tests.sh`) checks the touch device
appears in `XIQueryDevice` and that all three events arrive with Wayland's
coordinates.

## Not yet

* XI2 **pointer/keyboard** event delivery (`XI_Motion`, `XI_ButtonPress`,
  `XI_KeyPress`, `XI_Enter/Leave/Focus`, smooth scroll).  Core input already
  works; an XI2-only client would get no pointer/keyboard events.
* Tablet/tool classes (`XIToolClass`), gesture classes, touch grabs and
  `XITouchOwnership`, and `XIGetSelectedEvents`.
* Touch is single-touch in practice (the compositor sends one point); the
  event path handles whatever ids Wayland delivers.
