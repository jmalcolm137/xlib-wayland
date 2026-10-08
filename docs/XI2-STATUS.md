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
`XIListProperties`, `XIGetProperty`, `XIGetSelectedEvents`.

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

* **Events** (input.c mirrors each Wayland input event as XI2 when a window
  selected it, independent of the core event mask): `XI_Motion`,
  `XI_ButtonPress`/`XI_ButtonRelease`, `XI_KeyPress`/`XI_KeyRelease`,
  `XI_Enter`/`XI_Leave` and `XI_FocusIn`/`XI_FocusOut`.  They are delivered as
  `XIDeviceEvent` / `XIEnterEvent` GenericEvent cookies, like touch.
  `tests/xi2_event_x.c` verifies motion/button/key arrive.

## Not yet

* **Tablet/tool classes** (`XIToolClass`).  Wayland exposes tablets only through
  the `zwp_tablet_v2` protocol (an optional libinput add-on), which the shim
  does not bind, so there is no tablet device to describe; this is deferred
  rather than emulated.
* Gesture classes, touch grabs and `XITouchOwnership`.
* **Smooth-scroll delivery**: the pointer devices advertise the two relative
  scroll valuators and `XIGetSelectedEvents` reports the masks, but Wayland axis
  events are still delivered as wheel buttons, not as XI2 scroll valuators.
* Touch is single-touch in practice (the compositor sends one point); the
  event path handles whatever ids Wayland delivers.
