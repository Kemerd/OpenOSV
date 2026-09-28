#!/bin/bash
# =============================================================================
#  build_mac.sh - build OpenOSV on a Mac (Apple Silicon) in one go.
#
#  The steps a user confirmed on a MacBook Pro M4 running macOS 26.6.1, with
#  DaVinci Resolve (GitHub issue #2 - thanks, @arkanos), in the order the
#  macOS CI runs them:
#
#    1. the Xcode command line tools (clang, the Metal compiler);
#    2. Homebrew's cmake, ninja and pkg-config.  vcpkg's FFmpeg port needs
#       pkg-config, and without it the first configure fails deep inside
#       vcpkg with nothing that says why;
#    3. vcpkg, cloned to ~/vcpkg unless VCPKG_ROOT already names one;
#    4. configure and build a preset (macos-release by default);
#    5. optionally install the DaVinci Resolve bundle (--install-ofx) or the
#       Premiere Pro plug-ins (--install-premiere, with the premiere preset).
#
#  Usage:
#    scripts/build_mac.sh                  # library, osvtool, Resolve bundle
#    scripts/build_mac.sh --install-ofx    # ...and install it for Resolve
#    scripts/build_mac.sh --preset macos-premiere-release --install-premiere
#    scripts/build_mac.sh --test           # also run the test suite
#
#  Every step prints what it is about to do; nothing is installed silently.
# =============================================================================
set -euo pipefail

# ---- options ------------------------------------------------------------------
preset="macos-release"
install_ofx=0
install_premiere=0
run_tests=0
while [ $# -gt 0 ]; do
    case "$1" in
        --preset) preset="${2:?--preset needs a name}"; shift 2 ;;
        --install-ofx) install_ofx=1; shift ;;
        --install-premiere) install_premiere=1; shift ;;
        --test) run_tests=1; shift ;;
        -h|--help) sed -n '2,26p' "$0"; exit 0 ;;
        *) echo "build_mac.sh: unknown option '$1' (try --help)" >&2; exit 1 ;;
    esac
done

# Run from the checkout's root, wherever the script was started from.
cd "$(cd "$(dirname "$0")/.." && pwd)"

say() { printf '\n==> %s\n' "$*"; }

# ---- 1. this is a Mac with the Xcode command line tools ------------------------
if [ "$(uname -s)" != "Darwin" ]; then
    echo "build_mac.sh is for macOS; on Windows see docs/BUILDING.md" >&2
    exit 1
fi
if ! xcode-select -p >/dev/null 2>&1; then
    say "Installing the Xcode command line tools (a dialog opens; run this script again when it is done)"
    xcode-select --install || true
    exit 1
fi

# ---- 2. Homebrew packages ------------------------------------------------------------
if ! command -v brew >/dev/null 2>&1; then
    echo "Homebrew is needed for cmake, ninja and pkg-config: https://brew.sh" >&2
    exit 1
fi
for pkg in cmake ninja pkg-config; do
    # pkg-config is provided by Homebrew's pkgconf formula nowadays; either counts.
    if brew list "$pkg" >/dev/null 2>&1 || { [ "$pkg" = "pkg-config" ] && brew list pkgconf >/dev/null 2>&1; }; then
        continue
    fi
    say "brew install $pkg"
    brew install "$pkg"
done

# ---- 3. vcpkg ---------------------------------------------------------------------
export VCPKG_ROOT="${VCPKG_ROOT:-$HOME/vcpkg}"
if [ ! -d "$VCPKG_ROOT/.git" ]; then
    say "Cloning vcpkg into $VCPKG_ROOT"
    git clone https://github.com/microsoft/vcpkg "$VCPKG_ROOT"
fi
if [ ! -x "$VCPKG_ROOT/vcpkg" ]; then
    say "Bootstrapping vcpkg"
    "$VCPKG_ROOT/bootstrap-vcpkg.sh" -disableMetrics
fi

# ---- 4. configure and build ----------------------------------------------------------
say "cmake --preset $preset (the first run builds FFmpeg and the other vcpkg ports: several minutes)"
cmake --preset "$preset"
say "cmake --build --preset $preset"
cmake --build --preset "$preset"

if [ "$run_tests" -eq 1 ]; then
    # The test presets are named after the build presets without "-release".
    test_preset="macos"
    [ "$preset" = "macos-premiere-release" ] && test_preset="macos-premiere"
    say "ctest --preset $test_preset"
    ctest --preset "$test_preset"
fi

# ---- 5. install ------------------------------------------------------------------------
if [ "$install_ofx" -eq 1 ]; then
    say "Installing the DaVinci Resolve bundle (quit Resolve first; sudo asks for your password)"
    scripts/install_ofx.sh
fi
if [ "$install_premiere" -eq 1 ]; then
    say "Installing the Premiere Pro plug-ins (quit Premiere first)"
    scripts/install_plugins.sh
fi

say "Done. osvtool is in build/$preset/bin; the Resolve bundle in build/$preset/plugins/ofx"
