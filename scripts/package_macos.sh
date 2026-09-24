#!/bin/bash
# =============================================================================
#  package_macos.sh - assemble the macOS release zip from a build tree.
#
#  Usage:
#    scripts/package_macos.sh --build-dir DIR --out FILE.zip
#                             [--plugin-stage DIR] [--vcpkg-installed DIR]
#
#    --build-dir        a configured and built macOS tree (holds bin/osvtool)
#    --out              the zip to write; its base name is also the top-level
#                       folder inside it (OpenOSV-<version>-macos-arm64)
#    --plugin-stage     the plug-in stage folder (OSV_PLUGIN_STAGE_DIR); its
#                       bundles are included when present
#    --vcpkg-installed  vcpkg's installed tree for the triplet (holds lib/ and
#                       share/); default <repo>/vcpkg_installed/arm64-osx-openosv
#
#  Layout of the zip (the same relative paths the repository uses, so
#  scripts/install_plugins.sh works unchanged from either):
#
#    OpenOSV-<version>-macos-arm64/
#      bin/osvtool               + the FFmpeg dylibs it loads, beside it
#      plugins/OpenOSV/          the Premiere bundles (only when they were built)
#      luts/ presets/ panel/     as in the repository
#      scripts/install_plugins.sh
#      docs/BUILDING_MAC.md
#      LICENSE NOTICE licenses/  OpenOSV's licence, notices, and the licence
#                                of every third-party library in the binaries
#
#  osvtool's build-tree RPATH points into vcpkg_installed; here it is
#  replaced by @executable_path so the copy runs from wherever the zip is
#  unpacked.  Everything is signed ad hoc afterwards (Apple Silicon refuses
#  unsigned code, and install_name_tool invalidates the linker's signature).
# =============================================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

BUILD_DIR=""
OUT=""
PLUGIN_STAGE=""
VCPKG_INSTALLED="$ROOT_DIR/vcpkg_installed/arm64-osx-openosv"

fail() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}
step() { printf '==> %s\n' "$*"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --build-dir) BUILD_DIR="${2:-}"; shift 2 ;;
        --out) OUT="${2:-}"; shift 2 ;;
        --plugin-stage) PLUGIN_STAGE="${2:-}"; shift 2 ;;
        --vcpkg-installed) VCPKG_INSTALLED="${2:-}"; shift 2 ;;
        -h|--help) sed -n '3,/^# =====/p' "${BASH_SOURCE[0]}" | sed '$d' | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) fail "unknown option '$1'" ;;
    esac
done

[ -n "$BUILD_DIR" ] || fail "--build-dir is required"
[ -n "$OUT" ] || fail "--out is required"
[ "$(uname -s)" = "Darwin" ] || fail "this script packages the macOS build"
TOOL="$BUILD_DIR/bin/osvtool"
[ -x "$TOOL" ] || fail "no osvtool at $TOOL (build first)"
[ -d "$VCPKG_INSTALLED/lib" ] || fail "no vcpkg lib folder at $VCPKG_INSTALLED/lib"

NAME="$(basename "$OUT" .zip)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/openosv-package.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
STAGE="$WORK/$NAME"
mkdir -p "$STAGE/bin" "$STAGE/licenses" "$STAGE/scripts" "$STAGE/docs"

# ---------------------------------------------------------------------------
#  osvtool and the shared libraries it loads.
#
#  `otool -L` lists every LC_LOAD_DYLIB; the ones spelled @rpath/... are ours
#  to ship (system libraries and frameworks use absolute /usr/lib and
#  /System paths).  Each shipped dylib is scanned the same way, so
#  libavformat brings libavcodec and libavutil along.
# ---------------------------------------------------------------------------
step "osvtool"
cp "$TOOL" "$STAGE/bin/osvtool"

rpath_deps() {
    otool -L "$1" | tail -n +2 | awk '{print $1}' | grep '^@rpath/' | sed 's|^@rpath/||' || true
}

queue=("$STAGE/bin/osvtool")
copied=""
while [ "${#queue[@]}" -gt 0 ]; do
    current="${queue[0]}"
    queue=("${queue[@]:1}")
    for dep in $(rpath_deps "$current"); do
        case " $copied " in
            *" $dep "*) continue ;;
        esac
        src="$VCPKG_INSTALLED/lib/$dep"
        [ -f "$src" ] || fail "$(basename "$current") needs @rpath/$dep, which is not in $VCPKG_INSTALLED/lib"
        # -L: vcpkg installs versioned names as symbolic links; ship the file.
        cp -L "$src" "$STAGE/bin/$dep"
        chmod u+w "$STAGE/bin/$dep"
        copied="$copied $dep"
        queue+=("$STAGE/bin/$dep")
    done
done
echo "    shared libraries:${copied:- none}"

# Rewrite every absolute LC_RPATH (the build tree, vcpkg_installed) to the
# folder the binary itself sits in.
fix_rpaths() {
    local file="$1" anchor="$2" rp have=0
    for rp in $(otool -l "$file" | awk '/cmd LC_RPATH/{getline; getline; print $2}'); do
        if [ "$rp" = "$anchor" ]; then
            have=1
            continue
        fi
        case "$rp" in
            /*) install_name_tool -delete_rpath "$rp" "$file" ;;
        esac
    done
    if [ "$have" -eq 0 ]; then
        install_name_tool -add_rpath "$anchor" "$file"
    fi
}
fix_rpaths "$STAGE/bin/osvtool" "@executable_path"
for dep in $copied; do
    install_name_tool -id "@rpath/$dep" "$STAGE/bin/$dep"
    fix_rpaths "$STAGE/bin/$dep" "@loader_path"
done

# Ad hoc signatures, libraries first.
for dep in $copied; do
    codesign --force --sign - --timestamp=none "$STAGE/bin/$dep"
done
codesign --force --sign - --timestamp=none "$STAGE/bin/osvtool"

# The staged copy must run on its own before it is shipped.
"$STAGE/bin/osvtool" --version >/dev/null || fail "the packaged osvtool does not start"

# ---------------------------------------------------------------------------
#  The Premiere plug-ins, when they were built.
# ---------------------------------------------------------------------------
if [ -n "$PLUGIN_STAGE" ] && [ -d "$PLUGIN_STAGE" ]; then
    count=0
    for b in "$PLUGIN_STAGE"/*.bundle "$PLUGIN_STAGE"/*.plugin; do
        [ -d "$b" ] || continue
        mkdir -p "$STAGE/plugins/OpenOSV"
        ditto "$b" "$STAGE/plugins/OpenOSV/$(basename "$b")"
        count=$((count + 1))
    done
    step "plug-ins: $count bundle(s)"
else
    step "plug-ins: not built (no Adobe SDK on this machine); the zip carries the tools only"
fi

# ---------------------------------------------------------------------------
#  Data, scripts, documentation, licences.
# ---------------------------------------------------------------------------
# The OpenFX bundle for DaVinci Resolve (plugins/ofx) needs no Adobe SDK, so
# every build makes it: ship it with its installer, its guide and the licence
# of the OpenFX headers it is compiled against.
OFX_BUNDLE="$BUILD_DIR/plugins/ofx/OpenOSV.ofx.bundle"
if [ -f "$OFX_BUNDLE/Contents/MacOS/OpenOSV.ofx" ]; then
    mkdir -p "$STAGE/plugins"
    ditto "$OFX_BUNDLE" "$STAGE/plugins/OpenOSV.ofx.bundle"
    cp "$ROOT_DIR/scripts/install_ofx.sh" "$STAGE/scripts/install_ofx.sh"
    chmod +x "$STAGE/scripts/install_ofx.sh"
    cp "$ROOT_DIR/docs/RESOLVE.md" "$STAGE/docs/RESOLVE.md"
    cp "$ROOT_DIR/plugins/ofx/openfx/LICENSE.md" "$STAGE/licenses/OpenFX.txt"
    step "DaVinci Resolve: OpenOSV.ofx.bundle (scripts/install_ofx.sh installs it)"
else
    step "DaVinci Resolve: no OpenOSV.ofx.bundle in $BUILD_DIR (configured with OSV_BUILD_OFX=OFF?)"
fi

step "data and documentation"
cp -R "$ROOT_DIR/luts" "$STAGE/luts"
cp -R "$ROOT_DIR/presets" "$STAGE/presets"
mkdir -p "$STAGE/panel"
for part in shared cep uxp; do
    cp -R "$ROOT_DIR/panel/$part" "$STAGE/panel/$part"
done
cp "$ROOT_DIR/scripts/install_plugins.sh" "$STAGE/scripts/install_plugins.sh"
chmod +x "$STAGE/scripts/install_plugins.sh"
cp "$ROOT_DIR/docs/BUILDING_MAC.md" "$STAGE/docs/BUILDING_MAC.md"
cp "$ROOT_DIR/LICENSE" "$ROOT_DIR/NOTICE" "$STAGE/"
# vcpkg records each port's licence as share/<port>/copyright.
for port_dir in "$VCPKG_INSTALLED"/share/*; do
    if [ -f "$port_dir/copyright" ]; then
        cp "$port_dir/copyright" "$STAGE/licenses/$(basename "$port_dir").txt"
    fi
done

# ---------------------------------------------------------------------------
#  The zip.  ditto keeps bundle symlinks and permissions intact, which a
#  plain `zip -r` does not.
# ---------------------------------------------------------------------------
mkdir -p "$(dirname "$OUT")"
rm -f "$OUT"
ditto -c -k --keepParent "$STAGE" "$OUT"
step "wrote $OUT ($(du -h "$OUT" | cut -f1))"
