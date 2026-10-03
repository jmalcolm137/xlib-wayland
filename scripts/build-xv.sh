#!/usr/bin/env bash
#
# build-xv.sh — configure and build XV 6.2 with CMake against the custom
# Motif/Wayland prefix ($MW_PREFIX).
#
# IMPORTANT: CMake's `find_package(X11)` must resolve to *our* prefix, not the
# system X11.  We pass -DCMAKE_PREFIX_PATH="$MW_PREFIX" (plus include/library
# hints) so that the X11 and Xt that XV links against are the shim's libX11 and
# the libXt we built in build-stack.sh, not /usr/lib.  If configure output ever
# reports /usr/include or /usr/lib for X11/Xt, fix the hints before building.
#
# Environment:
#   MW_PREFIX   install prefix          (default: $HOME/.local/motif-wayland)
#   JOBS        parallel build jobs     (default: nproc)
#   MW_SRC      reference source trees  (default: ${TMPDIR:-/tmp}/xlib-wayland)
#   XV_SRC      path to XV source       (default: $MW_SRC/xv)
#
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
PROJECT_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd -P)"

MW_PREFIX="${MW_PREFIX:-$HOME/.local/motif-wayland}"
MW_SRC="${MW_SRC:-${TMPDIR:-/tmp}/xlib-wayland}"
XV_SRC="${XV_SRC:-$MW_SRC/xv}"

if [ -n "${JOBS:-}" ]; then
    :
elif command -v nproc >/dev/null 2>&1; then
    JOBS="$(nproc)"
else
    JOBS=4
fi

log()  { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || \
        die "required tool '$1' was not found in PATH. Install it and retry."
}

# ---------------------------------------------------------------------------
# argument parsing
# ---------------------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --prefix=*)  MW_PREFIX="${1#*=}" ;;
        --prefix)    MW_PREFIX="${2:?--prefix needs an argument}"; shift ;;
        -h|--help)
            cat <<EOF
Usage: $(basename -- "$0") [--prefix=DIR]

Configure and build XV with CMake against \$MW_PREFIX.

Environment: MW_PREFIX, JOBS, XV_SRC
EOF
            exit 0 ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
    shift
done

export MW_PREFIX

log "XV build"
printf '    project   : %s\n' "$PROJECT_ROOT"
printf '    prefix    : %s\n' "$MW_PREFIX"
printf '    XV source : %s\n' "$XV_SRC"
printf '    jobs      : %s\n' "$JOBS"

require_cmd cmake
[ -f "$XV_SRC/CMakeLists.txt" ] || \
    die "no CMakeLists.txt in $XV_SRC — is XV_SRC correct?"

# XV depends on libXt (built by build-stack.sh) and the shim's libX11.
if [ ! -e "$MW_PREFIX/lib/libXt.so" ] && [ ! -L "$MW_PREFIX/lib/libXt.so" ]; then
    warn "$MW_PREFIX/lib/libXt.so not found — run scripts/build-stack.sh first,"
    warn "otherwise CMake may pick up the system libXt instead of ours."
fi

BUILD_DIR="$MW_PREFIX/build/xv"

# Point pkg-config at our prefix so any pkg-config based lookups see it too.
export PKG_CONFIG_PATH="$MW_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

log "XV: cmake configure -> $BUILD_DIR"
cmake -S "$XV_SRC" -B "$BUILD_DIR" \
    -DCMAKE_PREFIX_PATH="$MW_PREFIX" \
    -DCMAKE_INCLUDE_PATH="$MW_PREFIX/include" \
    -DCMAKE_LIBRARY_PATH="$MW_PREFIX/lib" \
    -DCMAKE_BUILD_TYPE=Release

log "XV: cmake --build -j$JOBS"
cmake --build "$BUILD_DIR" -j"$JOBS"

log "XV build complete: $BUILD_DIR/src/xv"
