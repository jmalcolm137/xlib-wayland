#!/usr/bin/env bash
#
# run-motif-demos.sh — smoke-test the shim against Open Motif's own demos.
#
# Open Motif ships no formal test suite, but demos/programs/ is a broad
# functional corpus (menus, dialogs, file selection, text, drawing, drag and
# drop, ...).  This builds them against the installed stack and runs each one
# under the headless compositor, reporting whether it mapped a window, painted
# something, and avoided a real crash.
#
# A process that merely exits with "XIO: fatal IO error: Wayland connection
# closed" is not a failure: that is the test compositor timing out.
#
# ---------------------------------------------------------------------------
# Environment:
#   MW_SRC      reference source trees (default: ${TMPDIR:-/tmp}/xlib-wayland)
#   MOTIF_SRC   Motif source tree      (default: $MW_SRC/motif)
#   MW_PREFIX   installed stack       (default: $HOME/.local/motif-wayland)
#   MW_BUILD    shim build directory  (default: <repo>/build)
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
MW_SRC="${MW_SRC:-${TMPDIR:-/tmp}/xlib-wayland}"
MOTIF_SRC="${MOTIF_SRC:-$MW_SRC/motif}"
MW_PREFIX="${MW_PREFIX:-$HOME/.local/motif-wayland}"
BUILD="${MW_BUILD:-$ROOT/build}"
RUNTIME="${MW_RUNTIME:-${TMPDIR:-/tmp}/xlib-wayland-runtime}"
DEMOS="$MOTIF_SRC/demos/programs"

[ -d "$DEMOS" ] || { echo "no demos at $DEMOS (set MOTIF_SRC)"; exit 1; }
[ -x "$BUILD/headless-compositor" ] || { echo "build the shim first"; exit 1; }
mkdir -p "$RUNTIME"; chmod 700 "$RUNTIME"
export XDG_RUNTIME_DIR="$RUNTIME"

if [ "${SKIP_BUILD:-0}" != 1 ]; then
    echo "==> building demos"
    for d in "$DEMOS"/*/; do
        [ -f "$d/Makefile" ] || continue
        make -C "$d" >/dev/null 2>&1 || true
    done
fi

painted=0; blank=0; crashed=0
printf '%-16s %-8s %s\n' DEMO RESULT NOTES
for d in "$DEMOS"/*/; do
    name="$(basename "$d")"
    bin=$(find "$d" -maxdepth 2 -type f -perm -u+x \
          ! -name '*.sh' ! -name '*.c' ! -name '*.man' ! -name '*.ad' \
          ! -name '*.uid' ! -name '*.uil' ! -name '*.xbm' ! -name '*.xpm' \
          2>/dev/null | head -1)
    [ -n "$bin" ] || continue
    WORK="$(mktemp -d)"; SOCK="mwdemo$$_${name//[^A-Za-z0-9]/}"
    timeout 30 "$BUILD/headless-compositor" --socket "$SOCK" --size 1000x700 \
        --timeout 5 --output "$WORK/o.png" >"$WORK/r" 2>"$WORK/e" &
    HC=$!
    for _ in $(seq 1 120); do grep -q READY "$WORK/r" 2>/dev/null && break; sleep 0.05; done
    # Demos expect to be started from their own directory with XAPPLRESDIR=. so
    # they can find their local resource and UIL files (hellomotif exits with
    # "can't open hierarchy" without it).
    ( cd "$d" && WAYLAND_DISPLAY="$SOCK" XAPPLRESDIR=. \
        LD_LIBRARY_PATH="$MW_PREFIX/lib" timeout 12 "$bin" ) >"$WORK/out" 2>&1
    arc=$?
    wait $HC 2>/dev/null

    res=$(python3 - "$WORK/o.png" 2>/dev/null <<'PY'
import sys
from PIL import Image
try:
    im = Image.open(sys.argv[1]).convert('RGB')
except Exception:
    print("none"); raise SystemExit
px = list(im.getdata())
print("painted" if len(set(px[:: max(1, len(px)//4000)])) > 1 else "blank")
PY
)
    note=$(grep -vE 'XIO: fatal IO error: Wayland connection closed' "$WORK/out" \
           | grep -m1 -iE 'segmentation|abort|assert|Error:|undefined' | head -c 70)
    if [ "$arc" -ge 128 ] || printf '%s' "$note" | grep -qiE 'segmentation|abort'; then
        crashed=$((crashed+1)); printf '%-16s %-8s %s\n' "$name" CRASH "$note"
    elif [ "$res" = painted ]; then
        painted=$((painted+1)); printf '%-16s %-8s %s\n' "$name" ok "$note"
    else
        blank=$((blank+1)); printf '%-16s %-8s %s\n' "$name" blank "$note"
    fi
    rm -rf "$WORK"
done

echo
echo "painted: $painted   blank: $blank   crashed: $crashed"
[ "$crashed" -eq 0 ]
