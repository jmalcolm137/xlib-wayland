#!/usr/bin/env bash
#
# build-stack.sh — build & install the Xt/Motif layer of the Motif/Wayland
# stack against a custom, Wayland-native libX11-compatible prefix.
#
# This script does NOT build the shim itself.  The shim (`libX11.so`) is built
# and installed with meson from the project root first:
#
#     cd <project-root>
#     meson setup build
#     meson install -C build
#
# Reference sources (checked out already, override with the env vars below):
#     libXt   (Xorg libXt, autotools)  $MW_SRC/libxt
#     Motif   (Open Motif, autotools)  $MW_SRC/motif
#
# Environment:
#   MW_PREFIX   install prefix          (default: $HOME/.local/motif-wayland)
#   JOBS        parallel build jobs     (default: nproc)
#   MW_SRC      reference source trees  (default: ${TMPDIR:-/tmp}/xlib-wayland)
#   LIBXT_SRC   path to libXt source    (default: $MW_SRC/libxt)
#   MOTIF_SRC   path to Open Motif src  (default: $MW_SRC/motif)
#
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
PROJECT_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd -P)"

MW_PREFIX="${MW_PREFIX:-$HOME/.local/motif-wayland}"
MW_SRC="${MW_SRC:-${TMPDIR:-/tmp}/xlib-wayland}"
LIBXT_SRC="${LIBXT_SRC:-$MW_SRC/libxt}"
MOTIF_SRC="${MOTIF_SRC:-$MW_SRC/motif}"

if [ -n "${JOBS:-}" ]; then
    :
elif command -v nproc >/dev/null 2>&1; then
    JOBS="$(nproc)"
else
    JOBS=4
fi

SKIP_XT=0
SKIP_MOTIF=0
MOTIF_DEMOS=0

# ---------------------------------------------------------------------------
# output helpers
# ---------------------------------------------------------------------------
log()  { printf '\n\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
    cat <<EOF
Usage: $(basename -- "$0") [OPTIONS]

Build and install libXt and Open Motif into \$MW_PREFIX against the
Motif/Wayland libX11 shim (which must already be installed).

Options:
  --prefix=DIR     Install prefix (default: \$MW_PREFIX or
                   \$HOME/.local/motif-wayland)
  --skip-xt        Do not rebuild libXt
  --skip-motif     Do not rebuild Open Motif
  --with-demos     Also build Open Motif's demo programs (they may need
                   Xlib locale entry points the shim does not implement)
  -h, --help       Show this help

Environment overrides:
  MW_PREFIX, JOBS, LIBXT_SRC, MOTIF_SRC

Examples:
  scripts/build-stack.sh
  MW_PREFIX=/opt/motifwl scripts/build-stack.sh
  scripts/build-stack.sh --skip-xt          # only rebuild Open Motif
EOF
}

# ---------------------------------------------------------------------------
# small helpers
# ---------------------------------------------------------------------------
require_cmd() {
    command -v "$1" >/dev/null 2>&1 || \
        die "required tool '$1' was not found in PATH. Install it (e.g. via your package manager) and retry."
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

# configure_supports <configure> <option>  -> 0 if ./configure --help lists it
configure_supports() {
    "$1" --help 2>/dev/null | grep -q -- "$2"
}

check_shim() {
    if [ ! -e "$MW_PREFIX/lib/libX11.so" ] && [ ! -L "$MW_PREFIX/lib/libX11.so" ]; then
        die "the Motif/Wayland libX11 shim is not installed at:
      $MW_PREFIX/lib/libX11.so

Build and install the shim first, from the project root:
      cd \"$PROJECT_ROOT\"
      meson setup build
      meson install -C build

If you installed it somewhere else, re-run this script with MW_PREFIX set
to the matching --prefix."
    fi
    log "Found shim: $MW_PREFIX/lib/libX11.so"
}

# ---------------------------------------------------------------------------
# argument parsing
# ---------------------------------------------------------------------------
while [ $# -gt 0 ]; do
    case "$1" in
        --prefix=*)   MW_PREFIX="${1#*=}" ;;
        --prefix)     MW_PREFIX="${2:?--prefix needs an argument}"; shift ;;
        --skip-xt)    SKIP_XT=1 ;;
        --skip-motif) SKIP_MOTIF=1 ;;
        --with-demos) MOTIF_DEMOS=1 ;;
        -h|--help)    usage; exit 0 ;;
        *)            die "unknown option: $1 (try --help)" ;;
    esac
    shift
done

export MW_PREFIX

# ---------------------------------------------------------------------------
# stage 0 — the shim
# ---------------------------------------------------------------------------
log "Motif/Wayland stack build"
printf '    project root : %s\n' "$PROJECT_ROOT"
printf '    prefix       : %s\n' "$MW_PREFIX"
printf '    libXt source : %s\n' "$LIBXT_SRC"
printf '    Motif source : %s\n' "$MOTIF_SRC"
printf '    jobs         : %s\n' "$JOBS"

check_shim
mkdir -p "$MW_PREFIX"

# ---------------------------------------------------------------------------
# stage 1 — libXt
# ---------------------------------------------------------------------------
build_libxt() {
    log "Stage 1/2: libXt ($LIBXT_SRC)"
    require_cmd make
    require_any_cmd gcc cc
    require_cmd autoconf
    require_cmd automake
    require_cmd aclocal
    require_cmd libtoolize
    require_cmd pkg-config

    [ -f "$LIBXT_SRC/Makefile.in" ] || \
        die "no Makefile.in in $LIBXT_SRC — is LIBXT_SRC correct?"

    cd "$LIBXT_SRC"

    # Point the compiler at our libX11 shim; the rpath keeps the built
    # libraries/tools finding it at runtime without LD_LIBRARY_PATH.
    export CPPFLAGS="-I$MW_PREFIX/include ${CPPFLAGS:-}"
    export LDFLAGS="-L$MW_PREFIX/lib -Wl,-rpath,$MW_PREFIX/lib ${LDFLAGS:-}"
    export PKG_CONFIG_PATH="$MW_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"

    # A git checkout must have its build system regenerated, which needs the
    # xorg-macros macros installed.  An official release tarball
    # (libXt-1.3.1.tar.xz from x.org) ships a ready ./configure, so prefer it
    # and only fall back to regenerating when there is nothing to use.
    if [ -f "$LIBXT_SRC/configure" ]; then
        log "libXt: using the ./configure shipped with the release tarball"
    elif [ -x "$LIBXT_SRC/autogen.sh" ]; then
        log "libXt: regenerating build system (NOCONFIGURE=1 ./autogen.sh)"
        NOCONFIGURE=1 ./autogen.sh
    else
        die "neither ./configure nor an executable autogen.sh in $LIBXT_SRC
— is LIBXT_SRC correct?"
    fi

    local -a conf_args=("--prefix=$MW_PREFIX")

    # Our shim does not implement XKB; only pass --disable-xkb when the
    # (freshly generated) configure script understands it.
    if configure_supports ./configure --disable-xkb; then
        conf_args+=(--disable-xkb)
        log "libXt: XKB support disabled (shim has no XKB implementation)."
    else
        warn "libXt: this configure has no --disable-xkb; leaving XKB enabled."
    fi

    log "libXt: ./configure ${conf_args[*]}"
    ./configure "${conf_args[@]}"

    log "libXt: make -j$JOBS"
    make -j"$JOBS"

    log "libXt: make install (prefix=$MW_PREFIX)"
    make install
}

# ---------------------------------------------------------------------------
# stage 2 — Open Motif
# ---------------------------------------------------------------------------
build_motif() {
    log "Stage 2/2: Open Motif ($MOTIF_SRC)"
    require_cmd make
    require_any_cmd gcc cc
    require_cmd autoconf
    require_cmd autoheader
    require_cmd automake
    require_cmd aclocal
    require_cmd libtoolize
    # Motif's configure wants a yacc and a lex to generate parsers.
    require_any_cmd bison byacc yacc
    require_any_cmd flex lex

    cd "$MOTIF_SRC"

    export CPPFLAGS="-I$MW_PREFIX/include ${CPPFLAGS:-}"
    export LDFLAGS="-L$MW_PREFIX/lib -Wl,-rpath,$MW_PREFIX/lib ${LDFLAGS:-}"
    export PKG_CONFIG_PATH="$MW_PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
    # make sure Motif's helper tools prefer the ones we install into the prefix
    export PATH="$MW_PREFIX/bin:$PATH"

    # Motif's host tools (makestrs.c and friends) use pre-C23 old-style function
    # prototypes, and its bundled libXpm omits <string.h> in places.  GCC 14+
    # promotes implicit declarations and narrowing pointer/integer conversions
    # from warnings to hard errors, so turn those specific diagnostics back
    # into warnings.  Build accommodation only -- no source is modified.
    export CFLAGS="-std=gnu17 -g -O2 -Wno-implicit-function-declaration \
-Wno-int-conversion -Wno-incompatible-pointer-types ${CFLAGS:-}"

    # A git checkout needs its build system regenerated; a release tarball
    # (motif-2.3.x.tar.gz) ships a pre-generated ./configure and has no
    # autogen.sh at all.  Support both rather than requiring the checkout.
    if [ -x "$MOTIF_SRC/autogen.sh" ]; then
        log "Motif: regenerating build system (./autogen.sh)"
        ./autogen.sh
    elif [ -f "$MOTIF_SRC/configure" ]; then
        log "Motif: no autogen.sh; using the ./configure shipped in the release tarball."
    else
        die "neither an executable autogen.sh nor a ./configure in $MOTIF_SRC
— is MOTIF_SRC correct?"
    fi

    local -a conf_args=(
        "--prefix=$MW_PREFIX"
        # Force AC_PATH_XTRA at our libX11-compatible prefix instead of /usr.
        "--x-includes=$MW_PREFIX/include"
        "--x-libraries=$MW_PREFIX/lib"
    )

    # Xft is deliberately left OFF for now.  The shim does provide
    # libXft.so.2 (see src/xft), and Motif built with --enable-xft does load
    # Xft fonts through it, but Motif then draws them through a path that ends
    # up with no font bound to the GC (_XmXftDrawCreate/TextF use Xft only when
    # its font list resolves to one; a core entry resets that), and a real
    # server would fall back to a default font where the shim draws nothing --
    # so menu and text labels disappear.  Leave it off until Motif's Xft draw
    # path is made to work.  Pass --with-xft to build it enabled for testing.
    if [ "${WITH_XFT:-0}" = 1 ] && configure_supports ./configure --enable-xft; then
        export PKG_CONFIG_PATH="$MW_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
        conf_args+=(--enable-xft)
        log "Motif: Xft ENABLED for testing (uses $MW_PREFIX/lib/libXft.so.2)."
    elif configure_supports ./configure --disable-xft; then
        conf_args+=(--disable-xft)
        log "Motif: Xft support disabled."
    elif configure_supports ./configure --enable-xft; then
        conf_args+=(--enable-xft=no)
        log "Motif: Xft support disabled via --enable-xft=no."
    else
        warn "Motif: this configure cannot disable Xft; leaving it enabled."
    fi

    # NOTE: Open Motif 2.3.9 has no --disable-build-demos option — demos are an
    # unconditional SUBDIRS entry.  We pass it only if a future tree supports
    # it; otherwise the demos build too (libXm/libMrm are what get installed).
    if configure_supports ./configure --disable-build-demos; then
        conf_args+=(--disable-build-demos)
        log "Motif: demos disabled."
    else
        warn "Motif: no --disable-build-demos in this configure; demos will also build (harmless)."
    fi

    log "Motif: ./configure ${conf_args[*]}"
    ./configure "${conf_args[@]}"

    log "Motif: make -j$JOBS"
    # Build and install only the subtrees the stack needs.  `lib` provides
    # libXm/libMrm (what NEdit and every other Motif client links against) and
    # `clients` provides mwm/uil/xmbind.  The `demos` subtree is deliberately
    # skipped: it is sample programs, and some of them call Xlib locale entry
    # points (e.g. XLocaleOfIM) that the shim does not implement -- full IME
    # is M5.  Pass --with-demos to build them anyway.
    for sub in lib clients; do
        log "Motif: make -C $sub -j$JOBS"
        make -C "$sub" -j"$JOBS"
        log "Motif: make -C $sub install (prefix=$MW_PREFIX)"
        make -C "$sub" install
    done

    if [ "$MOTIF_DEMOS" -eq 1 ]; then
        warn "Motif: --with-demos requested; building demos (may fail on"
        warn "       unimplemented Xlib locale entry points)."
        make -C demos -j"$JOBS"
        make -C demos install
    else
        log "Motif: skipping demos (use --with-demos to build them)."
    fi
}

if [ "$SKIP_XT" -eq 0 ]; then
    build_libxt
else
    log "Skipping libXt (--skip-xt)"
fi

if [ "$SKIP_MOTIF" -eq 0 ]; then
    build_motif
else
    log "Skipping Open Motif (--skip-motif)"
fi

log "Stack build complete."
printf '    libX11 : %s\n' "$MW_PREFIX/lib/libX11.so"
printf '    libXt  : %s\n' "$MW_PREFIX/lib/libXt.so"
printf '    libXm  : %s\n' "$MW_PREFIX/lib/libXm.so"
log "Next: scripts/build-xv.sh and scripts/build-nedit.sh"
