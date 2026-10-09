#!/usr/bin/env bash
#
# run-firefox-headless.sh — run Firefox 157, built against the shim, under the
# bundled headless Wayland compositor with no X server and no Xwayland.
#
# Firefox is forced onto the X11 toolkit (GDK_BACKEND=x11,
# MOZ_ENABLE_WAYLAND=0) and is built with
# --enable-default-toolkit=cairo-gtk3-x11-only; the shim turns its Xlib calls
# into Wayland surfaces.  The compositor runs for a fixed time and writes a PNG
# of what Firefox presented.  See docs/FIREFOX-STATUS.md and
# scripts/firefox-mozconfig.
#
# Usage: scripts/run-firefox-headless.sh [size] [timeout] [-- firefox args...]
#
# Environment:
#   SHIM_PREFIX  installed shim        (default: $HOME/.local/xlib-wayland)
#   FF_SRC       Firefox source tree   (default: <repo>/../src/firefox-157.0.1)
#   FF_BIN       Firefox binary        (default: $FF_SRC/obj-x11/dist/bin/firefox)
#   COMPOSITOR   headless compositor   (default: <repo>/build/headless-compositor)
#   WORK         scratch dir           (default: mktemp -d)
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SHIM_PREFIX="${SHIM_PREFIX:-$HOME/.local/xlib-wayland}"
FF_SRC="${FF_SRC:-$ROOT/../src/firefox-157.0.1}"
FF_BIN="${FF_BIN:-$FF_SRC/obj-x11/dist/bin/firefox}"
COMPOSITOR="${COMPOSITOR:-$ROOT/build/headless-compositor}"
SIZE="${1:-1024x768}"; shift || true
TIMEOUT="${1:-30}"; shift || true
[ "${1:-}" = "--" ] && shift || true

[ -x "$FF_BIN" ] || { echo "no Firefox binary at $FF_BIN" >&2; exit 1; }
[ -x "$COMPOSITOR" ] || { echo "no compositor at $COMPOSITOR" >&2; exit 1; }

WORK="${WORK:-$(mktemp -d /tmp/opencode/ff-run.XXXXXX)}"
RUNTIME="$WORK/runtime"; mkdir -p "$RUNTIME"; chmod 700 "$RUNTIME"
PROFILE="$WORK/profile"; mkdir -p "$PROFILE"
SOCK="ff$(date +%s%N | tail -c 6)"
OUT="$WORK/screen.png"

cat > "$PROFILE/user.js" <<'EOF'
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("browser.aboutwelcome.enabled", false);
user_pref("datareporting.policy.dataSubmissionEnabled", false);
user_pref("toolkit.telemetry.enabled", false);
user_pref("toolkit.telemetry.reportingpolicy.firstRun", false);
user_pref("browser.newtabpage.enabled", false);
user_pref("browser.startup.page", 0);
user_pref("gfx.webrender.software", true);
user_pref("gfx.canvas.accelerated", false);
user_pref("layers.acceleration.disabled", true);
EOF

echo "==> compositor $SIZE socket=$SOCK work=$WORK"
XDG_RUNTIME_DIR="$RUNTIME" "$COMPOSITOR" --socket "$SOCK" --size "$SIZE" \
    --timeout "$TIMEOUT" --output "$OUT" >"$WORK/hc.out" 2>"$WORK/hc.err" &
HC=$!
for _ in $(seq 1 400); do grep -q READY "$WORK/hc.out" 2>/dev/null && break; sleep 0.05; done

# No X11 transport: the shim does the real I/O over Wayland and never opens an
# X socket.  But Firefox's X11-only startup path reads $DISPLAY and bails if it
# is unset, so hand it a token that cannot resolve to a real server (no
# Xwayland, no /tmp/.X11-unix socket); the shim uses it only as an XID key.
unset DISPLAY
export DISPLAY=":99"
export XDG_RUNTIME_DIR="$RUNTIME"
export WAYLAND_DISPLAY="$SOCK"
export GDK_BACKEND=x11
export MOZ_ENABLE_WAYLAND=0
export MOZ_X11_EGL=0
export LIBGL_ALWAYS_SOFTWARE=1
export LD_LIBRARY_PATH="$SHIM_PREFIX/lib:${LD_LIBRARY_PATH:-}"

echo "==> launching $FF_BIN"
"$FF_BIN" -no-remote -profile "$PROFILE" "$@" \
    >"$WORK/firefox.out" 2>"$WORK/firefox.err" &
FF=$!

wait "$HC" 2>/dev/null
echo "==> compositor done: $(tail -3 "$WORK/hc.err" 2>/dev/null | tr '\n' ' ')"
kill -TERM "$FF" 2>/dev/null
sleep 1
kill -KILL "$FF" 2>/dev/null
wait "$FF" 2>/dev/null
echo "==> screenshot: $OUT"
echo "==> work dir: $WORK"
echo "==> firefox stderr tail:"
tail -20 "$WORK/firefox.err" 2>/dev/null | sed 's/^/    /'
