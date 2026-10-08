# Session management (the X11 session protocol)

## Two different things

* **XSMP** (the X Session Management Protocol) is an **ICE** protocol: a session
  manager (mate-session, gnome-session, …) listens on a socket and apps connect
  with libSM/libICE, driven by the `SESSION_MANAGER` env var.  It never touches
  Xlib, so it is not this shim's concern — it works (or does not) independently.
* **The X11 session protocol** is two ClientMessages to a top-level window:
  **WM_SAVE_YOURSELF** (save state, then republish `WM_COMMAND`) and
  **WM_DELETE_WINDOW** (close).  A client opts in by listing the atom in
  `WM_PROTOCOLS`.  This *is* libX11-adjacent, and it is what the shim now
  supports.

The existing `smprops.c` already shares the CDE session manager's `_DT_SM_*`
properties across processes; `session.c` adds the message protocol.

## Why it needs a relay

The sender is the session manager (or the window manager on its behalf) — one
process.  Every X client here is its own X server (`DESIGN.md` §3), so that
process cannot deliver a ClientMessage to another process's windows.  The relay
bridges them the same way `broker.c` does for selections and `smprops.c` for
properties.

## How it works

With `XLIB_WAYLAND_SESSION` set, each shim process creates and listens on a
Unix socket at

    $XDG_RUNTIME_DIR/xlib-wayland/session/<pid>

`tools/mw-session` (installed next to the shim) connects to every socket in that
directory and writes `save` or `close`.  The receiving process then walks its
root's children and, for each mapped top-level window that lists the atom in
`WM_PROTOCOLS`, posts the ClientMessage:

* `save`  → `WM_SAVE_YOURSELF` (the client updates `WM_COMMAND` itself);
* `close` → `WM_DELETE_WINDOW` (with a timestamp in `data.l[1]`).

The session socket is watched on every path a client can idle in:

* the blocking `mw_block_for_events` poll set;
* the non-blocking `XPending` path (`mw_process_events(d, false)`);
* the wake thread, so a client blocked on `XConnectionNumber` — which is what
  GTK's main loop does — is still woken.

## Properties the session manager needs

These come from the client, not the shim: GDK sets `_NET_WM_PID` on each
top-level and `SM_CLIENT_ID` on the leader window (`gdkdisplay-x11.c`,
`gdkmain-x11.c`), and `XChangeProperty` handles them.  Nothing extra is needed.

## Test

`tests/session_x.c` (run by `scripts/run-tests.sh`) lists both atoms in
`WM_PROTOCOLS`, then `mw-session save` / `mw-session close` are relayed and the
client reports receiving both ClientMessages.

## Not yet

* No aggregation of replies: the shim delivers the messages but does not report
  back which clients saved or refused to close.  A real session manager (over
  XSMP) does that side itself.
* `WM_SAVE_YOURSELF`'s "save to `WM_COMMAND`" is the client's job; the shim only
  delivers the message.
* XSMP itself (ICE/libSM) remains out of scope for libX11.
