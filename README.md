# Xlib for Wayland

A Wayland-native, ABI-compatible implementation of `libX11` (Xlib).

The motivation for this project is to run the Motif text editor NEdit natively on Wayland.

NEdit -> OpenMotif -> libXt -> Xlib -> Wayland

libXt (from Xorg) builds and runs unmodified on Xlib (for Wayland).

OpenMotif builds and runs without modification on libXt.

NEdit builds and runs unmodified on OpenMotif.

This is not the same as Xwayland. NEdit run this way is a real native Wayland app. No X11 protocol is involved.

See [DESIGN.md](DESIGN.md) for the full rationale.

## Status

The shim (`src/`, installed as `libX11.so.6`) is implemented and working.
It is validated end-to-end, without any X server, against a bundled headless
Wayland compositor:

| Milestone | State |
|---|---|
| M0 model + build + unit tests | ✅ done (`meson test`) |
| M1 Wayland backend, windows, drawing, `XImage`, events | ✅ done |
| M2 fonts/text, colours, cursors, selections, Xrm, XRandR, XShape, local XIM | ✅ done |
| M3 upstream libXt built unmodified; real Xt program runs | ✅ done |
| M4 Open Motif (`libXm`) built unmodified; a Motif program runs | ✅ done |
| **XV 6.2 built unmodified and rendering** | ✅ **done** |
| **NEdit 5.7 built unmodified and rendering** | ✅ **done** |
| X resource database (`RESOURCE_MANAGER`, `~/.Xresources`, `$XENVIRONMENT`) | ✅ done |
| Client-side decorations when the compositor has none | ✅ done |
| M5 Xft/Render, full IME | ✅ **done** — Render is implemented and on by default; XIM is a real bridge to Wayland `zwp_text_input_v3` ([docs/IME-STATUS.md](docs/IME-STATUS.md)) |
| GTK+ 2.24.33 built unmodified; gtester 14/14 | ✅ done |
| MATE 1.10 applications built unmodified (29 components) | ✅ done |
| GIMP 2.10.24 built unmodified; runs and renders with no XWayland | ✅ done |
| EWMH window-manager messages (`_NET_WM_STATE` fullscreen/maximize) | ✅ done |
| XSETTINGS `_XSETTINGS_S*` manager (theme/font/Xft from a config file) | ✅ done ([docs/XSETTINGS-STATUS.md](docs/XSETTINGS-STATUS.md)) |
| Clipboard carries non-text (image/uri-list) both ways | ✅ done ([docs/CLIPBOARD-STATUS.md](docs/CLIPBOARD-STATUS.md)) |
| PRIMARY selection (select-to-paste) bridged to Wayland | ✅ done ([docs/CLIPBOARD-STATUS.md](docs/CLIPBOARD-STATUS.md)) |
| XDND drag-and-drop bridged to Wayland (both directions) | ✅ done ([docs/XDND-STATUS.md](docs/XDND-STATUS.md)); Motif DnD also bridged (`dnd.c`) |
| XFIXES (`libXfixes`): selection-owner notifications for GDK | ✅ done ([docs/XFIXES-STATUS.md](docs/XFIXES-STATUS.md)) |
| Xcursor (`libXcursor`): themed/image cursors for GDK | ✅ done ([docs/XFIXES-STATUS.md](docs/XFIXES-STATUS.md)) |
| Xext (`libXext`): SHAPE, MIT-SHM, Sync, extutil | ✅ done ([docs/XEXT-STATUS.md](docs/XEXT-STATUS.md)) |
| Compositing / RGBA windows under Wayland | ✅ done ([docs/COMPOSITE-STATUS.md](docs/COMPOSITE-STATUS.md)) |
| XInput2 device classes + touch events from Wayland | ✅ done ([docs/XI2-STATUS.md](docs/XI2-STATUS.md)) |
| X11 session protocol (WM_SAVE_YOURSELF/WM_DELETE_WINDOW) relayed | ✅ done ([docs/SESSION-STATUS.md](docs/SESSION-STATUS.md)) |

The GTK2 / MATE 1.10 / GIMP 2.10 targets are built and run by the
[`gtk2-wayland`](https://github.com/jmalcolm137/gtk2-wayland) project; their
status is recorded there.

Verified behaviour (see `scripts/run-tests.sh`): display/screen/visual setup,
the X window tree, GCs and the drawing primitives, `XPutImage`/`XGetImage`
pixel-exact, core-font text, `Expose`/`Configure`/`Button`/`Key`/`Motion`
events, atoms and window properties, Xrm, X's integer pixel grid for 1px
stroked lines, and menu interaction — an override-redirect popup is promoted to
an `xdg_popup`, takes a grab, arms items on `EnterNotify`, and routes presses
to the item window.  Beyond the unit/golden tests, the stock **libXt**,
**libXm**, **XV** and **NEdit** have all been built from unmodified sources
against the prefix and run under the headless compositor.

> Note: Open Motif needs `-std=gnu17` (its build tools predate C23) and its
> `configure` must be given `--disable-xft`; NEdit needs `-std=gnu89 -fcommon`.
> Both are host-compiler accommodations, not shim changes.

## Quick start

```sh
# build the shim and run the unit + headless render tests
scripts/run-tests.sh
```

The test runner needs `python3`+Pillow for pixel verification; it builds the
shim with meson, runs `test_core`, renders `test_draw` under the bundled
`headless-compositor`, and asserts the captured frame pixel-for-pixel.

## Verifying against real X11 programs

There is no drop-in conformance suite for an in-process `libX11`: the X Test
Suite exercises the X wire protocol (which this shim does not speak), and libX11
ships no tests of its own. The effective substitute is to run real, widely used
X clients and see what they need:

```sh
scripts/run-x11-clients.sh        # or a subset: ... xterm xclock
```

Each client runs under the bundled compositor with the shim first on the library
path. `xmessage`, `xlogo`, `xload`, `xcalc`, `xclock`, `xman`, `xedit` and
`xterm` start and paint; `xev` runs and reports the events the shim delivers,
`xdpyinfo` — which also pulls in libXtst — produces a full
display/screen/visual report, `xinput` enumerates a synthetic input-device set
through the XInput2 query path, and `setxkbmap -query` reads back the layout
names.  There are no known gaps left in the matrix; a client that needs an
unimplemented symbol is still reported as `gap` with the name of the symbol.  A `crash` is always a regression.  The matrix also runs as part of
`scripts/run-tests.sh`, which additionally drives `xev` through a
map/enter/motion/button/key sequence and checks it reports each event, and
checks that xdpyinfo's report contains each section, that xinput lists the
devices, and that setxkbmap reads the layout names.

`xclock`, `xlogo`, `xload`, `xcalc`, `xman`, `xedit` and `xev` are the
`xorg-xclock`, `xorg-xlogo`, `xorg-xload`, `xorg-xcalc`, `xorg-xman`,
`xorg-xedit` and `xorg-xev` packages. Use them from the system, or unpack their
binaries under `$MW_PREFIX` without root:

```sh
pacman -Sp --print-format '%l' xorg-xclock xorg-xlogo xorg-xload xorg-xcalc \
                                   xorg-xman xorg-xedit xorg-xev \
    | xargs -n1 curl -fsLO
d="$(mktemp -d)"; for f in *.pkg.tar.zst; do tar --zstd -xf "$f" -C "$d"; done
mkdir -p "$MW_PREFIX/bin" "$MW_PREFIX/share/X11/app-defaults"
cp "$d"/usr/bin/x{clock,logo,load,calc,man,edit,ev} "$MW_PREFIX/bin/"
cp "$d"/usr/share/X11/app-defaults/X{Clock,Logo,Load,Calc,man,edit}* \
   "$MW_PREFIX/share/X11/app-defaults/"
```

`xedit` needs one extra step: the directory it loads its Lisp files from is
compiled in (`${libdir}/X11/xedit/lisp`), so the packaged binary exits as soon
as it cannot find them. Build it from source with the path under the prefix
rather than unpacking it:

```sh
./configure --prefix="$MW_PREFIX" --with-lispdir="$MW_PREFIX/share/xedit/lisp"
make -j && make install
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
tests/          test_core.c (unit), test_draw.c (Xlib), test_xt.c (Xt),
                test_xm_menu.c (Motif menu), test_popup.c (menu interaction,
                Motif-free), popup.input (scripted clicks for test_popup)
scripts/        run-tests.sh, build-stack.sh, build-xv.sh, build-nedit.sh
```

## Building the full stack

The stack is built in layers, bottom-up. The build scripts install into a
single prefix, so every consumer links against the shim plus the libXt/libXm
built here, never against the system copies.

### 0. Prerequisites

* Build tools: `meson`, `ninja`, `gcc`/`cc`, `make`, `cmake`, `pkg-config`,
  `autoconf`, `automake`, `libtool`/`libtoolize`, plus `bison`/`byacc` and
  `flex`/`lex` for Open Motif.
* Upstream sources, unpacked under `$MW_SRC` (default
  `${TMPDIR:-/tmp}/xlib-wayland`; set `MW_SRC` to wherever you keep them):

  | Component | Path              | Build system |
  |-----------|-------------------|--------------|
  | libXt     | `$MW_SRC/libxt`   | autotools |
  | Open Motif| `$MW_SRC/motif`   | autotools |
  | XV 6.2    | `$MW_SRC/xv`      | CMake     |
  | NEdit 5.7 | `$MW_SRC/nedit`   | plain makefiles |

No `sudo` is required: everything installs under `$MW_PREFIX`.

### 1. Choose a prefix

All scripts read the install prefix from `MW_PREFIX` (default
`$HOME/.local/motif-wayland`). Set it once and it propagates to every script:

```sh
export MW_PREFIX="${MW_PREFIX:-$HOME/.local/motif-wayland}"
```

### 2. Build and install the libX11 shim

`build-stack.sh` deliberately does **not** build the shim; it expects
`$MW_PREFIX/lib/libX11.so` to already be installed. Build it with meson from
the project root (use the same prefix):

```sh
meson setup build --prefix="$MW_PREFIX"
meson install -C build
```

If `$MW_PREFIX/lib/libX11.so` is missing, `build-stack.sh` stops immediately
with this exact instruction.

### 3. Build libXt + Open Motif

```sh
scripts/build-stack.sh
```

This runs two stages, printing a banner for each:

1. **libXt** — regenerates autotools and configures with
   `CPPFLAGS=-I$MW_PREFIX/include`,
   `LDFLAGS=-L$MW_PREFIX/lib -Wl,-rpath,$MW_PREFIX/lib` and
   `PKG_CONFIG_PATH=$MW_PREFIX/lib/pkgconfig`, then `make -j`/`make install`.
   `--disable-xkb` is passed only if the generated `configure` accepts it.
2. **Open Motif** — regenerates autotools with `./autogen.sh`, then configures
   with `--prefix=$MW_PREFIX`, `--x-includes=$MW_PREFIX/include`,
   `--x-libraries=$MW_PREFIX/lib`, and `--disable-xft` (only if supported),
   then `make -j`/`make install`.
   (`--disable-build-demos` is passed only if a future tree supports it;
   Open Motif 2.3.9 has no such option, so its demos build too — harmless.)

Useful flags / overrides:

```sh
# Rebuild only one stage
scripts/build-stack.sh --skip-xt        # libXt already good, rebuild Motif
scripts/build-stack.sh --skip-motif     # rebuild libXt only

# Different prefix for this run
MW_PREFIX=/opt/motifwl scripts/build-stack.sh
scripts/build-stack.sh --prefix=/opt/motifwl

# Fewer parallel jobs
JOBS=4 scripts/build-stack.sh
```

Both stages are idempotent: re-running simply reconfigures and reinstalls.

### 4. Build XV

```sh
scripts/build-xv.sh
```

Configures with CMake against the prefix and builds:

```sh
cmake -S "$MW_SRC/xv" -B "$MW_PREFIX/build/xv" \
      -DCMAKE_PREFIX_PATH="$MW_PREFIX" -DCMAKE_BUILD_TYPE=Release
cmake --build "$MW_PREFIX/build/xv" -j
```

`find_package(X11)` **must** resolve to `$MW_PREFIX` (our libX11/libXt), not
the system X11. The script passes `CMAKE_PREFIX_PATH` plus include/library
hints to force this; check the configure output if in doubt.

### 5. Build NEdit

```sh
scripts/build-nedit.sh
```

NEdit ships hand-written makefiles and no configuration system, so the script
builds it unmodified by overriding `CFLAGS`/`LIBS` on the make command line
(the hardcoded `/usr/X11R6` paths in `makefiles/Makefile.linux` are replaced
with `$MW_PREFIX`):

```sh
make -C "$MW_SRC/nedit" linux \
     CFLAGS="-O2 -I$MW_PREFIX/include ... -DHAVE__XMVERSIONSTRING" \
     LIBS="-L$MW_PREFIX/lib -lXm -lXt -lX11 -lm -Wl,-rpath,$MW_PREFIX/lib"
```

`HAVE__XMVERSIONSTRING` is enabled because Open Motif 2.3.9 exports
`_XmVersionString`, which lets `nedit -V` report both compile-time and
run-time Motif versions. No extra "NEDIT version symbol" is needed for Motif
2.1 compat: NEdit reads `XmVersion` from `<Xm/Xm.h>`. (The `-lXp`/`-lXpm`
note in `Makefile.linux` only applies when linking `-lXext`, which NEdit's
baseline link line does not.)

The result is a self-contained `source/nedit` and `source/nc`; the script
leaves them in the source tree and prints their paths.

### One-shot sequence

```sh
export MW_PREFIX="$HOME/.local/motif-wayland"

meson setup build --prefix="$MW_PREFIX" && meson install -C build
scripts/build-stack.sh
scripts/build-xv.sh
scripts/build-nedit.sh
```

### Script environment variables

| Variable     | Default                              | Used by |
|--------------|--------------------------------------|---------|
| `MW_PREFIX`  | `$HOME/.local/motif-wayland`         | all |
| `MW_SRC`     | `${TMPDIR:-/tmp}/xlib-wayland`       | all |
| `JOBS`       | `nproc`                              | all |
| `LIBXT_SRC`  | `$MW_SRC/libxt`                      | `build-stack.sh` |
| `MOTIF_SRC`  | `$MW_SRC/motif`                       | `build-stack.sh` |
| `XV_SRC`     | `$MW_SRC/xv`                         | `build-xv.sh` |
| `NEDIT_SRC`  | `$MW_SRC/nedit`                      | `build-nedit.sh` |
| `NEDIT_MAKE_TARGET` | `linux`                       | `build-nedit.sh` |

Each script also accepts `--prefix=DIR` and `-h`/`--help`.
