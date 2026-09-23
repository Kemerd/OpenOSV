#Requires -Version 5.1
<#
.SYNOPSIS
    Installs (or removes) the OpenOSV Premiere Pro plug-ins.

.DESCRIPTION
    Two installations in one script, because they always go together.

    1. THE MODULES.  The staged plug-in folder - OpenOSVImporter.prm,
       Open360Reframe.aex, OpenOSVSourceSettings.aex and the runtime DLLs the
       build put next to them - into

           C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\OpenOSV\

       That folder is scanned by Premiere Pro, Media Encoder and After Effects
       alike, which is why one copy serves all three (docs/PREMIERE.md).  It
       lives under Program Files, so this needs an elevated session; when the
       script is not elevated it explains why and relaunches itself with the
       same arguments through UAC.

       The .cube LUTs from <repo>\luts are copied into a LUTs\ subfolder of
       that same directory, so Lumetri has a stable path to browse to and
       -Uninstall removes them with everything else.  They are build output
       (scripts\gen_luts.ps1); a checkout where that has never been run just
       gets a note and no LUTs.

    2. THE SEQUENCE PRESETS.  The three .sqpreset files in <repo>\presets into
       the CURRENT USER's Premiere settings folder:

           %USERPROFILE%\Documents\Adobe\Premiere Pro\<ver>\Profile-<user>\Settings\SequencePresets\OpenOSV\

       They then appear in File > New > Sequence under a group called OpenOSV.
       See presets\README.md for how that path was established from the
       installed application (it is not documented by Adobe).

       This half is deliberately NOT done by the elevated child process.  A
       per-user path resolved inside an elevated session belongs to whichever
       account answered the UAC prompt, which on a machine with a separate
       admin account is not the person editing video - the presets would land
       in a profile nobody opens.  So the parent (unelevated) process installs
       the presets before handing the modules to the elevated child, and the
       child is told to skip them.  Pass -NoPresets to skip them entirely.

    3. THE COMPANION PANEL "OpenOSV" (panel\, docs\PANEL.md).  It puts Open
       360 Reframe on every .OSV / .LRF clip dropped on a timeline.  Per user,
       and done by the unelevated parent for the same reason as the presets.
       Two builds of one panel; -PanelFlavor picks (Auto by default):

         Uxp - Premiere 25.6+.  Packed as a .ccx (a ZIP with manifest.json at
               its root) and installed with Adobe's Unified Plugin Installer
               Agent (UPIA), which ships with the Creative Cloud app.  Without
               UPIA the .ccx is handed to whatever opens .ccx files (Creative
               Cloud's installer asks for one confirmation).
         Cep - Premiere 22 and later, while Premiere still runs CEP.  Copied
               to %APPDATA%\Adobe\CEP\extensions\com.openosv.panel.  It is
               unsigned, so the per-user switch PlayerDebugMode = "1" is set
               under HKCU\Software\Adobe\CSXS.<n> for the CEP version of each
               installed Premiere - only where it was not set already, and
               -Uninstall puts back exactly what it changed.
         Auto - Uxp when UPIA is present, Cep otherwise: whichever installs
               with no clicks.

       Pass -NoPanel to skip it, -PanelOnly to do nothing else.

.PARAMETER StageDir
    The folder produced by the build (OSV_PLUGIN_STAGE_DIR, by default
    <build>\plugins\OpenOSV).  When omitted the script looks for the newest
    plugins\OpenOSV under <repo>\build\*.

.PARAMETER Destination
    Overrides the MediaCore folder.  Mostly useful for a dry run into a
    scratch directory, or for a non-default Adobe installation.

.PARAMETER Uninstall
    Removes the installed folder instead of copying into it.  -StageDir is
    then ignored.  The sequence presets are removed too, unless -NoPresets.

.PARAMETER NoPresets
    Skips the sequence presets and installs (or removes) only the modules.

.PARAMETER PresetDir
    The folder holding the .sqpreset sources.  Defaults to <repo>\presets.

.PARAMETER PresetDestination
    Overrides where the presets are installed.  When given, the script writes
    exactly there and does not go looking for a Premiere profile - useful for
    a dry run, for a second Premiere version, or when the profile discovery
    below cannot find a settings root.

.PARAMETER NoPanel
    Skips the companion panel.

.PARAMETER PanelOnly
    Installs (or, with -Uninstall, removes) only the companion panel.  Needs
    no administrator rights and leaves the modules and presets alone.

.PARAMETER PanelFlavor
    Auto (default), Uxp or Cep.  See item 3 above.

.PARAMETER PanelDestination
    A dry run for the panel: stage it into this folder (the CEP extension
    folder, or the .ccx for Uxp) without registering anything - no UPIA, no
    registry change.

.PARAMETER NoElevate
    Fails with an explanation instead of relaunching through UAC.  Use it from
    a build script that must not pop a dialog.

.PARAMETER Force
    Copies even when a host application is running.  The copy will simply fail
    on whichever module the host holds open, so this is only useful when you
    know the plug-in was never loaded.

.EXAMPLE
    scripts\install_plugins.ps1
    Installs from the newest build folder.

.EXAMPLE
    scripts\install_plugins.ps1 -StageDir build\windows-msvc-premiere-release\plugins\OpenOSV

.EXAMPLE
    scripts\install_plugins.ps1 -Uninstall

.EXAMPLE
    scripts\install_plugins.ps1 -NoPresets
    Installs only the three modules, leaving the sequence presets alone.

.EXAMPLE
    scripts\install_plugins.ps1 -PresetDestination C:\temp\presets
    Installs the modules normally and the presets into a scratch folder.

.EXAMPLE
    scripts\install_plugins.ps1 -PanelOnly
    Installs just the companion panel, no administrator rights needed.

.EXAMPLE
    scripts\install_plugins.ps1 -PanelOnly -PanelFlavor Uxp
    Installs the UXP build of the panel (Premiere 25.6+).

.NOTES
    Output is deliberately plain ASCII: the Windows console still defaults to
    a legacy code page on many machines and a box-drawing character there is
    worse than useless.
#>
[CmdletBinding()]
param(
    [string] $StageDir,
    [string] $Destination = "$env:ProgramFiles\Adobe\Common\Plug-ins\7.0\MediaCore\OpenOSV",
    [switch] $Uninstall,
    [switch] $NoElevate,
    [switch] $Force,
    [switch] $NoPresets,
    [string] $PresetDir,
    [string] $PresetDestination,
    [switch] $NoPanel,
    [switch] $PanelOnly,
    [ValidateSet('Auto', 'Uxp', 'Cep')]
    [string] $PanelFlavor = 'Auto',
    [string] $PanelDestination
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Plain ASCII output on a legacy code page as well as on UTF-8.
$OutputEncoding = [System.Text.Encoding]::ASCII

# The three modules that make up the installation.  A stage directory without
# any of them is a build that never produced a plug-in.
#
# OpenOSVSourceSettings.aex is a separate module from Open360Reframe.aex, and
# has to be: the AE SDK lists "multiple PiPLs in a single plug-in" among the
# features Premiere does not support.  Without it installed, the importer
# still works and the modal Source Settings dialog still opens, but the
# Effect Controls panel shows no stitch options - the importer advertises a
# match name that resolves to nothing.
$script:PluginFiles = @('OpenOSVImporter.prm', 'Open360Reframe.aex', 'OpenOSVSourceSettings.aex')

# The subfolder created under Premiere's SequencePresets directory.  Its name
# is the group heading the New Sequence dialog shows, which is how Adobe's own
# presets are grouped (HD 1080p, UHD (4K), Social, Legacy).
$script:PresetGroupName = 'OpenOSV'

# The subfolder the .cube LUTs land in, beside the modules:
#
#     ...\MediaCore\OpenOSV\LUTs\
#
# Deliberately NOT one of Premiere's own Lumetri LUT folders.  Those are
# per-user and per-version, and dropping files into them makes our tables
# indistinguishable from Adobe's in the Creative > Look dropdown, which is a
# poor trade for saving the user one "Browse...".  Keeping them in our own
# installed folder means: one copy for all three hosts, they are removed with
# -Uninstall along with everything else, and the path is stable enough to
# document (docs/COLOR.md tells the user to point Lumetri here).
$script:LutFolderName = 'LUTs'

# ---------------------------------------------------------------------------
#  Small output helpers - one place to change the prefixes.
# ---------------------------------------------------------------------------
function Write-Step { param([string] $Message) Write-Host "==> $Message" }
function Write-Info { param([string] $Message) Write-Host "    $Message" }
function Write-Warn { param([string] $Message) Write-Warning $Message }

# ---------------------------------------------------------------------------
#  True when the current process has the Administrators group in its token.
# ---------------------------------------------------------------------------
function Test-Elevated {
    $identity  = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# ---------------------------------------------------------------------------
#  Relaunch this script elevated, passing the arguments through unchanged.
#  Returns the child's exit code; throws when the user declines the prompt.
# ---------------------------------------------------------------------------
function Invoke-Elevated {
    $arguments = @(
        '-NoProfile'
        '-ExecutionPolicy', 'Bypass'
        '-File', ('"{0}"' -f $PSCommandPath)
    )
    if ($StageDir)    { $arguments += @('-StageDir',    ('"{0}"' -f $StageDir)) }
    if ($Destination) { $arguments += @('-Destination', ('"{0}"' -f $Destination)) }
    if ($Uninstall)   { $arguments += '-Uninstall' }
    if ($Force)       { $arguments += '-Force' }
    # ALWAYS -NoPresets for the child.  The parent has already installed (or
    # removed) them into the calling user's own profile; letting the elevated
    # child do it again would resolve %USERPROFILE% to whichever account
    # answered the UAC prompt and drop the presets in a profile nobody opens.
    $arguments += '-NoPresets'
    # The companion panel is per-user too (%APPDATA%, HKCU): same reason.
    $arguments += '-NoPanel'

    Write-Step 'Administrator rights are required'
    Write-Info "The plug-ins are installed into a folder under Program Files:"
    Write-Info "    $Destination"
    Write-Info 'Only an elevated process may write there, so this script is'
    Write-Info 'restarting itself and Windows will show a UAC prompt now.'
    Write-Info 'Nothing else about the machine is touched: the script only'
    Write-Info 'copies files into that one folder.'

    $process = Start-Process -FilePath (Get-Process -Id $PID).Path `
                             -ArgumentList $arguments -Verb RunAs -Wait -PassThru
    return $process.ExitCode
}

# ---------------------------------------------------------------------------
#  Find the stage directory when the caller did not name one: the newest
#  <repo>\build\*\plugins\OpenOSV that actually holds a plug-in.
# ---------------------------------------------------------------------------
function Find-StageDir {
    $repoRoot = Split-Path -Parent $PSScriptRoot
    $buildRoot = Join-Path $repoRoot 'build'
    if (-not (Test-Path -LiteralPath $buildRoot)) {
        throw "No -StageDir given and '$buildRoot' does not exist. Build the plug-ins first (see docs/BUILDING.md) or pass -StageDir."
    }

    # Ranked by the newest MODULE inside each folder, not by the folder's own
    # timestamp.  A directory's LastWriteTime changes only when an entry is
    # added, removed or renamed in it - and the linker overwrites a module IN
    # PLACE - so a freshly rebuilt folder keeps an old timestamp while a
    # folder created later by some other build looks newer.  That once
    # installed a stale build from a scratch directory over a verified one.
    $candidates = Get-ChildItem -LiteralPath $buildRoot -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName 'plugins\OpenOSV' } |
        Where-Object { Test-Path -LiteralPath $_ } |
        Where-Object {
            # Only a folder that really contains a module counts.
            $folder = $_
            $script:PluginFiles | Where-Object { Test-Path -LiteralPath (Join-Path $folder $_) }
        } |
        Sort-Object {
            # A plain sort rather than Measure-Object -Maximum, which Windows
            # PowerShell 5.1 refuses for non-numeric values like DateTime.
            $folder = $_
            $script:PluginFiles |
                ForEach-Object { Join-Path $folder $_ } |
                Where-Object { Test-Path -LiteralPath $_ } |
                ForEach-Object { (Get-Item -LiteralPath $_).LastWriteTimeUtc } |
                Sort-Object -Descending |
                Select-Object -First 1
        } -Descending

    if (-not $candidates) {
        throw "No built plug-ins found under '$buildRoot'. Configure with -DOSV_BUILD_PREMIERE=ON, build, then run this script again (or pass -StageDir)."
    }
    return [string]($candidates | Select-Object -First 1)
}

# ---------------------------------------------------------------------------
#  Reject a stage directory that is not what we expect before anything is
#  copied into Program Files.
# ---------------------------------------------------------------------------
function Test-StageDir {
    param([string] $Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "Stage directory '$Path' does not exist."
    }
    $found = @($script:PluginFiles | Where-Object { Test-Path -LiteralPath (Join-Path $Path $_) })
    if ($found.Count -eq 0) {
        throw "Stage directory '$Path' contains none of: $($script:PluginFiles -join ', '). That is not a plug-in build."
    }
    if ($found.Count -lt $script:PluginFiles.Count) {
        $missing = $script:PluginFiles | Where-Object { $found -notcontains $_ }
        Write-Warn "Only part of the set is present; missing: $($missing -join ', '). Installing what there is."
    }
    return $found
}

# ---------------------------------------------------------------------------
#  Where Premiere Pro records which plug-ins it loaded, newest version first.
#  Returns $null when no such file exists yet.
# ---------------------------------------------------------------------------
function Get-PluginLoadingLog {
    $root = Join-Path $env:APPDATA 'Adobe\Premiere Pro'
    if (-not (Test-Path -LiteralPath $root)) {
        return $null
    }
    $log = Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName 'Plugin Loading.log' } |
        Where-Object { Test-Path -LiteralPath $_ } |
        Select-Object -First 1
    return $log
}

# ---------------------------------------------------------------------------
#  Refuse to overwrite a module a host still has loaded: a locked file only
#  produces a confusing error halfway through the copy.  The check applies to
#  the real MediaCore folder only - a copy into a scratch directory cannot
#  collide with anything - and -Force skips it.
# ---------------------------------------------------------------------------
function Assert-HostsClosed {
    param([string] $Path)

    if ($Force) {
        return
    }
    if ($Path -notmatch '(?i)\\MediaCore\\') {
        return
    }
    $running = Get-Process -Name 'Adobe Premiere Pro', 'Adobe Media Encoder', 'AfterFX' -ErrorAction SilentlyContinue
    if ($running) {
        $names = ($running | Select-Object -ExpandProperty ProcessName -Unique) -join ', '
        throw "Close $names first: a loaded plug-in file cannot be replaced while the host holds it open. Pass -Force to try anyway."
    }
}

# ---------------------------------------------------------------------------
#  Colour LUTs
#
#  The .cube files in <repo>\luts are copied into <Destination>\LUTs so a user
#  can point Lumetri at a stable path instead of hunting through a build tree.
#
#  This runs in the ELEVATED half, unlike the sequence presets: the target is
#  under Program Files next to the modules, is machine-wide rather than
#  per-user, and is removed by -Uninstall with the rest of the folder.
#
#  Never fatal, for the same reason the presets are not: the modules are the
#  installation that matters, and a missing luts\ directory (a source checkout
#  where scripts\gen_luts.ps1 has not been run) should print one line, not
#  fail the install.
# ---------------------------------------------------------------------------
function Install-Luts {
    param([string] $Path)

    $source = Join-Path (Split-Path -Parent $PSScriptRoot) 'luts'
    if (-not (Test-Path -LiteralPath $source -PathType Container)) {
        Write-Info "No luts directory at '$source'; skipping the colour LUTs."
        return
    }
    $files = @(Get-ChildItem -LiteralPath $source -File -Filter '*.cube' -ErrorAction SilentlyContinue)
    if ($files.Count -eq 0) {
        Write-Info "No .cube files in '$source'; skipping the colour LUTs."
        Write-Info 'Run scripts\gen_luts.ps1 to generate them.'
        return
    }

    $target = Join-Path $Path $script:LutFolderName
    try {
        if (-not (Test-Path -LiteralPath $target)) {
            New-Item -ItemType Directory -Path $target -Force | Out-Null
        }
        foreach ($file in $files) {
            Copy-Item -LiteralPath $file.FullName -Destination $target -Force
        }
        Write-Step "Installed $($files.Count) colour LUT(s)"
        Write-Info "into $target"
        foreach ($file in $files) {
            Write-Info "    $($file.Name)"
        }
        Write-Info 'Apply one with Lumetri Color > Creative > Look > Browse...,'
        Write-Info 'or Basic Correction > Input LUT > Browse..., and point it there.'
        Write-Info 'Only on a D-Log M PASSTHROUGH output - never on top of a'
        Write-Info 'PQ / HLG / 709 output, which is already converted.'
    }
    catch {
        Write-Warn "Could not install the colour LUTs into '$target': $($_.Exception.Message)"
    }
}

# ---------------------------------------------------------------------------
#  Sequence presets
# ---------------------------------------------------------------------------

# Where the .sqpreset sources live (<repo>\presets unless overridden).
function Get-PresetSourceDir {
    if ($PresetDir) {
        return $PresetDir
    }
    return (Join-Path (Split-Path -Parent $PSScriptRoot) 'presets')
}

# ---------------------------------------------------------------------------
#  Find the current user's Premiere sequence-preset folder.
#
#  The layout, established from the installed application rather than from
#  documentation (presets\README.md records the evidence):
#
#      Documents\Adobe\Premiere Pro\<version>\Profile-<user>\Settings\SequencePresets\
#
#  Three things make this discovery rather than a guess:
#
#    * the <version> folder is enumerated and the NEWEST numeric one wins, so
#      a machine with 24.0 and 26.0 installed gets 26.0 without the version
#      being hard-coded here;
#    * the Profile-* folder is enumerated too, because its suffix is the
#      account name and must never be assembled from $env:USERNAME (a domain
#      account, a renamed profile or a Creative Cloud profile all break that);
#    * when nothing matches, the function returns $null and the caller says so
#      and skips, instead of inventing a path and reporting success.
#
#  SequencePresets\ itself may legitimately not exist yet - Premiere creates it
#  the first time a user saves a preset - so only the Settings folder has to be
#  there for the location to count as found.
# ---------------------------------------------------------------------------
function Find-PresetDestination {
    $root = Join-Path ([Environment]::GetFolderPath('MyDocuments')) 'Adobe\Premiere Pro'
    if (-not (Test-Path -LiteralPath $root -PathType Container)) {
        return $null
    }

    # Newest version first.  Sorting on the parsed number, not on the string,
    # so "9.0" does not outrank "26.0".
    $versionDirs = Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^\d+(\.\d+)?$' } |
        Sort-Object { [double]$_.Name } -Descending

    foreach ($versionDir in $versionDirs) {
        $profileDirs = Get-ChildItem -LiteralPath $versionDir.FullName -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -like 'Profile-*' }
        foreach ($profileDir in $profileDirs) {
            $settings = Join-Path $profileDir.FullName 'Settings'
            if (Test-Path -LiteralPath $settings -PathType Container) {
                return (Join-Path $settings ('SequencePresets\{0}' -f $script:PresetGroupName))
            }
        }
    }
    return $null
}

# ---------------------------------------------------------------------------
#  Copy the .sqpreset files into the user's preset folder.
#
#  Never fatal.  A missing presets directory, an undiscoverable profile or a
#  failed copy each print one clear line and leave the exit code alone: the
#  modules are the installation that matters, and failing the whole script
#  because a convenience preset could not be placed would be the wrong
#  trade.
# ---------------------------------------------------------------------------
function Install-Presets {
    $source = Get-PresetSourceDir
    if (-not (Test-Path -LiteralPath $source -PathType Container)) {
        Write-Warn "No presets directory at '$source'; skipping the sequence presets."
        return
    }
    $files = @(Get-ChildItem -LiteralPath $source -File -Filter '*.sqpreset' -ErrorAction SilentlyContinue)
    if ($files.Count -eq 0) {
        Write-Warn "No .sqpreset files in '$source'; skipping the sequence presets."
        return
    }

    $target = if ($PresetDestination) { $PresetDestination } else { Find-PresetDestination }
    if (-not $target) {
        Write-Warn 'Could not find a Premiere Pro settings folder for this user, so the'
        Write-Warn 'sequence presets were NOT installed. Expected a folder like:'
        Write-Warn '    Documents\Adobe\Premiere Pro\<version>\Profile-<user>\Settings'
        Write-Warn 'Start Premiere Pro once to create it, then re-run this script - or'
        Write-Warn 'pass -PresetDestination <dir> to install them somewhere specific.'
        return
    }

    try {
        if (-not (Test-Path -LiteralPath $target)) {
            New-Item -ItemType Directory -Path $target -Force | Out-Null
        }
        foreach ($file in $files) {
            Copy-Item -LiteralPath $file.FullName -Destination $target -Force
        }
        Write-Step "Installed $($files.Count) sequence preset(s)"
        Write-Info "into $target"
        foreach ($file in $files) {
            Write-Info "    $($file.Name)"
        }
        Write-Info 'They appear in File > New > Sequence under the "OpenOSV" group.'
        Write-Info 'Premiere caches the preset list, so RESTART it if it is running.'
    }
    catch {
        Write-Warn "Could not install the sequence presets into '$target': $($_.Exception.Message)"
    }
}

# ---------------------------------------------------------------------------
#  Remove the installed preset group.  Only ever removes OUR subfolder, never
#  the SequencePresets directory itself - that one may hold presets the user
#  saved by hand.
# ---------------------------------------------------------------------------
function Uninstall-Presets {
    $target = if ($PresetDestination) { $PresetDestination } else { Find-PresetDestination }
    if (-not $target) {
        Write-Info 'No Premiere Pro settings folder found; no presets to remove.'
        return
    }
    if (-not (Test-Path -LiteralPath $target)) {
        Write-Info "No preset folder at '$target'; nothing to remove."
        return
    }
    try {
        Remove-Item -LiteralPath $target -Recurse -Force
        Write-Step "Removed the sequence presets from $target"
    }
    catch {
        Write-Warn "Could not remove '$target': $($_.Exception.Message)"
    }
}

# ===========================================================================
#  [WP-PANEL] The companion panel "OpenOSV"                      (begin)
#
#  Everything the panel needs lives between this banner and its (end)
#  banner; Main calls Install-Panel / Uninstall-Panel and nothing else.
#  docs/PANEL.md explains the two flavours and the evidence behind them.
#
#  Like the presets, the panel is never fatal to the plug-in install: every
#  failure prints what went wrong and what to do by hand, and the modules
#  are still installed.  Only -PanelOnly reports a failure in the exit code.
# ===========================================================================

# The CEP bundle id and the UXP plug-in id (the same string on purpose).
$script:PanelId = 'com.openosv.panel'

# The UXP manifest's "name": what UPIA's /remove takes.
$script:PanelUxpName = 'OpenOSV'

# Where CEP reads PlayerDebugMode: HKCU\Software\Adobe\CSXS.<n>.
$script:PanelCsxsRoot = 'HKCU:\Software\Adobe'

# Where the .ccx and the record of what the install changed are kept.  A
# -PanelDestination dry run keeps them inside that folder instead.
function Get-PanelStateDir {
    if ($PanelDestination) {
        return (Join-Path $PanelDestination 'OpenOSV-panel-state')
    }
    $base = $env:LOCALAPPDATA
    if (-not $base) {
        $base = Join-Path $env:USERPROFILE 'AppData\Local'
    }
    return (Join-Path $base 'OpenOSV\panel')
}

function Get-PanelStateFile { Join-Path (Get-PanelStateDir) 'install-state.json' }

function Get-PanelSourceDir { Join-Path (Split-Path -Parent $PSScriptRoot) 'panel' }

# A property of a parsed JSON object, or $null - StrictMode forbids reading
# a property that is not there.
function Get-PanelProperty {
    param($Object, [string] $Name)
    if ($null -ne $Object -and $Object.PSObject.Properties[$Name]) {
        return $Object.$Name
    }
    return $null
}

# The panel version, from the UXP manifest (a test keeps both manifests and
# OsvCore.PANEL_VERSION equal).
function Get-PanelVersion {
    $manifest = Join-Path (Get-PanelSourceDir) 'uxp\manifest.json'
    $parsed = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
    $version = Get-PanelProperty $parsed 'version'
    if (-not $version) {
        throw "No version in '$manifest'."
    }
    return [string]$version
}

# Adobe's Unified Plugin Installer Agent, at the path Adobe documents
# (developer.adobe.com/premiere-pro/uxp/plugins/distribution/install), or $null.
function Find-Upia {
    $common = [Environment]::GetFolderPath('CommonProgramFiles')
    if (-not $common) {
        return $null
    }
    $exe = Join-Path $common 'Adobe\Adobe Desktop Common\RemoteComponents\UPI\UnifiedPluginInstallerAgent\UnifiedPluginInstallerAgent.exe'
    if (Test-Path -LiteralPath $exe -PathType Leaf) {
        return $exe
    }
    return $null
}

# The per-user CEP extension folder (or the dry-run folder).
function Get-CepExtensionsDir {
    if ($PanelDestination) {
        return $PanelDestination
    }
    return (Join-Path $env:APPDATA 'Adobe\CEP\extensions')
}

# ---------------------------------------------------------------------------
#  Stage one flavour of the panel into $Target:
#      panel\shared\*     -> $Target\shared\
#      panel\<flavour>\*  -> $Target\
#  which is the layout both index.html files load from.
# ---------------------------------------------------------------------------
function New-PanelStage {
    param(
        [ValidateSet('Uxp', 'Cep')] [string] $Flavor,
        [string] $Target
    )
    $source = Get-PanelSourceDir
    $shared = Join-Path $source 'shared'
    $own = Join-Path $source $Flavor.ToLowerInvariant()
    foreach ($required in @((Join-Path $shared 'osvcore.js'), (Join-Path $own 'index.html'), (Join-Path $own 'main.js'))) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
            throw "The panel sources are incomplete: '$required' is missing."
        }
    }
    if (Test-Path -LiteralPath $Target) {
        Remove-Item -LiteralPath $Target -Recurse -Force
    }
    New-Item -ItemType Directory -Path $Target -Force | Out-Null
    # The flavour's own files (manifest, page, adapter, host script)...
    Get-ChildItem -LiteralPath $own | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $Target -Recurse -Force
    }
    # ...and the shared core, controller, view and styles beside them.
    $sharedTarget = Join-Path $Target 'shared'
    New-Item -ItemType Directory -Path $sharedTarget -Force | Out-Null
    Get-ChildItem -LiteralPath $shared -File | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $sharedTarget -Force
    }
}

# ---------------------------------------------------------------------------
#  Pack a staged UXP folder as a .ccx.  Adobe: "A .ccx file is a regular ZIP
#  file under the hood" with manifest.json at its root, and a .ccx needs no
#  signature.
#
#  Entries are written one by one with forward-slash names.  Neither
#  Compress-Archive nor ZipFile.CreateFromDirectory can be trusted with that:
#  under Windows PowerShell 5.1 both store "shared\boot.js", which the ZIP
#  specification does not allow and which unpacks as one oddly named file
#  anywhere a backslash is not a separator.
# ---------------------------------------------------------------------------
function New-PanelCcx {
    param([string] $StageDir, [string] $OutFile)
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if (Test-Path -LiteralPath $OutFile) {
        Remove-Item -LiteralPath $OutFile -Force
    }
    $root = (Resolve-Path -LiteralPath $StageDir).Path.TrimEnd('\')
    $stream = [System.IO.File]::Open($OutFile, [System.IO.FileMode]::CreateNew)
    try {
        $zip = New-Object System.IO.Compression.ZipArchive($stream, [System.IO.Compression.ZipArchiveMode]::Create)
        try {
            Get-ChildItem -LiteralPath $root -Recurse -File | Sort-Object FullName | ForEach-Object {
                $entryName = $_.FullName.Substring($root.Length + 1).Replace('\', '/')
                [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                    $zip, $_.FullName, $entryName, [System.IO.Compression.CompressionLevel]::Optimal)
            }
        }
        finally {
            $zip.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }
}

# ---------------------------------------------------------------------------
#  The CSXS (CEP) versions of the installed Premiere Pro builds, read from
#  each one's PlugPlug.dll: Premiere 2026 ships CEP 12 (PlugPlug 12.0.1), and
#  the PlayerDebugMode key has to name that major version (Adobe's PProPanel
#  guide: "you'll need to perform this step again, but for key CSXS.11").
#  Falls back to 12, Premiere 24-26's version, when nothing is found.
# ---------------------------------------------------------------------------
function Get-PremiereCsxsVersions {
    $root = Join-Path $env:ProgramFiles 'Adobe'
    $versions = @()
    if (Test-Path -LiteralPath $root) {
        $versions = @(Get-ChildItem -LiteralPath $root -Directory -Filter 'Adobe Premiere Pro*' -ErrorAction SilentlyContinue |
            ForEach-Object { Join-Path $_.FullName 'PlugPlug.dll' } |
            Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } |
            ForEach-Object { (Get-Item -LiteralPath $_).VersionInfo.FileMajorPart } |
            Where-Object { $_ -ge 9 } |
            Sort-Object -Unique)
    }
    if ($versions.Count -eq 0) {
        Write-Info 'No installed Premiere Pro found to read its CEP version from; assuming CEP 12.'
        $versions = @(12)
    }
    return $versions
}

# The record of what an install changed, or an empty one.
function Read-PanelState {
    $file = Get-PanelStateFile
    $empty = [pscustomobject]@{ flavor = ''; version = ''; cepPath = ''; ccx = ''; csxs = @() }
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        return $empty
    }
    try {
        $parsed = Get-Content -LiteralPath $file -Raw | ConvertFrom-Json
        return [pscustomobject]@{
            flavor  = [string](Get-PanelProperty $parsed 'flavor')
            version = [string](Get-PanelProperty $parsed 'version')
            cepPath = [string](Get-PanelProperty $parsed 'cepPath')
            ccx     = [string](Get-PanelProperty $parsed 'ccx')
            csxs    = @(Get-PanelProperty $parsed 'csxs' | Where-Object { $null -ne $_ })
        }
    }
    catch {
        Write-Warn "Could not read '$file' ($($_.Exception.Message)); treating it as empty."
        return $empty
    }
}

function Write-PanelState {
    param($State)
    $dir = Get-PanelStateDir
    if (-not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    $State | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Get-PanelStateFile) -Encoding UTF8
}

# ---------------------------------------------------------------------------
#  Set HKCU\Software\Adobe\CSXS.<n>\PlayerDebugMode = "1" (a string, as
#  Adobe's guide shows) for each version, where it is not "1" already.
#  Returns one record per change, with the value it replaced, so -Uninstall
#  can put back exactly that and nothing else.  Only the current user's
#  hive: no administrator rights, nothing machine-wide.
# ---------------------------------------------------------------------------
function Enable-PlayerDebugMode {
    param([int[]] $Versions)
    $changes = @()
    foreach ($v in $Versions) {
        $key = Join-Path $script:PanelCsxsRoot "CSXS.$v"
        $existed = Test-Path -LiteralPath $key
        $previous = $null
        if ($existed) {
            $item = Get-ItemProperty -LiteralPath $key -ErrorAction SilentlyContinue
            $previous = Get-PanelProperty $item 'PlayerDebugMode'
        }
        if ("$previous" -eq '1') {
            Write-Info "CSXS.$v PlayerDebugMode is already 1; left as it is."
            continue
        }
        if (-not $existed) {
            New-Item -Path $key -Force | Out-Null
        }
        New-ItemProperty -LiteralPath $key -Name 'PlayerDebugMode' -Value '1' -PropertyType String -Force | Out-Null
        Write-Info "Set $key\PlayerDebugMode = 1."
        $changes += [pscustomobject]@{ key = "CSXS.$v"; previous = $previous; createdKey = (-not $existed) }
    }
    return $changes
}

# Put back what Enable-PlayerDebugMode changed, per its records.
function Restore-PlayerDebugMode {
    param($Changes)
    foreach ($c in @($Changes)) {
        $name = [string](Get-PanelProperty $c 'key')
        if ($name -notmatch '^CSXS\.\d+$') {
            continue
        }
        $key = Join-Path $script:PanelCsxsRoot $name
        if (-not (Test-Path -LiteralPath $key)) {
            continue
        }
        try {
            $previous = Get-PanelProperty $c 'previous'
            if ($null -eq $previous) {
                Remove-ItemProperty -LiteralPath $key -Name 'PlayerDebugMode' -ErrorAction SilentlyContinue
                # A key this install created, now empty, goes too.
                $leftover = @((Get-Item -LiteralPath $key).Property)
                if ((Get-PanelProperty $c 'createdKey') -eq $true -and $leftover.Count -eq 0 -and
                    @(Get-ChildItem -LiteralPath $key).Count -eq 0) {
                    Remove-Item -LiteralPath $key -Force
                }
                Write-Info "Removed the PlayerDebugMode this install set under $name."
            }
            else {
                New-ItemProperty -LiteralPath $key -Name 'PlayerDebugMode' -Value ([string]$previous) -PropertyType String -Force | Out-Null
                Write-Info "Put $name PlayerDebugMode back to '$previous'."
            }
        }
        catch {
            Write-Warn "Could not restore $name PlayerDebugMode: $($_.Exception.Message)"
        }
    }
}

# Remove this panel's CEP folder (only ours, never the extensions folder).
function Remove-PanelCepCopy {
    $dir = Join-Path (Get-CepExtensionsDir) $script:PanelId
    if (Test-Path -LiteralPath $dir) {
        Remove-Item -LiteralPath $dir -Recurse -Force
        Write-Info "Removed the CEP panel from $dir"
    }
}

# Premiere must be restarted to see a new or changed panel.
function Write-PanelRestartNote {
    param([string] $Where)
    if (Get-Process -Name 'Adobe Premiere Pro' -ErrorAction SilentlyContinue) {
        Write-Info 'Premiere Pro is running: restart it to load the panel.'
    }
    Write-Info "Open it from $Where."
}

# ---------------------------------------------------------------------------
#  CEP: copy, unlock, record.
# ---------------------------------------------------------------------------
function Install-PanelCep {
    param($State, [string] $Version)
    $target = Join-Path (Get-CepExtensionsDir) $script:PanelId
    New-PanelStage -Flavor Cep -Target $target
    Write-Step "Installed the OpenOSV panel (CEP) $Version"
    Write-Info "into $target"

    if ($PanelDestination) {
        Write-Info 'Dry run (-PanelDestination): the registry was not touched.'
    }
    else {
        # Merge with earlier records: the FIRST recorded previous value is the
        # user's own, and must survive a reinstall.
        $changes = @(Enable-PlayerDebugMode -Versions (Get-PremiereCsxsVersions))
        $known = @($State.csxs | ForEach-Object { [string](Get-PanelProperty $_ 'key') })
        $State.csxs = @($State.csxs) + @($changes | Where-Object { $known -notcontains $_.key })
        Write-Info 'PlayerDebugMode lets Premiere load a panel that is not signed.'
        Write-Info 'It is per user and only affects CEP panels; -Uninstall undoes it.'
    }

    # The UXP build must not run beside this one: two panels, one timeline.
    if ($State.flavor -eq 'Uxp' -and -not $PanelDestination) {
        $upia = Find-Upia
        if ($upia) {
            & $upia /remove $script:PanelUxpName 2>&1 | ForEach-Object { Write-Info "UPIA: $_" }
        }
        else {
            Write-Warn 'The UXP build of the panel is installed too. Remove it in the Creative Cloud app (Stock & Marketplace > Plugins > Manage plugins) so only one runs.'
        }
    }
    $State.flavor = 'Cep'
    $State.cepPath = $target
    Write-PanelRestartNote -Where 'Window > Extensions > OpenOSV'
    return $true
}

# ---------------------------------------------------------------------------
#  UXP: install the .ccx with UPIA, or hand it to its file association.
# ---------------------------------------------------------------------------
function Install-PanelUxp {
    param($State, [string] $Ccx, [string] $Upia)
    if ($PanelDestination) {
        Write-Step 'Built the OpenOSV panel (UXP) installer'
        Write-Info "    $Ccx"
        Write-Info 'Dry run (-PanelDestination): nothing was installed.'
        return $true
    }
    if ($Upia) {
        Write-Step 'Installing the OpenOSV panel (UXP) with Adobe''s plug-in installer'
        $output = @(& $Upia /install $Ccx 2>&1)
        $code = $LASTEXITCODE
        foreach ($line in $output) {
            Write-Info "UPIA: $line"
        }
        if ($code -ne 0) {
            Write-Warn "UPIA exited with code $code."
            return $false
        }
        # One panel at a time: drop a CEP copy left from an earlier install.
        Remove-PanelCepCopy
        $State.flavor = 'Uxp'
        Write-PanelRestartNote -Where 'Window > UXP Plugins > OpenOSV'
        return $true
    }

    # No UPIA: the documented alternative is opening the .ccx, which the
    # Creative Cloud app (or whatever is registered for .ccx) installs after
    # one confirmation.
    $handler = $null
    try {
        $handler = (Get-ItemProperty -LiteralPath 'Registry::HKEY_CLASSES_ROOT\.ccx' -ErrorAction Stop).'(default)'
    }
    catch {
        $handler = $null
    }
    if ($handler) {
        Write-Step 'Opening the OpenOSV panel installer (UXP)'
        Write-Info "    $Ccx"
        Write-Info 'Confirm the install in the window that opens.'
        Start-Process -FilePath $Ccx
        Remove-PanelCepCopy
        $State.flavor = 'Uxp'
        Write-PanelRestartNote -Where 'Window > UXP Plugins > OpenOSV'
        return $true
    }
    Write-Warn 'Neither Adobe''s plug-in installer (UPIA) nor a .ccx handler was found, so the UXP panel was NOT installed.'
    Write-Warn 'Install or update the Creative Cloud app, then double-click:'
    Write-Warn "    $Ccx"
    Write-Warn 'or run this script with -PanelFlavor Cep.'
    return $false
}

# ---------------------------------------------------------------------------
#  Install the panel.  Returns $true on success; never throws.
# ---------------------------------------------------------------------------
function Install-Panel {
    $work = $null
    try {
        $version = Get-PanelVersion
        $state = Read-PanelState
        $state.version = $version
        $stateDir = Get-PanelStateDir
        if (-not (Test-Path -LiteralPath $stateDir)) {
            New-Item -ItemType Directory -Path $stateDir -Force | Out-Null
        }

        # The .ccx is always built: it IS the UXP install, and the file a
        # user double-clicks later to move from CEP to UXP.
        $work = Join-Path ([System.IO.Path]::GetTempPath()) ('OpenOSV-panel-' + [guid]::NewGuid().ToString('N'))
        New-PanelStage -Flavor Uxp -Target (Join-Path $work 'uxp')
        $ccx = Join-Path $stateDir ("OpenOSV-panel-$version.ccx")
        New-PanelCcx -StageDir (Join-Path $work 'uxp') -OutFile $ccx
        $state.ccx = $ccx

        $upia = Find-Upia
        $flavor = $PanelFlavor
        if ($flavor -eq 'Auto') {
            if ($upia -and -not $PanelDestination) {
                $flavor = 'Uxp'
                Write-Info 'Adobe''s plug-in installer is present: installing the UXP build.'
            }
            else {
                $flavor = 'Cep'
                if (-not $PanelDestination) {
                    Write-Info 'Adobe''s plug-in installer (UPIA) is not on this machine: installing the'
                    Write-Info 'CEP build, which needs no clicks. The UXP build is ready at'
                    Write-Info "    $ccx"
                    Write-Info 'for when Premiere drops CEP (docs/PANEL.md).'
                }
            }
        }

        $ok = $false
        if ($flavor -eq 'Uxp') {
            $ok = Install-PanelUxp -State $state -Ccx $ccx -Upia $upia
            if (-not $ok -and $PanelFlavor -eq 'Auto') {
                Write-Info 'Falling back to the CEP build.'
                $ok = Install-PanelCep -State $state -Version $version
            }
        }
        else {
            $ok = Install-PanelCep -State $state -Version $version
        }
        Write-PanelState -State $state
        return $ok
    }
    catch {
        Write-Warn "Could not install the OpenOSV panel: $($_.Exception.Message)"
        return $false
    }
    finally {
        if ($work -and (Test-Path -LiteralPath $work)) {
            Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

# ---------------------------------------------------------------------------
#  Remove the panel and undo what the install changed.  Never throws.
# ---------------------------------------------------------------------------
function Uninstall-Panel {
    try {
        $state = Read-PanelState
        Write-Step 'Removing the OpenOSV panel'
        Remove-PanelCepCopy
        if (-not $PanelDestination) {
            Restore-PlayerDebugMode -Changes $state.csxs
            if ($state.flavor -eq 'Uxp') {
                $upia = Find-Upia
                if ($upia) {
                    & $upia /remove $script:PanelUxpName 2>&1 | ForEach-Object { Write-Info "UPIA: $_" }
                }
                else {
                    Write-Warn 'Remove the UXP panel in the Creative Cloud app: Stock & Marketplace > Plugins > Manage plugins > OpenOSV > Uninstall.'
                }
            }
        }
        $stateDir = Get-PanelStateDir
        if (Test-Path -LiteralPath $stateDir) {
            Remove-Item -LiteralPath $stateDir -Recurse -Force
        }
        Write-Info 'Done. Restart Premiere Pro if it is running.'
        return $true
    }
    catch {
        Write-Warn "Could not fully remove the OpenOSV panel: $($_.Exception.Message)"
        return $false
    }
}

# ===========================================================================
#  [WP-PANEL] The companion panel "OpenOSV"                        (end)
# ===========================================================================

# ===========================================================================
#  Main
# ===========================================================================
try {
    # [WP-PANEL] -PanelOnly: the panel and nothing else, no elevation.
    if ($PanelOnly) {
        $panelOk = if ($Uninstall) { Uninstall-Panel } else { Install-Panel }
        if ($panelOk) {
            exit 0
        }
        exit 1
    }

    # The presets go into THIS user's profile, so they are handled here,
    # before any elevation.  Doing it in the elevated child would resolve the
    # profile to whichever account answered the UAC prompt (see the header).
    # Invoke-Elevated always passes -NoPresets to the child for that reason.
    if (-not $NoPresets) {
        if ($Uninstall) {
            Uninstall-Presets
        }
        else {
            Install-Presets
        }
        Write-Host ''
    }

    # [WP-PANEL] The panel is per-user too, so it is handled here as well;
    # the elevated child always gets -NoPanel.
    if (-not $NoPanel) {
        if ($Uninstall) {
            [void](Uninstall-Panel)
        }
        else {
            [void](Install-Panel)
        }
        Write-Host ''
    }

    if (-not (Test-Elevated)) {
        if ($NoElevate) {
            throw "Administrator rights are required to write to '$Destination', and -NoElevate was given. Re-run this script from an elevated prompt."
        }
        exit (Invoke-Elevated)
    }

    Assert-HostsClosed -Path $Destination

    if ($Uninstall) {
        Write-Step "Removing $Destination"
        if (Test-Path -LiteralPath $Destination) {
            Remove-Item -LiteralPath $Destination -Recurse -Force
            Write-Info 'Removed.'
        }
        else {
            Write-Info 'Nothing to remove: the folder does not exist.'
        }
        Write-Host ''
        Write-Step 'Next time you start Premiere Pro'
        Write-Info 'Hold Shift while it launches to force a full plug-in rescan,'
        Write-Info 'otherwise the cached registry entries keep the plug-ins listed.'
        exit 0
    }

    if (-not $StageDir) {
        $StageDir = Find-StageDir
        Write-Step "Using the newest build: $StageDir"
    }
    # Validate before resolving: Resolve-Path on a missing path throws its
    # own message, which is far less helpful than ours.
    $modules = Test-StageDir -Path $StageDir
    $StageDir = (Resolve-Path -LiteralPath $StageDir).Path

    Write-Step "Installing from $StageDir"
    Write-Info "into $Destination"

    if (-not (Test-Path -LiteralPath $Destination)) {
        New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    }

    # Copy the whole stage folder: the modules and every runtime DLL the
    # build's post-build step put beside them (FFmpeg, OpenCL, fmt, ...).
    # The plug-ins delay-load those by name from their own directory, so a
    # missing one would only fail at the first frame, not at load time.
    $items = Get-ChildItem -LiteralPath $StageDir -File
    foreach ($item in $items) {
        Copy-Item -LiteralPath $item.FullName -Destination $Destination -Force
    }
    $dllCount = @($items | Where-Object { $_.Extension -eq '.dll' }).Count

    Write-Info ("Copied {0} module(s): {1}" -f $modules.Count, ($modules -join ', '))
    Write-Info ("Copied {0} runtime DLL(s)." -f $dllCount)

    # The LUTs go beside the modules, in the same elevated pass.
    Write-Host ''
    Install-Luts -Path $Destination

    Write-Host ''
    Write-Step 'Checking the installation'
    foreach ($module in $modules) {
        $target = Join-Path $Destination $module
        $size = (Get-Item -LiteralPath $target).Length
        Write-Info ("{0,-24} {1,10:N0} bytes" -f $module, $size)
    }

    Write-Host ''
    Write-Step 'Did Premiere Pro load them?'
    $log = Get-PluginLoadingLog
    if ($log) {
        Write-Info 'After the next launch, search this file for OpenOSV:'
        Write-Info "    $log"
    }
    else {
        Write-Info 'Premiere Pro writes a plug-in loading log at:'
        Write-Info '    %APPDATA%\Adobe\Premiere Pro\<version>\Plugin Loading.log'
        Write-Info 'It appears once Premiere Pro has been started at least once.'
    }

    Write-Host ''
    Write-Step 'Hold Shift while Premiere Pro launches'
    Write-Info 'Premiere Pro caches the plug-in list in the registry and will not'
    Write-Info 'notice a new or replaced module on an ordinary start. Holding'
    Write-Info 'Shift from the moment you launch it until the splash screen'
    Write-Info 'appears forces a full rescan, which is what picks up this'
    Write-Info 'installation.'
    exit 0
}
catch {
    Write-Host ''
    Write-Host "ERROR: $($_.Exception.Message)"
    exit 1
}
