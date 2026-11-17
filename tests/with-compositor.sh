#!/usr/bin/env bash
# with-compositor.sh COMPOSITOR TEST [ARGS...]
# Run TEST against a private headless compositor, so `meson test` never talks
# to the desktop session's compositor (whatever WAYLAND_DISPLAY points at).
set -u
hc="$1"; shift
rt="$(mktemp -d)"
chmod 700 "$rt"
sock="mwtest$$"
XDG_RUNTIME_DIR="$rt" "$hc" --socket "$sock" --size 640x480 --timeout 60 \
    --output "$rt/frame.png" >"$rt/ready" 2>"$rt/hc.err" &
hcpid=$!
for _ in $(seq 1 100); do
    grep -q READY "$rt/ready" 2>/dev/null && break
    sleep 0.05
done
XDG_RUNTIME_DIR="$rt" WAYLAND_DISPLAY="$sock" "$@"
rc=$?
kill "$hcpid" 2>/dev/null
wait "$hcpid" 2>/dev/null
rm -rf "$rt"
exit "$rc"
