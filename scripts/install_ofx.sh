#!/bin/bash
# =============================================================================
#  install_ofx.sh - install (or remove) OpenOSV's OpenFX plug-ins for
#  DaVinci Resolve on macOS.  The Mac twin of scripts/install_ofx.ps1.
#
#  Copies OpenOSV.ofx.bundle - OpenOSV.ofx and its FFmpeg inside
#  Contents/Frameworks - into the folder every OpenFX host on macOS scans:
#
#      /Library/OFX/Plugins/OpenOSV.ofx.bundle
#
#  DaVinci Resolve (free or Studio) lists the two effects after a restart,
#  in the Effects Library under OpenFX, group OpenOSV: OpenOSV Source (a
#  generator, for .OSV clips) and OpenOSV 360 Reframe (a filter, for any 360
#  clip).  See docs/RESOLVE.md.
#
#  The folder belongs to root, so the copy runs through sudo (you are asked
#  for your password once).  The installed bundle loses the quarantine
#  attribute a download carries - Gatekeeper would otherwise refuse it inside
#  Resolve without a word - and is signed ad hoc: Apple Silicon refuses to
#  load unsigned code, and an ad-hoc signature is what a build from source
#  gets.
#
#  UNTESTED: nobody has run this bundle inside Resolve on a Mac yet.  If it
#  misbehaves, please file an issue or a pull request:
#  https://github.com/Kemerd/OpenOSV/issues
#
#  Usage:
#    scripts/install_ofx.sh [options]
#
#  Options:
#    --bundle DIR        the built OpenOSV.ofx.bundle (default: <root>/plugins/
#                        OpenOSV.ofx.bundle in a release zip, else the newest
#                        <root>/build/*/plugins/ofx/OpenOSV.ofx.bundle)
#    --destination DIR   the OFX plug-in folder (default /Library/OFX/Plugins)
#    --uninstall         remove instead of install
#    --no-sudo           fail instead of asking for sudo
#    --no-sign           do not ad-hoc sign the installed code
#    --force             install even while DaVinci Resolve runs
#    -h, --help          this text
#
#  Output is plain ASCII.  Works with the bash 3.2 macOS ships.
# =============================================================================
set -euo pipefail

readonly BUNDLE_NAME="OpenOSV.ofx.bundle"
readonly BINARY_RELATIVE="Contents/MacOS/OpenOSV.ofx"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

BUNDLE=""
DESTINATION="/Library/OFX/Plugins"
UNINSTALL=0
NO_SUDO=0
NO_SIGN=0
FORCE=0
NEED_SUDO=0

fail() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
warn() { printf 'WARNING: %s\n' "$*" >&2; }
step() { printf '==> %s\n' "$*"; }
info() { printf '    %s\n' "$*"; }

usage() { sed -n '3,/^# =====/p' "${BASH_SOURCE[0]}" | sed '$d' | sed 's/^# \{0,1\}//'; }

need_value() {
    [ $# -ge 2 ] && [ -n "$2" ] || fail "$1 needs a value"
}

while [ $# -gt 0 ]; do
    case "$1" in
        --bundle) need_value "$@"; BUNDLE="$2"; shift 2 ;;
        --destination) need_value "$@"; DESTINATION="$2"; shift 2 ;;
        --uninstall) UNINSTALL=1; shift ;;
        --no-sudo) NO_SUDO=1; shift ;;
        --no-sign) NO_SIGN=1; shift ;;
        --force) FORCE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown option '$1' (see --help)" ;;
    esac
done

[ "$(uname -s)" = "Darwin" ] || fail "this script installs the macOS bundle; on Windows run scripts\\install_ofx.ps1"

TARGET="$DESTINATION/$BUNDLE_NAME"

# ---------------------------------------------------------------------------
#  sudo only when the destination is not writable as it is (a dry run into a
#  scratch folder needs none).
# ---------------------------------------------------------------------------
decide_sudo() {
    local probe="$DESTINATION"
    while [ ! -e "$probe" ] && [ "$probe" != "/" ]; do
        probe="$(dirname "$probe")"
    done
    if [ -w "$probe" ] && { [ ! -e "$DESTINATION" ] || [ -w "$DESTINATION" ]; }; then
        NEED_SUDO=0
        return
    fi
    [ "$NO_SUDO" -eq 0 ] || fail "'$DESTINATION' needs administrator rights and --no-sudo was given"
    NEED_SUDO=1
    step "Administrator rights are required"
    info "OpenFX plug-ins live in a folder owned by root:"
    info "    $DESTINATION"
    info "sudo will ask for your password once. Nothing outside that folder is touched."
    sudo -v || fail "sudo was refused; nothing was changed"
}

priv() {
    if [ "$NEED_SUDO" -eq 1 ]; then
        sudo "$@"
    else
        "$@"
    fi
}

# A loaded bundle is mapped into the host: replacing it under a running
# Resolve is how crashes on quit happen.
assert_resolve_closed() {
    [ "$FORCE" -eq 0 ] || return 0
    if pgrep -x Resolve >/dev/null 2>&1; then
        fail "Quit DaVinci Resolve first: a loaded plug-in must not be replaced under it. Pass --force to try anyway."
    fi
}

# The bundle to install when none was named.
find_bundle() {
    if [ -f "$ROOT_DIR/plugins/$BUNDLE_NAME/$BINARY_RELATIVE" ]; then
        printf '%s\n' "$ROOT_DIR/plugins/$BUNDLE_NAME"
        return
    fi
    local newest="" candidate
    for candidate in "$ROOT_DIR"/build/*/plugins/ofx/"$BUNDLE_NAME"; do
        [ -f "$candidate/$BINARY_RELATIVE" ] || continue
        if [ -z "$newest" ] || [ "$candidate/$BINARY_RELATIVE" -nt "$newest/$BINARY_RELATIVE" ]; then
            newest="$candidate"
        fi
    done
    printf '%s\n' "$newest"
}

sign_bundle() {
    local bundle="$1" lib
    [ "$NO_SIGN" -eq 0 ] || return 0
    if [ -d "$bundle/Contents/Frameworks" ]; then
        for lib in "$bundle"/Contents/Frameworks/*.dylib; do
            [ -f "$lib" ] || continue
            priv codesign --force --sign - --timestamp=none "$lib" >/dev/null 2>&1 || warn "could not sign $lib"
        done
    fi
    priv codesign --force --sign - --timestamp=none "$bundle" >/dev/null 2>&1 ||
        warn "could not sign $bundle (Apple Silicon will refuse to load it)"
}

# ---------------------------------------------------------------------------
#  Main
# ---------------------------------------------------------------------------
assert_resolve_closed
decide_sudo

if [ "$UNINSTALL" -eq 1 ]; then
    step "Removing $TARGET"
    if [ -e "$TARGET" ]; then
        priv rm -rf "$TARGET"
        info "Removed. Restart DaVinci Resolve to drop the effects from its list."
    else
        info "Nothing installed there."
    fi
    exit 0
fi

if [ -z "$BUNDLE" ]; then
    BUNDLE="$(find_bundle)"
    [ -n "$BUNDLE" ] || fail "no built $BUNDLE_NAME found; build it (docs/RESOLVE.md) or pass --bundle"
fi
[ -f "$BUNDLE/$BINARY_RELATIVE" ] || fail "'$BUNDLE' is not an OpenOSV bundle (no $BINARY_RELATIVE inside)"

step "Installing $BUNDLE_NAME"
info "from $BUNDLE"
info "to   $TARGET"
priv mkdir -p "$DESTINATION"
# A clean copy: a library left over from an older build must never be the
# one the bundle loads.
priv rm -rf "$TARGET"
priv ditto "$BUNDLE" "$TARGET"
priv xattr -dr com.apple.quarantine "$TARGET" 2>/dev/null || true
sign_bundle "$TARGET"

info "Installed. Start DaVinci Resolve and look in the Effects Library under"
info "OpenFX, group OpenOSV: OpenOSV Source (a generator) and OpenOSV 360 Reframe"
info "(a filter). This build is UNTESTED on macOS: please report what you see at"
info "https://github.com/Kemerd/OpenOSV/issues"
