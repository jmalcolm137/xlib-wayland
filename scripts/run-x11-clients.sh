#!/usr/bin/env bash
#
# run-x11-clients.sh — run real X11 programs against the shim.
#
# There is no drop-in third-party conformance suite for an in-process libX11:
# the X Test Suite validates the X wire protocol (which this shim does not
# speak), and libX11 ships no tests of its own.  The effective substitute is to
# run actual, widely used X11 clients and see what they need.  In practice this
# surfaces missing symbols, ABI gaps and memory bugs far faster than any hand
# written test -- xterm alone found two missing exports in seconds, and
# xmessage found an XIM ownership bug that aborted every libXaw text client.
#
# Each client runs under the headless compositor with our libX11 first on the
# library path and is classified as:
#
#   ok      started and painted (more than one colour in the captured frame)
#   ran     started and exited without a symbol error, but painted nothing
#   gap     could not start: it needs a libX11 symbol we do not export yet
#   crash   died on a signal (a shim defect, not an unimplemented feature)
#
# The EXPECTED column is a floor, not an equality: a client listed as `ok` must
# at least run, so a regression fails the script; a client listed as `gap` is a
# known, tracked limitation and may be anything except a crash.  When a gap
# closes it shows up as `ok` here with no edit needed.  A crash always fails.
#
# Usage: run-x11-clients.sh [client ...]   (default: every client below)
#
# Environment:
#   MW_BUILD    shim build directory   (default: <repo>/build)
#   MW_PREFIX   installed stack        (default: $HOME/.local/motif-wayland)
#   MW_RUNTIME  compositor runtime dir (default: ${TMPDIR:-/tmp}/xlib-wayland-runtime)
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BUILD="${MW_BUILD:-$ROOT/build}"
PREFIX="${MW_PREFIX:-$HOME/.local/motif-wayland}"
RUNTIME="${MW_RUNTIME:-${TMPDIR:-/tmp}/xlib-wayland-runtime}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# name|expected|arguments.  Keep in rough order of how much API they exercise.
# xclock/xlogo/xload/xcalc come from the xorg-apps packages; when they are not
# installed system-wide, unpacking them under $MW_PREFIX/bin is enough (see
# README).  Clients found there are picked up automatically.
CLIENTS=(
    "xmessage|ok|-geometry 300x120 x11-client-matrix"
    "xlogo|ok|"
    "xload|ok|"
    "xcalc|ok|"
    "xclock|ok|"
    "xman|ok|"
    "xedit|ok|"
    "xev|ran|"
    "xdpyinfo|ran|"
    "xterm|ok|-geometry 80x24"
    "xinput|gap|list"
    "setxkbmap|gap|-query"
)

[ -x "$BUILD/headless-compositor" ] || { echo "build the shim first ($BUILD)"; exit 1; }
mkdir -p "$RUNTIME"; chmod 700 "$RUNTIME"
# Clients installed alongside the stack under $MW_PREFIX are still X11 clients
# of interest, so look there as well as on the system PATH.
PATH="$PREFIX/bin:$PATH"

# Only run the clients named on the command line, if any were given.
want=("$@")
selected() {
    [ "${#want[@]}" -eq 0 ] && return 0
    local c; for c in "${want[@]}"; do [ "$c" = "$1" ] && return 0; done
    return 1
}

frame_colours() { # png -> "N W H" (empty when there is no frame)
    python3 - "$1" 2>/dev/null <<'PY'
import sys
from PIL import Image
try:
    im = Image.open(sys.argv[1]).convert('RGB')
except Exception:
    print(""); raise SystemExit
print(len(set(im.getdata())), im.size[0], im.size[1])
PY
}

printf '%-12s %-6s %s\n' CLIENT RESULT DETAIL
fail=0; n_ok=0; n_gap=0; n_crash=0; n_skip=0
for entry in "${CLIENTS[@]}"; do
    IFS='|' read -r name expect args <<<"$entry"
    selected "$name" || continue
    command -v "$name" >/dev/null 2>&1 || {
        n_skip=$((n_skip+1)); printf '%-12s %-6s %s\n' "$name" skip "not installed"; continue; }

    sock="mwxc$$_${name//[^A-Za-z0-9]/}"
    XDG_RUNTIME_DIR="$RUNTIME" "$BUILD/headless-compositor" \
        --socket "$sock" --size 640x400 --timeout 6 \
        --output "$WORK/$name.png" >"$WORK/$name.ready" 2>"$WORK/$name.hc" &
    hc=$!
    for _ in $(seq 1 200); do
        grep -q READY "$WORK/$name.ready" 2>/dev/null && break; sleep 0.05
    done
    # shellcheck disable=SC2086
    ( XDG_RUNTIME_DIR="$RUNTIME" WAYLAND_DISPLAY="$sock" \
      LD_LIBRARY_PATH="$PREFIX/lib:${LD_LIBRARY_PATH:-}" \
      timeout 7 "$name" $args ) >"$WORK/$name.out" 2>&1
    rc=$?
    wait "$hc" 2>/dev/null

    colours="$(frame_colours "$WORK/$name.png")"
    npix="${colours%% *}"
    missing="$(grep -oE 'undefined symbol: [A-Za-z_0-9]+' "$WORK/$name.out" \
               | head -1 | awk '{print $3}')"
    detail=""
    if [ "$rc" -ge 128 ] && [ "$rc" -ne 124 ]; then
        result=crash; n_crash=$((n_crash+1))
        detail="$(grep -m1 -iE 'free\(\)|segmentation|assert|abort' "$WORK/$name.out" | head -c 60)"
        [ -n "$detail" ] || detail="signal $((rc-128))"
    elif [ -n "$missing" ]; then
        result=gap; n_gap=$((n_gap+1)); detail="needs $missing"
    elif [ -n "$npix" ] && [ "$npix" -gt 1 ]; then
        result=ok; n_ok=$((n_ok+1)); detail="painted ${colours#* } (${npix} colours)"
    else
        result=ran; detail="no frame painted"
    fi
    printf '%-12s %-6s %s\n' "$name" "$result" "$detail"

    # A crash is always a shim defect; a client we claim works must not
    # regress.  "ran" only requires that it started without a missing symbol.
    if [ "$result" = crash ]; then
        fail=1
    elif [ "$expect" = ok ] && [ "$result" != ok ]; then
        echo "  ^ expected $name to run and paint; it did not" >&2
        fail=1
    elif [ "$expect" = ran ] && [ "$result" = gap ]; then
        echo "  ^ expected $name to start; a symbol is missing" >&2
        fail=1
    fi
done

echo
echo "ran: $n_ok   known gaps: $n_gap   crashes: $n_crash   skipped: $n_skip"
if [ "$n_ok" -eq 0 ] && [ "$n_gap" -eq 0 ]; then
    echo "(no X11 clients available to test)"
fi
exit "$fail"
