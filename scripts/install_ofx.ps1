#Requires -Version 5.1
<#
.SYNOPSIS
    Installs (or removes) OpenOSV's OpenFX plug-ins for DaVinci Resolve.

.DESCRIPTION
    Copies the OpenOSV.ofx.bundle folder the build assembled - OpenOSV.ofx and
    the runtime DLLs beside it - into the folder every OpenFX host on Windows
    scans:

        C:\Program Files\Common Files\OFX\Plugins\OpenOSV.ofx.bundle\

    DaVinci Resolve (free or Studio) lists its two effects in the Effects
    Library's OpenFX section, group OpenOSV, after a restart:

        OpenOSV Source      a generator: a .OSV clip, stitched
        Open 360 Reframe    a filter: reframes any 360 equirectangular clip

    Resolve scans for plug-ins only when it starts, so close it before
    running this script and start it again afterwards.  See docs\RESOLVE.md.

    The folder is under Program Files, so this needs an elevated session;
    when the script is not elevated it relaunches itself through UAC with the
    same arguments.

.PARAMETER StageDir
    The bundle to install.  When omitted the script takes the release
    package's plugins\OpenOSV.ofx.bundle when it runs from a package
    (scripts\package_release.ps1), and otherwise the newest
    <repo>\build\*\plugins\ofx\OpenOSV.ofx.bundle.

.PARAMETER Destination
    Overrides the OFX plug-in folder (a dry run into a scratch directory, or
    a host configured with OFX_PLUGIN_PATH).

.PARAMETER Uninstall
    Removes the installed bundle instead of copying it.

.PARAMETER NoElevate
    Never relaunch through UAC; fail instead when rights are missing.

.EXAMPLE
    scripts\install_ofx.ps1
    scripts\install_ofx.ps1 -Uninstall
    scripts\install_ofx.ps1 -Destination C:\Temp\ofx-dry-run
#>
param(
    [string] $StageDir,
    [string] $Destination = "$env:CommonProgramFiles\OFX\Plugins",
    [switch] $Uninstall,
    [switch] $NoElevate
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$OutputEncoding = [System.Text.Encoding]::ASCII

# The bundle's name is fixed by the OpenFX packaging rules and by Resolve's
# plug-in cache, which records it: never rename it.
$BundleName = 'OpenOSV.ofx.bundle'
$BinaryRelative = 'Contents\Win64\OpenOSV.ofx'

function Write-Step { param([string] $Message) Write-Host "==> $Message" }
function Write-Info { param([string] $Message) Write-Host "    $Message" }

# ---------------------------------------------------------------------------
#  True when the current process has the Administrators group in its token.
# ---------------------------------------------------------------------------
function Test-Elevated {
    $identity  = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# ---------------------------------------------------------------------------
#  True when this process can create files in `$Path` (or its nearest
#  existing parent) - which a dry-run destination usually allows unelevated.
# ---------------------------------------------------------------------------
function Test-Writable {
    param([string] $Path)
    $probeDir = $Path
    while ($probeDir -and -not (Test-Path -LiteralPath $probeDir)) {
        $probeDir = Split-Path -Parent $probeDir
    }
    if (-not $probeDir) { return $false }
    $probe = Join-Path $probeDir ('.openosv-write-probe-' + [guid]::NewGuid().ToString('N'))
    try {
        New-Item -ItemType File -Path $probe -Force | Out-Null
        Remove-Item -LiteralPath $probe -Force
        return $true
    } catch {
        return $false
    }
}

# ---------------------------------------------------------------------------
#  Relaunch elevated with the same arguments; returns the child's exit code.
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

    Write-Step 'Administrator rights are required'
    Write-Info 'OpenFX plug-ins live in a folder under Program Files:'
    Write-Info "    $Destination"
    Write-Info 'Windows will show a UAC prompt now. Nothing else is touched:'
    Write-Info 'the script only copies files into (or removes them from) that folder.'
    $process = Start-Process -FilePath (Get-Process -Id $PID).Path `
                             -ArgumentList $arguments -Verb RunAs -Wait -PassThru
    return $process.ExitCode
}

# ---------------------------------------------------------------------------
#  The bundle to install when the caller named none: the release package's
#  plugins\OpenOSV.ofx.bundle beside this script's folder, else the newest
#  one under <repo>\build\*.
# ---------------------------------------------------------------------------
function Find-Bundle {
    $repo = Split-Path -Parent $PSScriptRoot
    $packaged = Join-Path $repo "plugins\$BundleName"
    if (Test-Path -LiteralPath (Join-Path $packaged $BinaryRelative)) {
        return $packaged
    }
    $build = Join-Path $repo 'build'
    if (-not (Test-Path -LiteralPath $build)) { return $null }
    $candidates = Get-ChildItem -LiteralPath $build -Directory | ForEach-Object {
        $bundle = Join-Path $_.FullName "plugins\ofx\$BundleName"
        $binary = Join-Path $bundle $BinaryRelative
        if (Test-Path -LiteralPath $binary) {
            [pscustomobject]@{ Bundle = $bundle; Written = (Get-Item -LiteralPath $binary).LastWriteTime }
        }
    }
    $newest = $candidates | Sort-Object Written -Descending | Select-Object -First 1
    if ($newest) { return $newest.Bundle }
    return $null
}

# ---------------------------------------------------------------------------
#  Main
# ---------------------------------------------------------------------------
$target = Join-Path $Destination $BundleName

if (-not (Test-Writable $Destination)) {
    if (Test-Elevated) {
        throw "Cannot write to '$Destination' even with administrator rights."
    }
    if ($NoElevate) {
        throw "Administrator rights are required to write to '$Destination', and -NoElevate was given."
    }
    exit (Invoke-Elevated)
}

if ($Uninstall) {
    Write-Step "Removing $target"
    if (Test-Path -LiteralPath $target) {
        Remove-Item -LiteralPath $target -Recurse -Force
        Write-Info 'Removed. Restart DaVinci Resolve to drop the effects from its list.'
    } else {
        Write-Info 'Nothing installed there.'
    }
    exit 0
}

if (-not $StageDir) {
    $StageDir = Find-Bundle
    if (-not $StageDir) {
        throw "No built $BundleName under the repository's build folder. Build with -DOSV_BUILD_OFX=ON (docs\RESOLVE.md) or pass -StageDir."
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $StageDir $BinaryRelative))) {
    throw "'$StageDir' is not an OpenOSV bundle (no $BinaryRelative inside)."
}

Write-Step "Installing $BundleName"
Write-Info "from $StageDir"
Write-Info "to   $target"

# A clean copy: a DLL left over from an older build must never be the one the
# plug-in's loader finds first.
if (Test-Path -LiteralPath $target) {
    try {
        Remove-Item -LiteralPath $target -Recurse -Force
    } catch {
        throw "Could not replace '$target' - is DaVinci Resolve still running? Close it and run this again."
    }
}
New-Item -ItemType Directory -Path $Destination -Force | Out-Null
Copy-Item -LiteralPath $StageDir -Destination $target -Recurse -Force
# Debug symbols and link leftovers stay in the build tree.  Collected first and
# removed one by one: Windows PowerShell 5.1 throws on an EMPTY pipeline into
# Remove-Item from Get-ChildItem -Include.
$leftovers = @(Get-ChildItem -LiteralPath $target -Recurse -File |
               Where-Object { @('.pdb', '.ilk', '.exp', '.lib') -contains $_.Extension.ToLowerInvariant() })
foreach ($file in $leftovers) {
    Remove-Item -LiteralPath $file.FullName -Force
}

$files = @(Get-ChildItem -LiteralPath $target -Recurse -File)
Write-Info ("Installed {0} files." -f $files.Count)
Write-Info 'Start DaVinci Resolve and look in the Effects Library under OpenFX, group'
Write-Info 'OpenOSV: OpenOSV Source (a generator) and Open 360 Reframe (a filter).'
exit 0
