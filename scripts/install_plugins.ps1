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
    [string] $PresetDestination
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

    $candidates = Get-ChildItem -LiteralPath $buildRoot -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName 'plugins\OpenOSV' } |
        Where-Object { Test-Path -LiteralPath $_ } |
        Where-Object {
            # Only a folder that really contains a module counts.
            $folder = $_
            $script:PluginFiles | Where-Object { Test-Path -LiteralPath (Join-Path $folder $_) }
        } |
        Sort-Object { (Get-Item -LiteralPath $_).LastWriteTimeUtc } -Descending

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
#  Main
# ===========================================================================
try {
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
