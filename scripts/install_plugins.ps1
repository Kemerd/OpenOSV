#Requires -Version 5.1
<#
.SYNOPSIS
    Installs (or removes) the OpenOSV Premiere Pro plug-ins.

.DESCRIPTION
    Copies the staged plug-in folder - the two modules OpenOSVImporter.prm and
    Open360Reframe.aex plus the runtime DLLs the build put next to them - into

        C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\OpenOSV\

    That folder is scanned by Premiere Pro, Media Encoder and After Effects
    alike, which is why one copy serves all three (docs/PREMIERE.md).

    The destination lives under Program Files, so the script needs an elevated
    session.  When it is not elevated it explains why and relaunches itself
    with the same arguments through UAC.

.PARAMETER StageDir
    The folder produced by the build (OSV_PLUGIN_STAGE_DIR, by default
    <build>\plugins\OpenOSV).  When omitted the script looks for the newest
    plugins\OpenOSV under <repo>\build\*.

.PARAMETER Destination
    Overrides the MediaCore folder.  Mostly useful for a dry run into a
    scratch directory, or for a non-default Adobe installation.

.PARAMETER Uninstall
    Removes the installed folder instead of copying into it.  -StageDir is
    then ignored.

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
    [switch] $Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Plain ASCII output on a legacy code page as well as on UTF-8.
$OutputEncoding = [System.Text.Encoding]::ASCII

# The two modules that make up the installation.  A stage directory without
# either of them is a build that never produced a plug-in.
$script:PluginFiles = @('OpenOSVImporter.prm', 'Open360Reframe.aex')

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

# ===========================================================================
#  Main
# ===========================================================================
try {
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
