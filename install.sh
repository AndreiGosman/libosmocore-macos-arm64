#!/usr/bin/env bash
#
# libosmocore-macos-arm64 install script
# Build and install libosmocore and all its submodules on macOS Apple
# Silicon, into an isolated prefix (default $HOME/sdr-lab/local).
#
# Usage:
#   ./install.sh                              # default prefix $HOME/sdr-lab/local
#   ./install.sh --prefix=/custom/path        # custom prefix
#
# What it does:
#   1. Checks the Homebrew prerequisites
#   2. Clones upstream libosmocore into ./build/libosmocore/
#   3. Applies the Darwin changes (setresgid, wrapping the Linux-only files,
#      the stubs, the compat header, and the patches/ series)
#   4. autoreconf, configure, make, make install into the prefix
#   5. Leaves /opt/homebrew and the home dotfiles alone

set -e
set -o pipefail

# ---- config ----
LIBOSMO_REPO="https://gitea.osmocom.org/osmocom/libosmocore.git"
# The commit this port is built and tested against. 1.14.2.4-2a26b is the
# string git-version-gen produces for it, not a ref: git checkout cannot
# resolve it, and with pipefail that aborted the script on a fresh clone.
LIBOSMO_REF="2a26b47cb6c6590fde8cb953f1caeaf498d5568b"   # 1.14.2 plus 4 commits

PREFIX="${HOME}/sdr-lab/local"

# parse args
for arg in "$@"; do
    case $arg in
        --prefix=*) PREFIX="${arg#*=}" ;;
        --ref=*)    LIBOSMO_REF="${arg#*=}" ;;
        --tag=*)    LIBOSMO_REF="${arg#*=}" ;;
        --help|-h)
            grep '^#' "$0" | head -20 | cut -c3-
            exit 0
            ;;
    esac
done

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$REPO_DIR/build"

log() { printf "\n\033[1;34m[libosmocore-macos]\033[0m %s\n" "$1"; }
err() { printf "\n\033[1;31m[libosmocore-macos ERR]\033[0m %s\n" "$1"; }

# ---- 1. Prerequisites ----
log "1. Checking prerequisites"
if [ "$(uname)" != "Darwin" ]; then
    err "This is not macOS. The port is Darwin specific."
    exit 1
fi
if [ "$(uname -m)" != "arm64" ]; then
    log "  WARNING: this is not Apple Silicon (arm64). Only ARM64 is tested, Intel may work."
fi
if ! command -v brew >/dev/null; then
    err "Homebrew is missing. Install it from https://brew.sh"
    exit 1
fi

BREW_DEPS=(cmake boost swig log4cpp cppunit pkg-config git autoconf automake libtool talloc gnutls)
MISSING=()
for pkg in "${BREW_DEPS[@]}"; do
    brew list --versions "$pkg" >/dev/null 2>&1 || MISSING+=("$pkg")
done
if [ ${#MISSING[@]} -gt 0 ]; then
    log "  Installing the missing dependencies: ${MISSING[*]}"
    brew install "${MISSING[@]}"
fi

# ---- 2. Clone libosmocore ----
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
if [ -d libosmocore/.git ]; then
    log "2. The libosmocore repository is present, running git fetch"
    cd libosmocore && git fetch --tags && cd ..
else
log "2. Cloning libosmocore $LIBOSMO_REF"
    git clone "$LIBOSMO_REPO" libosmocore
fi
cd libosmocore
git checkout --detach "$LIBOSMO_REF"

# ---- 3. Apply Darwin patches ----
log "3. Applying the Darwin changes"

# 3a. exec.c setresgid/setresuid
if ! grep -q "setregid.*pw_gid, pw->pw_gid)" src/core/exec.c; then
    echo "  change: setresgid/setresuid to setregid/setreuid (exec.c)"
    sed -i.bak -E 's/setresgid\(([^,]+), ([^,]+), [^)]+\)/setregid(\1, \2)/g; s/setresuid\(([^,]+), ([^,]+), [^)]+\)/setreuid(\1, \2)/g' src/core/exec.c
fi

# 3b. Wrap Linux-only sources
# src/vty/cpu_sched_vty.c used to be wrapped here as well. It is handled by
# patches/005 instead, because wrapping it removes the public
# osmo_cpu_sched_vty_init() that every Osmocom daemon calls, so the file needs
# a replacement definition and not only a guard.
echo "  change: wrap the Linux-only files in #ifdef __linux__"
WRAPPED=0
for f in $(grep -l '^#include <linux/' src/*/*.c 2>/dev/null); do
    if [ -f "$f" ] && ! head -1 "$f" | grep -q '^#ifdef __linux__'; then
        { echo '#ifdef __linux__'; cat "$f"; echo '#endif'; } > "$f.new"
        mv "$f.new" "$f"
        echo "    wrapped: $f"
        WRAPPED=$((WRAPPED + 1))
    fi
done
[ "$WRAPPED" -eq 0 ] && echo "    (already applied)"

# 3c. The Darwin sources and headers. Copied whenever they differ, so that
# re-running the script over an existing tree picks up an updated file.
copy_if_changed() {
    if ! cmp -s "$1" "$2"; then
        echo "  change: $3"
        mkdir -p "$(dirname "$2")"
        cp "$1" "$2"
    fi
}
copy_if_changed "$REPO_DIR/darwin_stubs.c"   src/core/darwin_stubs.c   "add src/core/darwin_stubs.c"
copy_if_changed "$REPO_DIR/darwin_timerfd.c" src/core/darwin_timerfd.c "add src/core/darwin_timerfd.c"
# sys/timerfd.h has to be visible at configure time: the check for it
# defines HAVE_SYS_TIMERFD_H, which is what compiles the timerfd wrappers in
# src/core/select.c. include/ is already on AM_CPPFLAGS; configure gets the
# same directory through CFLAGS below.
copy_if_changed "$REPO_DIR/darwin_timerfd.h" include/sys/timerfd.h     "add include/sys/timerfd.h"

# 3d. Add the Darwin sources to Makefile.am
if ! grep -q 'darwin_stubs\.c' src/core/Makefile.am; then
    echo "  change: add darwin_stubs.c to src/core/Makefile.am"
    sed -i.bak 's|stats_tcp\.c|stats_tcp.c \\\n\tdarwin_stubs.c|' src/core/Makefile.am
fi
if ! grep -q 'darwin_timerfd\.c' src/core/Makefile.am; then
    echo "  change: add darwin_timerfd.c to src/core/Makefile.am"
    sed -i.bak 's|darwin_stubs\.c|darwin_stubs.c \\\n\tdarwin_timerfd.c|' src/core/Makefile.am
fi

# 3e. Copy darwin_compat.h into the source root
copy_if_changed "$REPO_DIR/darwin_compat.h" darwin_compat.h "copy darwin_compat.h into the source root"

# 3f. Numbered patch series against the upstream sources
PATCHES_DIR="$REPO_DIR/patches"
if [ -d "$PATCHES_DIR" ]; then
    for patch_file in "$PATCHES_DIR"/*.patch; do
        [ -f "$patch_file" ] || continue
        if patch -p1 --forward --silent < "$patch_file" >/dev/null 2>&1; then
            echo "  patch: $(basename "$patch_file")"
        elif patch -p1 --reverse --dry-run --silent < "$patch_file" >/dev/null 2>&1; then
            # --forward refuses to reapply, but still reports failure. A clean
            # reverse dry-run means the change is already in the tree, which is
            # what makes re-running this script safe.
            echo "  patch: $(basename "$patch_file") (already applied)"
        else
            err "patch $(basename "$patch_file") did not apply"
            exit 1
        fi
    done
fi

# ---- 4. Autoreconf ----
log "4. autoreconf -fi"
autoreconf -fi

# ---- 5. Configure ----
log "5. configure --prefix=$PREFIX"
rm -f config.cache
mkdir -p "$PREFIX"

CFLAGS="-include $PWD/darwin_compat.h -I$PWD/include" ./configure \
    --prefix="$PREFIX" \
    --disable-doxygen --disable-pcsc --disable-systemd-logging \
    --disable-uring --disable-libmnl --disable-libsctp

# ---- 6. Make ----
log "6. make -j$(sysctl -n hw.ncpu)"
# LDFLAGS is applied at make time only. Passing it to configure makes some
# feature tests succeed when they should not.
make -j$(sysctl -n hw.ncpu) LDFLAGS="-Wl,-undefined,dynamic_lookup"

# ---- 7. Install ----
log "7. make install"
make install

# The upstream header list does not know sys/timerfd.h, so install it by
# hand for consumers that include it directly. pkg-config --cflags already
# carries $PREFIX/include.
mkdir -p "$PREFIX/include/sys"
cp include/sys/timerfd.h "$PREFIX/include/sys/timerfd.h"

# ---- 8. Verify ----
log "8. Verifying the installation"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PKG_CONFIG_PATH"
for pc in libosmocore libosmogsm libosmocodec libosmocoding libosmovty libosmoisdn; do
    if pkg-config --exists "$pc" 2>/dev/null; then
        echo "  ok: $pc $(pkg-config --modversion $pc)"
    else
        echo "  MISSING: $pc"
    fi
done

# ---- 9. Symbol check ----
echo ""
log "9. Checking the Darwin symbols (darwin_stubs.c, darwin_timerfd.c, select.c)"
for sym in osmo_tcp_stats_config osmo_stats_tcp_set_interval osmo_tundev_alloc \
           timerfd_create timerfd_settime timerfd_gettime osmo_timerfd_setup; do
    if nm -gU "$PREFIX/lib/libosmocore.dylib" 2>/dev/null | grep -q "_$sym\$"; then
        echo "  ok: $sym exported"
    else
        echo "  MISSING: $sym (the stubs are incomplete)"
    fi
done

log "Done. libosmocore is installed in $PREFIX"
echo ""
echo "To use it in later builds (gr-gsm and so on):"
echo "  export PKG_CONFIG_PATH=$PREFIX/lib/pkgconfig:\$PKG_CONFIG_PATH"
echo "  export DYLD_LIBRARY_PATH=$PREFIX/lib:\$DYLD_LIBRARY_PATH"
echo ""
echo "Uninstall: rm -rf $PREFIX/lib/libosmo*.dylib \\"
echo "                 $PREFIX/lib/pkgconfig/libosmo*.pc \\"
echo "                 $PREFIX/include/osmocom"
