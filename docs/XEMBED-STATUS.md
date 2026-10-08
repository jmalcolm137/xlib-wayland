# XEmbed

XEmbed embeds one X client's window inside another's: the *plug* is reparented
into the *socket*, carries an `_XEMBED_INFO` property, and the two exchange
`_XEMBED` ClientMessages; the embedder also adds the plug to its save-set.

## Why it cannot be fully done here

* **Cross-process is impossible under Wayland.** Embedding means one client's
  window becoming a child of another client's window.  Wayland only lets a
  client create subsurfaces of *its own* surface; there is no cross-client
  embedding (this is why cross-client XEmbed does not work under XWayland
  either).  Every shim process is a separate Wayland client, so a plug in one
  process cannot be drawn or clipped inside a socket in another.
* **GTK2 refuses even the same-process case.**  `GtkPlug`'s ReparentNotify
  handler calls `gdk_window_lookup_for_display()` on the socket and, if it finds
  it in the same process, warns *"Plug reparented unexpectedly into window in the
  same process"* and bails (`gtk/gtkplug-x11.c`).  So `GtkPlug`/`GtkSocket` are a
  **documented non-goal** for this architecture: panels embedding applets
  cross-process cannot work, and GTK2 will not do the same-process fallback.

## What the shim does provide

For XEmbed-capable same-process embedders (not GTK2), the protocol primitives
are in place:

* **`XReparentWindow`** now actually re-homes a window.  Moving a window into a
  non-root parent drops its Wayland toplevel (`mw_toplevel_destroy`) so it is
  composited into its ancestor's toplevel — the plug is drawn inside the socket;
  moving it back to the root re-creates the toplevel.  It emits **ReparentNotify**
  to the window (how a plug learns which socket it landed in, from
  `xre->parent`) and to the old/new parents under `SubstructureNotify`, plus a
  `ConfigureNotify`.
* **`XSendEvent`** delivers the `_XEMBED` ClientMessage to the plug window
  (it already delivers to the client regardless of mask, which is what XEmbed
  needs).
* **`_XEMBED_INFO`** is an ordinary `XChangeProperty`/`XGetWindowProperty` on the
  plug window.
* **`XFixesChangeSaveSet`** exists in the `libXfixes` facade as a no-op (there is
  no server-side save-set here).

## Test

`tests/xembed_x.c` (run by `scripts/run-tests.sh`) creates a socket and a plug,
sets `_XEMBED_INFO`, reparents the plug into the socket at (5,5), and checks that
`ReparentNotify` arrives with the socket as parent, that the plug's attributes
are socket-relative, that an `_XEMBED` ClientMessage is delivered, and that
`_XEMBED_INFO` reads back.
