# Xlib for Wayland

A Wayland-native, ABI-compatible implementation of `libX11` (Xlib).

The motivation for this project is to run the Motif text editor NEdit natively on Wayland.

NEdit -> OpenMotif -> libXt -> Xlib -> Wayland

libXt (from Xorg) builds and runs unmodified on Xlib (for Wayland).

OpenMotif builds and runs without modification on libXt.

NEdit builds and runs unmodified on OpenMotif.

This is not the same as Xwayland. NEdit run this way is a real native Wayland app. No X11 protocol is involved.

See [DESIGN.md](DESIGN.md) for the full rationale.

Compatibility is good enough to run applications built with Xlib, the Athena Toolkit, the Xt Toolkit,
CDE / OpenMotif, GIMP 2.10 over GTK2, and Firefox 157 over GTK3 (X11 back-end).

## Quick start

```sh
# Build xlib-wayland
meson setup build --prefix="$XLIB_PREFIX"
meson install -C build
```

```sh
# build the shim and run the unit + headless render tests
scripts/run-tests.sh
```

### Prerequisites

* Build tools: `meson`, `ninja`, `gcc`/`cc`, `make`, `cmake`, `pkg-config`,
  `autoconf`, `automake`, `libtool`/`libtoolize`, plus `bison`/`byacc` and
  `flex`/`lex` for Open Motif.

## Verifying against real X11 programs

There is no drop-in conformance suite for an in-process `libX11`: the X Test
Suite exercises the X wire protocol (which this shim does not speak), and libX11
ships no tests of its own. The effective substitute is to run real, widely used
X clients and see what they need:

```sh
scripts/run-x11-clients.sh        # or a subset: ... xterm xclock
```

## Repository layout

```
src/raster/     pluggable raster interface (raster.h) + cairo backend
src/wayland/    wl.c (connection/registry), surface.c (xdg_toplevel + compositor)
src/xlib/       the Xlib implementation (display, window, gcontext, image,
                font, event, input, atom/property, selection, cursor, Xrm,
                region, XIM, XRandR, XShape, and xlibint.c: the internal ABI
                that libXext/libXrender/libcairo depend on)
tools/          headless-compositor.c (test compositor)
tests/          test_core.c, test_internal.c (unit), test_draw.c (Xlib), test_xt.c (Xt),
                test_xm_menu.c (Motif menu), test_popup.c (menu interaction,
                Motif-free), popup.input (scripted clicks for test_popup)
scripts/        run-tests.sh, run-x11-clients.sh, run-firefox-headless.sh,
                build-stack.sh, build-xv.sh, build-nedit.sh, firefox-mozconfig
```
