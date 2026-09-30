#Requires -Version 5.1
<#
.SYNOPSIS
    Installs (or removes) OpenOSV for VEGAS Pro: the OpenFX bundle, the
    Application Extension, and a clean plug-in scan.

.DESCRIPTION
    Three things, in this order:

     1. The OpenFX bundle.  This calls install_ofx.ps1 (same folder), which
        copies OpenOSV.ofx.bundle into
            C:\Program Files\Common Files\OFX\Plugins\
        VEGAS scans that folder as well as its own "OFX Video Plug-Ins", and
        DaVinci Resolve reads it too, so one installed bundle serves both
        editors.  OpenOSV Source (a generator) and OpenOSV 360 Reframe (a
        filter) show up in VEGAS's Media Generators and Video FX.
     2. The Application Extension: OpenOSV.Vegas.dll and OpenOSV.Vegas.Core.dll
        go into
            %ProgramData%\VEGAS Pro\Application Extensions\
        and are unblocked (a DLL that came out of a downloaded zip carries
        the "from the internet" mark, and .NET may refuse to load it).  The
        extension is what adds Tools > Extensions > "Import OSV...".
     3. The plug-in caches.  VEGAS remembers what it found in a scan and does
        not look again on its own.  For every VEGAS version folder under
            %LOCALAPPDATA%\VEGAS Pro\<version>\
        the script deletes svfx_plugin_cache.bin, plugin_manager_cache.bin and
        OpenOSV's own OpenFX describe logs (svfx_Ofx*org.openosv*.log), so the
        next start scans afresh.  Nothing else in those folders is touched.

    VEGAS must be closed: it holds plug-ins and caches open while it runs, and
    the script refuses (naming the process) while any vegas* process exists.

    Folders under Program Files need an elevated session.  When one is needed
    and the script is not elevated, it relaunches itself once through UAC with
    the same arguments and shows you what the elevated run did.

    -Uninstall removes exactly what the install put in place: the bundle
    (unless -SkipBundle), the two DLLs and the caches.  It leaves VEGAS's own
    folders, and everything else in them, alone.

.PARAMETER StageDir
    The bundle to install (passed to install_ofx.ps1).  When omitted, that
    script picks the release package's plugins\OpenOSV.ofx.bundle, and
    otherwise the newest <repo>\build\*\plugins\ofx\OpenOSV.ofx.bundle.

.PARAMETER BundleDestination
    Overrides the OpenFX plug-in folder (install_ofx.ps1's -Destination).
    Default: %CommonProgramFiles%\OFX\Plugins.

.PARAMETER ExtensionSource
    The folder holding OpenOSV.Vegas.dll and OpenOSV.Vegas.Core.dll.  When
    omitted the script takes the release package's extension\ folder, and
    otherwise the newest <repo>\build\*\plugins\vegas\extension.

.PARAMETER ExtensionDestination
    Overrides VEGAS's Application Extensions folder.
    Default: %ProgramData%\VEGAS Pro\Application Extensions.

.PARAMETER LocalAppData
    Overrides %LOCALAPPDATA%, whose "VEGAS Pro" folder holds the caches.

.PARAMETER ProgramFilesDir
    Overrides the Program Files folder that is searched for installed VEGAS
    versions (it only lists them; nothing is written there).

.PARAMETER SkipBundle
    Leave the OpenFX bundle alone: only the extension and the caches.  Use it
    with -Uninstall to keep the bundle for DaVinci Resolve.

.PARAMETER Uninstall
    Removes what the install put in place instead of copying it.

.PARAMETER DryRun
    Shows what would happen and changes nothing.  Never elevates, and does not
    refuse while VEGAS is running.

.PARAMETER IgnoreRunningVegas
    Skips the "VEGAS is running" refusal.  For tests against scratch folders;
    a real install with VEGAS open only fails halfway.

.PARAMETER NoElevate
    Never relaunch through UAC; fail instead when rights are missing.

.PARAMETER ElevatedLog
    Internal: the file an elevated child writes its output to, so the window
    that asked for elevation can show it.

.EXAMPLE
    scripts\install_vegas.ps1
    scripts\install_vegas.ps1 -Uninstall
    scripts\install_vegas.ps1 -DryRun
    scripts\install_vegas.ps1 -BundleDestination C:\Temp\ofx -ExtensionDestination C:\Temp\ext -LocalAppData C:\Temp\lad
#>
param(
    [string] $StageDir,
    [string] $BundleDestination = "$env:CommonProgramFiles\OFX\Plugins",
    [string] $ExtensionSource,
    [string] $ExtensionDestination = "$env:ProgramData\VEGAS Pro\Application Extensions",
    [string] $LocalAppData = $env:LOCALAPPDATA,
    [string] $ProgramFilesDir = $env:ProgramFiles,
    [switch] $SkipBundle,
    [switch] $Uninstall,
    [switch] $DryRun,
    [switch] $IgnoreRunningVegas,
    [switch] $NoElevate,
    [string] $ElevatedLog
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$OutputEncoding = [System.Text.Encoding]::ASCII

# What the install owns.  The extension's two assemblies are the only files it
# puts in the Application Extensions folder, so they are all -Uninstall removes
# from there.
$ExtensionFiles = @('OpenOSV.Vegas.dll', 'OpenOSV.Vegas.Core.dll')

# The per-user files VEGAS rebuilds by itself and that hold its plug-in list.
# The third pattern matches only OpenOSV's own OpenFX describe logs, e.g.
# svfx_Ofx1_1_plugin_x64-'org.openosv.OSVSource' (0, Generator).log
$CacheFiles = @('svfx_plugin_cache.bin', 'plugin_manager_cache.bin')
$OwnLogPattern = 'svfx_Ofx*org.openosv*.log'

# ---------------------------------------------------------------------------
#  Output.  Everything printed also goes to $ElevatedLog when there is one, so
#  the window that asked for elevation can replay what the elevated child did.
# ---------------------------------------------------------------------------
function Write-Line {
    param([string] $Text)
    Write-Host $Text
    if ($ElevatedLog) {
        try { Add-Content -LiteralPath $ElevatedLog -Value $Text -Encoding ASCII } catch { }
    }
}
function Write-Step { param([string] $Message) Write-Line "==> $Message" }
function Write-Info { param([string] $Message) Write-Line "    $Message" }
function Write-Note { param([string] $Message) Write-Line "  * $Message" }

# ---------------------------------------------------------------------------
#  True when the current process has the Administrators group in its token.
# ---------------------------------------------------------------------------
function Test-Elevated {
    $identity  = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# ---------------------------------------------------------------------------
#  True when this process can create files in $Path, or in its nearest
#  existing parent when $Path does not exist yet (a scratch destination
#  usually allows that unelevated; Program Files does not).
# ---------------------------------------------------------------------------
function Test-Writable {
    param([string] $Path)
    if (-not $Path) { return $false }
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
#  One command-line argument, quoted for the Windows command-line parser:
#  backslashes that end up in front of the closing quote are doubled.
# ---------------------------------------------------------------------------
function ConvertTo-QuotedArgument {
    param([string] $Value)
    return '"' + ($Value -replace '(\\+)$', '$1$1') + '"'
}

# ---------------------------------------------------------------------------
#  Relaunch elevated with the same (resolved) arguments; returns the child's
#  exit code.  The child's window closes when it ends, so it writes its output
#  to a log file, which is echoed here.  The local-app-data folder is passed
#  explicitly: an administrator account other than yours has its own.
# ---------------------------------------------------------------------------
function Invoke-Elevated {
    param([string[]] $Needed)

    $log = Join-Path ([System.IO.Path]::GetTempPath()) ('openosv-vegas-' + [guid]::NewGuid().ToString('N') + '.log')
    $arguments = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (ConvertTo-QuotedArgument $PSCommandPath))
    $named = [ordered]@{
        '-StageDir'             = $StageDir
        '-BundleDestination'    = $BundleDestination
        '-ExtensionSource'      = $ExtensionSource
        '-ExtensionDestination' = $ExtensionDestination
        '-LocalAppData'         = $LocalAppData
        '-ProgramFilesDir'      = $ProgramFilesDir
    }
    foreach ($key in $named.Keys) {
        if ($named[$key]) { $arguments += @($key, (ConvertTo-QuotedArgument $named[$key])) }
    }
    if ($SkipBundle)         { $arguments += '-SkipBundle' }
    if ($Uninstall)          { $arguments += '-Uninstall' }
    if ($IgnoreRunningVegas) { $arguments += '-IgnoreRunningVegas' }
    $arguments += @('-NoElevate', '-ElevatedLog', (ConvertTo-QuotedArgument $log))

    Write-Step 'Administrator rights are required'
    foreach ($path in $Needed) { Write-Info "    $path" }
    Write-Info 'Windows will show a UAC prompt now. The elevated run only copies files'
    Write-Info 'into (or removes them from) the folders above and clears VEGAS''s plug-in'
    Write-Info 'caches. Nothing else is touched.'

    $process = Start-Process -FilePath (Get-Process -Id $PID).Path `
                             -ArgumentList $arguments -Verb RunAs -Wait -PassThru
    # Replay what the elevated run printed.
    if (Test-Path -LiteralPath $log) {
        Get-Content -LiteralPath $log | ForEach-Object { Write-Host $_ }
        Remove-Item -LiteralPath $log -Force -ErrorAction SilentlyContinue
    }
    return $process.ExitCode
}

# ---------------------------------------------------------------------------
#  The first place with the needed files: the release package's extension\
#  folder (beside this script's scripts\ folder), else the newest
#  <repo>\build\*\plugins\vegas\extension.  $null when there is none.
# ---------------------------------------------------------------------------
function Test-ExtensionFolder {
    param([string] $Folder)
    if (-not $Folder) { return $false }
    foreach ($name in $ExtensionFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $Folder $name) -PathType Leaf)) { return $false }
    }
    return $true
}

function Find-ExtensionSource {
    $repo = Split-Path -Parent $PSScriptRoot
    $packaged = Join-Path $repo 'extension'
    if (Test-ExtensionFolder $packaged) { return $packaged }
    $build = Join-Path $repo 'build'
    if (-not (Test-Path -LiteralPath $build)) { return $null }
    $candidates = @(Get-ChildItem -LiteralPath $build -Directory | ForEach-Object {
        $folder = Join-Path $_.FullName 'plugins\vegas\extension'
        if (Test-ExtensionFolder $folder) {
            [pscustomobject]@{
                Folder  = $folder
                Written = (Get-Item -LiteralPath (Join-Path $folder $ExtensionFiles[0])).LastWriteTime
            }
        }
    })
    $newest = $candidates | Sort-Object Written -Descending | Select-Object -First 1
    if ($newest) { return $newest.Folder }
    return $null
}

# ---------------------------------------------------------------------------
#  VEGAS versions on this machine, from two sources: install folders under
#  Program Files ("VEGAS\VEGAS Pro 17.0", "BorisFX\Vegas Pro 2026") and
#  per-user data folders (%LOCALAPPDATA%\VEGAS Pro\<version>).  Read-only.
# ---------------------------------------------------------------------------
function Get-VegasInstalls {
    $found = @()
    foreach ($vendor in @('VEGAS', 'BorisFX')) {
        $parent = Join-Path $ProgramFilesDir $vendor
        if (-not (Test-Path -LiteralPath $parent -PathType Container)) { continue }
        $found += @(Get-ChildItem -LiteralPath $parent -Directory -ErrorAction SilentlyContinue |
                    Where-Object { $_.Name -like 'VEGAS Pro*' })
    }
    return $found
}

function Get-VegasDataFolders {
    $root = Join-Path $LocalAppData 'VEGAS Pro'
    if (-not (Test-Path -LiteralPath $root -PathType Container)) { return @() }
    return @(Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue)
}

# ---------------------------------------------------------------------------
#  Delete the plug-in caches of every VEGAS version folder.  Only the exact
#  files named at the top of this script.  Returns how many were (or, with
#  -DryRun, would be) deleted.
# ---------------------------------------------------------------------------
function Clear-VegasCaches {
    $count = 0
    foreach ($folder in (Get-VegasDataFolders)) {
        $victims = @(Get-ChildItem -LiteralPath $folder.FullName -File -ErrorAction SilentlyContinue |
            Where-Object { ($CacheFiles -contains $_.Name.ToLowerInvariant()) -or ($_.Name -like $OwnLogPattern) })
        foreach ($file in $victims) {
            if ($DryRun) {
                Write-Info "would delete $($file.FullName)"
            } else {
                try {
                    Remove-Item -LiteralPath $file.FullName -Force
                } catch {
                    throw "Could not delete '$($file.FullName)'. Is VEGAS still running? Close it and run this again."
                }
            }
            $count++
        }
        if ($victims.Count -gt 0) {
            Write-Info ("{0}: {1} cache file(s) {2}" -f $folder.Name, $victims.Count, $(if ($DryRun) { 'to clear' } else { 'cleared' }))
        }
    }
    return $count
}

# ---------------------------------------------------------------------------
#  Run install_ofx.ps1 with the options this script was given, echoing its
#  output through Write-Line.  It always gets -NoElevate: this script has
#  already elevated (once) if the bundle folder needed it.
# ---------------------------------------------------------------------------
function Invoke-BundleInstaller {
    $installer = Join-Path $PSScriptRoot 'install_ofx.ps1'
    if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) {
        throw "install_ofx.ps1 is missing from '$PSScriptRoot'. Keep the scripts folder together."
    }
    $arguments = @{ Destination = $BundleDestination; NoElevate = $true }
    if ($StageDir)  { $arguments['StageDir'] = $StageDir }
    if ($Uninstall) { $arguments['Uninstall'] = $true }

    # A failure inside it throws; an "exit 1" sets $LASTEXITCODE.
    $global:LASTEXITCODE = 0
    $output = & $installer @arguments *>&1
    foreach ($item in @($output)) { Write-Line ([string]$item) }
    if ($LASTEXITCODE -ne 0) {
        throw "install_ofx.ps1 failed (exit code $LASTEXITCODE)."
    }
}

# ---------------------------------------------------------------------------
#  Main
# ---------------------------------------------------------------------------
try {
    $verb = if ($Uninstall) { 'Removing' } else { 'Installing' }
    Write-Step "$verb OpenOSV for VEGAS Pro$(if ($DryRun) { ' (dry run: nothing is changed)' })"

    if (-not $LocalAppData) {
        throw 'No %LOCALAPPDATA% to look for VEGAS caches in; pass -LocalAppData.'
    }

    # -- which VEGAS versions are here ----------------------------------------
    $installs = @(Get-VegasInstalls)
    $dataFolders = @(Get-VegasDataFolders)
    if ($installs.Count -gt 0) {
        Write-Info 'VEGAS Pro found:'
        foreach ($item in $installs) { Write-Info "    $($item.Name)   ($($item.FullName))" }
    } else {
        Write-Info 'No VEGAS Pro install found under Program Files. Carrying on: the OpenFX'
        Write-Info 'folder and the Application Extensions folder are the same for every version.'
    }
    if ($dataFolders.Count -gt 0) {
        Write-Info ('Plug-in caches to reset in: ' + (($dataFolders | ForEach-Object { $_.Name }) -join ', '))
    }

    # -- VEGAS must be closed -------------------------------------------------
    $running = @(Get-Process -Name 'vegas*' -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) {
        $names = ($running | ForEach-Object { '{0} (pid {1})' -f $_.ProcessName, $_.Id }) -join ', '
        if ($DryRun -or $IgnoreRunningVegas) {
            Write-Note "VEGAS is running: $names. A real run would refuse."
        } else {
            throw "VEGAS is running: $names. It holds its plug-ins and caches open. Close it, then run this again."
        }
    }

    # -- what this needs to write, and whether that needs rights --------------
    $needed = @()
    if (-not $SkipBundle) { $needed += $BundleDestination }
    $needed += $ExtensionDestination
    if (-not $DryRun) {
        $locked = @($needed | Where-Object { -not (Test-Writable $_) })
        if ($locked.Count -gt 0) {
            if (Test-Elevated) {
                throw "Cannot write to '$($locked[0])' even with administrator rights."
            }
            if ($NoElevate) {
                throw "Administrator rights are required to write to '$($locked[0])', and -NoElevate was given."
            }
            exit (Invoke-Elevated -Needed $locked)
        }
    }

    # -- uninstall --------------------------------------------------------------
    if ($Uninstall) {
        if ($SkipBundle) {
            Write-Info 'OpenFX bundle: kept (-SkipBundle).'
        } elseif ($DryRun) {
            Write-Info "would run install_ofx.ps1 -Uninstall -Destination '$BundleDestination'"
        } else {
            Invoke-BundleInstaller
        }

        foreach ($name in $ExtensionFiles) {
            $file = Join-Path $ExtensionDestination $name
            if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { continue }
            if ($DryRun) {
                Write-Info "would delete $file"
            } else {
                try {
                    Remove-Item -LiteralPath $file -Force
                } catch {
                    throw "Could not delete '$file'. Is VEGAS still running? Close it and run this again."
                }
                Write-Info "removed $file"
            }
        }

        # A fresh scan drops the effects and the menu entry from VEGAS's lists.
        [void](Clear-VegasCaches)

        Write-Line ''
        Write-Line 'OpenOSV is out of VEGAS Pro. Start VEGAS: it rescans on its own.'
        if (-not $SkipBundle) {
            Write-Line 'The OpenFX bundle was shared with DaVinci Resolve; it is gone there too.'
            Write-Line 'Use -SkipBundle next time to keep it.'
        }
        exit 0
    }

    # -- install: sources -------------------------------------------------------
    if (-not $ExtensionSource) {
        $ExtensionSource = Find-ExtensionSource
        if (-not $ExtensionSource) {
            throw ("No built extension found (OpenOSV.Vegas.dll and OpenOSV.Vegas.Core.dll) in the package's " +
                   "extension folder or under the repository's build folder. Build with VEGAS Pro installed " +
                   "(docs\VEGAS.md) or pass -ExtensionSource.")
        }
    }
    if (-not (Test-ExtensionFolder $ExtensionSource)) {
        throw "'$ExtensionSource' is not the OpenOSV extension folder (it needs $($ExtensionFiles -join ' and '))."
    }
    if ($StageDir -and -not (Test-Path -LiteralPath $StageDir -PathType Container)) {
        throw "'$StageDir' does not exist."
    }

    # -- install: 1. the OpenFX bundle ------------------------------------------
    if ($SkipBundle) {
        Write-Info 'OpenFX bundle: skipped (-SkipBundle).'
    } elseif ($DryRun) {
        Write-Info "would run install_ofx.ps1 -Destination '$BundleDestination'$(if ($StageDir) { " -StageDir '$StageDir'" })"
    } else {
        Invoke-BundleInstaller
    }

    # -- install: 2. the extension ------------------------------------------------
    Write-Step 'Copying the VEGAS extension'
    Write-Info "from $ExtensionSource"
    Write-Info "to   $ExtensionDestination"
    if (-not $DryRun) {
        New-Item -ItemType Directory -Path $ExtensionDestination -Force | Out-Null
    }
    foreach ($name in $ExtensionFiles) {
        $source = Join-Path $ExtensionSource $name
        $target = Join-Path $ExtensionDestination $name
        if ($DryRun) {
            Write-Info "would copy $name"
            continue
        }
        try {
            Copy-Item -LiteralPath $source -Destination $target -Force
        } catch {
            throw "Could not replace '$target'. Is VEGAS still running? Close it and run this again."
        }
        # Take off the "downloaded from the internet" mark: .NET can refuse to
        # load a marked assembly.  It is a no-op on a file that has none.
        try {
            Unblock-File -LiteralPath $target
        } catch {
            Write-Note "Could not unblock $name ($($_.Exception.Message)). Right-click it > Properties > Unblock."
        }
        Write-Info "installed $name"
    }

    # -- install: 3. the caches ---------------------------------------------------
    Write-Step 'Resetting the plug-in caches'
    $cleared = Clear-VegasCaches
    if ($cleared -eq 0) {
        Write-Info 'No cache files yet: VEGAS scans on its first start anyway.'
    }

    # -- the summary ----------------------------------------------------------------
    Write-Line ''
    Write-Line '==============================================================='
    Write-Line $(if ($DryRun) { ' Dry run done. Nothing was changed.' } else { ' OpenOSV is installed for VEGAS Pro (a preview).' })
    Write-Line ''
    Write-Line ' Start VEGAS. It rescans plug-ins on its first start, so that one'
    Write-Line ' takes a little longer. Then:'
    Write-Line '   * Tools > Extensions > "Import OSV..." drops an .OSV on the'
    Write-Line '     timeline, sized, audio included. (View > Extensions > OpenOSV'
    Write-Line '     is the dock panel.)'
    Write-Line '   * By hand: Media Generators > OpenOSV Source, and Video FX >'
    Write-Line '     OpenOSV 360 Reframe for footage that is already equirect.'
    Write-Line ''
    Write-Line ' Nothing in the list? Reports go to the issue tracker, with'
    Write-Line '   %LOCALAPPDATA%\OpenOSV\OpenOSVOfx.log and OpenOSVVegas.log.'
    Write-Line '==============================================================='
    exit 0
}
catch {
    Write-Line ''
    Write-Line "ERROR: $($_.Exception.Message)"
    exit 1
}
