#!/usr/bin/env bash
#
# build-nedit.sh — build NEdit 5.7 unmodified against the custom Motif/Wayland
# prefix ($MW_PREFIX).
#
# NEdit has no configure/CMake; it ships hand-written system Makefiles under
# makefiles/.  makefiles/Makefile.linux hardcodes /usr/X11R6 include and
# library paths, so we override CFLAGS and LIBS on the make command line
# (GNU make propagates command-line variables to the recursive sub-makes).
# No source or makefile is modified.  The upstream top-level `make linux`
# target builds util/libNUtil.a, Xlt/libXlt.a, Microline/XmL/libXmL.a and then
# source/nedit + source/nc, and finally runs `source/nedit -V` (which prints
# the version and exits without needing a display).
#
# ---------------------------------------------------------------------------
# Motif 2.1 / version notes (from makefiles/Makefile.linux):
#
#   "# If using a Motif 2.1 compatible library (LessTif, OM) add
#    #  a '-lXp' in front of the -lXext in LIBS. You also drop the
#    #  -lXpm from that list."
#
#   * That link-ordering advice only applies when the link line pulls in
#     -lXext/-lXpm.  NEdit's baseline LIBS here is `-lXm -lXt -lX11 -lm`,
#     which has neither, so no -lXp is needed; libXm carries its own
#     DT_NEEDED dependencies, resolved via the -rpath below.
#
#   * No extra "NEDIT version symbol" is required for Motif 2.1+: NEdit
#     detects the Motif ABI at compile time from `XmVersion` in <Xm/Xm.h>
#     (e.g. `#if XmVersion >= 2001` in source/help.c, source/nedit.c), which
#     comes from our installed Open Motif 2.3.9 headers.
#
#   * We DO define HAVE__XMVERSIONSTRING.  source/help.c references
#     `extern char _XmVersionString[]` when it is set, and Open Motif 2.3.9
#     exports `_XmVersionString` (lib/Xm/VendorS.c), so `nedit -V` can show
#     both the compile-time and run-time Motif versions.  This is the
#     documented "additional Motif version info" flag (README, OpenMotif
#     2.1.30 is known-good).
# ---------------------------------------------------------------------------
#
# Environment:
#   MW_PREFIX          install prefix       (default: $HOME/.local/motif-wayland)
#   JOBS               parallel build jobs  (default: nproc)
#   NEDIT_SRC          path to NEdit source (default: /tmp/opencode/src/nedit)
#   NEDIT_MAKE_TARGET  make target           (default: linux; also: linux-static)
#
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
PROJECT_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd -P)"

MW_PREFIX="${MW_PREFIX:-$HOME/.local/motif-wayland}"
NEDIT_SRC="${NEDIT_SRC:-/tmp/opencode/src/nedit}"
NEDIT_MAKE_TARGET="${NEDIT_MAKE_TARGET:-linux}"

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

require_any_cmd() {
    local c
    for c in "$@"; do
        if command -v "$c" >/dev/null 2>&1; then
            return 0
        fi
    done
    die "none of the required tools were found in PATH: $*. Install one of them and retry."
}

# ---------------------------------------------------------------------------
# argument parsing
# ---------------------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --prefix=*)  MW_PREFIX="${1#*=}" ;;
        --prefix)    MW_PREFIX="${2:?--prefix needs an argument}"; shift ;;
        --target=*)  NEDIT_MAKE_TARGET="${1#*=}" ;;
        --target)    NEDIT_MAKE_TARGET="${2:?--target needs an argument}"; shift ;;
        -h|--help)
            cat <<EOF
Usage: $(basename -- "$0") [--prefix=DIR] [--target=linux|linux-static]

Build NEdit 5.7 unmodified against \$MW_PREFIX.

Environment: MW_PREFIX, JOBS, NEDIT_SRC, NEDIT_MAKE_TARGET
EOF
            exit 0 ;;
        *) die "unknown option: $1 (try --help)" ;;
    esac
    shift
done

export MW_PREFIX

log "NEdit build"
printf '    project   : %s\n' "$PROJECT_ROOT"
printf '    prefix    : %s\n' "$MW_PREFIX"
printf '    source    : %s\n' "$NEDIT_SRC"
printf '    target    : %s\n' "$NEDIT_MAKE_TARGET"
printf '    jobs      : %s\n' "$JOBS"

require_cmd make
require_any_cmd gcc cc

[ -f "$NEDIT_SRC/Makefile" ] || \
    die "no top-level Makefile in $NEDIT_SRC — is NEDIT_SRC correct?"
[ -f "$NEDIT_SRC/makefiles/Makefile.linux" ] || \
    die "no makefiles/Makefile.linux in $NEDIT_SRC — is NEDIT_SRC correct?"

# NEdit links Motif + Xt + the shim's libX11.  The rpath keeps the executable
# pinned to our prefix at runtime, without LD_LIBRARY_PATH.
NEDIT_CFLAGS="-O2 -I$MW_PREFIX/include -DUSE_DIRENT -DUSE_LPR_PRINT_CMD -ansi -U__STRICT_ANSI__ -DHAVE__XMVERSIONSTRING"
NEDIT_LIBS="-L$MW_PREFIX/lib -lXm -lXt -lX11 -lm -Wl,-rpath,$MW_PREFIX/lib"

if [ ! -e "$MW_PREFIX/lib/libXm.so" ] && [ ! -L "$MW_PREFIX/lib/libXm.so" ]; then
    warn "$MW_PREFIX/lib/libXm.so not found — run scripts/build-stack.sh first,"
    warn "otherwise the linker will pick up the system libXm (if any)."
fi

log "NEdit: make -C \"$NEDIT_SRC\" -j$JOBS $NEDIT_MAKE_TARGET"
log "NEdit: CFLAGS=\"$NEDIT_CFLAGS\""
log "NEdit: LIBS=\"$NEDIT_LIBS\""

make -C "$NEDIT_SRC" -j"$JOBS" "$NEDIT_MAKE_TARGET" \
    CFLAGS="$NEDIT_CFLAGS" \
    LIBS="$NEDIT_LIBS"

# ---------------------------------------------------------------------------
# Xft consistency check.
#
# source/nedit.c picks its Motif fallback font resources with
#     #if (XmVersion >= 2003 && XmUPDATE_LEVEL >= 3 && USE_XFT == 1)
# The XFT branch installs "*textRenderTable: fixedRT" plus
# "*fixedRT.fontType: FONT_IS_XFT".  USE_XFT comes from <Xm/Xm.h> and is only
# defined when Motif itself was built with Xft.
#
# If NEdit is compiled with USE_XFT==1 but libXm was built --enable-xft=no,
# Motif cannot resolve a FONT_IS_XFT rendition, XmText's LoadFontMetrics()
# fails, and _XmTextOutputCreate() dereferences a NULL font in LoadGCs()
# (lib/Xm/TextOut.c) — NEdit segfaults before it draws anything.
#
# Verify the branch actually compiled into the binary matches the installed
# Motif, so a stale object file or a stale libXm can never silently disagree.
# ---------------------------------------------------------------------------
NEDIT_BIN="$NEDIT_SRC/source/nedit"
[ -x "$NEDIT_BIN" ] || die "expected $NEDIT_BIN to exist after the build"

if grep -Eq '^[[:space:]]*#[[:space:]]*define[[:space:]]+USE_XFT[[:space:]]+1' \
        "$MW_PREFIX/include/Xm/Xm.h"; then
    motif_xft=yes
else
    motif_xft=no
fi

nedit_xft=no
strings "$NEDIT_BIN" | grep -q 'fixedRT' && nedit_xft=yes

if [ "$motif_xft" != "$nedit_xft" ]; then
    die "Xft mismatch: Motif ($MW_PREFIX/include/Xm/Xm.h) says USE_XFT=$motif_xft
       but the built NEdit binary took the XFT font branch ($nedit_xft).
       NEdit would segfault at startup in Motif's LoadGCs() because
       FONT_IS_XFT renditions cannot resolve without Xft.
       Rebuild Motif (see scripts/build-stack.sh) and then force a full
       NEdit rebuild:  rm -f $NEDIT_SRC/source/*.o $NEDIT_SRC/source/nedit"
fi

log "NEdit build complete:"
log "    Motif USE_XFT=$motif_xft, NEdit XFT branch=$nedit_xft (consistent)"
printf '    %s\n' "$NEDIT_SRC/source/nedit"
printf '    %s\n' "$NEDIT_SRC/source/nc"
log "NEdit is self-contained; copy source/nedit and source/nc onto your PATH to use it."
