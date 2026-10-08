# Render (XRender / Xft)

The Render extension is implemented on the shim's raster backend
(`src/xlib/render.c`) and reported as present, so libXrender, Xft and
cairo-xlib take their real compositing paths instead of the core-protocol
fallback.  The shim installs as `libX11.so.6`, so libXrender drives us through
Xlib's request machinery; we intercept the Render major opcode and keep a
Picture / GlyphSet object model, rasterising each operation with cairo or with a
small software compositor for the cases cairo cannot express.

Xft is a separate library (`src/xft/`); its draw-level entry points render
through the shim's FreeType/fontconfig path, and its Render-level entry points
(`XftGlyphSpecRender` and friends) look up the drawable behind the destination
Picture and draw through the same path.

## Conformance: rendercheck 1.6

rendercheck passes **10372 / 10372 tests** with no failures.  rendercheck is not
packaged here; build it and run it against the bundled compositor:

```sh
# build rendercheck once (needs meson + the X11 dev packages)
git clone https://gitlab.freedesktop.org/xorg/test/rendercheck
meson setup rendercheck/build rendercheck && meson compile -C rendercheck/build

# run it against a headless instance of the shim
RT=/tmp/mwrt; mkdir -p "$RT"
XDG_RUNTIME_DIR="$RT" build/headless-compositor --socket mwrc --size 1024x768 \
    --timeout 360 --output /tmp/rc.png &
sleep 1
XDG_RUNTIME_DIR="$RT" WAYLAND_DISPLAY=mwrc LD_LIBRARY_PATH="$HOME/.local/motif-wayland/lib" \
    rendercheck/build/rendercheck
```

## What it covers

* **Composite / FillRectangles / CompositeGlyphs** for every operator, including
  the whole Disjoint/Conjoint family.
* **Linear, radial and conical gradients** with their repeat modes, and
  gradient/picture transforms.
* **Masks**, including component-alpha masks (the mask's R/G/B/A weight the
  source's channels independently) and transformed masks.
* **Triangles / Trapezoids / TriStrip / TriFan**, with the geometry applied as a
  mask across the whole clipped drawable.
* PictFormats `a8r8g8b8`, `r8g8b8`, `x8r8g8b8`, `x8b8g8r8`, `a8`, `a1`.

## Implementation notes

* **Formats.** Every surface is ARGB32 internally. A destination picture whose
  format has no alpha channel (`r8g8b8`, `x8r8g8b8`, `x8b8g8r8`) is treated as
  opaque: the alpha byte is forced back to 1 over the pixels written, and an
  `x8b8g8r8` destination's stored bytes are red/blue-swapped on read and write.
* **Colours.** Render colours are premultiplied; they are unpremultiplied before
  being handed to cairo (solids and gradient stops).
* **Operators cairo cannot express.** The Disjoint/Conjoint operators have
  non-linear Fa/Fb weights, so they run through a small software compositor that
  mirrors rendercheck's reference. Component-alpha masks use the same path.
* **Geometry as a mask.** Triangles/trapezoids build one coverage mask for the
  whole request and apply the operator across the entire clipped drawable, as
  Render specifies: operators such as `Src` and `Clear` affect the masked-out
  area too.
* **Errors.** Compositing *to* a gradient or solid picture delivers an X
  `BadDrawable`, dispatched from `XSync` like a synchronous Xlib call.

## Known simplifications

* Destination picture transforms are handled by the software compositor for
  Composite/FillRectangles (sampled through the inverse map); triangles and
  trapezoids with a destination transform still are not, which rendercheck does
  not exercise.
* `XRenderCompositeGlyphs` uses nearest glyph sampling.

Clip masks are implemented: `XSetClipMask` clips to the exact set-bit
rectangles of a depth-1 mask, and a Render `CPClipMask` picture is applied as an
alpha multiplier on the source (in the software compositor and the triangle
path).

See [DESIGN.md](../DESIGN.md) §3.9 and §7, and [IME-STATUS.md](IME-STATUS.md) for
the text-input side.
