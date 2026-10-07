# XDND — drag-and-drop over Wayland

GTK2's drag-and-drop is **XDND**: a drag source owns the `XdndSelection`
selection and sends `XdndEnter` / `XdndPosition` / `XdndDrop` ClientMessages to
the drop site (a window advertising `XdndAware`); the site answers `XdndStatus`,
pulls the data from `XdndSelection` with `XConvertSelection` after the drop, and
sends `XdndFinished` back to the source.

None of that crosses between shim processes (each is its own X server), so this
mirrors the Motif bridge (`dnd.c`): the compositor routes the drag over
`wl_data_device`, and `xlib/xdnd.c` speaks XDND to the X client in each process.

## Wayland → X (a Wayland drag onto an X client)

When a Wayland drag enters a surface whose X window is `XdndAware`, the shim acts
as the XDND **source** to that client:

* a synthetic source window carries `XdndTypeList` (the offered MIMEs as atoms)
  and `XdndActionList`;
* the shim sends `XdndEnter` / `XdndPosition` / `XdndDrop` and consumes the
  target's `XdndStatus` / `XdndFinished` (in `XSendEvent`);
* the dropped bytes are fetched when the drag drops — before the Wayland offer
  is torn down with the drag — and stashed, then served to the target's
  `XConvertSelection(XdndSelection, …)`.

The test compositor implements minimal drag routing (`start_drag` +
`enter`/`motion`/`drop`); `tests/xdnd_x.c` is a raw-Xlib drop target, and
`scripts/run-tests.sh` checks a `text/uri-list` drop end to end.

## X → Wayland (a GDK/GTK2 drag onto a Wayland client)

When an X client takes `XdndSelection` (GDK does this when a drag starts), the
shim:

* asks the owner for its target list (`XConvertSelection(XdndSelection, TARGETS)`)
  and offers those MIME names on a Wayland data source;
* serves the Wayland `send()` by converting `XdndSelection` for the matching
  target and piping the bytes — the same `XConvertSelection` + clip-serve path
  the clipboard source uses.

`tests/xdnd_src_x.c` exercises the detection, the TARGETS query and the drag
start (`MWXDND: started Wayland drag … offered=N`).

## Choosing XDND vs Motif

`clipboard.c` dispatches an incoming Wayland drag by what the X window under the
pointer accepts: a Motif drag carries the private `application/x-motif-drag`
payload; otherwise an `XdndAware` window gets XDND, and anything else falls back
to the Motif presentation.

## Not yet

* A test of the X→Wayland direction's full data transfer to a Wayland drop
  target (the send path is shared with the clipboard, which is covered).
* `XdndActionMove`/`Link` semantics beyond advertising the actions; only Copy is
  negotiated.
* `XdndProxy` (used by some toolkits to forward drags to a proxy window).
