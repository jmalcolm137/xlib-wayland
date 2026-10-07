#!/usr/bin/env bash
# run-tests.sh — build the shim, run unit tests, and run the headless
# render/verify tests (no desktop session required).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BUILD="${MW_BUILD:-$ROOT/build}"
# $MW_SRC holds the reference source/build trees (Motif, Xt, XV, NEdit); the
# X and NEdit tests are skipped when one is absent.
MW_SRC="${MW_SRC:-${TMPDIR:-/tmp}/xlib-wayland}"
PREFIX="${MW_PREFIX:-${TMPDIR:-/tmp}/mw-prefix}"
RUNTIME="${MW_RUNTIME:-${TMPDIR:-/tmp}/mw-runtime}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

say() { printf '\n=== %s ===\n' "$*"; }

say "configure + build"
meson setup "$BUILD" "$ROOT" --prefix="$PREFIX" --reconfigure >/dev/null
meson compile -C "$BUILD"

say "unit tests"
meson test -C "$BUILD" --print-errorlogs

say "install"
meson install -C "$BUILD" >/dev/null

mkdir -p "$RUNTIME"; chmod 700 "$RUNTIME"

run_headless() { # name, socket, size, output, client-cmd...
    local name="$1" sock="$2" size="$3" out="$4"; shift 4
    local ready="$WORK/$name.ready"
    local input_args=()
    [ -n "${INPUT:-}" ] && input_args=(--input "$INPUT")
    # HC_TIMEOUT lets a test keep the compositor alive long enough for a longer
    # input script to finish; the default is fine for the short ones.
    XDG_RUNTIME_DIR="$RUNTIME" "$BUILD/headless-compositor" \
        --socket "$sock" --size "$size" --timeout "${HC_TIMEOUT:-2}" \
        --output "$out" \
        "${input_args[@]}" \
        >"$ready" 2>"$WORK/$name.err" &
    local hc=$!
    for _ in $(seq 1 100); do grep -q READY "$ready" 2>/dev/null && break; sleep 0.05; done
    XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY="$sock" \
        LD_LIBRARY_PATH="$PREFIX/lib:${LD_LIBRARY_PATH:-}" \
        timeout 20 "$@" >"$WORK/$name.client" 2>&1 || true
    wait "$hc" 2>/dev/null || true
}

say "headless render test (Xlib)"
run_headless draw mwdraw 640x400 "$WORK/draw.png" "$BUILD/test_draw" 5
grep -q 'captured' "$WORK/draw.err" && echo "  captured frame" || { echo "  FAIL: no frame"; exit 1; }

say "format-32 properties and mask-based event selection"
# Two bugs behind NEdit's frozen Cut: format-32 property data is a long array
# (packing raw bytes zeroed Motif's clipboard lock record), and XWindowEvent()
# must map PropertyNotify to PropertyChangeMask through Xlib's type-to-mask
# table rather than shifting the event type.
run_headless props mwprops 400x300 "$WORK/props.png" "$BUILD/test_props"
if grep -q 'all checks passed' "$WORK/props.client"; then
    echo "  ok   format-32 long-array round trip and PropertyNotify masking"
else
    echo "  FAIL: property/mask checks failed"
    cat "$WORK/props.client"
    exit 1
fi

# One test client is a plain Wayland peer (the "mousepad/Konsole" side), the
# other is an X client using the shim.  They run against the same compositor.
run_clip_case() { # name, timeout, server-cmd, client-cmd, expected
    local name="$1" tmo="$2" server="$3" client="$4" expect="$5"
    local sock="mwclip$name$$"
    XDG_RUNTIME_DIR="$RUNTIME" "$BUILD/headless-compositor" \
        --socket "$sock" --size 400x300 --timeout "$tmo" \
        --output "$WORK/$name.png" \
        >"$WORK/$name.ready" 2>"$WORK/$name.hc" &
    local hc=$!
    for _ in $(seq 1 100); do
        grep -q READY "$WORK/$name.ready" 2>/dev/null && break; sleep 0.05
    done
    XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY="$sock" \
        LD_LIBRARY_PATH="$PREFIX/lib:${LD_LIBRARY_PATH:-}" \
        eval "$server" >"$WORK/$name.server" 2>&1 &
    local sp=$!
    sleep 0.7
    XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY="$sock" \
        LD_LIBRARY_PATH="$PREFIX/lib:${LD_LIBRARY_PATH:-}" \
        eval "$client" >"$WORK/$name.client" 2>&1
    wait "$sp" 2>/dev/null || true
    wait "$hc" 2>/dev/null || true
    grep -q "$expect" "$WORK/$name.client"
}

say "clipboard bridge: X selection -> Wayland client"
if run_clip_case xtowl 8 "$BUILD/clip_x own hello-from-x" \
        "$BUILD/clip_wl receive" "GOT:hello-from-x"; then
    echo "  ok   a Wayland client received the text an X client put on CLIPBOARD"
else
    echo "  FAIL: Wayland peer did not get the X selection"
    cat "$WORK/xtowl.client"
    exit 1
fi

say "clipboard bridge: Wayland clipboard -> X client"
if run_clip_case wltox 8 "$BUILD/clip_wl offer hello-from-wayland" \
        "$BUILD/clip_x convert" "GOT:hello-from-wayland"; then
    echo "  ok   an X client converted the compositor clipboard to STRING"
else
    echo "  FAIL: X client did not get the Wayland clipboard"
    cat "$WORK/wltox.client"
    exit 1
fi

say "clipboard bridge: Wayland -> X after the X client already owned it"
# The sequence a real desktop actually produces: cut in NEdit (Motif takes
# CLIPBOARD and records that it owns it), copy in a Wayland application, then
# paste in NEdit.  Motif's WeOwnSelection() compares the owner it recorded
# against the real one, so unless the shim takes the selection back from that
# now-stale owner NEdit keeps pasting its own old text and never asks us.
STALESOCK="mwstale$$"
XDG_RUNTIME_DIR="$RUNTIME" "$BUILD/headless-compositor" --socket "$STALESOCK" \
    --size 400x300 --timeout 10 --output "$WORK/stale.png" \
    >"$WORK/stale.ready" 2>"$WORK/stale.hc" &
STALEHC=$!
for _ in $(seq 1 100); do
    grep -q READY "$WORK/stale.ready" 2>/dev/null && break; sleep 0.05
done
XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY="$STALESOCK" \
    LD_LIBRARY_PATH="$PREFIX/lib:${LD_LIBRARY_PATH:-}" \
    "$BUILD/clip_x" own-then-convert stale-x-text >"$WORK/stale.x" 2>&1 &
STALEXP=$!
sleep 0.8
XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY="$STALESOCK" \
    LD_LIBRARY_PATH="$PREFIX/lib:${LD_LIBRARY_PATH:-}" \
    "$BUILD/clip_wl" offer fresh-wayland-text >"$WORK/stale.wl" 2>&1
wait "$STALEXP" 2>/dev/null || true
wait "$STALEHC" 2>/dev/null || true
if grep -q 'GOT:fresh-wayland-text' "$WORK/stale.x"; then
    echo "  ok   stale X selection owner ceded the clipboard to Wayland"
else
    echo "  FAIL: X kept serving its own stale clipboard"
    cat "$WORK/stale.x"
    exit 1
fi

say "clipboard bridge: non-text (image/png), Wayland -> X"
# The offer carries more than text: an X client converts the mime atom directly
# and gets the bytes back unchanged.
if run_clip_case binwltox 8 "$BUILD/clip_wl offer-mime image/png PNGDATA-wayland-42" \
        "$BUILD/clip_x convert-target image/png" "GOT:PNGDATA-wayland-42"; then
    echo "  ok   an X client received the compositor's image/png"
else
    echo "  FAIL: X client did not get the non-text clipboard"
    cat "$WORK/binwltox.client"
    exit 1
fi

say "clipboard bridge: non-text (image/png), X -> Wayland"
if run_clip_case binxtowl 8 "$BUILD/clip_x own-mime image/png PNGDATA-x11-7" \
        "$BUILD/clip_wl receive-mime image/png" "GOT:PNGDATA-x11-7"; then
    echo "  ok   a Wayland client received the X owner's image/png"
else
    echo "  FAIL: Wayland peer did not get the X non-text selection"
    cat "$WORK/binxtowl.client"
    exit 1
fi

say "primary selection: non-text (image/png), Wayland -> X"
if run_clip_case primwltox 8 "$BUILD/clip_wl primary-offer-mime image/png PRIMDATA-wl-9" \
        "$BUILD/clip_x primary-convert image/png" "GOT:PRIMDATA-wl-9"; then
    echo "  ok   an X client received the compositor's PRIMARY image/png"
else
    echo "  FAIL: X client did not get the Wayland PRIMARY"
    cat "$WORK/primwltox.client"
    exit 1
fi

say "primary selection: text, X -> Wayland"
if run_clip_case primtxt 8 "$BUILD/clip_x primary-own-mime STRING x-primary-text" \
        "$BUILD/clip_wl primary-receive" "GOT:x-primary-text"; then
    echo "  ok   a Wayland client received the X PRIMARY text"
else
    echo "  FAIL: Wayland peer did not get the X PRIMARY text"
    cat "$WORK/primtxt.client"
    exit 1
fi

say "verify pixels"
python3 - "$WORK/draw.png" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGBA')
def near(a, b, tol=6): return all(abs(x-y) <= tol for x, y in zip(a[:3], b[:3]))
# If the compositor offers no server-side decorations, the shim draws a
# client-side titlebar and insets the X content below it.
csd = sum(im.getpixel((5,5))[:3]) < 200 and im.getpixel((5,5))[:3] != (255,255,255)
off = 24 if csd else 0
print("  client-side titlebar inset: %d" % off)
checks = {
    "white background  (460,150)": ((460,150+off), (255,255,255)),
    "red filled rect   (60,60)":   ((60,60+off),   (204,0,0)),
    "white (outside)   (160,60)":  ((160,60+off),  (255,255,255)),
    "blue filled arc   (265,75)":  ((265,75+off),  (0,0,204)),
    "green polygon     (405,100)": ((405,100+off), (0,136,0)),
    "XPutImage gradient(60,200)":  ((60,200+off),  (38,85,128)),
}
bad = 0
for name, (pt, want) in checks.items():
    got = im.getpixel(pt)
    ok = near(got, want)
    print(("  ok   " if ok else "  FAIL ") + name + " -> " + str(got))
    bad += 0 if ok else 1
# X pixel-grid: the drawn line must land exactly on content row 260
dark260 = sum(1 for x in range(40,480) if im.getpixel((x,260+off))[0] < 128)
print(("  ok   " if dark260 > 300 else "  FAIL ") + "line on pixel row y=260 (%d px)" % dark260)
bad += 0 if dark260 > 300 else 1
sys.exit(1 if bad else 0)
PY

if [ -x "$BUILD/test_popup" ]; then
    say "client-side decoration fallback (compositor without xdg-decoration)"
# GNOME/Mutter has no xdg-decoration support, so the shim has to fall back to
# drawing its own titlebar and insetting the X content below it.  HC_NO_DECO
# makes the test compositor behave like one of those.
HC_NO_DECO=1 run_headless draw-csd mwdrawcsd 640x400 "$WORK/draw-csd.png" \
    "$BUILD/test_draw" 5
python3 - "$WORK/draw-csd.png" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGBA')
w, h = im.size
top = im.getpixel((5, 5))[:3]
# The drawn titlebar is dark; the X content below it starts at y=24.
body = im.getpixel((5, 100))[:3]
ok = sum(top) < 200 and sum(body) >= 600
print(("  ok   " if ok else "  FAIL ") +
      "client-side titlebar present: top=%s content=%s" % (top, body))
sys.exit(0 if ok else 1)
PY

say "XIM bridge (zwp_text_input_v3 preedit + commit)"
# The headless compositor's text-input server sends a canned preedit and then a
# canned commit once the input context is focused; test_xim registers preedit
# callbacks and reads the commit back through XmbLookupString.
HC_TIMEOUT=6 run_headless xim mwxim 400x300 "$WORK/xim.png" "$BUILD/test_xim"
sed 's/^/  /' "$WORK/xim.client" || true
if grep -q '^XIM:COMMIT' "$WORK/xim.client" \
   && grep -q 'XIM:RESULT start=1 draw=1 done=1' "$WORK/xim.client"; then
    echo "  ok   preedit callbacks fired and the commit came back as UTF-8"
else
    echo "  FAIL: XIM bridge did not deliver preedit and commit"
    exit 1
fi

say "XIM over-the-spot preedit (XIMPreeditPosition)"
# HC_IME_COMMIT="" makes the compositor hold the preedit so the shim's own
# drawing of the composing string can be captured and checked.
HC_IME_COMMIT= HC_TIMEOUT=6 run_headless xim-pos mwximpos 400x300 \
    "$WORK/xim-pos.png" "$BUILD/test_xim" position
python3 - "$WORK/xim-pos.png" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
px = im.load(); w, h = im.size
# The spot is (20,40); the shim paints the preedit string there.
n = sum(1 for y in range(20, min(55, h)) for x in range(15, min(150, w))
        if sum(px[x, y]) < 720)
print(("  ok   " if n > 20 else "  FAIL ") +
      "shim drew the preedit at the spot (%d non-white px)" % n)
sys.exit(0 if n > 20 else 1)
PY

say "menu interaction test (override-redirect popup + grab)"
    # A Motif menu popup is an override-redirect window parented to the app's
    # toplevel shell plus an XGrabPointer with owner_events.  test_popup.c
    # reproduces that without needing libXt/libXm, and popup.input replays the
    # clicks: open the menu, move onto item 1, click it.
    INPUT="$ROOT/tests/popup.input" \
        run_headless popup mwpop 400x300 "$WORK/popup.png" "$BUILD/test_popup" 8
    sed 's/^/  /' "$WORK/popup.client" || true
    grep -q '^MENU:Beta$' "$WORK/popup.client" || {
        echo "  FAIL: menu item was not selected (expected 'MENU:Beta')"
        exit 1
    }
    grep -q '^ARM:1$' "$WORK/popup.client" || {
        echo "  FAIL: item 1 never armed (no EnterNotify while grabbed)"
        exit 1
    }
    grep -q '^ITEM:1 ' "$WORK/popup.client" || {
        echo "  FAIL: press was not delivered to the item window"
        exit 1
    }
    echo "  ok   menu posted, item armed, press routed to item, selection fired"

    say "menu re-map test (unmap then re-map the same popup surface)"
    # xdg-shell: committing a NULL buffer puts a surface into the "newly
    # unmapped" state, which requires a fresh commit + configure + ack before
    # the next buffer may be attached.  The headless compositor enforces this
    # (as KWin does) and kills the client with "attached a buffer before
    # configure event" otherwise.  Motif dismisses a menu with XUnmapWindow
    # and re-posts it with XMapWindow, so this is the path a real menu takes.
    # HC_KWIN_REMAP makes the compositor behave like KWin, which does NOT send
    # a second xdg_popup.configure when an already-configured popup is mapped
    # again.  A shim that waits for one renders nothing on the re-post: the
    # window is there and clicks reach it, but nothing is ever painted, so no
    # menu appears to drop down.
    HC_KWIN_REMAP=1 MW_TRACE=1 HC_TIMEOUT=8 INPUT="$ROOT/tests/popup-remap.input" \
        run_headless popup-remap mwpop 400x300 "$WORK/remap.png" \
            "$BUILD/test_popup" 12 3
    sed 's/^/  /' "$WORK/popup-remap.client" || true
    if grep -q 'configure event' "$WORK/popup-remap.err" \
       || grep -q 'configure event' "$WORK/popup-remap.client"; then
        echo "  FAIL: buffer attached before configure event"
        exit 1
    fi
    grep -q '^MENU:UNPOSTED$' "$WORK/popup-remap.client" || {
        echo "  FAIL: menu was never dismissed (no NULL-buffer commit)"
        exit 1
    }
    n_post=$(grep -c '^MENU:POSTED$' "$WORK/popup-remap.client" || true)
    n_sel=$(grep -c '^MENU:Beta$' "$WORK/popup-remap.client" || true)
    if [ "$n_post" -lt 3 ] || [ "$n_sel" -lt 3 ]; then
        echo "  FAIL: expected 3 post/select cycles, got $n_post posts, $n_sel selections"
        exit 1
    fi
    # Every posting must have been configured and painted.  Counting renders
    # (rather than just selections) is what catches an invisible re-post.  The
    # popup window id is read from the client rather than hard-coded: the shim's
    # init-time allocation shifts it.
    n_cfg=$(grep -c 'popup configure placed' "$WORK/popup-remap.client" || true)
    popup_id=$(sed -n 's/^POPUP:0x\([0-9a-f]*\).*/\1/p' "$WORK/popup-remap.client" | head -1)
    n_render=0
    [ -n "$popup_id" ] && \
        n_render=$(grep -c "render 0x$popup_id" "$WORK/popup-remap.client" || true)
    if [ -z "$popup_id" ] || [ "$n_cfg" -lt "$n_post" ] || [ "$n_render" -lt "$n_post" ]; then
        echo "  FAIL: re-posted menu was not rendered" \
             "($n_cfg configures, $n_render renders of 0x${popup_id:-?} for $n_post postings)"
        exit 1
    fi
    echo "  ok   re-mapped the same popup $n_post times" \
         "($n_cfg configures, $n_render renders)"
fi

if [ -x "$BUILD/test_popup" ]; then
    say "menu dismissal test (compositor popup_done, then re-post)"
    # A dismissed xdg_popup is inert, so the menu has to be rebuilt when it is
    # posted again -- and anchored to a real toplevel, not to the previous
    # menu shell.  Getting that wrong made every menu after the first one
    # refuse to drop down.
    HC_TIMEOUT=8 MW_TRACE=1 INPUT="$ROOT/tests/popup-dismiss.input" \
        run_headless popup-dismiss mwpop 400x300 "$WORK/dismiss.png" \
            "$BUILD/test_popup"
    sed 's/^/  /' "$WORK/popup-dismiss.client" || true
    # A popup that failed to anchor would silently become a plain toplevel;
    # the functional checks below catch that, since such a window cannot be
    # clicked as a menu at all.
    grep -q '^MENU:DISMISSED$' "$WORK/popup-dismiss.client" || {
        echo "  FAIL: popup_done never reached the client"
        exit 1
    }
    n_post=$(grep -c '^MENU:POSTED$' "$WORK/popup-dismiss.client" || true)
    if [ "$n_post" -lt 2 ]; then
        echo "  FAIL: menu was not re-posted after dismissal ($n_post posts)"
        exit 1
    fi
    grep -q '^MENU:Beta$' "$WORK/popup-dismiss.client" || {
        echo "  FAIL: item could not be selected from the re-posted menu"
        exit 1
    }
    echo "  ok   dismissed popup rebuilt and re-anchored, second menu worked"
fi

if [ -f "$PREFIX/lib/libXm.so.4" ] && [ -f "$PREFIX/lib/libXt.so" ]; then
    say "Xm file selection dialog (NEdit's File -> Open path)"
    # NEdit builds its Open dialog with XmCreateFileSelectionDialog and
    # XmDIALOG_FULL_APPLICATION_MODAL, then busy-waits in a nested
    # XtAppProcessEvent loop until the cancel callback flips a flag.  An
    # XmDialogShell that never receives the ConfigureNotify for the size it
    # asked for stays at the 1x1 it was created with, so the FileSelectionBox
    # children -- Cancel among them -- are laid out outside the window and can
    # never be clicked.  The test prints the real geometry so the input script
    # can aim at Cancel, and reports whether the loop ever exited.
    cc -o "$BUILD/test_xm_filesel" "$ROOT/tests/test_xm_filesel.c" \
        -I"$PREFIX/include" -L"$PREFIX/lib" -lXm -lXt -lX11 \
        -Wl,-rpath,"$PREFIX/lib"
    INPUT="$ROOT/tests/filesel.input" HC_TIMEOUT=8 \
        run_headless filesel mwfsb 900x700 "$WORK/filesel.png" \
            "$BUILD/test_xm_filesel"
    sed 's/^/  /' "$WORK/filesel.client" || true
    if grep -q 'GEO:DIALOG.*size=1x1' "$WORK/filesel.client"; then
        echo "  FAIL: dialog shell stayed at its 1x1 creation size"
        exit 1
    fi
    grep -q '^FSB:CANCEL$' "$WORK/filesel.client" || {
        echo "  FAIL: the Cancel button never fired"
        exit 1
    }
    grep -q '^FSB:DONE$' "$WORK/filesel.client" || {
        echo "  FAIL: the dialog event loop never exited (app wedged)"
        exit 1
    }
    echo "  ok   dialog sized, Cancel fired, event loop exited"

    say "Motif menu bar: second cascade after the first has been used"
    # A menu posted after another menu has been used and dismissed is the case
    # that stopped working on a real session.  Each cascade produces its own
    # popup, so this covers the re-post and re-anchor path with real Motif.
    cc -o "$BUILD/test_xm_menubar" "$ROOT/tests/test_xm_menubar.c" \
        -I"$PREFIX/include" -L"$PREFIX/lib" -lXm -lXt -lX11 \
        -Wl,-rpath,"$PREFIX/lib"
    INPUT="$ROOT/tests/menubar.input" HC_TIMEOUT=12 \
        run_headless menubar mwmb 500x400 "$WORK/menubar.png" \
            "$BUILD/test_xm_menubar"
    sed 's/^/  /' "$WORK/menubar.client" || true
    grep -q '^MENU:Alpha$' "$WORK/menubar.client" || {
        echo "  FAIL: the first cascade produced no selection"
        exit 1
    }
    grep -q '^MENU:Gamma$' "$WORK/menubar.client" || {
        echo "  FAIL: the second cascade never dropped down"
        exit 1
    }
    echo "  ok   both cascades opened and fired"

    say "compositor-driven resize (server-side decorations)"
    # The compositor sends a new size; the X window must adopt it, the client
    # must be told (ConfigureNotify) so it re-lays out, and the newly exposed
    # area must be painted rather than left black.
    INPUT="$ROOT/tests/resize.input" HC_TIMEOUT=8 MW_TRACE=1 \
        run_headless resize mwrs 700x600 "$WORK/resize.png" \
            "$BUILD/test_xm_menubar"
    # The menu bar is 38px tall, so the work area below it is 500x362.
    grep -q 'xwindow=500x362' "$WORK/resize.client" || {
        sed 's/^/  /' "$WORK/resize.client"
        echo "  FAIL: the X window did not adopt the compositor's size"
        exit 1
    }
    # Repaints must be coalesced.  Compositing and uploading a whole window once
    # per drawing step made an interactive resize crawl, so the poll paths cap
    # themselves to roughly one frame per display refresh and defer the rest.
    n_render=$(grep -c 'MW: render' "$WORK/resize.client" || true)
    if [ "$n_render" -gt 12 ]; then
        echo "  FAIL: resize repainted $n_render times (frames must be coalesced)"
        exit 1
    fi
    echo "  ok   resize repaints coalesced ($n_render frames)"
    python3 - "$WORK/resize.png" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
w, h = im.size
if (w, h) != (500, 400):
    print("  FAIL: surface is %dx%d, expected 500x400" % (w, h))
    sys.exit(1)
px = [im.getpixel((x, y)) for y in range(0, h, 4) for x in range(0, w, 4)]
dark = sum(1 for p in px if sum(p) < 150) / len(px)
print(("  ok   " if dark < 0.05 else "  FAIL ") +
      "resized content repainted (dark fraction %.2f)" % dark)
sys.exit(0 if dark < 0.05 else 1)
PY

    say "Motif flow: menu -> modal dialog -> menu again"
    # NEdit's File menu opens a modal file selection dialog; the menu has to
    # work again once the dialog is gone.  This is the reported symptom.
    cc -o "$BUILD/test_xm_flow" "$ROOT/tests/test_xm_flow.c" \
        -I"$PREFIX/include" -L"$PREFIX/lib" -lXm -lXt -lX11 \
        -Wl,-rpath,"$PREFIX/lib"
    INPUT="$ROOT/tests/flow.input" HC_TIMEOUT=12 \
        run_headless flow mwflow 500x400 "$WORK/flow.png" \
            "$BUILD/test_xm_flow"
    sed 's/^/  /' "$WORK/flow.client" || true
    grep -q '^MENU:OPEN ' "$WORK/flow.client" || {
        echo "  FAIL: File -> Open did not open the dialog"
        exit 1
    }
    grep -q '^FSB:SHOWN ' "$WORK/flow.client" || {
        echo "  FAIL: the dialog was not shown"
        exit 1
    }
    grep -q '^FSB:DONE ' "$WORK/flow.client" || {
        echo "  FAIL: OK did not complete the dialog"
        exit 1
    }
    grep -q '^MENU:QUIT ' "$WORK/flow.client" || {
        echo "  FAIL: the menu after the dialog never dropped down"
        exit 1
    }
    echo "  ok   menu worked again after the dialog was dismissed"

    say "modal dialog buttons (NEdit's File -> Exit shape)"
    # Motif's _XmGetPointVisibility() translates the widget's position to root
    # coordinates and requires the event's x_root/y_root to fall inside it.
    # Reporting top-level-relative root coordinates made every dialog button
    # arm and then do nothing.  NEdit centres dialogs on the pointer (XtNx/
    # XtNy), so the test positions its dialog off-origin too.
    cc -o "$BUILD/test_xm_msg" "$ROOT/tests/test_xm_msg.c" \
        -I"$PREFIX/include" -L"$PREFIX/lib" -lXm -lXt -lX11 \
        -Wl,-rpath,"$PREFIX/lib"
    printf 'sleep 2500\nmotion 52 81\nsleep 500\nbutton press left\nsleep 300\nbutton release left\nsleep 1500\n' \
        > "$WORK/msg.input"
    INPUT="$WORK/msg.input" HC_TIMEOUT=8 \
        run_headless msgdialog mwmsg 800x600 "$WORK/msg.png" \
            "$BUILD/test_xm_msg"
    sed 's/^/  /' "$WORK/msgdialog.client" || true
    grep -q '^RESULT:1$' "$WORK/msgdialog.client" || {
        echo "  FAIL: the dialog's OK button never fired"
        exit 1
    }
    echo "  ok   off-origin dialog button fired"

    say "keyboard input into a dialog's text field"
    cc -o "$BUILD/test_xm_kbd" "$ROOT/tests/test_xm_kbd.c" \
        -I"$PREFIX/include" -L"$PREFIX/lib" -lXm -lXt -lX11 \
        -Wl,-rpath,"$PREFIX/lib"
    printf 'sleep 1600\nmotion 134 54\nsleep 400\nbutton press left\nbutton release left\nsleep 600\nkey press 48\nkey release 48\nkey press 48\nkey release 48\nsleep 1500\n' \
        > "$WORK/kbd.input"
    # HC_SMALL captures the dialog; its text field is at rel=(11,35) 246x38.
    INPUT="$WORK/kbd.input" HC_SMALL=1 HC_TIMEOUT=8 \
        run_headless kbd mwkbd 800x600 "$WORK/kbd.png" "$BUILD/test_xm_kbd"
    sed 's/^/  /' "$WORK/kbd.client" || true
    grep -q '^DLG:' "$WORK/kbd.client" || {
        echo "  FAIL: typing never reached the dialog's text field"
        exit 1
    }
    python3 - "$WORK/kbd.png" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
# Sample the middle of the text field.  It must be the widget's light
# background, not black: a stippled fill (Motif draws the text cursor that way)
# used to paint the whole window with the GC foreground, turning every text
# field black and flickering as the cursor blinked.
got = im.getpixel((120, 50))
light = sum(got) > 450
print(("  ok   " if light else "  FAIL ") +
      "dialog text field is readable (bg %s)" % (got,))
sys.exit(0 if light else 1)
PY
    echo "  ok   dialog text field received typed characters"

    say "backspace in a dialog text field leaves nothing behind"
    # Motif places the text cursor and its erase boxes from XTextWidth(), so the
    # renderer must advance by the same width.  When it let cairo lay out the
    # string itself the two drifted apart and a deleted character stayed on
    # screen.  Type four characters, delete them all, and the field must be
    # empty apart from the cursor.
    {
        printf 'sleep 2500\nmotion 120 46\nsleep 400\nbutton press left\nbutton release left\nsleep 800\n'
        for k in 30 48 46 32; do printf 'key press %s\nkey release %s\nsleep 400\n' "$k" "$k"; done
        printf 'sleep 800\n'
        for _ in 1 2 3 4; do printf 'key press 14\nkey release 14\nsleep 900\n'; done
        printf 'sleep 2500\n'
    } > "$WORK/backspace.input"
    HC_SMALL=1 HC_TIMEOUT=12 \
        INPUT="$WORK/backspace.input" \
        run_headless backspace mwbs 800x600 "$WORK/backspace.png" "$BUILD/test_xm_kbd"
    python3 - "$WORK/backspace.png" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
# Look at the field interior, past the 2px border.  Four characters reach
# about x=57; with them deleted only the cursor remains, near x=30.  A
# character left behind by backspace would keep the rightmost ink well out.
cols = [x for x in range(16, 250)
        if any(sum(im.getpixel((x, y))) < 3 * 128 for y in range(41, 68))]
right = max(cols) if cols else 0
ok = right < 45
print(("  ok   " if ok else "  FAIL ") +
      "field empty after deleting all typed characters (ink ends at x=%d)" % right)
sys.exit(0 if ok else 1)
PY
fi

if [ -f "$PREFIX/lib/libXt.so" ] && [ -f "$ROOT/build/test_xt" ]; then
    say "headless render test (Xt Intrinsics)"
    run_headless xt mwxt 500x300 "$WORK/xt.png" "$ROOT/build/test_xt"
    grep -q 'captured' "$WORK/xt.err" && echo "  captured frame" || echo "  (skipped: no frame)"
fi

XV_BIN="${XV_BIN:-$MW_SRC/xv-build/src/xv}"
XV_PIC="${XV_PIC:-$MW_SRC/testpic.png}"
if [ -x "$XV_BIN" ] && [ -f "$XV_PIC" ]; then
    say "XV smoke test"
    run_headless xv mwxv 640x480 "$WORK/xv.png" "$XV_BIN" "$XV_PIC"
    python3 - "$WORK/xv.png" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
W = im.size[0]
row = [im.getpixel((x,120))[0] for x in range(0, min(W, 640), 4)]
inc = sum(1 for i in range(1, len(row)) if row[i] >= row[i-1])
print("  XV rendered a gradient: %d/%d monotonic red steps" % (inc, len(row)-1))
sys.exit(0 if inc > len(row) * 0.9 else 1)
PY
fi

NEDIT_BIN="${NEDIT_SRC:-$MW_SRC/nedit}/source/nedit"
if [ -x "$NEDIT_BIN" ]; then
    say "XDrawImageString erase extent (NEdit's main text cursor)"
    # Motif paints the editor text with XDrawImageString, which fills the
    # string's *pixel* extent with the background before drawing the glyphs and
    # is what erases the old text cursor.  Using the byte count as the width
    # under-filled badly, so typing left the cursor's remains behind as the
    # insertion point advanced.
    {
        printf 'sleep 3500\nmotion 300 100\nsleep 400\n'
        printf 'button press left\nbutton release left\nsleep 800\n'
        for _ in 1 2 3 4 5 6; do printf 'key press 30\nkey release 30\nsleep 400\n'; done
        printf 'sleep 2500\n'
    } > "$WORK/maintype.input"
    printf 'hello\nworld\n' > "$WORK/maintype.txt"
    HC_SMALL=1 HC_TIMEOUT=11 INPUT="$WORK/maintype.input" \
        run_headless maintype mwmt 1400x900 "$WORK/maintype.png" \
            "$NEDIT_BIN" "$WORK/maintype.txt"
    python3 - "$WORK/maintype.png" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
W, H = im.size
if (W, H) != (747, 397):
    print("  (skipped: unexpected main window size %dx%d)" % (W, H)); sys.exit(0)
# The typed line sits at cell rows 61..75 with glyphs in 64..69.  Anything dark
# on rows 61..63 is a leftover cursor bar (six characters produced ~19 px; a
# correct erase leaves none).
cols = [x for x in range(4, 120)
        if any(sum(im.getpixel((x, y))) < 3 * 128 for y in range(61, 64))]
# Group contiguous columns.  One group is the text cursor blinking (3px wide);
# leftovers from the old erase bug showed one group per typed character.
groups = 0
for i, x in enumerate(cols):
    if i == 0 or x != cols[i - 1] + 1:
        groups += 1
ok = groups <= 1
print(("  ok   " if ok else "  FAIL ") +
      "no cursor remains left behind (%d stray groups of %d px)" % (groups, len(cols)))
sys.exit(0 if ok else 1)
PY
fi

if [ -x "$NEDIT_BIN" ]; then
    say "mouse drag selects text (primary selection highlight)"
    # NEdit extends a drag with "Button1~Ctrl<MotionNotify>: extend_adjust()".
    # The shim built the pointer state mask as 1 << (button - 1), which for
    # button 1 is ShiftMask, so the motion events never carried Button1Mask and
    # the translation could not match: dragging only moved the insertion point
    # and selected nothing.  Button1Mask lives at 1 << 8.
    {
        printf 'sleep 3500\nmotion 10 36\nsleep 400\nbutton press left\nsleep 200\n'
        printf 'motion 30 36\nsleep 130\nmotion 50 36\nsleep 130\nmotion 80 36\nsleep 200\n'
        printf 'button release left\nsleep 2500\n'
    } > "$WORK/dragselect.input"
    printf 'hello\nworld\n' > "$WORK/dragselect.txt"
    HC_SMALL=1 HC_TIMEOUT=11 INPUT="$WORK/dragselect.input" \
        run_headless dragselect mwds 1400x900 "$WORK/dragselect.png" \
            "$NEDIT_BIN" "$WORK/dragselect.txt"
    python3 - "$WORK/dragselect.png" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
if im.size != (747, 397):
    print("  (skipped: unexpected main window size %dx%d)" % im.size); sys.exit(0)
# The drag covers part of the first text line (roughly y=28..46 in the window).
# NEdit's default selection background is rgb:cc/cc/cc against the widget's
# lighter grey, so a completed drag leaves a band of those pixels there.  A
# plain click paints none of them, and before the fix a drag painted none.
sel = sum(1 for y in range(28, 46) for x in range(0, 120)
          if im.getpixel((x, y)) == (204, 204, 204))
ok = sel > 100
print(("  ok   " if ok else "  FAIL ") +
      "drag highlighted the selection (%d selection-coloured pixels)" % sel)
sys.exit(0 if ok else 1)
PY

    say "Ctrl+key translations match the modifier map"
    # Xt turns an event's ControlMask back into keycodes through
    # XGetModifierMapping when it matches "Ctrl<Key>x".  The shim picked an
    # arbitrary keycode for the Control modifier (KP_Multiply), so no Ctrl+key
    # binding could ever fire.  NEdit binds select_all() to Ctrl+slash, so after
    # it the entire buffer must be highlighted.
    {
        printf 'sleep 3500\nmotion 200 36\nsleep 400\nbutton press left\nbutton release left\nsleep 500\n'
        printf 'key press 29\nsleep 80\nkey press 53\nkey release 53\nsleep 80\nkey release 29\nsleep 2500\n'
    } > "$WORK/ctrlall.input"
    printf 'hello\nworld\n' > "$WORK/ctrlall.txt"
    HC_SMALL=1 HC_TIMEOUT=11 INPUT="$WORK/ctrlall.input" \
        run_headless ctrlall mwca 1400x900 "$WORK/ctrlall.png" \
            "$NEDIT_BIN" "$WORK/ctrlall.txt"
    python3 - "$WORK/ctrlall.png" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('RGB')
if im.size != (747, 397):
    print("  (skipped: unexpected main window size %dx%d)" % im.size); sys.exit(0)
# Select All covers both text lines; a single-line selection reaches only ~500
# pixels, so requiring well over that proves the buffer-wide action ran.
sel = sum(1 for y in range(28, 62) for x in range(0, 120)
          if im.getpixel((x, y)) == (204, 204, 204))
ok = sel > 1500
print(("  ok   " if ok else "  FAIL ") +
      "Ctrl+slash selected the whole buffer (%d selection-coloured pixels)" % sel)
sys.exit(0 if ok else 1)
PY

    say "cut and paste round trip (clipboard lock and format-32 data)"
    # The full path: drag-select, Cut (Shift+Delete), then paste elsewhere.
    # Motif locks the clipboard through a root-window property whose record is
    # {Window windowId; long lockLevel;} -- format-32 data is a long array, and
    # it takes a timestamp by appending to a root property and waiting for the
    # resulting PropertyNotify.  Getting either wrong froze or aborted Cut.
    {
        printf 'sleep 3500\nmotion 10 36\nsleep 400\nbutton press left\nsleep 150\n'
        printf 'motion 30 36\nsleep 120\nmotion 60 36\nsleep 120\nmotion 85 36\nsleep 200\n'
        printf 'button release left\nsleep 800\n'
        printf 'key press 42\nsleep 100\nkey press 111\nkey release 111\nsleep 150\nkey release 42\nsleep 900\n'
        printf 'motion 250 48\nsleep 400\nbutton press left\nbutton release left\nsleep 400\n'
        printf 'key press 42\nsleep 100\nkey press 110\nkey release 110\nsleep 150\nkey release 42\nsleep 2200\n'
    } > "$WORK/cutpaste.input"
    printf 'hello\nworld\n' > "$WORK/cutpaste.txt"
    HC_SMALL=1 HC_TIMEOUT=13 INPUT="$WORK/cutpaste.input" \
        run_headless cutpaste mwcp 1400x900 "$WORK/cutpaste.png" \
            "$NEDIT_BIN" "$WORK/cutpaste.txt"
    python3 - "$WORK/cutpaste.png" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('L')
if im.size != (747, 397):
    print("  (skipped: unexpected main window size %dx%d)" % im.size); sys.exit(0)
def ink(y0, y1):
    cols = [x for x in range(0, 300)
            if any(im.getpixel((x, y)) < 128 for y in range(y0, y1))]
    return max(cols) if cols else -1
l1 = ink(28, 44)   # first text line
l2 = ink(44, 60)   # second text line
# Line 1 lost "ello" (its ink no longer reaches the five-glyph "hello" extent)
# and line 2 gained it, so the clipboard really carried the cut text across.
cut = l1 < 30
pasted = l2 > 65
print(("  ok   " if (cut and pasted) else "  FAIL ") +
      "cut text left line 1 and pasted into line 2 (ink ends at %d and %d)" % (l1, l2))
sys.exit(0 if (cut and pasted) else 1)
PY

    say "clipboard bridge: NEdit -> Wayland client"
    # The real application on the X side: select text, Cut (Shift+Delete), and a
    # plain Wayland client must be able to read it back off the compositor.
    {
        printf 'sleep 3500\nmotion 10 36\nsleep 400\nbutton press left\nsleep 150\n'
        printf 'motion 30 36\nsleep 120\nmotion 60 36\nsleep 120\nmotion 85 36\nsleep 200\n'
        printf 'button release left\nsleep 700\n'
        printf 'key press 42\nsleep 100\nkey press 111\nkey release 111\nsleep 150\nkey release 42\n'
        printf 'sleep 6000\n'
    } > "$WORK/neclip.input"
    printf 'hello\nworld\n' > "$WORK/neclip.txt"
    NESOCK="mwneclip$$"
    XDG_RUNTIME_DIR="$RUNTIME" "$BUILD/headless-compositor" \
        --socket "$NESOCK" --size 1400x900 --timeout 12 \
        --output "$WORK/neclip.png" --input "$WORK/neclip.input" \
        >"$WORK/neclip.ready" 2>"$WORK/neclip.hc" &
    NEHC=$!
    for _ in $(seq 1 100); do
        grep -q READY "$WORK/neclip.ready" 2>/dev/null && break; sleep 0.05
    done
    XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY="$NESOCK" \
        LD_LIBRARY_PATH="$PREFIX/lib:${LD_LIBRARY_PATH:-}" \
        "$NEDIT_BIN" "$WORK/neclip.txt" >"$WORK/neclip.ne" 2>&1 &
    NEPID=$!
    sleep 7
    XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY="$NESOCK" \
        LD_LIBRARY_PATH="$PREFIX/lib:${LD_LIBRARY_PATH:-}" \
        "$BUILD/clip_wl" receive >"$WORK/neclip.peer" 2>&1
    kill -9 "$NEPID" 2>/dev/null || true
    wait "$NEPID" 2>/dev/null || true
    wait "$NEHC" 2>/dev/null || true
    if grep -q 'GOT:ello' "$WORK/neclip.peer"; then
        echo "  ok   NEdit's cut reached a Wayland client ($(cat "$WORK/neclip.peer"))"
    else
        echo "  FAIL: Wayland client did not receive NEdit's selection"
        cat "$WORK/neclip.peer"
        exit 1
    fi

    say "clipboard bridge: NEdit pastes a Wayland clipboard it used to own"
    # The sequence that actually failed on a desktop: NEdit copies (so Motif
    # owns CLIPBOARD and libXt has a selection context for it), then another
    # application copies, then NEdit pastes.  libXt answers a paste locally
    # while it still believes it owns the selection, and it only learns
    # otherwise from a SelectionClear whose serial is not older than the one it
    # recorded -- ours carried serial 0, so Motif kept serving its own stale
    # clipboard and the Wayland text never arrived.
    {
        printf 'sleep 3500\nmotion 100 36\nsleep 300\nbutton press left\nbutton release left\nsleep 400\n'
        printf 'key press 29\nkey press 53\nkey release 53\nkey release 29\nsleep 600\n'
        printf 'key press 29\nkey press 46\nkey release 46\nkey release 29\nsleep 600\n'
        printf 'sleep 4500\n'
        printf 'motion 250 36\nsleep 400\nbutton press left\nbutton release left\nsleep 500\n'
        printf 'key press 42\nsleep 100\nkey press 110\nkey release 110\nsleep 150\nkey release 42\n'
        printf 'sleep 3000\n'
    } > "$WORK/nepaste.input"
    printf 'hello\nworld\n' > "$WORK/nepaste.txt"
    NPSOCK="mwnepaste$$"
    XDG_RUNTIME_DIR="$RUNTIME" "$BUILD/headless-compositor" \
        --socket "$NPSOCK" --size 1400x900 --timeout 16 \
        --output "$WORK/nepaste.png" --input "$WORK/nepaste.input" \
        >"$WORK/nepaste.ready" 2>"$WORK/nepaste.hc" &
    NPHC=$!
    for _ in $(seq 1 100); do
        grep -q READY "$WORK/nepaste.ready" 2>/dev/null && break; sleep 0.05
    done
    XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY="$NPSOCK" \
        LD_LIBRARY_PATH="$PREFIX/lib:${LD_LIBRARY_PATH:-}" \
        "$NEDIT_BIN" "$WORK/nepaste.txt" >"$WORK/nepaste.ne" 2>&1 &
    NPPID=$!
    sleep 6.5
    XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY="$NPSOCK" \
        LD_LIBRARY_PATH="$PREFIX/lib:${LD_LIBRARY_PATH:-}" \
        "$BUILD/clip_wl" offer 0123456789ABCDEFGHIJ >"$WORK/nepaste.wl" 2>&1
    wait "$NPPID" 2>/dev/null || true
    wait "$NPHC" 2>/dev/null || true
    python3 - "$WORK/nepaste.png" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert('L')
if im.size != (747, 397):
    print("  (skipped: unexpected main window size %dx%d)" % im.size); sys.exit(0)
cols = [x for x in range(0, 400)
        if any(im.getpixel((x, y)) < 128 for y in range(30, 46))]
right = max(cols) if cols else 0
ok = right > 150
print(("  ok   " if ok else "  FAIL ") +
      "NEdit pasted the Wayland clipboard (line 1 ink ends at x=%d)" % right)
sys.exit(0 if ok else 1)
PY

    say "menu accelerators fire (Ctrl+Z undo)"
    # Motif registers menu accelerators -- undo, select-all, ... -- with
    # XtGrabKey on the top manager, where its keyboard handler listens.  The
    # shim recorded the passive key grab but never honoured it, so the key went
    # to the focused text widget and its "<KeyPress>: self_insert()"
    # translation ran instead: Ctrl+Z did nothing and Ctrl+A typed an "a".
    printf 'line one\n' > "$WORK/undo.txt"
    for variant in typed undo; do
        {
            printf 'sleep 3500\nmotion 250 36\nsleep 400\nbutton press left\nbutton release left\nsleep 500\n'
            printf 'key press 30\nkey release 30\nsleep 300\n'
            printf 'key press 48\nkey release 48\nsleep 300\n'
            printf 'key press 46\nkey release 46\nsleep 800\n'
            if [ "$variant" = undo ]; then
                printf 'key press 29\nkey press 44\nkey release 44\nkey release 29\n'
            fi
            printf 'sleep 2000\n'
        } > "$WORK/undo-$variant.input"
        HC_SMALL=1 HC_TIMEOUT=11 INPUT="$WORK/undo-$variant.input" \
            run_headless "undo$variant" "mwundo$variant" 1400x900 \
                "$WORK/undo-$variant.png" "$NEDIT_BIN" "$WORK/undo.txt"
    done
    python3 - "$WORK/undo-typed.png" "$WORK/undo-undo.png" <<'PY'
import sys
from PIL import Image
a = Image.open(sys.argv[1]).convert('L')
b = Image.open(sys.argv[2]).convert('L')
if a.size != (747, 397) or b.size != (747, 397):
    print("  (skipped: unexpected main window size %dx%d)" % a.size); sys.exit(0)
def ink(im):
    cols = [x for x in range(0, 400)
            if any(im.getpixel((x, y)) < 128 for y in range(30, 46))]
    return max(cols) if cols else 0
typed, undone = ink(a), ink(b)
ok = typed > 95 and undone < typed - 15
print(("  ok   " if ok else "  FAIL ") +
      "Ctrl+Z undid the last insert (line 1 ink %d -> %d)" % (typed, undone))
sys.exit(0 if ok else 1)
PY
fi

say "xev reports the events it selects"
# xev exists to print every event it receives, so it is a direct check that the
# shim delivers what a real X server would for a map/enter/motion/button/key
# sequence -- including the KeymapNotify that X follows a FocusIn with, and the
# LeaveNotify when the pointer leaves.
if [ -x "$PREFIX/bin/xev" ]; then
    printf 'sleep 1500\nmotion 90 90\nsleep 300\nbutton press left\nsleep 150\nbutton release left\nsleep 200\nkey press 38\nsleep 120\nkey release 38\nsleep 200\nmotion 380 280\nsleep 300\nmotion 20 20\nsleep 1200\n' > "$WORK/xev.input"
    INPUT="$WORK/xev.input" HC_TIMEOUT=8 \
        run_headless xev mwxev 400x300 "$WORK/xev.png" "$PREFIX/bin/xev"
    missing=
    for e in Expose PropertyNotify FocusIn KeymapNotify EnterNotify LeaveNotify \
             MotionNotify ButtonPress ButtonRelease KeyPress KeyRelease; do
        grep -q "^$e event" "$WORK/xev.client" || missing="$missing $e"
    done
    if [ -z "$missing" ]; then
        echo "  ok   expose, focus, keymap, crossing, motion, button and key events"
    else
        echo "  FAIL: xev never saw:$missing"
        sed 's/^/  /' "$WORK/xev.client" | head -20
        exit 1
    fi
else
    echo "  (skipped: xev not installed)"
fi

say "the mouse wheel maps to buttons 4/5 and 6/7"
# Wayland delivers scroll on wl_pointer.axis; X clients expect the wheel
# buttons instead (4/5 vertical, 6/7 horizontal).  The shim used to ignore the
# sign, so every scroll went one way.  tests/wheel.input sends two detents
# down, one up and one right.
cc -o "$BUILD/test_wheel" "$ROOT/tests/test_wheel.c" \
    -I"$PREFIX/include" -L"$PREFIX/lib" -lX11 \
    -Wl,-rpath,"$PREFIX/lib" ${CFLAGS:-}
INPUT="$ROOT/tests/wheel.input" HC_TIMEOUT=6 \
    run_headless wheel mwwheel 400x300 "$WORK/wheel.png" "$BUILD/test_wheel"
if grep -q 'all checks passed' "$WORK/wheel.client"; then
    echo "  ok   down/up/right became buttons 5/4/7"
else
    echo "  FAIL: wheel buttons wrong"
    sed 's/^/  /' "$WORK/wheel.client" | head
    exit 1
fi

say "key repeat is paced by the fd the client waits on"
# libXt waits in select() on ConnectionNumber(dpy) -- the macro, which reads
# dpy->fd -- and only then calls into Xlib.  The shim points dpy->fd at a pipe
# its helper thread signals at each repeat deadline; without that a held key
# only repeated when unrelated Wayland traffic arrived (2-9 repeats instead of
# the ~33 a 25/s rate over the 1.5s hold implies).
cc -o "$BUILD/test_repeat" "$ROOT/tests/test_repeat.c" \
    -I"$PREFIX/include" -L"$PREFIX/lib" -lX11 \
    -Wl,-rpath,"$PREFIX/lib" ${CFLAGS:-}
INPUT="$ROOT/tests/repeat.input" HC_TIMEOUT=6 \
    run_headless repeat mwrepeat 400x300 "$WORK/repeat.png" "$BUILD/test_repeat"
if grep -q 'all checks passed' "$WORK/repeat.client"; then
    echo "  ok   $(cat "$WORK/repeat.client")"
else
    echo "  FAIL: key repeat not paced"
    sed 's/^/  /' "$WORK/repeat.client" | head
    exit 1
fi

say "xdpyinfo queries the shim"
# xdpyinfo drives the display/screen/visual and extension queries through
# libX11 (and pulls in libXtst for XTest), so it is the broadest API query
# client we run.  It opens no window: check the report is complete instead.
XDPY="$PREFIX/bin/xdpyinfo"; [ -x "$XDPY" ] || XDPY="$(command -v xdpyinfo)"
if [ -n "$XDPY" ] && [ -x "$XDPY" ]; then
    run_headless xdpyinfo mwxdpy 640x480 "$WORK/xdpyinfo.png" "$XDPY"
    missing=
    for line in "version number" "vendor string:" "maximum request size" \
                "motion buffer size" "keycode range" "number of extensions" \
                "default screen number" "number of screens" "dimensions" \
                "depth of root window" "default visual id" \
                "red, green, blue masks"; do
        grep -q "$line" "$WORK/xdpyinfo.client" || missing="$missing '$line'"
    done
    if [ -z "$missing" ]; then
        echo "  ok   display, screen, visual and extension queries all answered"
    else
        echo "  FAIL: xdpyinfo output lacked:$missing"
        sed 's/^/  /' "$WORK/xdpyinfo.client" | head -20
        exit 1
    fi
else
    echo "  (skipped: xdpyinfo not installed)"
fi

say "xinput enumerates the (virtual) input devices"
# xinput drives the XInput2 query path through libXi: extension version,
# XIQueryVersion and XIQueryDevice.  The shim answers those from a synthetic
# device set (a master pointer/keyboard pair plus XTEST slaves).
XINPUT_BIN="$PREFIX/bin/xinput"; [ -x "$XINPUT_BIN" ] || XINPUT_BIN="$(command -v xinput)"
if [ -n "$XINPUT_BIN" ] && [ -x "$XINPUT_BIN" ]; then
    run_headless xinput mwxi 400x300 "$WORK/xinput.png" "$XINPUT_BIN" list
    if grep -q 'Virtual core' "$WORK/xinput.client"; then
        echo "  ok   XIQueryVersion/XIQueryDevice answered (devices listed)"
    else
        echo "  FAIL: xinput could not enumerate the devices"
        sed 's/^/  /' "$WORK/xinput.client" | head -10
        exit 1
    fi
else
    echo "  (skipped: xinput not installed)"
fi

say "setxkbmap reads the layout names"
# setxkbmap opens the display through XkbOpenDisplay and reads the layout from
# the _XKB_RULES_NAMES root property, so it exercises both the XKB entry point
# and the property the shim publishes there.
SETXKB="$(command -v setxkbmap)"
if [ -n "$SETXKB" ] && [ -x "$SETXKB" ]; then
    run_headless setxkbmap mwskb 400x300 "$WORK/setxkbmap.png" "$SETXKB" -query
    missing=
    for line in "rules:" "model:" "layout:"; do
        grep -q "$line" "$WORK/setxkbmap.client" || missing="$missing '$line'"
    done
    if [ -z "$missing" ]; then
        echo "  ok   rules/model/layout read back from _XKB_RULES_NAMES"
    else
        echo "  FAIL: setxkbmap output lacked:$missing"
        sed 's/^/  /' "$WORK/setxkbmap.client" | head
        exit 1
    fi
else
    echo "  (skipped: setxkbmap not installed)"
fi

say "real X11 client matrix"
# Running actual X11 programs is the most effective verification we have; see
# scripts/run-x11-clients.sh for why a conformance suite does not apply here.
# Known gaps (xterm/XKB, xdpyinfo/async, ...) are reported but do not fail.
MW_BUILD="$BUILD" MW_PREFIX="$PREFIX" MW_RUNTIME="$RUNTIME" \
    "$HERE/run-x11-clients.sh" || exit 1

say "all tests passed"
