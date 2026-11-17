#!/usr/bin/env bash
# with-compositor.sh COMPOSITOR TEST [ARGS...]
# Run TEST against a private headless compositor, so `meson test` never talks
# to the desktop session's compositor (whatever WAYLAND_DISPLAY points at),
# and in an environment of its own: the user's resource files and the
# XLIB_WAYLAND_* sharing settings must not change what a test sees.
set -u
hc="$1"; shift
rt="$(mktemp -d)" || exit 1
chmod 700 "$rt"
hcpid=
cleanup() {
    [ -n "$hcpid" ] && kill "$hcpid" 2>/dev/null && wait "$hcpid" 2>/dev/null
    rm -rf "$rt"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

sock="mwtest$$"
mkdir -p "$rt/home"
# A long compositor timeout: the trap stops it when the test is done.
XDG_RUNTIME_DIR="$rt" "$hc" --socket "$sock" --size 640x480 --timeout 600 \
    --output "$rt/frame.png" >"$rt/ready" 2>"$rt/hc.err" &
hcpid=$!
for _ in $(seq 1 100); do
    grep -q READY "$rt/ready" 2>/dev/null && break
    kill -0 "$hcpid" 2>/dev/null || break
    sleep 0.05
done
if ! grep -q READY "$rt/ready" 2>/dev/null || ! kill -0 "$hcpid" 2>/dev/null; then
    echo "with-compositor: the headless compositor did not start" >&2
    cat "$rt/hc.err" >&2
    exit 1
fi

# Under the wrapper a missing display is a failure, not a skip.
env -u XENVIRONMENT -u XLIB_WAYLAND_SHARE_PROPERTIES \
    -u XLIB_WAYLAND_SHARE_SELECTIONS -u XLIB_WAYLAND_SESSION \
    -u XLIB_WAYLAND_XSETTINGS -u XLIB_WAYLAND_ATOMS -u XLIB_WAYLAND_KEYBOARD \
    -u DISPLAY \
    HOME="$rt/home" XDG_CONFIG_HOME="$rt/home/.config" \
    XDG_RUNTIME_DIR="$rt" WAYLAND_DISPLAY="$sock" MW_TEST_REQUIRE_DISPLAY=1 \
    "$@"
