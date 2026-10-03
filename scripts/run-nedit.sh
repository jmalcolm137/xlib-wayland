#!/usr/bin/env bash
#
# run-nedit.sh — launch NEdit 5.7 on the *live* Wayland session.
#
# NEdit links the whole Motif stack from $MW_PREFIX (RUNPATH is baked into the
# binary, so no LD_LIBRARY_PATH is required).  The shim connects to the
# compositor named by $WAYLAND_DISPLAY, exactly like any other Wayland client.
#
# This is the manual test for the menu bug: open a menu, move the pointer over
# an item, and click.  Items must highlight on Enter (XmMenuButton arming) and
# fire on ButtonPress — previously the menu posted but nothing was selectable.
#
# ---------------------------------------------------------------------------
# Environment:
#   MW_PREFIX     install prefix                (default: $HOME/.local/motif-wayland)
#   NEDIT_SRC     NEdit source/build tree       (default: /tmp/opencode/src/nedit)
#   WAYLAND_DISPLAY  compositor to connect to   (default: whatever your session uses)
#   MW_TRACE      set to 1 for shim tracing
# ---------------------------------------------------------------------------
set -euo pipefail

MW_PREFIX="${MW_PREFIX:-$HOME/.local/motif-wayland}"
NEDIT_SRC="${NEDIT_SRC:-/tmp/opencode/src/nedit}"

die() { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

NEDIT_BIN="$NEDIT_SRC/source/nedit"
[ -x "$NEDIT_BIN" ] || die "$NEDIT_BIN not found — run scripts/build-nedit.sh first."
[ -f "$MW_PREFIX/lib/libXm.so.4" ] || die "$MW_PREFIX/lib/libXm.so.4 missing — run scripts/build-stack.sh first."

printf '==>\033[0m nedit\n'
printf '    binary : %s\n' "$NEDIT_BIN"
printf '    prefix : %s\n' "$MW_PREFIX"
printf '    socket : %s\n' "${WAYLAND_DISPLAY:-<default>}"

exec "$NEDIT_BIN" "$@"