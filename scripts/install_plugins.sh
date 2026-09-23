#!/bin/bash
# =============================================================================
#  install_plugins.sh - install (or remove) the OpenOSV Premiere Pro plug-ins
#  on macOS.  The Mac twin of scripts/install_plugins.ps1; read that file's
#  header for the reasoning behind each part, which is the same here.
#
#  Four installations in one script, because they always go together:
#
#  1. THE MODULES.  The staged plug-in bundles - OpenOSVImporter.bundle,
#     Open360Reframe.plugin, OpenOSVSourceSettings.plugin, each carrying its
#     own FFmpeg inside Contents/Frameworks - into
#
#       /Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/OpenOSV/
#
#     which Premiere Pro, Media Encoder and After Effects all scan.  That
#     folder belongs to root, so the copy runs through sudo (you are asked
#     for your password once).  Every installed file loses the quarantine
#     attribute a download carries, and is signed ad hoc: Apple Silicon
#     refuses to load unsigned code, and an ad-hoc signature is what a build
#     from source gets.
#
#     The .cube LUTs go into a LUTs/ folder beside the modules.
#
#  2. THE SEQUENCE PRESETS into the CURRENT user's Premiere settings:
#
#       ~/Documents/Adobe/Premiere Pro/<version>/Profile-<user>/Settings/SequencePresets/OpenOSV/
#
#     newest version first, the Profile-* folder discovered rather than
#     assembled from the account name (presets/README.md).
#
#  3. THE COMPANION PANEL (panel/, docs/PANEL.md), per user:
#       cep  ~/Library/Application Support/Adobe/CEP/extensions/com.openosv.panel
#            plus `defaults write com.adobe.CSXS.<n> PlayerDebugMode 1` (the
#            panel is unsigned); what that changed is recorded and
#            --uninstall puts back exactly the previous values.
#       uxp  a .ccx installed with Adobe's Unified Plugin Installer Agent.
#       auto (default) uxp when the installer agent is present, cep otherwise.
#
#  The per-user parts run as YOU, never under sudo: a path resolved under
#  sudo would be root's home, where nobody opens Premiere.
#
#  Usage:
#    scripts/install_plugins.sh [options]
#
#  Options:
#    --stage-dir DIR          the built plug-ins (default: <root>/plugins/OpenOSV
#                             in a release zip, else the newest
#                             <root>/build/*/plugins/OpenOSV of a source build)
#    --destination DIR        the MediaCore folder to install into
#    --uninstall              remove instead of install
#    --no-presets             skip the sequence presets
#    --preset-dir DIR         where the .sqpreset files are (default <root>/presets)
#    --preset-destination DIR install the presets exactly there
#    --no-panel               skip the companion panel
#    --panel-only             only the panel (no sudo needed)
#    --panel-flavor auto|cep|uxp
#    --panel-destination DIR  dry run: stage the panel there, change nothing else
#    --no-sudo                fail instead of asking for sudo
#    --no-sign                do not ad-hoc sign the installed code
#    --force                  install even while a host application runs
#    -h, --help               this text
#
#  Output is plain ASCII.  Works with the bash 3.2 macOS ships.
# =============================================================================
set -euo pipefail

# ---------------------------------------------------------------------------
#  Constants
# ---------------------------------------------------------------------------
# (A plain array: bash 3.2 cannot mark an array readonly in one statement.)
PLUGIN_BUNDLES=("OpenOSVImporter.bundle" "Open360Reframe.plugin" "OpenOSVSourceSettings.plugin")
readonly DEFAULT_DESTINATION="/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/OpenOSV"
readonly PRESET_GROUP="OpenOSV"
readonly LUT_FOLDER="LUTs"
readonly PANEL_ID="com.openosv.panel"
readonly PANEL_UXP_NAME="OpenOSV"
readonly UPIA="/Library/Application Support/Adobe/Adobe Desktop Common/RemoteComponents/UPI/UnifiedPluginInstallerAgent/UnifiedPluginInstallerAgent.app/Contents/MacOS/UnifiedPluginInstallerAgent"
readonly PANEL_STATE_DIR_DEFAULT="$HOME/Library/Application Support/OpenOSV/panel"

# The repository root (or the root of an unpacked release zip): the folder
# above this script.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly SCRIPT_DIR
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
readonly ROOT_DIR

# ---------------------------------------------------------------------------
#  Options
# ---------------------------------------------------------------------------
STAGE_DIR=""
DESTINATION="$DEFAULT_DESTINATION"
UNINSTALL=0
NO_PRESETS=0
PRESET_DIR=""
PRESET_DESTINATION=""
NO_PANEL=0
PANEL_ONLY=0
PANEL_FLAVOR="auto"
PANEL_DESTINATION=""
NO_SUDO=0
NO_SIGN=0
FORCE=0

# ---------------------------------------------------------------------------
#  Output helpers - one place to change the prefixes.
# ---------------------------------------------------------------------------
step() { printf '==> %s\n' "$*"; }
info() { printf '    %s\n' "$*"; }
warn() { printf 'WARNING: %s\n' "$*" >&2; }
fail() {
    printf '\nERROR: %s\n' "$*" >&2
    exit 1
}

usage() {
    # Print the header comment block (between the first two rule lines).
    sed -n '3,/^# =====/p' "${BASH_SOURCE[0]}" | sed '$d' | sed 's/^# \{0,1\}//'
}

# Every option that takes a value checks that it got one.
need_value() {
    if [ $# -lt 2 ] || [ -z "${2:-}" ]; then
        fail "$1 needs a value (see --help)"
    fi
}

while [ $# -gt 0 ]; do
    case "$1" in
        --stage-dir) need_value "$@"; STAGE_DIR="$2"; shift 2 ;;
        --destination) need_value "$@"; DESTINATION="$2"; shift 2 ;;
        --uninstall) UNINSTALL=1; shift ;;
        --no-presets) NO_PRESETS=1; shift ;;
        --preset-dir) need_value "$@"; PRESET_DIR="$2"; shift 2 ;;
        --preset-destination) need_value "$@"; PRESET_DESTINATION="$2"; shift 2 ;;
        --no-panel) NO_PANEL=1; shift ;;
        --panel-only) PANEL_ONLY=1; shift ;;
        --panel-flavor)
            need_value "$@"
            case "$2" in
                auto|cep|uxp) PANEL_FLAVOR="$2" ;;
                *) fail "--panel-flavor must be auto, cep or uxp (got '$2')" ;;
            esac
            shift 2 ;;
        --panel-destination) need_value "$@"; PANEL_DESTINATION="$2"; shift 2 ;;
        --no-sudo) NO_SUDO=1; shift ;;
        --no-sign) NO_SIGN=1; shift ;;
        --force) FORCE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown option '$1' (see --help)" ;;
    esac
done

if [ "$(uname -s)" != "Darwin" ]; then
    fail "this script installs the macOS plug-ins; on Windows use scripts/install_plugins.ps1"
fi

# ---------------------------------------------------------------------------
#  Privileged commands.  The MediaCore folder belongs to root, so writes to
#  it go through sudo - unless the destination is writable as it is (a dry
#  run into a scratch folder), in which case nothing is elevated at all.
# ---------------------------------------------------------------------------
NEED_SUDO=0
decide_sudo() {
    local probe="$DESTINATION"
    # Walk up to the nearest existing ancestor: that is what mkdir -p would
    # have to write into.
    while [ ! -e "$probe" ] && [ "$probe" != "/" ]; do
        probe="$(dirname "$probe")"
    done
    if [ -w "$probe" ] && { [ ! -e "$DESTINATION" ] || [ -w "$DESTINATION" ]; }; then
        NEED_SUDO=0
        return
    fi
    if [ "$NO_SUDO" -eq 1 ]; then
        fail "'$DESTINATION' needs administrator rights and --no-sudo was given"
    fi
    NEED_SUDO=1
    step "Administrator rights are required"
    info "The plug-ins are installed into a folder owned by root:"
    info "    $DESTINATION"
    info "sudo will ask for your password once. Nothing outside that folder"
    info "is changed with it."
    sudo -v || fail "sudo was refused; nothing was installed"
}

priv() {
    if [ "$NEED_SUDO" -eq 1 ]; then
        sudo "$@"
    else
        "$@"
    fi
}

# ---------------------------------------------------------------------------
#  Refuse to replace a module a host has loaded (a loaded bundle is mapped;
#  replacing it under a running host is how crashes on quit happen).
# ---------------------------------------------------------------------------
assert_hosts_closed() {
    if [ "$FORCE" -eq 1 ]; then
        return
    fi
    local running=""
    local pattern
    for pattern in "Adobe Premiere Pro" "Adobe Media Encoder" "Adobe After Effects"; do
        if pgrep -f "$pattern" >/dev/null 2>&1; then
            running="${running:+$running, }$pattern"
        fi
    done
    if [ -n "$running" ]; then
        fail "Quit $running first: a loaded plug-in must not be replaced under a running host. Pass --force to try anyway."
    fi
}

# ---------------------------------------------------------------------------
#  Where the built bundles are.
# ---------------------------------------------------------------------------
stage_has_bundle() {
    local dir="$1" b
    for b in "${PLUGIN_BUNDLES[@]}"; do
        if [ -d "$dir/$b" ]; then
            return 0
        fi
    done
    return 1
}

find_stage_dir() {
    # A release zip carries them at plugins/OpenOSV.
    if stage_has_bundle "$ROOT_DIR/plugins/OpenOSV"; then
        printf '%s\n' "$ROOT_DIR/plugins/OpenOSV"
        return
    fi
    # A source tree: the newest build/*/plugins/OpenOSV that holds a bundle,
    # ranked by the newest bundle binary inside (a folder's own time stamp
    # does not change when the linker rewrites a file in it).
    local best="" bestTime=0 dir b t
    for dir in "$ROOT_DIR"/build/*/plugins/OpenOSV; do
        [ -d "$dir" ] || continue
        for b in "${PLUGIN_BUNDLES[@]}"; do
            local bin="$dir/$b/Contents/MacOS/${b%.*}"
            if [ -f "$bin" ]; then
                t="$(stat -f %m "$bin" 2>/dev/null || echo 0)"
                if [ "$t" -gt "$bestTime" ]; then
                    bestTime="$t"
                    best="$dir"
                fi
            fi
        done
    done
    if [ -z "$best" ]; then
        fail "No built plug-ins found under $ROOT_DIR/plugins/OpenOSV or $ROOT_DIR/build/*/plugins/OpenOSV. Build them (docs/BUILDING_MAC.md) or pass --stage-dir."
    fi
    printf '%s\n' "$best"
}

# ---------------------------------------------------------------------------
#  Ad-hoc signing: nested code first (the FFmpeg dylibs), then the bundle.
# ---------------------------------------------------------------------------
sign_bundle() {
    local bundle="$1" lib
    if [ "$NO_SIGN" -eq 1 ]; then
        return
    fi
    if [ -d "$bundle/Contents/Frameworks" ]; then
        for lib in "$bundle"/Contents/Frameworks/*.dylib; do
            [ -f "$lib" ] || continue
            priv codesign --force --sign - --timestamp=none "$lib" >/dev/null 2>&1 ||
                warn "could not sign $lib"
        done
    fi
    priv codesign --force --sign - --timestamp=none "$bundle" >/dev/null 2>&1 ||
        warn "could not sign $bundle (Apple Silicon will refuse to load it)"
}

# ---------------------------------------------------------------------------
#  The modules and the LUTs.
# ---------------------------------------------------------------------------
install_modules() {
    if [ -z "$STAGE_DIR" ]; then
        STAGE_DIR="$(find_stage_dir)"
        step "Using the build at $STAGE_DIR"
    fi
    [ -d "$STAGE_DIR" ] || fail "Stage directory '$STAGE_DIR' does not exist."
    stage_has_bundle "$STAGE_DIR" ||
        fail "'$STAGE_DIR' contains none of: ${PLUGIN_BUNDLES[*]}. That is not a plug-in build."

    decide_sudo
    assert_hosts_closed

    step "Installing from $STAGE_DIR"
    info "into $DESTINATION"
    priv mkdir -p "$DESTINATION"

    local b installed=() missing=()
    for b in "${PLUGIN_BUNDLES[@]}"; do
        if [ ! -d "$STAGE_DIR/$b" ]; then
            missing+=("$b")
            continue
        fi
        # Replace, never merge: a file left over from an older build inside a
        # bundle would be loaded along with the new one.
        priv rm -rf "$DESTINATION/$b"
        priv ditto "$STAGE_DIR/$b" "$DESTINATION/$b"
        installed+=("$b")
    done
    if [ "${#missing[@]}" -gt 0 ]; then
        warn "Only part of the set is present; missing: ${missing[*]}. Installed what there is."
    fi

    # A downloaded zip marks every file it unpacks as quarantined; Gatekeeper
    # would then refuse the bundles inside the host without a word.
    priv xattr -dr com.apple.quarantine "$DESTINATION" 2>/dev/null || true
    for b in "${installed[@]}"; do
        sign_bundle "$DESTINATION/$b"
    done
    info "Installed ${#installed[@]} module(s): ${installed[*]}"

    install_luts
    echo
    step "Checking the installation"
    for b in "${installed[@]}"; do
        local bin="$DESTINATION/$b/Contents/MacOS/${b%.*}"
        if [ -f "$bin" ]; then
            info "$(printf '%-30s %s' "$b" "$(lipo -archs "$bin" 2>/dev/null || echo '?')")"
        else
            warn "$b has no executable at Contents/MacOS/${b%.*}"
        fi
    done

    echo
    step "Did Premiere Pro load them?"
    local log=""
    log="$(ls -t "$HOME/Library/Application Support/Adobe/Premiere Pro"/*/"Plugin Loading.log" 2>/dev/null | head -1 || true)"
    if [ -n "$log" ]; then
        info "After the next launch, search this file for OpenOSV:"
        info "    $log"
    else
        info "Premiere Pro writes a plug-in loading log under"
        info "    ~/Library/Application Support/Adobe/Premiere Pro/<version>/"
        info "once it has been started at least once."
    fi
    echo
    step "Hold Shift while Premiere Pro launches"
    info "Premiere caches its plug-in list and does not notice a new or"
    info "replaced module on an ordinary start. Holding Shift from the moment"
    info "you launch it until the splash screen appears forces a full rescan."
}

install_luts() {
    local source="$ROOT_DIR/luts"
    if [ ! -d "$source" ]; then
        info "No luts directory at '$source'; skipping the colour LUTs."
        return
    fi
    local count=0 f
    for f in "$source"/*.cube; do
        [ -f "$f" ] && count=$((count + 1))
    done
    if [ "$count" -eq 0 ]; then
        info "No .cube files in '$source'; skipping the colour LUTs."
        return
    fi
    local target="$DESTINATION/$LUT_FOLDER"
    if ! priv mkdir -p "$target"; then
        warn "could not create $target; the colour LUTs were not installed"
        return
    fi
    for f in "$source"/*.cube; do
        [ -f "$f" ] || continue
        priv cp -f "$f" "$target/" || warn "could not copy $(basename "$f")"
    done
    echo
    step "Installed $count colour LUT(s)"
    info "into $target"
    info "Apply one with Lumetri Color > Creative > Look > Browse..., only on a"
    info "D-Log M PASSTHROUGH output - never on top of a PQ / HLG / 709 output."
}

uninstall_modules() {
    decide_sudo
    assert_hosts_closed
    step "Removing $DESTINATION"
    if [ -d "$DESTINATION" ]; then
        priv rm -rf "$DESTINATION"
        info "Removed."
    else
        info "Nothing to remove: the folder does not exist."
    fi
    echo
    step "Next time you start Premiere Pro"
    info "Hold Shift while it launches to force a full plug-in rescan."
}

# ---------------------------------------------------------------------------
#  Sequence presets (per user; never fatal).
# ---------------------------------------------------------------------------
find_preset_destination() {
    local root="$HOME/Documents/Adobe/Premiere Pro"
    [ -d "$root" ] || return 0
    # Newest numeric version first ("26.0" before "9.0": sort as numbers).
    local version profile
    for version in $(ls -1 "$root" 2>/dev/null | grep -E '^[0-9]+(\.[0-9]+)?$' | sort -t. -k1,1nr -k2,2nr); do
        for profile in "$root/$version"/Profile-*; do
            if [ -d "$profile/Settings" ]; then
                printf '%s\n' "$profile/Settings/SequencePresets/$PRESET_GROUP"
                return 0
            fi
        done
    done
    return 0
}

install_presets() {
    local source="${PRESET_DIR:-$ROOT_DIR/presets}"
    if [ ! -d "$source" ]; then
        warn "No presets directory at '$source'; skipping the sequence presets."
        return
    fi
    local target="$PRESET_DESTINATION"
    if [ -z "$target" ]; then
        target="$(find_preset_destination)"
    fi
    if [ -z "$target" ]; then
        warn "Could not find a Premiere Pro settings folder for this user, so the"
        warn "sequence presets were NOT installed. Expected a folder like:"
        warn "    ~/Documents/Adobe/Premiere Pro/<version>/Profile-<user>/Settings"
        warn "Start Premiere Pro once to create it, then re-run this script - or"
        warn "pass --preset-destination DIR."
        return
    fi
    if ! mkdir -p "$target"; then
        warn "could not create '$target'; the sequence presets were not installed"
        return
    fi
    local count=0 f
    for f in "$source"/*.sqpreset; do
        [ -f "$f" ] || continue
        if cp -f "$f" "$target/"; then
            count=$((count + 1))
        else
            warn "could not copy $(basename "$f")"
        fi
    done
    step "Installed $count sequence preset(s)"
    info "into $target"
    info "They appear in File > New > Sequence under the \"$PRESET_GROUP\" group."
    info "Premiere caches the preset list, so RESTART it if it is running."
}

uninstall_presets() {
    local target="$PRESET_DESTINATION"
    if [ -z "$target" ]; then
        target="$(find_preset_destination)"
    fi
    if [ -z "$target" ] || [ ! -d "$target" ]; then
        info "No preset folder found; nothing to remove."
        return
    fi
    # Only ever OUR group folder, never SequencePresets itself.
    if rm -rf "$target"; then
        step "Removed the sequence presets from $target"
    else
        warn "could not remove '$target'"
    fi
}

# ---------------------------------------------------------------------------
#  The companion panel (per user; never fatal unless --panel-only).
# ---------------------------------------------------------------------------
panel_state_dir() {
    if [ -n "$PANEL_DESTINATION" ]; then
        printf '%s\n' "$PANEL_DESTINATION/OpenOSV-panel-state"
    else
        printf '%s\n' "$PANEL_STATE_DIR_DEFAULT"
    fi
}

cep_extensions_dir() {
    if [ -n "$PANEL_DESTINATION" ]; then
        printf '%s\n' "$PANEL_DESTINATION"
    else
        printf '%s\n' "$HOME/Library/Application Support/Adobe/CEP/extensions"
    fi
}

panel_version() {
    # "version": "1.2.3" from the UXP manifest (a test keeps both manifests
    # and the panel's own constant equal).
    sed -n 's/.*"version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$ROOT_DIR/panel/uxp/manifest.json" | head -1
}

# Stage one flavour: panel/shared -> <target>/shared, panel/<flavour>/* -> <target>.
stage_panel() {
    local flavor="$1" target="$2"
    local shared="$ROOT_DIR/panel/shared" own="$ROOT_DIR/panel/$flavor"
    local required
    for required in "$shared/osvcore.js" "$own/index.html" "$own/main.js"; do
        [ -f "$required" ] || { warn "the panel sources are incomplete: '$required' is missing"; return 1; }
    done
    rm -rf "$target"
    mkdir -p "$target/shared"
    cp -R "$own"/. "$target"/
    cp "$shared"/* "$target/shared/" 2>/dev/null || true
    return 0
}

# CEP major versions of the installed Premiere Pro builds (CSXS.<n>), read
# from each app's PlugPlug framework; 12 (Premiere 24-26) when none is found.
premiere_csxs_versions() {
    local found="" plist v app
    for app in /Applications/Adobe\ Premiere\ Pro*/*.app; do
        [ -d "$app" ] || continue
        for plist in "$app"/Contents/Frameworks/PlugPlug*.framework/Resources/Info.plist \
                     "$app"/Contents/Frameworks/PlugPlug*.framework/Versions/*/Resources/Info.plist; do
            [ -f "$plist" ] || continue
            v="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$plist" 2>/dev/null |
                 cut -d. -f1 || true)"
            case "$v" in
                ''|*[!0-9]*) continue ;;
            esac
            if [ "$v" -ge 9 ]; then
                found="$found $v"
            fi
        done
    done
    found="$(printf '%s\n' $found | sort -un | tr '\n' ' ')"
    if [ -z "${found// /}" ]; then
        info "No installed Premiere Pro found to read its CEP version from; assuming CEP 12." >&2
        found="12"
    fi
    printf '%s\n' "$found"
}

# Record file: one line per changed CSXS domain, "<n> <previous>", where
# <previous> is the value before the install or the word ABSENT.
panel_record_file() { printf '%s\n' "$(panel_state_dir)/csxs-changes"; }

enable_player_debug_mode() {
    local record v previous
    record="$(panel_record_file)"
    mkdir -p "$(dirname "$record")"
    touch "$record"
    for v in $(premiere_csxs_versions); do
        previous="$(defaults read "com.adobe.CSXS.$v" PlayerDebugMode 2>/dev/null || echo ABSENT)"
        if [ "$previous" = "1" ]; then
            info "com.adobe.CSXS.$v PlayerDebugMode is already 1; left as it is."
            continue
        fi
        defaults write "com.adobe.CSXS.$v" PlayerDebugMode 1
        info "Set com.adobe.CSXS.$v PlayerDebugMode = 1."
        # The FIRST recorded value is the user's own: keep it over reinstalls.
        if ! grep -q "^$v " "$record"; then
            printf '%s %s\n' "$v" "$previous" >>"$record"
        fi
    done
}

restore_player_debug_mode() {
    local record v previous
    record="$(panel_record_file)"
    [ -f "$record" ] || return 0
    while read -r v previous; do
        case "$v" in
            ''|*[!0-9]*) continue ;;
        esac
        if [ "$previous" = "ABSENT" ]; then
            defaults delete "com.adobe.CSXS.$v" PlayerDebugMode 2>/dev/null || true
            info "Removed the PlayerDebugMode this install set in com.adobe.CSXS.$v."
        else
            defaults write "com.adobe.CSXS.$v" PlayerDebugMode "$previous"
            info "Put com.adobe.CSXS.$v PlayerDebugMode back to '$previous'."
        fi
    done <"$record"
}

install_panel_cep() {
    local version="$1" target
    target="$(cep_extensions_dir)/$PANEL_ID"
    stage_panel cep "$target" || return 1
    xattr -dr com.apple.quarantine "$target" 2>/dev/null || true
    step "Installed the OpenOSV panel (CEP) $version"
    info "into $target"
    if [ -n "$PANEL_DESTINATION" ]; then
        info "Dry run (--panel-destination): the preferences were not touched."
    else
        enable_player_debug_mode
        info "PlayerDebugMode lets Premiere load a panel that is not signed."
        info "It is per user and only affects CEP panels; --uninstall undoes it."
        printf 'cep\n' >"$(panel_state_dir)/flavor"
    fi
    info "Open it from Window > Extensions > OpenOSV (restart Premiere first)."
    return 0
}

install_panel_uxp() {
    local version="$1" work ccx
    work="$(mktemp -d "${TMPDIR:-/tmp}/OpenOSV-panel.XXXXXX")"
    stage_panel uxp "$work/uxp" || { rm -rf "$work"; return 1; }
    mkdir -p "$(panel_state_dir)"
    ccx="$(panel_state_dir)/OpenOSV-panel-$version.ccx"
    rm -f "$ccx"
    # A .ccx is a ZIP with manifest.json at its root.
    (cd "$work/uxp" && zip -qrX "$ccx" .) || { rm -rf "$work"; warn "could not pack the .ccx"; return 1; }
    rm -rf "$work"
    if [ -n "$PANEL_DESTINATION" ]; then
        step "Built the OpenOSV panel (UXP) installer"
        info "    $ccx"
        info "Dry run (--panel-destination): nothing was installed."
        return 0
    fi
    if [ ! -x "$UPIA" ]; then
        warn "Adobe's plug-in installer agent was not found, so the UXP panel was NOT installed."
        warn "Double-click $ccx (Creative Cloud installs it), or use --panel-flavor cep."
        return 1
    fi
    step "Installing the OpenOSV panel (UXP) with Adobe's plug-in installer"
    if ! "$UPIA" --install "$ccx"; then
        warn "the plug-in installer agent reported a failure"
        return 1
    fi
    # One panel at a time: drop a CEP copy from an earlier install.
    rm -rf "$(cep_extensions_dir)/$PANEL_ID"
    printf 'uxp\n' >"$(panel_state_dir)/flavor"
    info "Open it from Window > UXP Plugins > OpenOSV (restart Premiere first)."
    return 0
}

install_panel() {
    local version flavor="$PANEL_FLAVOR"
    version="$(panel_version)"
    [ -n "$version" ] || { warn "no version in panel/uxp/manifest.json; the panel was not installed"; return 1; }
    mkdir -p "$(panel_state_dir)"
    if [ "$flavor" = "auto" ]; then
        if [ -x "$UPIA" ] && [ -z "$PANEL_DESTINATION" ]; then
            flavor="uxp"
            info "Adobe's plug-in installer is present: installing the UXP build."
        else
            flavor="cep"
        fi
    fi
    if [ "$flavor" = "uxp" ]; then
        if install_panel_uxp "$version"; then
            return 0
        fi
        if [ "$PANEL_FLAVOR" = "auto" ]; then
            info "Falling back to the CEP build."
            install_panel_cep "$version"
            return $?
        fi
        return 1
    fi
    install_panel_cep "$version"
}

uninstall_panel() {
    step "Removing the OpenOSV panel"
    rm -rf "$(cep_extensions_dir)/$PANEL_ID"
    if [ -z "$PANEL_DESTINATION" ]; then
        restore_player_debug_mode
        if [ "$(cat "$(panel_state_dir)/flavor" 2>/dev/null || true)" = "uxp" ]; then
            if [ -x "$UPIA" ]; then
                "$UPIA" --remove "$PANEL_UXP_NAME" || warn "the plug-in installer agent could not remove the UXP panel"
            else
                warn "Remove the UXP panel in the Creative Cloud app (Stock & Marketplace > Plugins > Manage plugins)."
            fi
        fi
    fi
    rm -rf "$(panel_state_dir)"
    info "Done. Restart Premiere Pro if it is running."
}

# ===========================================================================
#  Main
# ===========================================================================
if [ "$PANEL_ONLY" -eq 1 ]; then
    if [ "$UNINSTALL" -eq 1 ]; then
        uninstall_panel
    else
        install_panel
    fi
    exit $?
fi

# The per-user parts first, as the calling user (see the header).
if [ "$NO_PRESETS" -eq 0 ]; then
    if [ "$UNINSTALL" -eq 1 ]; then uninstall_presets; else install_presets; fi
    echo
fi
if [ "$NO_PANEL" -eq 0 ]; then
    if [ "$UNINSTALL" -eq 1 ]; then
        uninstall_panel || true
    else
        install_panel || warn "the panel was not installed (the plug-ins are unaffected)"
    fi
    echo
fi

if [ "$UNINSTALL" -eq 1 ]; then
    uninstall_modules
else
    install_modules
fi
exit 0
