#Requires -Version 5.1
<#
.SYNOPSIS
    Builds, assembles, checks and zips an OpenOSV release for Windows x64.

.DESCRIPTION
    One command from a source checkout to the zips that go on GitHub
    Releases, one per editor:

        dist\OpenOSV-<version>-premiere-windows-x64\      Premiere Pro, unzipped
        dist\OpenOSV-<version>-premiere-windows-x64.zip   the same, zipped
        dist\OpenOSV-<version>-resolve-windows-x64\       DaVinci Resolve, unzipped
        dist\OpenOSV-<version>-resolve-windows-x64.zip    the same, zipped
        dist\RELEASE_NOTES.md                             a draft for the release page

    Each zip carries only what its editor uses, with its own Install.cmd,
    Uninstall.cmd, README.txt, licences and SHA256SUMS.txt; both carry
    cli\osvtool.exe and the LUTs.  The Resolve zip is made whenever the
    build made the OpenFX bundle (OSV_BUILD_OFX, on in the release preset).

    <version> is the CMake project version (project(openosv VERSION ...)).

    WHY THE MAINTAINER'S MACHINE BUILDS IT
    --------------------------------------
    The Premiere plug-ins compile against the Adobe Premiere Pro and After
    Effects SDKs, which Adobe does not allow anyone to redistribute.  They are
    never committed, so a hosted CI runner cannot build the plug-ins.  The
    release is built here, by this script, and uploaded by hand
    (docs/RELEASING.md).

    WHAT IT DOES, IN ORDER
    ----------------------
     1. Builds the windows-msvc-premiere-release preset into
        build\release-package (tests off, the optional neural-flow runtime
        off: the zip does not ship it).  vcpkg installs into its own tree,
        <VCPKG_ROOT>\installed-openosv-release, with binary caching off.
        That is deliberate: FFmpeg records its whole configure line in its
        DLLs, including the vcpkg install directory, and the default
        manifest tree lives inside the source checkout.  A release built
        from that tree would carry the maintainer's source path to every
        user.  -BuildDir / -SkipBuild package an existing build instead.
     2. Checks the build: Release, plug-ins on, the expected CUDA
        architectures, and an osvtool that reports this version.
     3. Assembles the packages.  In the Premiere package, everything
        install_plugins.ps1 needs sits where that script looks for it
        (plugins\OpenOSV, luts, presets, panel, all beside its own scripts\
        folder), so the packaged script installs from the package with no
        arguments.  In the Resolve package, the OpenFX bundle sits at
        plugins\OpenOSV.ofx.bundle, where install_ofx.ps1 looks for it.
        cli\ holds osvtool.exe and exactly the DLLs it imports.  The module
        and CLI DLL sets are the import closure walked with dumpbin, so a
        stale DLL left in the build folder by an older FFmpeg is never
        shipped.
     4. Generates the LUTs with the PACKAGED osvtool (scripts\gen_luts.ps1),
        which also proves cli\ runs on its own, and builds the panel's .ccx
        with the PACKAGED install_plugins.ps1 (a dry run into a temporary
        folder).
     5. Writes Install.cmd, Uninstall.cmd, README.txt, licenses\ (every
        third-party component that is in a shipped binary, FFmpeg's LGPL
        build record included) and SHA256SUMS.txt.
     6. Hygiene scan: every printable string, ASCII and UTF-16, of every
        shipped file.  Any user-profile path, any path into the folder that
        holds this checkout, or anything listed in the maintainer's private
        rules file (see -PrivateRules) fails the package.  Third-party
        licence texts are exempt from the private phrases only: they are
        verbatim legal texts.
     7. Zips each package, prints its SHA-256 and writes the release notes
        draft (scanned too, since it is published).

    Nothing here installs anything, pushes, tags or creates a release.

.PARAMETER BuildDir
    An existing build to package, or where to build.  Defaults to
    <repo>\build\release-package.  Given on its own, the folder is brought
    up to date with `cmake --build` (not reconfigured) and then packaged.

.PARAMETER SkipBuild
    Package the build folder exactly as it is.

.PARAMETER OutDir
    Where the package, the zip and the release notes go.  Defaults to
    <repo>\dist.

.PARAMETER VcpkgRoot
    The vcpkg checkout.  Defaults to $env:VCPKG_ROOT, then C:\vcpkg.

.PARAMETER VcpkgInstalledDir
    The vcpkg install tree for the release build.  Defaults to
    <VcpkgRoot>\installed-openosv-release.  It must NOT be inside the
    checkout, for the reason given under step 1.

.PARAMETER PremiereSdkDir
    Passed to the configure as OSV_PREMIERE_SDK_DIR when the SDK is not at
    the preset's default place.

.PARAMETER AeSdkDir
    Passed to the configure as OSV_AE_SDK_DIR likewise.

.PARAMETER Jobs
    Parallel build jobs; 0 (the default) lets Ninja decide.

.PARAMETER PrivateRules
    A local text file of extra strings no shipped file may contain, kept
    OUTSIDE the repository because the strings themselves are private.
    Defaults to %LOCALAPPDATA%\OpenOSV\release-hygiene.txt.  One rule per
    line, blank lines and '#' comments ignored:
        phrase <words>    words that may be joined by spaces, '-' or '_',
                          matched case-insensitively
        secret <text>     letters and digits matched case-insensitively
                          and never printed, not even in a finding
    A missing file is a warning, not an error: the built-in rules still run.

.EXAMPLE
    scripts\package_release.ps1
    Builds everything and packages it.

.EXAMPLE
    scripts\package_release.ps1 -SkipBuild
    Re-packages build\release-package as it is.

.NOTES
    Output is deliberately plain ASCII, like the other scripts here.
    Runs under Windows PowerShell 5.1 and PowerShell 7.
#>
[CmdletBinding()]
param(
    [string] $BuildDir,
    [switch] $SkipBuild,
    [string] $OutDir,
    [string] $VcpkgRoot,
    [string] $VcpkgInstalledDir,
    [string] $PremiereSdkDir,
    [string] $AeSdkDir,
    [ValidateRange(0, 256)]
    [int] $Jobs = 0,
    [string] $PrivateRules = (Join-Path $env:LOCALAPPDATA 'OpenOSV\release-hygiene.txt')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$OutputEncoding = [System.Text.Encoding]::ASCII

# ===========================================================================
#  Constants
# ===========================================================================

# The checkout this script belongs to.
$script:RepoRoot = Split-Path -Parent $PSScriptRoot

# The configure preset a release is built from, and where it goes.
$script:Preset = 'windows-msvc-premiere-release'
$script:DefaultBuildDir = Join-Path $script:RepoRoot 'build\release-package'

# The three modules install_plugins.ps1 installs (the same list it keeps).
$script:PluginModules = @('OpenOSVImporter.prm', 'Open360Reframe.aex', 'OpenOSVSourceSettings.aex')

# The OpenFX bundle for DaVinci Resolve (plugins/ofx, docs/RESOLVE.md): the
# folder install_ofx.ps1 installs and the binary inside it.  It ships in its
# own package, the Resolve zip, whenever the build made it (OSV_BUILD_OFX,
# on in the release preset).
$script:OfxBundleName = 'OpenOSV.ofx.bundle'
$script:OfxBinary = 'Contents\Win64\OpenOSV.ofx'

# ---------------------------------------------------------------------------
#  The CUDA architectures README.txt, the release notes and README.md
#  describe.  SASS for Turing (7.5), Ampere (8.6), Ada (8.9) and Blackwell
#  (12.0), plus compute_120 PTX that later 12.x parts compile on first use.
#  A build with any other list fails the package, because the GPU text
#  below would then be wrong: change the text and this value together.
# ---------------------------------------------------------------------------
$script:ExpectedCudaArchitectures = '75-real;86-real;89-real;120-real;120-virtual'

# DLLs a module may import that are neither ours nor Windows': the NVIDIA
# driver installs nvcuda.dll (delay-loaded, only touched on an NVIDIA GPU).
$script:DriverDlls = @('nvcuda.dll')

# ---------------------------------------------------------------------------
#  Third-party components, by vcpkg port.
#
#    Dll       regex of the DLL file names that ARE the component; the
#              component is listed when a shipped DLL matches.
#    In        for code compiled into our own binaries (header-only or
#              static), described in words.  Listed always.
#    InFiles   the same, as the binaries that contain it: listed when one
#              of them is in the package, naming only those that are.
#    File      the name its licence text gets under licenses\.
#
#  Any shipped DLL that matches no entry fails the package: a new
#  dependency must bring its licence along before it can ship.
# ---------------------------------------------------------------------------
$script:ThirdParty = @(
    @{ Port = 'ffmpeg';        Name = 'FFmpeg';                   Licence = 'LGPL-2.1-or-later';
       Dll  = '^(avcodec|avformat|avutil|swresample|swscale|avfilter|avdevice|postproc)-\d+\.dll$';
       File = 'FFmpeg-LGPL-2.1.txt' }
    @{ Port = 'zlib';          Name = 'zlib';                     Licence = 'Zlib';
       Dll  = '^(z|zlib|zlib1)\.dll$';                            File = 'zlib.txt' }
    @{ Port = 'fmt';           Name = '{fmt}';                    Licence = 'MIT';
       Dll  = '^fmt\.dll$';                                       File = 'fmt.txt' }
    @{ Port = 'spdlog';        Name = 'spdlog';                   Licence = 'MIT';
       Dll  = '^spdlog\.dll$';                                    File = 'spdlog.txt' }
    @{ Port = 'opencl';        Name = 'Khronos OpenCL ICD Loader'; Licence = 'Apache-2.0';
       Dll  = '^OpenCL\.dll$';                                    File = 'OpenCL-ICD-Loader.txt' }
    @{ Port = 'miniz';         Name = 'miniz';                    Licence = 'MIT';
       Dll  = '^miniz\.dll$';                                     File = 'miniz.txt' }
    @{ Port = 'ffnvcodec';     Name = 'nv-codec-headers';         Licence = 'MIT';
       In   = 'the FFmpeg DLLs (NVDEC / NVENC interface)';        File = 'nv-codec-headers.txt' }
    @{ Port = 'nlohmann-json'; Name = 'nlohmann/json';            Licence = 'MIT';
       InFiles = @('OpenOSVImporter.prm', 'OpenOSVSourceSettings.aex', 'osvtool.exe'); File = 'nlohmann-json.txt' }
    @{ Port = 'cli11';         Name = 'CLI11';                    Licence = 'BSD-3-Clause';
       InFiles = @('osvtool.exe');                                File = 'CLI11.txt' }
    @{ Port = 'tinyexr';       Name = 'tinyexr';                  Licence = 'BSD-3-Clause';
       InFiles = @('osvtool.exe');                                File = 'tinyexr.txt' }
)

# ===========================================================================
#  Output helpers - one place to change the prefixes.
# ===========================================================================
function Write-Step { param([string] $Message) Write-Host "==> $Message" }
function Write-Info { param([string] $Message) Write-Host "    $Message" }
function Write-Warn { param([string] $Message) Write-Warning $Message }

# ===========================================================================
#  Small utilities
# ===========================================================================

# ---------------------------------------------------------------------------
#  Run a native program and capture its output as strings.
#
#  $ErrorActionPreference is relaxed around the call: under Windows
#  PowerShell 5.1 a native program's stderr line, redirected with 2>&1,
#  becomes a terminating error when the preference is Stop, which would turn
#  every compiler warning into a crash of this script.  The exit code is
#  what decides success.
# ---------------------------------------------------------------------------
function Invoke-Captured {
    param(
        [string]   $FilePath,
        [string[]] $Arguments = @()
    )
    if (-not $FilePath) {
        throw 'Invoke-Captured: no program given.'
    }
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(& $FilePath @Arguments 2>&1 | ForEach-Object { "$_" })
        $code = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previous
    }
    return [pscustomobject]@{ ExitCode = $code; Output = $output }
}

# ---------------------------------------------------------------------------
#  Run a native program with its output streamed to the console (builds,
#  child scripts).  Out-Host sends it to the console even when the caller
#  captures or discards this function's output, and keeps it out of any
#  value the caller returns.  Throws with $What when it fails.
# ---------------------------------------------------------------------------
function Invoke-Streamed {
    param(
        [string]   $What,
        [string]   $FilePath,
        [string[]] $Arguments = @()
    )
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $FilePath @Arguments | Out-Host
        $code = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previous
    }
    if ($code -ne 0) {
        throw "$What failed with exit code $code."
    }
}

# The PowerShell executable running this script, for child scripts: they
# then run under the same edition the maintainer chose.
function Get-PowerShellExe { return (Get-Process -Id $PID).Path }

# ---------------------------------------------------------------------------
#  Write a text file as plain ASCII with the given line ending.  Refuses
#  anything outside ASCII: the package's texts must read the same on every
#  code page, and a stray typographic character is a bug here.
# ---------------------------------------------------------------------------
function Write-AsciiFile {
    param(
        [string] $Path,
        [string] $Text,
        [ValidateSet('CRLF', 'LF')] [string] $LineEnding = 'CRLF'
    )
    if ($null -eq $Text) {
        throw "Write-AsciiFile: no text for '$Path'."
    }
    if ($Text -match '[^\x00-\x7F]') {
        throw "Write-AsciiFile: '$Path' would contain non-ASCII text: '$($Matches[0])'."
    }
    # Normalise to LF first, end with exactly one newline, then apply the
    # requested ending.
    $normalised = ($Text -replace "`r`n", "`n").TrimEnd("`n") + "`n"
    if ($LineEnding -eq 'CRLF') {
        $normalised = $normalised -replace "`n", "`r`n"
    }
    [System.IO.File]::WriteAllText($Path, $normalised, [System.Text.Encoding]::ASCII)
}

# Write UTF-8 text without a byte-order mark (Markdown for GitHub).
function Write-Utf8File {
    param([string] $Path, [string] $Text)
    $encoding = New-Object System.Text.UTF8Encoding($false)
    [System.IO.File]::WriteAllText($Path, ($Text -replace "`r`n", "`n"), $encoding)
}

# ---------------------------------------------------------------------------
#  SHA-256 of a file as lower-case hex, with .NET directly rather than
#  Get-FileHash, which Windows PowerShell 5.1 loads from a script module and
#  so depends on module autoloading working in this process.
# ---------------------------------------------------------------------------
function Get-Sha256 {
    param([string] $Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Get-Sha256: '$Path' does not exist."
    }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $hash = $sha.ComputeHash($stream)
    }
    finally {
        $stream.Dispose()
        $sha.Dispose()
    }
    return (($hash | ForEach-Object { $_.ToString('x2') }) -join '')
}

# A path relative to $Root, with backslashes.
function Get-RelativePath {
    param([string] $Root, [string] $Path)
    $rootFull = [System.IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $full = [System.IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith($rootFull, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Get-RelativePath: '$Path' is not inside '$Root'."
    }
    return $full.Substring($rootFull.Length)
}

# Create a directory (and its parents) when it is missing.
function New-Directory {
    param([string] $Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        New-Item -ItemType Directory -Path $Path -Force | Out-Null
    }
}

# A package folder, emptied first, so nothing an earlier run left in it can
# ship.  Returns the path.
function New-EmptyPackage {
    param([string] $Path)
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
    New-Directory $Path
    return $Path
}

# ===========================================================================
#  Versions and build facts
# ===========================================================================

# ---------------------------------------------------------------------------
#  The CMake project version, straight from CMakeLists.txt: the one place
#  the version is defined (Version.h, the CLI banner and the plug-ins'
#  resources all derive from it).
# ---------------------------------------------------------------------------
function Get-ProjectVersion {
    $cmakeLists = Join-Path $script:RepoRoot 'CMakeLists.txt'
    if (-not (Test-Path -LiteralPath $cmakeLists -PathType Leaf)) {
        throw "Cannot find '$cmakeLists'."
    }
    $text = Get-Content -LiteralPath $cmakeLists -Raw
    $match = [regex]::Match($text, '(?s)project\s*\(\s*openosv\s+VERSION\s+(\d+\.\d+\.\d+)')
    if (-not $match.Success) {
        throw "No 'project(openosv VERSION x.y.z' in '$cmakeLists'."
    }
    $version = $match.Groups[1].Value

    # vcpkg.json carries the same number; a mismatch is worth a warning,
    # not a failure (it changes nothing in the binaries).
    $manifestPath = Join-Path $script:RepoRoot 'vcpkg.json'
    if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
        $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
        if ($manifest.PSObject.Properties['version-string'] -and $manifest.'version-string' -ne $version) {
            Write-Warn "vcpkg.json says version $($manifest.'version-string'), CMakeLists.txt says $version. Keep them equal (docs/RELEASING.md)."
        }
    }
    return $version
}

# ---------------------------------------------------------------------------
#  Parse CMakeCache.txt into a hashtable NAME -> VALUE.
# ---------------------------------------------------------------------------
function Read-CMakeCache {
    param([string] $Dir)
    $cachePath = Join-Path $Dir 'CMakeCache.txt'
    if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
        throw "'$Dir' is not a configured CMake build (no CMakeCache.txt)."
    }
    $cache = @{}
    foreach ($line in [System.IO.File]::ReadAllLines($cachePath)) {
        # NAME:TYPE=VALUE, skipping comments.
        $m = [regex]::Match($line, '^([A-Za-z_][A-Za-z0-9_.+-]*):([A-Z_]+)=(.*)$')
        if ($m.Success) {
            $cache[$m.Groups[1].Value] = $m.Groups[3].Value
        }
    }
    return $cache
}

# A cache value or $null.
function Get-CacheValue {
    param([hashtable] $Cache, [string] $Name)
    if ($Cache.ContainsKey($Name)) {
        return [string]$Cache[$Name]
    }
    return $null
}

# The commit the checkout is at, and whether it has local changes.  Only
# informational (README.txt, release notes): git missing is not an error.
function Get-SourceCommit {
    $git = Get-Command git -ErrorAction SilentlyContinue
    if (-not $git) {
        return 'unknown'
    }
    $head = Invoke-Captured -FilePath $git.Source -Arguments @('-C', $script:RepoRoot, 'rev-parse', 'HEAD')
    if ($head.ExitCode -ne 0 -or $head.Output.Count -eq 0) {
        return 'unknown'
    }
    $commit = $head.Output[0].Trim()
    $status = Invoke-Captured -FilePath $git.Source -Arguments @('-C', $script:RepoRoot, 'status', '--porcelain', '--untracked-files=no')
    if ($status.ExitCode -eq 0 -and @($status.Output | Where-Object { $_.Trim() }).Count -gt 0) {
        Write-Warn 'The checkout has uncommitted changes; the package records the commit with "(with local changes)".'
        return "$commit (with local changes)"
    }
    return $commit
}

# ===========================================================================
#  Step 1 - the build
# ===========================================================================

# ---------------------------------------------------------------------------
#  Import the Visual Studio x64 developer environment into this process,
#  through scripts\vsdev.cmd (which already knows where Visual Studio is):
#  vsdev.cmd runs its arguments inside the environment, so "set" prints it.
# ---------------------------------------------------------------------------
function Import-VsDevEnvironment {
    $vsdev = Join-Path $PSScriptRoot 'vsdev.cmd'
    if (-not (Test-Path -LiteralPath $vsdev -PathType Leaf)) {
        throw "Cannot find '$vsdev'."
    }
    # /s strips the outer quotes only, so a path with spaces or brackets in
    # it survives cmd's parsing.
    $dump = Invoke-Captured -FilePath 'cmd.exe' -Arguments @('/d', '/s', '/c', ('"' + '"' + $vsdev + '" set' + '"'))
    if ($dump.ExitCode -ne 0) {
        throw "Could not enter the Visual Studio developer environment: $($dump.Output -join ' ')"
    }
    $count = 0
    foreach ($line in $dump.Output) {
        $m = [regex]::Match($line, '^([^=]+)=(.*)$')
        # PSModulePath stays this process's own: the value cmd reports can
        # put another PowerShell edition's modules first, after which
        # Windows PowerShell 5.1 can no longer autoload its own commands.
        if ($m.Success -and $m.Groups[1].Value -ne 'PSModulePath') {
            [System.Environment]::SetEnvironmentVariable($m.Groups[1].Value, $m.Groups[2].Value, 'Process')
            $count++
        }
    }
    if ($count -eq 0 -or -not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw 'The Visual Studio developer environment did not provide cl.exe. Install Visual Studio 2022 with the C++ workload (docs/BUILDING.md).'
    }
    Write-Info 'Visual Studio x64 developer environment loaded.'
}

# ---------------------------------------------------------------------------
#  Configure and build the release.
#
#  The preset is used as it is, with four overrides:
#    -B <BuildDir>                  its own build folder, so a developer
#                                   build is never reconfigured;
#    VCPKG_INSTALLED_DIR            outside the checkout (see the header);
#    OSV_BUILD_TESTS=OFF            nothing from tests\ can end up shipped,
#                                   and the build is a third shorter;
#    OSV_ENABLE_ONNXRUNTIME=OFF     the neural-flow runtime is ~1.4 GB and is
#                                   not shipped, so the backend is not built
#                                   into a binary that could never load it;
#                                   the classical flow is always there.
#  VCPKG_BINARY_SOURCES=clear keeps vcpkg from restoring an FFmpeg archive
#  that was built with the checkout's install path in its configure line
#  (the path is not part of vcpkg's package hash).  The first run therefore
#  builds every port from source; later runs find them installed.
# ---------------------------------------------------------------------------
function Invoke-ReleaseBuild {
    param(
        [string] $Dir,
        [switch] $BuildOnly
    )
    Import-VsDevEnvironment
    $env:VCPKG_ROOT = $VcpkgRoot

    if (-not $BuildOnly) {
        # The install tree must not live in the checkout: its path ends up
        # inside FFmpeg's DLLs.
        $installedFull = [System.IO.Path]::GetFullPath($VcpkgInstalledDir)
        $repoFull = [System.IO.Path]::GetFullPath($script:RepoRoot).TrimEnd('\') + '\'
        if ($installedFull.StartsWith($repoFull, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "-VcpkgInstalledDir '$installedFull' is inside the checkout. FFmpeg records that path in its DLLs; use a folder outside '$($script:RepoRoot)'."
        }

        $arguments = @(
            '--preset', $script:Preset,
            '-B', ($Dir -replace '\\', '/'),
            ('-DVCPKG_INSTALLED_DIR=' + ($installedFull -replace '\\', '/')),
            '-DOSV_BUILD_TESTS=OFF',
            '-DOSV_ENABLE_ONNXRUNTIME=OFF'
        )
        if ($PremiereSdkDir) {
            $arguments += ('-DOSV_PREMIERE_SDK_DIR=' + ([System.IO.Path]::GetFullPath($PremiereSdkDir) -replace '\\', '/'))
        }
        if ($AeSdkDir) {
            $arguments += ('-DOSV_AE_SDK_DIR=' + ([System.IO.Path]::GetFullPath($AeSdkDir) -replace '\\', '/'))
        }

        Write-Step "Configuring $($script:Preset) into $Dir"
        Write-Info "vcpkg install tree: $installedFull (binary caching off)"
        $previousSources = $env:VCPKG_BINARY_SOURCES
        $env:VCPKG_BINARY_SOURCES = 'clear'
        # --preset is resolved against the current directory.
        Push-Location -LiteralPath $script:RepoRoot
        try {
            Invoke-Streamed -What 'CMake configure' -FilePath 'cmake' -Arguments $arguments
        }
        finally {
            Pop-Location
            $env:VCPKG_BINARY_SOURCES = $previousSources
        }
    }

    Write-Step "Building $Dir"
    $buildArgs = @('--build', ($Dir -replace '\\', '/'), '--parallel')
    if ($Jobs -gt 0) {
        $buildArgs += [string]$Jobs
    }
    Invoke-Streamed -What 'CMake build' -FilePath 'cmake' -Arguments $buildArgs
}

# ===========================================================================
#  Step 2 - what the build is
# ===========================================================================

# ---------------------------------------------------------------------------
#  Collect and check everything the packaging needs from a build folder.
# ---------------------------------------------------------------------------
function Get-BuildInfo {
    param([string] $Dir, [string] $Version)

    $cache = Read-CMakeCache -Dir $Dir

    # Release only: a Debug build imports the debug CRT, which Microsoft
    # does not allow anyone to redistribute, and is slow besides.
    $buildType = Get-CacheValue $cache 'CMAKE_BUILD_TYPE'
    if ($buildType -ne 'Release') {
        throw "'$Dir' is a '$buildType' build. Package a Release build (the $($script:Preset) preset)."
    }
    if ((Get-CacheValue $cache 'OSV_BUILD_PREMIERE') -notmatch '^(ON|TRUE|1|YES)$') {
        throw "'$Dir' was configured without the Premiere plug-ins (OSV_BUILD_PREMIERE is off)."
    }

    # Where the plug-ins were staged and where osvtool is.
    $stage = Get-CacheValue $cache 'OSV_PLUGIN_STAGE_DIR'
    if (-not $stage) {
        $stage = Join-Path $Dir 'plugins\OpenOSV'
    }
    $stage = [System.IO.Path]::GetFullPath($stage)
    foreach ($module in $script:PluginModules) {
        if (-not (Test-Path -LiteralPath (Join-Path $stage $module) -PathType Leaf)) {
            throw "The build has no '$module' in '$stage'. Build the $($script:Preset) preset completely first."
        }
    }
    $bin = Join-Path $Dir 'bin'
    $osvtool = Join-Path $bin 'osvtool.exe'
    if (-not (Test-Path -LiteralPath $osvtool -PathType Leaf)) {
        throw "The build has no '$osvtool'."
    }

    # The OpenFX bundle: required when the build was configured with it, so
    # a half-built bundle fails here rather than shipping without Resolve.
    $ofxBundle = $null
    if ((Get-CacheValue $cache 'OSV_BUILD_OFX') -match '^(ON|TRUE|1|YES)$') {
        $ofxStage = Get-CacheValue $cache 'OSV_OFX_STAGE_DIR'
        if (-not $ofxStage) {
            $ofxStage = Join-Path $Dir 'plugins\ofx'
        }
        $ofxBundle = [System.IO.Path]::GetFullPath((Join-Path $ofxStage $script:OfxBundleName))
        if (-not (Test-Path -LiteralPath (Join-Path $ofxBundle $script:OfxBinary) -PathType Leaf)) {
            throw "The build has no '$($script:OfxBinary)' in '$ofxBundle'. Build the $($script:Preset) preset completely first."
        }
    }

    # dumpbin walks the import tables.  The plug-in build records the one
    # it used; the developer environment's is the fallback.
    $dumpbin = Get-CacheValue $cache 'OSV_DUMPBIN_EXE'
    if (-not $dumpbin -or -not (Test-Path -LiteralPath $dumpbin -PathType Leaf)) {
        $found = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
        if (-not $found) {
            throw 'dumpbin.exe was not found (neither OSV_DUMPBIN_EXE in the build nor on PATH).'
        }
        $dumpbin = $found.Source
    }

    # vcpkg's install tree: the licence texts and FFmpeg's provenance.
    $installed = Get-CacheValue $cache 'VCPKG_INSTALLED_DIR'
    $triplet = Get-CacheValue $cache 'VCPKG_TARGET_TRIPLET'
    if (-not $triplet) {
        $triplet = 'x64-windows'
    }
    if (-not $installed) {
        throw "'$Dir' does not record VCPKG_INSTALLED_DIR; it was not configured with vcpkg."
    }
    $share = Join-Path $installed "$triplet\share"
    if (-not (Test-Path -LiteralPath $share -PathType Container)) {
        throw "vcpkg's share folder '$share' is missing; the licence texts come from there."
    }

    # CUDA: the kernels' architecture list must be the one the texts
    # describe, and the toolkit gives the runtime's version and EULA.
    $cuda = $null
    if ((Get-CacheValue $cache 'OSV_ENABLE_CUDA') -match '^(ON|TRUE|1|YES)$') {
        $archs = Get-CacheValue $cache 'CMAKE_CUDA_ARCHITECTURES'
        if ($archs -ne $script:ExpectedCudaArchitectures) {
            throw "CMAKE_CUDA_ARCHITECTURES is '$archs', but README.txt, the release notes and README.md describe '$($script:ExpectedCudaArchitectures)'. Update the GPU text in this script and README.md, then `$script:ExpectedCudaArchitectures."
        }
        $nvcc = Get-CacheValue $cache 'CMAKE_CUDA_COMPILER'
        if (-not $nvcc -or -not (Test-Path -LiteralPath $nvcc -PathType Leaf)) {
            throw "The build's CUDA compiler '$nvcc' is gone; the CUDA runtime's licence and version come from that toolkit."
        }
        $toolkit = Split-Path -Parent (Split-Path -Parent $nvcc)
        $cuda = [pscustomobject]@{ Architectures = $archs; Toolkit = $toolkit }
    }

    # The binary must be the version the source says: a stale build would
    # ship under the wrong number.  Run it from the build folder, where its
    # DLLs are.
    $run = Invoke-Captured -FilePath $osvtool -Arguments @('--version')
    if ($run.ExitCode -ne 0) {
        throw "'$osvtool --version' failed with exit code $($run.ExitCode): $($run.Output -join ' ')"
    }
    $banner = @($run.Output | Where-Object { $_ -match '^OpenOSV\s' } | Select-Object -First 1)
    if ($banner.Count -eq 0 -or $banner[0].Trim() -ne "OpenOSV $Version") {
        throw "The build's osvtool reports '$($banner -join '')' but CMakeLists.txt says $Version. Rebuild before packaging."
    }
    $ffmpegLine = @($run.Output | Where-Object { $_ -match '^\s*ffmpeg\s*:' } | Select-Object -First 1)

    return [pscustomobject]@{
        Dir          = $Dir
        Cache        = $cache
        Stage        = $stage
        Bin          = $bin
        OsvTool      = $osvtool
        Dumpbin      = $dumpbin
        Installed    = $installed
        Share        = $share
        Cuda         = $cuda
        OfxBundle    = $ofxBundle
        FfmpegBanner = if ($ffmpegLine.Count -gt 0) { $ffmpegLine[0].Trim() } else { '' }
    }
}

# ===========================================================================
#  Import closures
# ===========================================================================

# The DLL names a PE file imports, static and delay-load, via dumpbin.
function Get-PeDependents {
    param([string] $Dumpbin, [string] $File)
    $run = Invoke-Captured -FilePath $Dumpbin -Arguments @('/NOLOGO', '/DEPENDENTS', $File)
    if ($run.ExitCode -ne 0) {
        throw "dumpbin failed on '$File': $($run.Output -join ' ')"
    }
    # The names are indented on lines of their own; the "Dump of file"
    # header contains spaces, so it never matches.
    return @($run.Output | ForEach-Object { $_.Trim() } | Where-Object { $_ -match '^[^\s]+\.dll$' })
}

# ---------------------------------------------------------------------------
#  Walk the import graph from $Roots.  A dependency found in $SearchDir is
#  shipped (and walked in turn); anything else must be Windows' own, the
#  VC++ runtime Premiere installs, or a driver DLL - or the package fails,
#  because the binary would not start on a user's machine.
#
#  Returns the full paths of the DLLs to ship, sorted by name.
# ---------------------------------------------------------------------------
function Resolve-DllClosure {
    param(
        [string[]] $Roots,
        [string]   $SearchDir,
        [string]   $Dumpbin
    )
    $system32 = Join-Path $env:SystemRoot 'System32'
    $ship = @{}
    $visited = @{}
    $queue = New-Object System.Collections.Queue
    foreach ($root in $Roots) {
        $queue.Enqueue($root)
    }
    while ($queue.Count -gt 0) {
        $file = [string]$queue.Dequeue()
        $key = $file.ToLowerInvariant()
        if ($visited.ContainsKey($key)) {
            continue
        }
        $visited[$key] = $true

        foreach ($name in (Get-PeDependents -Dumpbin $Dumpbin -File $file)) {
            $lower = $name.ToLowerInvariant()
            # The debug CRT is never redistributable: a Debug object slipped in.
            if ($lower -match '^(msvcp|vcruntime|ucrtbase|concrt|vccorlib)\d*d(_\d+)?\.dll$') {
                throw "'$file' imports the debug runtime '$name'. Only a Release build can ship."
            }
            $local = Join-Path $SearchDir $name
            if (Test-Path -LiteralPath $local -PathType Leaf) {
                if (-not $ship.ContainsKey($lower)) {
                    $ship[$lower] = $local
                    $queue.Enqueue($local)
                }
                continue
            }
            # API sets, Windows DLLs and the VC++ runtime live in System32.
            if ($lower -like 'api-ms-win-*' -or $lower -like 'ext-ms-*') {
                continue
            }
            if (Test-Path -LiteralPath (Join-Path $system32 $name) -PathType Leaf) {
                continue
            }
            if ($script:DriverDlls -contains $lower) {
                continue
            }
            throw "'$file' imports '$name', which is neither in '$SearchDir' nor a Windows or driver DLL. It would not load on a user's machine."
        }
    }
    return @($ship.Values | Sort-Object { Split-Path -Leaf $_ })
}

# ===========================================================================
#  Step 3/4 - assembly
# ===========================================================================

# ---------------------------------------------------------------------------
#  plugins\OpenOSV: the three modules plus their import closure, taken from
#  the stage folder alone (so an incomplete stage fails here, not on a
#  user's machine).  Anything else in the stage folder - PDBs, a DLL an
#  older build left behind - stays out, and is reported.
# ---------------------------------------------------------------------------
function Copy-PluginModules {
    param($Build, [string] $Package)
    $target = Join-Path $Package 'plugins\OpenOSV'
    New-Directory $target

    $roots = @($script:PluginModules | ForEach-Object { Join-Path $Build.Stage $_ })
    $dlls = Resolve-DllClosure -Roots $roots -SearchDir $Build.Stage -Dumpbin $Build.Dumpbin
    $files = @($roots) + @($dlls)
    foreach ($file in $files) {
        Copy-Item -LiteralPath $file -Destination $target -Force
    }

    $shippedNames = @($files | ForEach-Object { (Split-Path -Leaf $_).ToLowerInvariant() })
    $left = @(Get-ChildItem -LiteralPath $Build.Stage -File |
        Where-Object { $shippedNames -notcontains $_.Name.ToLowerInvariant() })
    foreach ($item in $left) {
        Write-Info "left out of the package (not imported by any module): $($item.Name)"
    }
    Write-Info ("plugins\OpenOSV: {0} modules, {1} DLLs" -f $roots.Count, $dlls.Count)
    return @($dlls | ForEach-Object { Split-Path -Leaf $_ })
}

# ---------------------------------------------------------------------------
#  plugins\OpenOSV.ofx.bundle: the OpenFX binary plus its import closure,
#  taken from the bundle's own Contents\Win64 (the folder its delay-load
#  hook loads them from), and the OpenFX licence under Contents\Resources.
#  Anything else in the built bundle - a PDB, a stale DLL - stays out.
# ---------------------------------------------------------------------------
function Copy-OfxBundle {
    param($Build, [string] $Package)
    $sourceBin = Join-Path $Build.OfxBundle 'Contents\Win64'
    $target = Join-Path $Package "plugins\$($script:OfxBundleName)"
    $targetBin = Join-Path $target 'Contents\Win64'
    $targetRes = Join-Path $target 'Contents\Resources'
    New-Directory $targetBin
    New-Directory $targetRes

    $root = Join-Path $Build.OfxBundle $script:OfxBinary
    $dlls = Resolve-DllClosure -Roots @($root) -SearchDir $sourceBin -Dumpbin $Build.Dumpbin
    foreach ($file in (@($root) + @($dlls))) {
        Copy-Item -LiteralPath $file -Destination $targetBin -Force
    }
    $licence = Join-Path $Build.OfxBundle 'Contents\Resources\OpenFX-LICENSE.md'
    if (-not (Test-Path -LiteralPath $licence -PathType Leaf)) {
        throw "The built bundle has no '$licence'."
    }
    Copy-Item -LiteralPath $licence -Destination $targetRes -Force

    $shipped = @((@($root) + @($dlls)) | ForEach-Object { (Split-Path -Leaf $_).ToLowerInvariant() })
    $left = @(Get-ChildItem -LiteralPath $sourceBin -File |
        Where-Object { $shipped -notcontains $_.Name.ToLowerInvariant() })
    foreach ($item in $left) {
        Write-Info "left out of the bundle (not imported by OpenOSV.ofx): $($item.Name)"
    }
    Write-Info ("plugins\{0}: OpenOSV.ofx, {1} DLLs" -f $script:OfxBundleName, $dlls.Count)
    return @($dlls | ForEach-Object { Split-Path -Leaf $_ })
}

# ---------------------------------------------------------------------------
#  cli\: osvtool.exe and exactly the DLLs it imports.
# ---------------------------------------------------------------------------
function Copy-CommandLineTool {
    param($Build, [string] $Package)
    $target = Join-Path $Package 'cli'
    New-Directory $target
    $dlls = Resolve-DllClosure -Roots @($Build.OsvTool) -SearchDir $Build.Bin -Dumpbin $Build.Dumpbin
    Copy-Item -LiteralPath $Build.OsvTool -Destination $target -Force
    foreach ($dll in $dlls) {
        Copy-Item -LiteralPath $dll -Destination $target -Force
    }
    Write-Info ("cli: osvtool.exe and {0} DLLs" -f $dlls.Count)
    return @($dlls | ForEach-Object { Split-Path -Leaf $_ })
}

# ---------------------------------------------------------------------------
#  Run $Block with PATH cut down to Windows' own folders, so a program
#  started inside it can only find its DLLs next to itself - which is what
#  a user's machine offers.  PATH is restored whatever happens.
# ---------------------------------------------------------------------------
function Invoke-WithBarePath {
    param([scriptblock] $Block)
    $previous = $env:PATH
    $env:PATH = (@(
        (Join-Path $env:SystemRoot 'System32'),
        $env:SystemRoot,
        (Join-Path $env:SystemRoot 'System32\Wbem'),
        (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0')
    ) -join ';')
    try {
        return (& $Block)
    }
    finally {
        $env:PATH = $previous
    }
}

# ---------------------------------------------------------------------------
#  Prove the packaged CLI runs from its own folder: `osvtool --version`
#  with a bare PATH, and the banner must name this version.
# ---------------------------------------------------------------------------
function Test-PackagedCli {
    param([string] $Package, [string] $Version)
    $exe = Join-Path $Package 'cli\osvtool.exe'
    $run = Invoke-WithBarePath { Invoke-Captured -FilePath $exe -Arguments @('--version') }
    if ($run.ExitCode -ne 0) {
        throw "The packaged '$exe --version' failed with exit code $($run.ExitCode): $($run.Output -join ' ')"
    }
    if (-not (@($run.Output | ForEach-Object { $_.Trim() }) -contains "OpenOSV $Version")) {
        throw "The packaged osvtool printed '$($run.Output -join ' | ')' instead of 'OpenOSV $Version'."
    }
    Write-Info "cli\osvtool.exe --version runs from the package alone:"
    foreach ($line in $run.Output) {
        Write-Info "    $line"
    }
}

# ---------------------------------------------------------------------------
#  luts\: generated by scripts\gen_luts.ps1 with the PACKAGED osvtool, so
#  the tables are exactly what this build computes.  A mismatch with the
#  committed luts\ is reported: it means the repository's copies are stale.
# ---------------------------------------------------------------------------
function New-PackageLuts {
    param([string] $Package)
    $target = Join-Path $Package 'luts'
    New-Directory $target
    $genLuts = Join-Path $PSScriptRoot 'gen_luts.ps1'
    $exe = Join-Path $Package 'cli\osvtool.exe'
    $psExe = Get-PowerShellExe
    Invoke-WithBarePath {
        Invoke-Streamed -What 'gen_luts.ps1' -FilePath $psExe -Arguments @(
            '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $genLuts, '-OsvTool', $exe, '-OutDir', $target)
    }

    $made = @(Get-ChildItem -LiteralPath $target -File -Filter '*.cube')
    if ($made.Count -eq 0) {
        throw "gen_luts.ps1 produced no .cube files in '$target'."
    }
    $committed = Join-Path $script:RepoRoot 'luts'
    foreach ($lut in $made) {
        $theirs = Join-Path $committed $lut.Name
        if (-not (Test-Path -LiteralPath $theirs -PathType Leaf)) {
            Write-Warn "luts\$($lut.Name) is not committed; run scripts\gen_luts.ps1 and commit it."
            continue
        }
        $a = (Get-Sha256 $lut.FullName)
        $b = (Get-Sha256 $theirs)
        if ($a -ne $b) {
            Write-Warn "The committed luts\$($lut.Name) differs from what this build generates; the package ships the generated one. Run scripts\gen_luts.ps1 and commit."
        }
    }
}

# presets\: the .sqpreset files install_plugins.ps1 installs.
function Copy-Presets {
    param([string] $Package)
    $source = Join-Path $script:RepoRoot 'presets'
    $files = @(Get-ChildItem -LiteralPath $source -File -Filter '*.sqpreset' -ErrorAction SilentlyContinue)
    if ($files.Count -eq 0) {
        throw "No .sqpreset files in '$source'."
    }
    $target = Join-Path $Package 'presets'
    New-Directory $target
    foreach ($file in $files) {
        Copy-Item -LiteralPath $file.FullName -Destination $target -Force
    }
    Write-Info ("presets: {0} sequence presets" -f $files.Count)
}

# ---------------------------------------------------------------------------
#  panel\: the sources install_plugins.ps1 stages (shared, uxp, cep - never
#  the tests), then the .ccx built by the PACKAGED install_plugins.ps1 in a
#  dry run, which checks the panel installs from the package layout and
#  leaves the UXP installer beside the sources for anyone installing it by
#  hand.
# ---------------------------------------------------------------------------
function Copy-Panel {
    param([string] $Package)
    $source = Join-Path $script:RepoRoot 'panel'
    $target = Join-Path $Package 'panel'
    New-Directory $target
    foreach ($part in @('shared', 'uxp', 'cep')) {
        $from = Join-Path $source $part
        if (-not (Test-Path -LiteralPath $from -PathType Container)) {
            throw "The panel sources are incomplete: '$from' is missing."
        }
        Copy-Item -LiteralPath $from -Destination $target -Recurse -Force
    }

    # The dry run writes only inside a temporary folder: no UPIA, no
    # registry, nothing in %APPDATA%.
    $work = Join-Path ([System.IO.Path]::GetTempPath()) ('OpenOSV-ccx-' + [guid]::NewGuid().ToString('N'))
    try {
        $installer = Join-Path $Package 'scripts\install_plugins.ps1'
        Invoke-Streamed -What 'Building the panel .ccx' -FilePath (Get-PowerShellExe) -Arguments @(
            '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $installer,
            '-PanelOnly', '-PanelFlavor', 'Uxp', '-PanelDestination', $work)
        $ccx = @(Get-ChildItem -LiteralPath (Join-Path $work 'OpenOSV-panel-state') -File -Filter '*.ccx' -ErrorAction SilentlyContinue)
        if ($ccx.Count -ne 1) {
            throw "install_plugins.ps1 did not leave exactly one .ccx in '$work'."
        }
        Copy-Item -LiteralPath $ccx[0].FullName -Destination $target -Force
        Write-Info "panel: CEP and UXP sources, $($ccx[0].Name)"
        return $ccx[0].Name
    }
    finally {
        if (Test-Path -LiteralPath $work) {
            Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

# ===========================================================================
#  Step 5 - texts
# ===========================================================================

# ---------------------------------------------------------------------------
#  Install.cmd / Uninstall.cmd of the Premiere Pro package.
#
#  Windows PowerShell 5.1 by full path (always present on Windows 10/11),
#  -ExecutionPolicy Bypass for that one process only: that is what lets a
#  script extracted from a downloaded zip run at all (Explorer copies the
#  zip's Mark-of-the-Web onto every file, and the default RemoteSigned
#  policy refuses such a script).  The machine's policy is never changed.
#  Every path is quoted, so a folder with spaces, brackets or '&' in its
#  name works; arguments are passed through to the script.
# ---------------------------------------------------------------------------
function Write-InstallCommands {
    param([string] $Package)

    $common = @'
@echo off
rem ===========================================================================
rem  OpenOSV - @TITLE@
rem
rem  Double-click it.  It runs scripts\install_plugins.ps1 from this folder
rem  with "-ExecutionPolicy Bypass" for that one PowerShell process, which is
rem  what lets a script that came out of a downloaded zip run.  The machine's
rem  policy is not changed.  The plug-ins live under Program Files, so the
rem  script asks Windows for administrator rights (a UAC prompt).
rem
rem  Arguments are passed on to the script, for example
rem      @NAME@ -NoPresets
rem ===========================================================================
setlocal EnableExtensions DisableDelayedExpansion

set "OSV_SCRIPT=%~dp0scripts\install_plugins.ps1"
if not exist "%OSV_SCRIPT%" goto :missing

rem A host that is running holds the plug-ins open: stop before anything.
call :running "Adobe Premiere Pro.exe" && goto :hostopen
call :running "Adobe Media Encoder.exe" && goto :hostopen
call :running "AfterFX.exe" && goto :hostopen

"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%OSV_SCRIPT%" @SWITCH@%*
set "OSV_EXIT=%ERRORLEVEL%"
echo.
if not "%OSV_EXIT%"=="0" goto :failed
@DONE@
goto :end

:failed
echo ==============================================================
echo  That did not work (exit code %OSV_EXIT%). The lines above say why.
echo  If a second window opened and closed, that was the part that
echo  needs administrator rights: open a Command Prompt as
echo  administrator and run @NAME@ from it to read its messages.
echo ==============================================================
goto :end

:hostopen
echo Close Premiere Pro, Media Encoder and After Effects first, then run
echo @NAME@ again. A running host holds the plug-in files open.
set "OSV_EXIT=1"
goto :end

:missing
echo Cannot find "%OSV_SCRIPT%".
echo Extract the WHOLE zip first, then run @NAME@ from the extracted folder.
set "OSV_EXIT=1"
goto :end

:running
tasklist /NH /FI "IMAGENAME eq %~1" 2>nul | find /I "%~1" >nul
exit /b %ERRORLEVEL%

:end
echo.
pause
exit /b %OSV_EXIT%
'@

    $installDone = @'
echo ==============================================================
echo  OpenOSV is installed.
echo.
echo  1. Start Premiere Pro while HOLDING SHIFT, until the splash
echo     screen appears. That makes it rescan its plug-ins.
echo  2. Open the OpenOSV window where the lines above say:
echo     Window ^> Extensions ^> OpenOSV, or Window ^> UXP Plugins ^> OpenOSV.
echo  3. Drop an .OSV on a timeline.
echo ==============================================================
'@

    $uninstallDone = @'
echo ==============================================================
echo  OpenOSV is removed.
echo.
echo  Start Premiere Pro once while HOLDING SHIFT, so it forgets
echo  the plug-ins it had cached.
echo ==============================================================
'@

    $install = $common.Replace('@TITLE@', 'Install.cmd: install OpenOSV into Premiere Pro from this folder.').
        Replace('@NAME@', 'Install.cmd').Replace('@SWITCH@', '').Replace('@DONE@', $installDone.TrimEnd())
    $uninstall = $common.Replace('@TITLE@', 'Uninstall.cmd: remove everything Install.cmd put in place.').
        Replace('@NAME@', 'Uninstall.cmd').Replace('@SWITCH@', '-Uninstall ').Replace('@DONE@', $uninstallDone.TrimEnd())

    # CRLF is not optional for a batch file: cmd.exe mis-parses labels in a
    # file with bare LF line endings.
    Write-AsciiFile -Path (Join-Path $Package 'Install.cmd') -Text $install -LineEnding CRLF
    Write-AsciiFile -Path (Join-Path $Package 'Uninstall.cmd') -Text $uninstall -LineEnding CRLF
}

# ---------------------------------------------------------------------------
#  Install.cmd / Uninstall.cmd of the DaVinci Resolve package: the same
#  wrapper around scripts\install_ofx.ps1, which copies
#  plugins\OpenOSV.ofx.bundle into Common Files\OFX\Plugins.  Resolve holds
#  its plug-ins open while it runs and only scans for them when it starts,
#  so it must be closed.
# ---------------------------------------------------------------------------
function Write-ResolveCommands {
    param([string] $Package)

    $common = @'
@echo off
rem ===========================================================================
rem  OpenOSV - @TITLE@
rem
rem  Double-click it.  It runs scripts\install_ofx.ps1 from this folder with
rem  "-ExecutionPolicy Bypass" for that one PowerShell process, which is what
rem  lets a script that came out of a downloaded zip run.  The machine's
rem  policy is not changed.  OpenFX plug-ins live under Program Files, so the
rem  script asks Windows for administrator rights (a UAC prompt).
rem ===========================================================================
setlocal EnableExtensions DisableDelayedExpansion

set "OSV_SCRIPT=%~dp0scripts\install_ofx.ps1"
if not exist "%OSV_SCRIPT%" goto :missing

tasklist /NH /FI "IMAGENAME eq Resolve.exe" 2>nul | find /I "Resolve.exe" >nul && goto :hostopen

"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -ExecutionPolicy Bypass -File "%OSV_SCRIPT%" @SWITCH@%*
set "OSV_EXIT=%ERRORLEVEL%"
echo.
if not "%OSV_EXIT%"=="0" goto :failed
@DONE@
goto :end

:failed
echo ==============================================================
echo  That did not work (exit code %OSV_EXIT%). The lines above say why.
echo ==============================================================
goto :end

:hostopen
echo Close DaVinci Resolve first, then run @NAME@ again.
echo It holds its plug-ins open while it runs.
set "OSV_EXIT=1"
goto :end

:missing
echo Cannot find "%OSV_SCRIPT%".
echo Extract the WHOLE zip first, then run @NAME@ from the extracted folder.
set "OSV_EXIT=1"
goto :end

:end
echo.
pause
exit /b %OSV_EXIT%
'@

    $installDone = @'
echo ==============================================================
echo  OpenOSV for DaVinci Resolve is installed (a preview).
echo.
echo  Start Resolve. In the Edit page's Effects panel, select Open FX
echo  and search for OpenOSV: OpenOSV Source (a generator, for .OSV
echo  clips) and OpenOSV 360 Reframe (a filter, for any 360 clip).
echo ==============================================================
'@

    $uninstallDone = @'
echo ==============================================================
echo  OpenOSV for DaVinci Resolve is removed.
echo ==============================================================
'@

    $install = $common.Replace('@TITLE@', 'Install.cmd: install OpenOSV into DaVinci Resolve from this folder.').
        Replace('@NAME@', 'Install.cmd').Replace('@SWITCH@', '').Replace('@DONE@', $installDone.TrimEnd())
    $uninstall = $common.Replace('@TITLE@', 'Uninstall.cmd: remove what Install.cmd put in place.').
        Replace('@NAME@', 'Uninstall.cmd').Replace('@SWITCH@', '-Uninstall ').Replace('@DONE@', $uninstallDone.TrimEnd())
    Write-AsciiFile -Path (Join-Path $Package 'Install.cmd') -Text $install -LineEnding CRLF
    Write-AsciiFile -Path (Join-Path $Package 'Uninstall.cmd') -Text $uninstall -LineEnding CRLF
}

# ---------------------------------------------------------------------------
#  The requirement lines README.txt and the release notes share, so the two
#  can never disagree: one list per editor, the GPU lines common to both
#  (the stitch runs on the same engine in either).  Plain ASCII.
# ---------------------------------------------------------------------------
function Get-RequirementLines {
    param([ValidateSet('Premiere', 'Resolve')] [string] $Editor = 'Premiere')
    $gpu = @(
        'NVIDIA: CUDA on GeForce GTX 16 / RTX 20 (Turing), RTX 30 (Ampere), RTX 40 (Ada) and RTX 50 (Blackwell) cards and their RTX / Quadro workstation counterparts: compute capability 7.5, 8.6, 8.9 and 12.0, plus PTX that later 12.x GPUs compile on first use. Keep the driver current.',
        'AMD and Intel: OpenCL, through the graphics driver. No usable GPU: the CPU renders, slower.'
    )
    if ($Editor -eq 'Resolve') {
        return @(
            'Windows 10 or 11, 64-bit.',
            'DaVinci Resolve, free or Studio (first run on Resolve 21).'
        ) + $gpu + @(
            'Older NVIDIA cards (GTX 10 series and earlier) have no CUDA kernels in this build: set Render Device to OpenCL in OpenOSV Source > Advanced.',
            'DaVinci Resolve closed while you install or uninstall.'
        )
    }
    return @(
        'Windows 10 or 11, 64-bit.',
        'Adobe Premiere Pro 2022 or later (tested on Premiere Pro 2026). Media Encoder and After Effects load the plug-ins too.'
    ) + $gpu + @(
        'Older NVIDIA cards (GTX 10 series and earlier) have no CUDA kernels in this build: set Render Device to OpenCL in OpenOSV Source Settings > Advanced.',
        'Premiere Pro, Media Encoder and After Effects closed while you install or uninstall.'
    )
}

# Wrap one paragraph to $Width columns with a hanging indent.
function Format-Wrapped {
    param([string] $Text, [string] $First, [string] $Rest, [int] $Width = 76)
    $lines = New-Object System.Collections.Generic.List[string]
    $line = $First
    $empty = $true
    foreach ($word in ($Text -split '\s+' | Where-Object { $_ })) {
        if (-not $empty -and ($line.Length + 1 + $word.Length) -gt $Width) {
            $lines.Add($line)
            $line = $Rest + $word
        }
        elseif ($empty) {
            $line += $word
        }
        else {
            $line += ' ' + $word
        }
        $empty = $false
    }
    $lines.Add($line)
    return ($lines -join "`n")
}

# ---------------------------------------------------------------------------
#  README.txt: what it is, requirements, install, uninstall, the Shift
#  launch, where the OpenOSV window is.  Notepad-friendly (ASCII, CRLF).
# ---------------------------------------------------------------------------
function Write-PackageReadme {
    param([string] $Package, [string] $Version, [string] $Commit, [string] $CcxName, [string] $ResolveZip = '')

    $requirements = (Get-RequirementLines -Editor Premiere | ForEach-Object { Format-Wrapped -Text $_ -First '* ' -Rest '  ' }) -join "`n"

    $text = @'
@TITLE@

DJI Osmo 360 .OSV files, straight into Adobe Premiere Pro: stitched,
converted to HDR and reframed on your graphics card while you edit.
Free and open source (Apache-2.0): https://github.com/Kemerd/OpenOSV
@OTHER@
Built from commit @COMMIT@.


REQUIREMENTS
------------
@REQUIREMENTS@


INSTALL
-------
1. Close Premiere Pro, Media Encoder and After Effects.
2. Extract this whole zip, then double-click Install.cmd.
   The files are not code-signed, so Windows SmartScreen may say it
   protected your PC: click "More info", then "Run anyway". Windows then
   asks for administrator rights, because the plug-ins go under
   Program Files.
3. Start Premiere Pro while HOLDING SHIFT, until the splash screen
   appears. That forces the plug-in rescan that finds OpenOSV. Skip it
   and Premiere keeps its cached plug-in list and never sees the new files.
4. Open the OpenOSV window: Window > Extensions > OpenOSV, or
   Window > UXP Plugins > OpenOSV when Install.cmd used Creative Cloud's
   plug-in installer (its output says which).
   Keep it open: every .OSV you drop on a timeline gets Open 360 Reframe.
5. New sequence: File > New > Sequence, group OpenOSV.

What goes where:
  Plug-ins, LUTs     C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\OpenOSV\
  Sequence presets   Documents\Adobe\Premiere Pro\<version>\Profile-<you>\
                     Settings\SequencePresets\OpenOSV\
  OpenOSV window     %APPDATA%\Adobe\CEP\extensions\com.openosv.panel\ (CEP),
                     or installed through Creative Cloud (UXP)

Install.cmd passes options on to scripts\install_plugins.ps1, for example
"Install.cmd -PanelFlavor Cep" or "Install.cmd -NoPresets". The options
are described at the top of that script.


UNINSTALL
---------
Close Premiere Pro and double-click Uninstall.cmd. Then start Premiere
once while holding Shift, so it forgets the plug-ins.


COMMAND LINE
------------
cli\osvtool.exe works without Premiere: inspect a clip, render stills or
HDR video, write LUTs. "cli\osvtool.exe --help" lists the commands.

    cli\osvtool.exe probe CAM_0001.OSV

It needs the Microsoft Visual C++ 2015-2022 Redistributable (x64), which
Premiere Pro installs.


IN THIS FOLDER
--------------
Install.cmd, Uninstall.cmd     double-click
plugins\OpenOSV\               the three plug-ins and the DLLs they load
luts\                          D-Log M to Rec.2100 PQ / HLG / Rec.709 LUTs
presets\                       Premiere sequence presets
panel\                         the OpenOSV window, CEP and UXP builds;
                               @CCX@ is the UXP installer
scripts\install_plugins.ps1    what Install.cmd and Uninstall.cmd run
cli\                           osvtool.exe and its DLLs
licenses\                      third-party licences (FFmpeg is LGPL-2.1)
LICENSE, NOTICE                OpenOSV is Apache-2.0
CHANGELOG.md                   what changed
SHA256SUMS.txt                 SHA-256 of every file here


TROUBLE
-------
* No OpenOSV in Premiere: start it once more while holding Shift.
* No OpenOSV window on Premiere older than 25.6: the UXP build went in,
  which needs 25.6. Run "Install.cmd -PanelFlavor Cep".
* "Running scripts is disabled": your organisation sets PowerShell's
  policy centrally, and it wins. Ask whoever runs your IT.
* Anything else: https://github.com/Kemerd/OpenOSV/issues
'@
    # Where the DaVinci Resolve download is, when there is one.
    $other = ''
    if ($ResolveZip) {
        $other = "`nEditing in DaVinci Resolve? That is the other download on the same`nrelease page: $ResolveZip`n"
    }
    $title = "OpenOSV $Version for Premiere Pro, Windows x64"
    $text = $text.Replace('@TITLE@', ($title + "`n" + ('=' * $title.Length))).Replace('@COMMIT@', $Commit).
        Replace('@REQUIREMENTS@', $requirements).Replace('@CCX@', $CcxName).Replace('@OTHER@', $other)
    Write-AsciiFile -Path (Join-Path $Package 'README.txt') -Text $text -LineEnding CRLF
}

# ---------------------------------------------------------------------------
#  README.txt of the DaVinci Resolve package: what it is, requirements,
#  install, how to use the two effects, audio, uninstall.  Notepad-friendly
#  (ASCII, CRLF), like the Premiere one.
# ---------------------------------------------------------------------------
function Write-ResolveReadme {
    param([string] $Package, [string] $Version, [string] $Commit, [string] $PremiereZip = '')

    $requirements = (Get-RequirementLines -Editor Resolve | ForEach-Object { Format-Wrapped -Text $_ -First '* ' -Rest '  ' }) -join "`n"

    $text = @'
@TITLE@

DJI Osmo 360 .OSV files in DaVinci Resolve, free or Studio: stitched,
converted and reframed by the same engine as OpenOSV's Premiere Pro
plug-ins, as two OpenFX effects. A preview: tested against a mock OpenFX
host, and run in Resolve 21 (free) on Windows. Please report what you see.
Free and open source (Apache-2.0): https://github.com/Kemerd/OpenOSV
@OTHER@
Built from commit @COMMIT@.


REQUIREMENTS
------------
@REQUIREMENTS@


INSTALL
-------
1. Close DaVinci Resolve.
2. Extract this whole zip, then double-click Install.cmd.
   The files are not code-signed, so Windows SmartScreen may say it
   protected your PC: click "More info", then "Run anyway". Windows then
   asks for administrator rights, because OpenFX plug-ins go under
   Program Files.
3. Start Resolve. It looks for plug-ins only when it starts.

What goes where:
  C:\Program Files\Common Files\OFX\Plugins\OpenOSV.ofx.bundle

USE
---
On the Edit page, open the Effects panel, select Open FX and search for
OpenOSV. The search looks only inside the category selected on its left.

* OpenOSV Source (Open FX > Generators) opens an .OSV:
  1. Drag it onto a video track.
  2. In the Inspector, click "Choose .OSV File...", or paste the clip's
     path into OSV File.
  3. Trim the generator to the length its Clip line shows: OpenFX gives
     a generator no way to tell Resolve how long it is.
  4. Frame the shot with Pan / Tilt / Roll / Zoom or a Preset. Set Output
     to "360 equirect" for the whole sphere instead.
* OpenOSV 360 Reframe (Open FX > Filters) reframes any 360 equirect clip.
  Set Project Settings > Image Scaling > Mismatched resolution files to
  "Stretch frame to all corners" so the sphere fills the frame.

Colour Output defaults to Rec. 709, what a new Resolve project expects.
BT.2100 PQ / HLG are for HDR projects; D-Log M is for grading it yourself,
with the LUTs in luts\.

A generator has no audio. Take the clip's own track out and put it under
the generator:
    cli\osvtool.exe extract CAM_0001.OSV --audio CAM_0001.aac

The full guide: https://github.com/Kemerd/OpenOSV/blob/main/docs/RESOLVE.md


UNINSTALL
---------
Close DaVinci Resolve and double-click Uninstall.cmd.


COMMAND LINE
------------
cli\osvtool.exe works on its own: inspect a clip, render stills or HDR
video, write LUTs, extract audio. "cli\osvtool.exe --help" lists the
commands. It needs the Microsoft Visual C++ 2015-2022 Redistributable
(x64): https://aka.ms/vs/17/release/vc_redist.x64.exe
The plug-ins themselves use the one DaVinci Resolve ships.


IN THIS FOLDER
--------------
Install.cmd, Uninstall.cmd     double-click
plugins\OpenOSV.ofx.bundle\    the OpenFX plug-ins and the DLLs they load
scripts\install_ofx.ps1        what Install.cmd and Uninstall.cmd run
luts\                          D-Log M to Rec.2100 PQ / HLG / Rec.709 LUTs
cli\                           osvtool.exe and its DLLs
licenses\                      third-party licences (FFmpeg is LGPL-2.1)
LICENSE, NOTICE                OpenOSV is Apache-2.0
CHANGELOG.md                   what changed
SHA256SUMS.txt                 SHA-256 of every file here


TROUBLE
-------
* No OpenOSV in the Effects panel: select Open FX before searching. Then
  check DaVinci Resolve > Preferences > System > Video Plugins, where
  every OpenFX plug-in can be switched on and off.
* A black or empty picture: the log says why. It is
  %LOCALAPPDATA%\OpenOSV\OpenOSVOfx.log.
* "Running scripts is disabled": your organisation sets PowerShell's
  policy centrally, and it wins. Ask whoever runs your IT.
* Anything else: https://github.com/Kemerd/OpenOSV/issues
'@
    # Where the Premiere Pro download is.
    $other = ''
    if ($PremiereZip) {
        $other = "`nEditing in Premiere Pro? That is the other download on the same`nrelease page: $PremiereZip`n"
    }
    $title = "OpenOSV $Version for DaVinci Resolve, Windows x64"
    $text = $text.Replace('@TITLE@', ($title + "`n" + ('=' * $title.Length))).Replace('@COMMIT@', $Commit).
        Replace('@REQUIREMENTS@', $requirements).Replace('@OTHER@', $other)
    Write-AsciiFile -Path (Join-Path $Package 'README.txt') -Text $text -LineEnding CRLF
}

# LICENSE, NOTICE and CHANGELOG.md, as committed.
function Copy-ProjectTexts {
    param([string] $Package)
    foreach ($name in @('LICENSE', 'NOTICE', 'CHANGELOG.md')) {
        $source = Join-Path $script:RepoRoot $name
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw "'$source' is missing."
        }
        Copy-Item -LiteralPath $source -Destination $Package -Force
    }
}

# ===========================================================================
#  licenses\
# ===========================================================================

# The version of a vcpkg port, from its SPDX document ("9.0.1#2" -> "9.0.1").
function Get-PortVersion {
    param([string] $Share, [string] $Port)
    $spdx = Join-Path $Share "$Port\vcpkg.spdx.json"
    if (-not (Test-Path -LiteralPath $spdx -PathType Leaf)) {
        return 'unknown'
    }
    $doc = Get-Content -LiteralPath $spdx -Raw | ConvertFrom-Json
    $package = @($doc.packages | Where-Object { $_.PSObject.Properties['name'] -and $_.name -eq $Port } | Select-Object -First 1)
    if ($package.Count -eq 0 -or -not $package[0].PSObject.Properties['versionInfo']) {
        return 'unknown'
    }
    return ([string]$package[0].versionInfo -split '#')[0]
}

# ---------------------------------------------------------------------------
#  FFmpeg's configure line, as the library records it (avutil_configuration
#  returns this same string).  Every FFmpeg DLL carries it; they must agree,
#  since they have to come from one build.
# ---------------------------------------------------------------------------
function Get-FfmpegConfiguration {
    param([string[]] $Dlls)
    $latin1 = [System.Text.Encoding]::GetEncoding(28591)
    $found = @{}
    foreach ($dll in $Dlls) {
        $text = $latin1.GetString([System.IO.File]::ReadAllBytes($dll))
        $m = [regex]::Match($text, '[\x20-\x7E]*--(?:enable|disable)-shared[\x20-\x7E]*')
        if (-not $m.Success) {
            throw "No FFmpeg configure line found in '$dll'."
        }
        $found[(Split-Path -Leaf $dll)] = $m.Value.Trim()
    }
    $distinct = @($found.Values | Sort-Object -Unique)
    if ($distinct.Count -ne 1) {
        throw "The shipped FFmpeg DLLs come from different builds: $($found.Keys -join ', ')."
    }
    return $distinct[0]
}

# ---------------------------------------------------------------------------
#  True when a vcpkg platform expression (a dependency or feature's
#  "platform", e.g. "!osx") holds for x64-windows, the only triplet this
#  script packages.  An empty expression holds everywhere.  Only what
#  vcpkg.json can reasonably say is understood - identifiers, '!', and
#  either '&' or '|' - and anything else throws: FFmpeg-BUILD.txt must
#  never record a guessed feature list.
# ---------------------------------------------------------------------------
function Test-PlatformForX64Windows {
    param([string] $Expression)
    $text = ([string]$Expression).Trim().ToLowerInvariant()
    if ($text.Length -eq 0) {
        return $true
    }
    if ($text -notmatch '^[a-z0-9_!&| ]+$' -or ($text.Contains('&') -and $text.Contains('|'))) {
        throw "vcpkg.json: the platform expression '$Expression' is beyond Test-PlatformForX64Windows; extend it before packaging."
    }
    # The identifiers true for this triplet; every other one is false.
    $truths = @('windows', 'x64')
    $values = foreach ($term in @($text -split '[&|]' | ForEach-Object { $_.Trim() })) {
        $negated = $term.StartsWith('!')
        $name = $term.TrimStart('!').Trim()
        if ($name.Length -eq 0) {
            throw "vcpkg.json: the platform expression '$Expression' has an empty term."
        }
        ($truths -contains $name) -xor $negated
    }
    if ($text.Contains('|')) {
        return @($values | Where-Object { $_ }).Count -gt 0
    }
    return @($values | Where-Object { -not $_ }).Count -eq 0
}

# ---------------------------------------------------------------------------
#  licenses\FFmpeg-BUILD.txt: what LGPL-2.1 asks of a binary distribution -
#  which FFmpeg, built how (and that nothing GPL or nonfree is in it), and
#  where the matching source is.
# ---------------------------------------------------------------------------
function Write-FfmpegRecord {
    param($Build, [string] $Target, [string[]] $ShippedFfmpeg, [string[]] $FfmpegPaths)

    $configuration = Get-FfmpegConfiguration -Dlls $FfmpegPaths
    # The licence guard: OpenOSV links FFmpeg under the LGPL only.
    foreach ($forbidden in @('--enable-gpl', '--enable-nonfree', '--enable-version3')) {
        if ($configuration -match ([regex]::Escape($forbidden) + '(\s|$)')) {
            throw "The FFmpeg build was configured with $forbidden. OpenOSV ships an LGPL-2.1 FFmpeg only (cmake/OsvFFmpeg.cmake)."
        }
    }

    # Provenance from vcpkg's SPDX document for the port.
    $spdxPath = Join-Path $Build.Share 'ffmpeg\vcpkg.spdx.json'
    if (-not (Test-Path -LiteralPath $spdxPath -PathType Leaf)) {
        throw "'$spdxPath' is missing; it records where FFmpeg's source came from."
    }
    $spdx = Get-Content -LiteralPath $spdxPath -Raw | ConvertFrom-Json
    $port = @($spdx.packages | Where-Object { $_.PSObject.Properties['name'] -and $_.name -eq 'ffmpeg' } | Select-Object -First 1)
    $upstream = @($spdx.packages | Where-Object {
            $_.PSObject.Properties['downloadLocation'] -and [string]$_.downloadLocation -match 'github\.com/ffmpeg/ffmpeg@'
        } | Select-Object -First 1)
    if ($port.Count -eq 0 -or $upstream.Count -eq 0) {
        throw "'$spdxPath' does not name the FFmpeg port and its upstream source."
    }
    $portVersion = [string]$port[0].versionInfo
    $version = ($portVersion -split '#')[0]
    $tag = ([string]$upstream[0].downloadLocation -split '@')[-1]
    $sha512 = ''
    if ($upstream[0].PSObject.Properties['checksums']) {
        $sha512 = [string](@($upstream[0].checksums | Where-Object { $_.algorithm -eq 'SHA512' } | Select-Object -First 1).checksumValue)
    }
    # The port's git tree id (gitoid) identifies the exact build recipe and
    # patches.  The builtin-baseline in vcpkg.json is a vcpkg commit; when
    # that commit's ports/ffmpeg IS this tree, it gives a browsable URL.
    $tree = ''
    foreach ($ref in @($port[0].externalRefs)) {
        if ([string]$ref.referenceLocator -match '^gitoid:tree:sha1:([0-9a-f]{40})$') {
            $tree = $Matches[1]
        }
    }
    $manifest = Get-Content -LiteralPath (Join-Path $script:RepoRoot 'vcpkg.json') -Raw | ConvertFrom-Json
    $baseline = [string]$manifest.'builtin-baseline'
    # A feature is a plain name, or {name, platform} when it applies to some
    # platforms only (nvcodec is "!osx"): kept when it applies to this build.
    $features = @()
    foreach ($dep in @($manifest.dependencies)) {
        if ($dep -isnot [string] -and $dep.name -eq 'ffmpeg') {
            foreach ($feature in @($dep.features)) {
                if ($feature -is [string]) {
                    $features += $feature
                } elseif ($feature -and $feature.name -and (Test-PlatformForX64Windows ([string]$feature.platform))) {
                    $features += [string]$feature.name
                }
            }
        }
    }
    if (@($features | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count -gt 0) {
        throw "vcpkg.json: an FFmpeg feature has no name; FFmpeg-BUILD.txt would record a broken install command."
    }
    $recipeUrl = "https://github.com/microsoft/vcpkg/tree/$baseline/ports/ffmpeg"
    $git = Get-Command git -ErrorAction SilentlyContinue
    if ($git -and $tree -and (Test-Path -LiteralPath (Join-Path $VcpkgRoot '.git'))) {
        $check = Invoke-Captured -FilePath $git.Source -Arguments @('-C', $VcpkgRoot, 'rev-parse', "${baseline}:ports/ffmpeg")
        if ($check.ExitCode -eq 0 -and $check.Output.Count -gt 0 -and $check.Output[0].Trim() -ne $tree) {
            throw "vcpkg's ports/ffmpeg at the baseline $baseline is tree $($check.Output[0].Trim()), but the built port is tree $tree. The source pointer in licenses\FFmpeg-BUILD.txt would be wrong; rebuild the vcpkg tree from the manifest."
        }
        if ($check.ExitCode -ne 0) {
            Write-Warn "Could not confirm the FFmpeg port against vcpkg commit $baseline ($($check.Output -join ' '))."
        }
    }

    # The configure line, one option per line, for the record.
    $options = ([regex]::Matches($configuration, "--[^\s']+(?:'[^']*'[^\s']*)*") | ForEach-Object { '    ' + $_.Value }) -join "`n"

    $text = @'
FFmpeg in this package
======================

What        FFmpeg @VERSION@ (vcpkg port "ffmpeg" @PORTVERSION@)
            @BANNER@
Licence     GNU Lesser General Public License, version 2.1 or later.
            The full text is FFmpeg-LGPL-2.1.txt beside this file, followed
            there by FFmpeg's own LICENSE.md.
Files       @FILES@
Linking     Dynamic. OpenOSV calls FFmpeg only through these DLLs and
            FFmpeg's public API, and loads them from its own folder. You
            may replace them with your own build of the same FFmpeg
            version, configured compatibly.
Changes     None beyond the build recipe named below; the DLLs are an
            unmodified build of that source.

GPL and nonfree parts: none. The configure line below has no
--enable-gpl, --enable-nonfree or --enable-version3, so every enabled part
is LGPL-2.1-or-later. OpenOSV's build refuses the gpl / nonfree features
(cmake/OsvFFmpeg.cmake).

Also inside: zlib (zlib.txt) and nv-codec-headers (nv-codec-headers.txt),
through which FFmpeg reaches NVIDIA's decoder in the graphics driver at
run time. D3D11VA, D3D12VA, DXVA2, Media Foundation and Schannel are parts
of Windows.

Configure line, as recorded in the DLLs
---------------------------------------
@OPTIONS@

vcpkg features: @FEATURES@

Source code
-----------
FFmpeg @VERSION@, git tag @TAG@:
    https://github.com/FFmpeg/FFmpeg/tree/@TAG@
    https://github.com/FFmpeg/FFmpeg/archive/@TAG@.tar.gz
    (SHA-512 @SHA512@)
The build recipe and the patches it applies (vcpkg port ffmpeg
@PORTVERSION@, git tree @TREE@):
    @RECIPE@
To rebuild: check out vcpkg at @BASELINE@ and run
    vcpkg install "ffmpeg[@FEATURELIST@]:x64-windows"

OpenOSV's own source: https://github.com/Kemerd/OpenOSV
'@
    $text = $text.Replace('@VERSION@', $version).Replace('@PORTVERSION@', $portVersion).
        Replace('@BANNER@', ($Build.FfmpegBanner -replace '^ffmpeg\s*:\s*', 'libraries: ')).
        Replace('@FILES@', ($ShippedFfmpeg -join "`n            ")).
        Replace('@OPTIONS@', $options).
        Replace('@FEATURES@', ($features -join ', ')).
        Replace('@FEATURELIST@', ($features -join ',')).
        Replace('@TAG@', $tag).Replace('@SHA512@', $sha512).Replace('@TREE@', $tree).
        Replace('@RECIPE@', $recipeUrl).Replace('@BASELINE@', $baseline)
    Write-AsciiFile -Path (Join-Path $Target 'FFmpeg-BUILD.txt') -Text $text -LineEnding CRLF
    return $version
}

# ---------------------------------------------------------------------------
#  licenses\: every third-party component inside a shipped binary, its
#  licence text, and README.txt saying which is where.
# ---------------------------------------------------------------------------
function Write-Licenses {
    param(
        $Build, [string] $Package,
        [ValidateSet('Premiere', 'Resolve')] [string] $Editor = 'Premiere',
        [string[]] $PluginDlls = @(), [string[]] $CliDlls = @(), [string[]] $OfxDlls = @()
    )

    $target = Join-Path $Package 'licenses'
    New-Directory $target
    # Every file name in the package, so a component compiled into our own
    # binaries is listed with only the binaries this package really holds.
    $shippedNames = @(Get-ChildItem -LiteralPath $Package -File -Recurse | ForEach-Object { $_.Name.ToLowerInvariant() })

    # Where each shipped DLL is, by lower-case name.
    $where = @{}
    foreach ($dll in $PluginDlls) {
        $where[$dll.ToLowerInvariant()] = @("plugins\OpenOSV\$dll")
    }
    foreach ($dll in $CliDlls) {
        $key = $dll.ToLowerInvariant()
        if ($where.ContainsKey($key)) {
            $where[$key] += "cli\$dll"
        }
        else {
            $where[$key] = @("cli\$dll")
        }
    }

    foreach ($dll in @($OfxDlls)) {
        $key = $dll.ToLowerInvariant()
        $path = "plugins\$($script:OfxBundleName)\Contents\Win64\$dll"
        if ($where.ContainsKey($key)) {
            $where[$key] += $path
        }
        else {
            $where[$key] = @($path)
        }
    }

    # Every shipped DLL must belong to a known component.
    $allDlls = @(@($PluginDlls) + @($CliDlls) + @($OfxDlls) | Sort-Object -Unique)
    foreach ($dll in $allDlls) {
        $owner = @($script:ThirdParty | Where-Object { $_.ContainsKey('Dll') -and $dll -match $_.Dll })
        if ($owner.Count -eq 0) {
            throw "No licence is known for '$dll'. Add its vcpkg port to `$script:ThirdParty in this script before it can ship."
        }
    }

    $rows = New-Object System.Collections.Generic.List[string]
    $ffmpegVersion = $null
    foreach ($component in $script:ThirdParty) {
        # Is it in the package at all?
        $files = @()
        $location = ''
        if ($component.ContainsKey('InFiles')) {
            $present = @($component.InFiles | Where-Object { $shippedNames -contains $_.ToLowerInvariant() })
            if ($present.Count -eq 0) {
                continue
            }
            $location = 'compiled into ' + ($present -join ', ')
        }
        if ($component.ContainsKey('Dll')) {
            $files = @($allDlls | Where-Object { $_ -match $component.Dll } | ForEach-Object { $where[$_.ToLowerInvariant()] } |
                ForEach-Object { $_ } | Sort-Object)
            if ($files.Count -eq 0) {
                continue
            }
        }
        $copyright = Join-Path $Build.Share "$($component.Port)\copyright"
        if (-not (Test-Path -LiteralPath $copyright -PathType Leaf)) {
            throw "vcpkg has no licence text for '$($component.Port)' at '$copyright'."
        }
        Copy-Item -LiteralPath $copyright -Destination (Join-Path $target $component.File) -Force
        $version = Get-PortVersion -Share $Build.Share -Port $component.Port

        if ($component.Port -eq 'ffmpeg') {
            # Read the configure line from the copies that ship.
            $ffmpegPaths = @($files | ForEach-Object { Join-Path $Package $_ })
            $ffmpegVersion = Write-FfmpegRecord -Build $Build -Target $target -ShippedFfmpeg $files -FfmpegPaths $ffmpegPaths
        }

        if (-not $location) {
            $location = if ($component.ContainsKey('In')) { "compiled into $($component.In)" } else { $files -join ', ' }
        }
        $rows.Add(("{0} {1}" -f $component.Name, $version))
        $rows.Add(("    licence : {0}, {1}" -f $component.Licence, $component.File))
        $rows.Add((Format-Wrapped -Text $location -First '    where   : ' -Rest '              '))
        if ($component.Port -eq 'ffmpeg') {
            $rows.Add('    build   : FFmpeg-BUILD.txt (configure line, source code)')
        }
        $rows.Add('')
    }
    if (-not $ffmpegVersion) {
        throw 'No FFmpeg DLL is in the package; the plug-ins cannot decode without it.'
    }

    # The CUDA runtime is linked statically into the GPU-capable binaries.
    # NVIDIA's EULA lists the static runtime as redistributable.
    if ($Build.Cuda) {
        $eula = Join-Path $Build.Cuda.Toolkit 'EULA.txt'
        if (-not (Test-Path -LiteralPath $eula -PathType Leaf)) {
            throw "The CUDA toolkit at '$($Build.Cuda.Toolkit)' has no EULA.txt to ship with its runtime."
        }
        Copy-Item -LiteralPath $eula -Destination (Join-Path $target 'NVIDIA-CUDA-EULA.txt') -Force
        $cudart = 'unknown'
        $versionJson = Join-Path $Build.Cuda.Toolkit 'version.json'
        if (Test-Path -LiteralPath $versionJson -PathType Leaf) {
            $v = Get-Content -LiteralPath $versionJson -Raw | ConvertFrom-Json
            if ($v.PSObject.Properties['cuda_cudart']) {
                $cudart = [string]$v.cuda_cudart.version
            }
        }
        # Only the GPU-capable binaries this package holds.
        $cudaWhere = (@('OpenOSVImporter.prm', 'Open360Reframe.aex', 'osvtool.exe', 'OpenOSV.ofx') |
            Where-Object { $shippedNames -contains $_.ToLowerInvariant() }) -join ', '
        if (-not $cudaWhere) {
            throw 'The package holds no CUDA-capable binary, yet the build has CUDA; the licence list would be wrong.'
        }
        $rows.Add("NVIDIA CUDA Runtime (cudart_static) $cudart")
        $rows.Add('    licence : NVIDIA CUDA Toolkit End User License Agreement, NVIDIA-CUDA-EULA.txt')
        $rows.Add('              (it lists the static runtime among the redistributable files)')
        $rows.Add((Format-Wrapped -Text "compiled into $cudaWhere" -First '    where   : ' -Rest '              '))
        $rows.Add('')
    }

    # The OpenFX API headers the Resolve plug-in is compiled against:
    # vendored in the repository, not a vcpkg port, so the text comes from
    # there.
    if ($shippedNames -contains 'openosv.ofx') {
        $ofxLicence = Join-Path $script:RepoRoot 'plugins\ofx\openfx\LICENSE.md'
        if (-not (Test-Path -LiteralPath $ofxLicence -PathType Leaf)) {
            throw "'$ofxLicence' is missing; the OpenFX headers' licence ships with the bundle."
        }
        Copy-Item -LiteralPath $ofxLicence -Destination (Join-Path $target 'OpenFX.txt') -Force
        $rows.Add('OpenFX API headers 1.5.1')
        $rows.Add('    licence : BSD-3-Clause, OpenFX.txt')
        $rows.Add('    where   : compiled into OpenOSV.ofx')
        $rows.Add('')
    }

    $readme = @'
Third-party components in this package
======================================

OpenOSV itself is Apache-2.0 (LICENSE and NOTICE, one folder up). These
are the other people's code inside the files it ships, with the licence
each comes under. Every text here is the component's own, verbatim.

@ROWS@
@RUNTIME@
'@
    # Who provides what is not shipped.
    $runtime = if ($Editor -eq 'Resolve') {
        "Windows' own DLLs and the Microsoft Visual C++ runtime are not shipped.`n" +
        "Windows provides the first; DaVinci Resolve ships the second for the`n" +
        "plug-ins, and cli\osvtool.exe uses the Visual C++ Redistributable."
    }
    else {
        "Windows' own DLLs and the Microsoft Visual C++ runtime are not shipped;`nWindows and Premiere Pro provide them."
    }
    $readme = $readme.Replace('@ROWS@', ($rows -join "`n")).Replace('@RUNTIME@', $runtime)
    Write-AsciiFile -Path (Join-Path $target 'README.txt') -Text $readme -LineEnding CRLF
    Write-Info ("licenses: {0}" -f ((Get-ChildItem -LiteralPath $target -File | ForEach-Object { $_.Name }) -join ', '))
}

# ---------------------------------------------------------------------------
#  SHA256SUMS.txt: sha256sum's format (hash, two spaces, path with forward
#  slashes, LF endings), so `sha256sum -c` in Git Bash checks it as it is.
# ---------------------------------------------------------------------------
function Write-Checksums {
    param([string] $Package)
    $lines = Get-ChildItem -LiteralPath $Package -File -Recurse |
        Where-Object { $_.Name -ne 'SHA256SUMS.txt' } |
        Sort-Object { (Get-RelativePath -Root $Package -Path $_.FullName) } |
        ForEach-Object {
            $hash = (Get-Sha256 $_.FullName)
            '{0}  {1}' -f $hash, ((Get-RelativePath -Root $Package -Path $_.FullName) -replace '\\', '/')
        }
    Write-AsciiFile -Path (Join-Path $Package 'SHA256SUMS.txt') -Text (($lines -join "`n") + "`n") -LineEnding LF
}

# ===========================================================================
#  Step 6 - hygiene scan
# ===========================================================================

# ---------------------------------------------------------------------------
#  The string scanner, in C# because a 14 MB avcodec is too much for a
#  PowerShell loop.  It walks every run of printable ASCII (one byte per
#  character) and of UTF-16LE (both byte alignments), tests each run against
#  the rules' regular expressions, and slides a window over every run of
#  letters and digits to compare its SHA-256 with a private secret's.  C# 5, so
#  Windows PowerShell's built-in compiler takes it.
# ---------------------------------------------------------------------------
$script:ScannerSource = @'
using System;
using System.Collections.Generic;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

namespace OpenOsvPackaging
{
    public sealed class ScanFinding
    {
        public string Rule;
        public long Offset;
        public string Kind;
        public string Excerpt;
    }

    public sealed class ScanResult
    {
        public List<ScanFinding> Findings = new List<ScanFinding>();
        public Dictionary<string, int> Totals = new Dictionary<string, int>();
    }

    public static class StringScanner
    {
        private static bool IsPrintable(int b)
        {
            return (b >= 0x20 && b <= 0x7E) || b == 0x09;
        }

        private static bool IsAlnum(char c)
        {
            return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        }

        public static ScanResult Scan(byte[] data, int minLength, string[] ruleNames, Regex[] rules,
                                      string secretRule, byte[] secretHash, int secretLength, int maxPerRule)
        {
            ScanResult result = new ScanResult();
            if (data == null || data.Length == 0)
            {
                return result;
            }
            if (ruleNames == null || rules == null || ruleNames.Length != rules.Length)
            {
                throw new ArgumentException("ruleNames and rules must be the same length");
            }
            using (SHA256 sha = SHA256.Create())
            {
                Runs(data, 0, 1, "ascii", minLength, ruleNames, rules, secretRule, secretHash, secretLength, maxPerRule, sha, result);
                Runs(data, 0, 2, "utf-16", minLength, ruleNames, rules, secretRule, secretHash, secretLength, maxPerRule, sha, result);
                Runs(data, 1, 2, "utf-16", minLength, ruleNames, rules, secretRule, secretHash, secretLength, maxPerRule, sha, result);
            }
            return result;
        }

        private static void Runs(byte[] data, int start, int step, string kind, int minLength,
                                 string[] ruleNames, Regex[] rules, string secretRule, byte[] secretHash,
                                 int secretLength, int maxPerRule, SHA256 sha, ScanResult result)
        {
            StringBuilder run = new StringBuilder();
            long runStart = start;
            for (int i = start; i + step - 1 < data.Length; i += step)
            {
                bool printable = step == 1 ? IsPrintable(data[i]) : (data[i + 1] == 0 && IsPrintable(data[i]));
                if (printable)
                {
                    if (run.Length == 0)
                    {
                        runStart = i;
                    }
                    run.Append((char)data[i]);
                    continue;
                }
                if (run.Length >= minLength)
                {
                    Check(run.ToString(), runStart, step, kind, ruleNames, rules, secretRule, secretHash, secretLength, maxPerRule, sha, result);
                }
                run.Length = 0;
            }
            if (run.Length >= minLength)
            {
                Check(run.ToString(), runStart, step, kind, ruleNames, rules, secretRule, secretHash, secretLength, maxPerRule, sha, result);
            }
        }

        private static void Add(ScanResult result, int maxPerRule, string rule, long offset, string kind, string excerpt)
        {
            int total;
            result.Totals.TryGetValue(rule, out total);
            total++;
            result.Totals[rule] = total;
            if (total <= maxPerRule)
            {
                ScanFinding f = new ScanFinding();
                f.Rule = rule;
                f.Offset = offset;
                f.Kind = kind;
                f.Excerpt = excerpt;
                result.Findings.Add(f);
            }
        }

        private static void Check(string text, long offset, int step, string kind, string[] ruleNames, Regex[] rules,
                                  string secretRule, byte[] secretHash, int secretLength, int maxPerRule,
                                  SHA256 sha, ScanResult result)
        {
            for (int r = 0; r < rules.Length; r++)
            {
                Match m = rules[r].Match(text);
                if (!m.Success)
                {
                    continue;
                }
                int from = Math.Max(0, m.Index - 40);
                int length = Math.Min(text.Length - from, 140);
                Add(result, maxPerRule, ruleNames[r], offset + (long)m.Index * step, kind, text.Substring(from, length));
            }
            if (secretHash == null || secretLength <= 0 || text.Length < secretLength)
            {
                return;
            }
            int i = 0;
            while (i < text.Length)
            {
                if (!IsAlnum(text[i]))
                {
                    i++;
                    continue;
                }
                int j = i;
                while (j < text.Length && IsAlnum(text[j]))
                {
                    j++;
                }
                for (int w = i; w + secretLength <= j; w++)
                {
                    byte[] bytes = Encoding.ASCII.GetBytes(text.Substring(w, secretLength).ToUpperInvariant());
                    byte[] hash = sha.ComputeHash(bytes);
                    bool same = hash.Length == secretHash.Length;
                    for (int k = 0; same && k < hash.Length; k++)
                    {
                        same = hash[k] == secretHash[k];
                    }
                    if (same)
                    {
                        Add(result, maxPerRule, secretRule, offset + (long)w * step, kind, "(the matching text is not printed)");
                    }
                }
                i = j;
            }
        }
    }
}
'@

# ---------------------------------------------------------------------------
#  Read the maintainer's private rules file (see -PrivateRules).  Returns
#  @{ Phrases = [string[]] (each a list of words joined by spaces);
#     Secret = [byte[]] SHA-256 of the upper-cased secret, or $null;
#     SecretLength = [int] }.  The secret is hashed at once and never kept,
#  printed or written anywhere as text.  A missing file yields no rules.
# ---------------------------------------------------------------------------
function Read-PrivateRules {
    param([string] $Path)
    $rules = @{ Phrases = @(); Secret = $null; SecretLength = 0 }
    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Write-Warn "No private rules file at '$Path': only the built-in hygiene rules run."
        return $rules
    }
    $phrases = New-Object System.Collections.Generic.List[string]
    $lineNumber = 0
    foreach ($line in [System.IO.File]::ReadAllLines($Path)) {
        $lineNumber++
        $text = $line.Trim()
        # Blank lines and comments carry no rule.
        if ($text.Length -eq 0 -or $text.StartsWith('#')) {
            continue
        }
        $m = [regex]::Match($text, '^(phrase|secret)\s+(.+)$', 'IgnoreCase')
        if (-not $m.Success) {
            throw "Private rules file, line ${lineNumber}: expected 'phrase <words>' or 'secret <text>'."
        }
        $value = $m.Groups[2].Value.Trim()
        if ($m.Groups[1].Value -ieq 'phrase') {
            $phrases.Add($value)
            continue
        }
        # One secret: letters and digits only, so the scanner's sliding
        # window over alphanumeric runs can find it.
        if ($null -ne $rules.Secret) {
            throw "Private rules file, line ${lineNumber}: only one secret is supported."
        }
        if ($value -notmatch '^[A-Za-z0-9]{6,64}$') {
            throw "Private rules file, line ${lineNumber}: a secret is 6-64 letters and digits."
        }
        $sha = [System.Security.Cryptography.SHA256]::Create()
        try {
            $rules.Secret = $sha.ComputeHash([System.Text.Encoding]::ASCII.GetBytes($value.ToUpperInvariant()))
        } finally {
            $sha.Dispose()
        }
        $rules.SecretLength = $value.Length
    }
    $rules.Phrases = $phrases.ToArray()
    $secretCount = 0
    if ($null -ne $rules.Secret) {
        $secretCount = 1
    }
    Write-Info ("private rules: {0} phrase(s), {1} secret(s)" -f $rules.Phrases.Count, $secretCount)
    return $rules
}

# ---------------------------------------------------------------------------
#  A regex for one absolute folder, matching it however it is written:
#  back or forward slashes, doubled separators, and the MSYS form (/l/Dev)
#  that FFmpeg's configure uses.  Never matches a longer sibling name.
# ---------------------------------------------------------------------------
function Get-FolderPattern {
    param([string] $Folder)
    $m = [regex]::Match($Folder, '^([A-Za-z]):[\\/]+(.+?)[\\/]*$')
    if (-not $m.Success) {
        # UNC or something unusual: match it literally, either slash.
        $parts = @($Folder -split '[\\/]+' | Where-Object { $_ } | ForEach-Object { [regex]::Escape($_) })
        return ('(?i)[\\/]{2}' + ($parts -join '[\\/]{1,2}') + '(?![A-Za-z0-9_])')
    }
    $drive = [regex]::Escape($m.Groups[1].Value)
    $rest = @($m.Groups[2].Value -split '[\\/]+' | Where-Object { $_ } | ForEach-Object { [regex]::Escape($_) }) -join '[\\/]{1,2}'
    return "(?i)(?:(?<![A-Za-z0-9])$drive" + ':[\\/]{1,2}' + "|(?<![A-Za-z0-9_.:/])/$drive/)$rest(?![A-Za-z0-9_])"
}

# The top-level folder of a local path: L:\Dev\x\y -> L:\Dev.
function Get-TopFolder {
    param([string] $Path)
    $full = [System.IO.Path]::GetFullPath($Path)
    $m = [regex]::Match($full, '^([A-Za-z]:\\[^\\]+)')
    if ($m.Success) {
        return $m.Groups[1].Value
    }
    return $full
}

# ---------------------------------------------------------------------------
#  The rules.  Returns name/regex/isWordRule records.
#
#    user profile  - any <drive>:\Users\ (either slash) or /c/Users/;
#    source folder - the top-level folder holding this checkout (and the
#                    build folder when it is elsewhere): our own source or
#                    build paths must never reach a user.  vcpkg's root is
#                    exempt - its build-tree paths are inside every vcpkg
#                    DLL and name no person or project;
#    private phrase - the phrases of the private rules file, if any.
#  The private secret is its own hash rule, applied by the scanner.
# ---------------------------------------------------------------------------
function Get-HygieneRules {
    param([string] $BuildDir, [string[]] $Phrases)
    $options = [System.Text.RegularExpressions.RegexOptions]::CultureInvariant
    $rules = New-Object System.Collections.Generic.List[object]

    $userProfile = '(?i)(?:(?<![A-Za-z0-9])[A-Za-z]:[\\/]{1,2}|(?<![A-Za-z0-9_.:/])/[A-Za-z]/)Users[\\/]'
    $rules.Add([pscustomobject]@{ Name = 'user profile path'; Regex = (New-Object System.Text.RegularExpressions.Regex($userProfile, $options)); Words = $false })

    $vcpkgTop = Get-TopFolder $VcpkgRoot
    $tops = @((Get-TopFolder $script:RepoRoot), (Get-TopFolder $BuildDir)) | Sort-Object -Unique
    foreach ($top in $tops) {
        if ($top -ieq $vcpkgTop) {
            Write-Warn "The source or build folder shares its top-level folder with vcpkg ($top); that folder cannot be scanned for."
            continue
        }
        $rules.Add([pscustomobject]@{
                Name  = "source path ($top)"
                Regex = (New-Object System.Text.RegularExpressions.Regex((Get-FolderPattern $top), $options))
                Words = $false
            })
    }

    # Each private phrase: its words, joined by any run of spaces, '-' or '_'.
    $words = @(@($Phrases) | Where-Object { $_ } | ForEach-Object {
            (@($_ -split '\s+' | Where-Object { $_ } | ForEach-Object { [regex]::Escape($_) }) -join '[\s_\-]*')
        })
    if ($words.Count -gt 0) {
        $rules.Add([pscustomobject]@{
                Name  = 'private phrase'
                Regex = (New-Object System.Text.RegularExpressions.Regex(('(?i)' + ($words -join '|')), $options))
                Words = $true
            })
    }
    return $rules
}

# ---------------------------------------------------------------------------
#  Scan $Files, named in the output relative to $Root.  Private phrases are
#  skipped for <Root>\licenses\ only: those are verbatim third-party legal
#  texts, not ours to change.  Returns the number of findings and prints
#  each one.
# ---------------------------------------------------------------------------
function Invoke-HygieneScan {
    param([string[]] $Files, [string] $Root, [string] $BuildDir)

    if (-not ('OpenOsvPackaging.StringScanner' -as [type])) {
        Add-Type -TypeDefinition $script:ScannerSource -Language CSharp
    }
    $private = Read-PrivateRules -Path $PrivateRules
    $rules = Get-HygieneRules -BuildDir $BuildDir -Phrases $private.Phrases

    $failures = 0
    $scannedBytes = 0
    foreach ($file in @($Files)) {
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
            throw "Invoke-HygieneScan: '$file' does not exist."
        }
        $display = Get-RelativePath -Root $Root -Path $file
        $isLicence = $display -like 'licenses\*'
        $active = @($rules | Where-Object { -not ($isLicence -and $_.Words) })
        $names = [string[]]@($active | ForEach-Object { $_.Name })
        $regexes = [System.Text.RegularExpressions.Regex[]]@($active | ForEach-Object { $_.Regex })

        $bytes = [System.IO.File]::ReadAllBytes($file)
        $scannedBytes += $bytes.Length
        $result = [OpenOsvPackaging.StringScanner]::Scan($bytes, 6, $names, $regexes,
            'private secret', $private.Secret, $private.SecretLength, 3)
        foreach ($finding in $result.Findings) {
            Write-Host ("    FAIL {0}: {1} at 0x{2:X} ({3}): {4}" -f $display, $finding.Rule, $finding.Offset, $finding.Kind, $finding.Excerpt)
        }
        foreach ($rule in $result.Totals.Keys) {
            $total = $result.Totals[$rule]
            $failures += $total
            if ($total -gt 3) {
                Write-Host ("    FAIL {0}: {1} more '{2}' hit(s)" -f $display, ($total - 3), $rule)
            }
        }
    }
    $ruleNames = @($rules | ForEach-Object { $_.Name })
    if ($null -ne $private.Secret) {
        $ruleNames += 'private secret'
    }
    Write-Info ("scanned {0} file(s), {1:N1} MB; rules: {2}" -f @($Files).Count, ($scannedBytes / 1MB), ($ruleNames -join ', '))
    return $failures
}

# ===========================================================================
#  Step 7 - zip and release notes
# ===========================================================================

# ---------------------------------------------------------------------------
#  Zip the package with its folder at the top.  Entries are written one by
#  one with forward-slash names, as install_plugins.ps1 does for the .ccx:
#  Windows PowerShell's Compress-Archive stores backslashes, which the ZIP
#  specification does not allow.
# ---------------------------------------------------------------------------
function New-ReleaseZip {
    param([string] $Package, [string] $ZipPath)
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    if (Test-Path -LiteralPath $ZipPath) {
        Remove-Item -LiteralPath $ZipPath -Force
    }
    $top = Split-Path -Leaf $Package
    $stream = [System.IO.File]::Open($ZipPath, [System.IO.FileMode]::CreateNew)
    try {
        $zip = New-Object System.IO.Compression.ZipArchive($stream, [System.IO.Compression.ZipArchiveMode]::Create)
        try {
            Get-ChildItem -LiteralPath $Package -Recurse -File | Sort-Object FullName | ForEach-Object {
                $entry = $top + '/' + ((Get-RelativePath -Root $Package -Path $_.FullName) -replace '\\', '/')
                [void][System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
                    $zip, $_.FullName, $entry, [System.IO.Compression.CompressionLevel]::Optimal)
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
#  The README's "What it is" bullets, verbatim, with relative links made
#  absolute (they are read on the release page, not in the repository).
# ---------------------------------------------------------------------------
function Get-ReadmeHighlights {
    param([string] $Tag)
    $readme = Get-Content -LiteralPath (Join-Path $script:RepoRoot 'README.md') -Raw
    $section = [regex]::Match($readme, '(?ms)^## What it is\s*$(.*?)^## ')
    if (-not $section.Success) {
        Write-Warn 'README.md has no "## What it is" section; the release notes get no highlights.'
        return ''
    }
    # From the first bullet to the end of the list: the bullets, their
    # continuation lines and nested bullets.
    $lines = $section.Groups[1].Value -split "`r?`n"
    $out = New-Object System.Collections.Generic.List[string]
    $inList = $false
    foreach ($line in $lines) {
        if ($line -match '^\* ') {
            $inList = $true
            $out.Add($line)
            continue
        }
        if ($inList) {
            if ($line -match '^\s+\S' -or $line -match '^\s*$') {
                $out.Add($line)
                continue
            }
            break
        }
    }
    $text = ($out -join "`n").Trim()
    return [regex]::Replace($text, '\]\((?!https?://|#|mailto:)([^)]+)\)', "](https://github.com/Kemerd/OpenOSV/blob/$Tag/`$1)")
}

# ---------------------------------------------------------------------------
#  The CHANGELOG section for this version as bullet titles: the bold lead
#  of each entry (or its first sentence), per group, without the internal
#  work-package tags.  Falls back to [Unreleased] with a note.
# ---------------------------------------------------------------------------
function Get-ChangelogTitles {
    param([string] $Version)
    $lines = [System.IO.File]::ReadAllLines((Join-Path $script:RepoRoot 'CHANGELOG.md'))
    $heading = '^## \[' + [regex]::Escape($Version) + '\]'
    $start = -1
    for ($i = 0; $i -lt $lines.Length; $i++) {
        if ($lines[$i] -match $heading) { $start = $i; break }
    }
    $fromUnreleased = $false
    if ($start -lt 0) {
        for ($i = 0; $i -lt $lines.Length; $i++) {
            if ($lines[$i] -match '^## \[Unreleased\]') { $start = $i; break }
        }
        $fromUnreleased = $true
        Write-Warn "CHANGELOG.md has no '## [$Version]' heading yet; the notes draft uses [Unreleased]. Move the entries first (docs/RELEASING.md)."
    }
    if ($start -lt 0) {
        return [pscustomobject]@{ Markdown = ''; FromUnreleased = $true }
    }

    # Join each entry's lines, grouped under its ### heading.
    $groups = New-Object System.Collections.Specialized.OrderedDictionary
    $group = 'Changes'
    $entry = $null
    $flush = {
        if ($null -ne $entry) {
            if (-not $groups.Contains($group)) { $groups[$group] = New-Object System.Collections.Generic.List[string] }
            $groups[$group].Add($entry)
        }
    }
    for ($i = $start + 1; $i -lt $lines.Length; $i++) {
        $line = $lines[$i]
        if ($line -match '^## ') { break }
        if ($line -match '^### (.+)$') {
            . $flush; $entry = $null
            $group = $Matches[1].Trim()
            continue
        }
        if ($line -match '^[*-] (.*)$') {
            . $flush
            $entry = $Matches[1]
            continue
        }
        if ($null -ne $entry -and $line -match '^\s+\S') {
            $entry += ' ' + $line.Trim()
            continue
        }
        if ($line -match '^\s*$') {
            . $flush; $entry = $null
        }
    }
    . $flush

    $md = New-Object System.Collections.Generic.List[string]
    foreach ($name in $groups.Keys) {
        $md.Add("### $name")
        $md.Add('')
        foreach ($item in $groups[$name]) {
            $bold = [regex]::Match($item, '^\*\*(.+?)\*\*')
            $title = if ($bold.Success) { $bold.Groups[1].Value } else {
                $first = [regex]::Match($item, '^(.+?[.:])(\s|$)')
                if ($first.Success) { $first.Groups[1].Value } else { $item }
            }
            $title = ($title -replace '\s*\(WP-[A-Z0-9-]+\)', '').Trim().TrimEnd('.', ':')
            if ($title.Length -gt 160) { $title = $title.Substring(0, 157).TrimEnd() + '...' }
            $md.Add("* $title")
        }
        $md.Add('')
    }
    return [pscustomobject]@{ Markdown = ($md -join "`n").Trim(); FromUnreleased = $fromUnreleased }
}

# ---------------------------------------------------------------------------
#  dist\RELEASE_NOTES.md: a draft for the GitHub release page.  $Zips holds
#  one record per zip made ({Editor, Name, Sha256, Bytes}), Premiere first.
# ---------------------------------------------------------------------------
function Write-ReleaseNotes {
    param(
        [string] $Path, [string] $Version, [string] $Commit, [object[]] $Zips, [string] $FfmpegVersion
    )
    $tag = "v$Version"
    $highlights = Get-ReadmeHighlights -Tag $tag
    $changes = Get-ChangelogTitles -Version $Version
    $draftNote = ''
    if ($changes.FromUnreleased) {
        $draftNote = "`n> **Draft:** the list below is CHANGELOG.md's [Unreleased] section. Move it under ``## [$Version]`` before publishing.`n"
    }
    $premiere = @($Zips | Where-Object { $_.Editor -eq 'Premiere' } | Select-Object -First 1)
    $resolve = @($Zips | Where-Object { $_.Editor -eq 'Resolve' } | Select-Object -First 1)
    if ($premiere.Count -eq 0) {
        throw 'Write-ReleaseNotes: there is no Premiere Pro zip.'
    }
    $url = "https://github.com/Kemerd/OpenOSV/releases/download/$tag/"

    # One download line and one checksum block per zip; one requirements
    # list per editor.
    $downloads = New-Object System.Collections.Generic.List[string]
    $requirements = New-Object System.Collections.Generic.List[string]
    $checksums = New-Object System.Collections.Generic.List[string]
    foreach ($zip in $Zips) {
        $label = if ($zip.Editor -eq 'Resolve') { 'DaVinci Resolve (preview)' } else { 'Premiere Pro' }
        $downloads.Add("* **${label}: [``$($zip.Name)``]($url$($zip.Name))**")
        $requirements.Add("**$label**")
        $requirements.Add('')
        foreach ($line in (Get-RequirementLines -Editor $zip.Editor)) {
            $requirements.Add("* $line")
        }
        $requirements.Add('')
        $checksums.Add(("``{0}`` ({1:N1} MB), SHA-256:" -f $zip.Name, ($zip.Bytes / 1MB)))
        $checksums.Add('')
        $checksums.Add('```')
        $checksums.Add($zip.Sha256)
        $checksums.Add('```')
        $checksums.Add('')
    }

    # The Resolve install steps, when there is a Resolve zip.
    $resolveInstall = ''
    if ($resolve.Count -gt 0) {
        $resolveInstall = @'

### DaVinci Resolve

1. Close DaVinci Resolve.
2. Unzip `@RESOLVEZIP@` and double-click **`Install.cmd`** (the same SmartScreen step; admin rights once).
3. Start Resolve. On the Edit page's Effects panel, select **Open FX** and search for **OpenOSV**: the search looks only inside the selected category.
4. Drag **OpenOSV Source** onto the timeline, click **Choose .OSV File...** in the Inspector, and trim the generator to the length its **Clip** line shows. **OpenOSV 360 Reframe** reframes any 360 clip.

**Uninstall:** close Resolve and double-click `Uninstall.cmd`. Guide: [`docs/RESOLVE.md`](https://github.com/Kemerd/OpenOSV/blob/@TAG@/docs/RESOLVE.md).
'@
        $resolveInstall = $resolveInstall.Replace('@RESOLVEZIP@', $resolve[0].Name)
    }

    $text = @'
# OpenOSV @VERSION@ for Windows

DJI Osmo 360 footage, straight into Premiere Pro or DaVinci Resolve on
Windows, with a better stitch and real HDR. Drop an `.OSV` on your timeline
and it's stitched, converted to HDR and ready to reframe.

One download per editor. Unzip it and double-click `Install.cmd`:

@DOWNLOADS@

## Highlights

@HIGHLIGHTS@

## What's new
@DRAFTNOTE@
@CHANGES@

The full list is in `CHANGELOG.md`, in each zip and in the repository.

## Requirements

@REQUIREMENTS@
## Install

### Premiere Pro

1. Close Premiere Pro, Media Encoder and After Effects.
2. Unzip `@PREMIEREZIP@` and double-click **`Install.cmd`**. The files aren't code-signed, so SmartScreen may step in: **More info @ARROW@ Run anyway**. Windows then asks for admin rights once: the plug-ins go under Program Files.
3. Launch Premiere **holding `Shift`** until the splash screen shows, so it rescans its plug-ins.
4. Open **Window > Extensions > OpenOSV**, or **Window > UXP Plugins > OpenOSV** when the installer used Creative Cloud's plug-in installer (it says which; the UXP build needs Premiere 25.6+, and `Install.cmd -PanelFlavor Cep` forces the other).
5. Drop an `.OSV` on a timeline. **File > New > Sequence > OpenOSV** has the sequence presets.

**Uninstall:** close Premiere, double-click `Uninstall.cmd`, then launch Premiere holding `Shift` once.
@RESOLVEINSTALL@
## Known limits

* **Unsigned binaries.** SmartScreen warns on first run; Windows 11's Smart App Control, when it is on, may block unsigned plug-ins like these.
* **CUDA covers Turing and newer** (GTX 16 / RTX 20 and later). On an older NVIDIA card, set **Render Device** to OpenCL in Source Settings; that setup is untested.
* **No neural optical flow in the download.** Its runtime is 1.4 GB, so Auto uses the classical flow, which is the one the stitch is tuned on. Building from source with ONNX Runtime adds it back.
* **Windows x64 downloads.** Premiere Pro, and a DaVinci Resolve preview (OpenFX, first run in Resolve 21 on Windows). macOS (Apple Silicon) builds from source and on CI, but is untested in Premiere and Resolve: see `docs/BUILDING_MAC.md`. No Final Cut plug-in; `osvtool` renders for any other editor.
* Tested on Premiere Pro 2026 with Osmo 360 footage.

## Checksums

@CHECKSUMS@
Every file inside a zip is listed in its `SHA256SUMS.txt`.

## Licences

OpenOSV is Apache-2.0. Each zip's `licenses` folder holds the licence of every third-party component it ships. FFmpeg @FFMPEG@ is used under the LGPL-2.1, dynamically linked, with no GPL or nonfree parts; `licenses/FFmpeg-BUILD.txt` records its configure line and where its source is.

Built from @COMMIT@.
'@
    # @RESOLVEINSTALL@ first: it carries an @TAG@ of its own.
    $text = $text.Replace('@RESOLVEINSTALL@', $resolveInstall).
        Replace('@VERSION@', $Version).Replace('@TAG@', $tag).Replace('@DOWNLOADS@', ($downloads -join "`n")).
        Replace('@HIGHLIGHTS@', $highlights).Replace('@DRAFTNOTE@', $draftNote).Replace('@CHANGES@', $changes.Markdown).
        Replace('@REQUIREMENTS@', ($requirements -join "`n")).Replace('@PREMIEREZIP@', $premiere[0].Name).
        Replace('@CHECKSUMS@', ($checksums -join "`n")).Replace('@FFMPEG@', $FfmpegVersion).
        Replace('@COMMIT@', "commit ``$Commit``").
        Replace('@ARROW@', [string][char]0x2192)   # a real arrow; this file stays ASCII
    Write-Utf8File -Path $Path -Text $text
}

# ===========================================================================
#  Main
# ===========================================================================
try {
    # ---- inputs -----------------------------------------------------------
    if (-not $VcpkgRoot) {
        $VcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { 'C:\vcpkg' }
    }
    $VcpkgRoot = [System.IO.Path]::GetFullPath($VcpkgRoot).TrimEnd('\')
    if (-not $VcpkgInstalledDir) {
        $VcpkgInstalledDir = Join-Path $VcpkgRoot 'installed-openosv-release'
    }
    if (-not $OutDir) {
        $OutDir = Join-Path $script:RepoRoot 'dist'
    }
    $OutDir = [System.IO.Path]::GetFullPath($OutDir).TrimEnd('\')
    $explicitBuildDir = [bool]$BuildDir
    if (-not $BuildDir) {
        $BuildDir = $script:DefaultBuildDir
    }
    $BuildDir = [System.IO.Path]::GetFullPath($BuildDir).TrimEnd('\')

    $version = Get-ProjectVersion
    # One package per editor: what a Premiere Pro user and a DaVinci Resolve
    # user each install, and nothing of the other's.
    $premiereName = "OpenOSV-$version-premiere-windows-x64"
    $resolveName = "OpenOSV-$version-resolve-windows-x64"
    Write-Step "OpenOSV $version, Windows x64"

    # ---- 1. build ---------------------------------------------------------
    if ($SkipBuild) {
        Write-Step "Packaging the existing build $BuildDir (-SkipBuild)"
    }
    elseif ($explicitBuildDir) {
        # An existing build folder: bring it up to date, keep its settings.
        Invoke-ReleaseBuild -Dir $BuildDir -BuildOnly
    }
    else {
        if (-not (Test-Path -LiteralPath (Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake') -PathType Leaf)) {
            throw "'$VcpkgRoot' is not a vcpkg checkout. Pass -VcpkgRoot or set VCPKG_ROOT."
        }
        Invoke-ReleaseBuild -Dir $BuildDir
    }

    # ---- 2. check it --------------------------------------------------------
    Write-Step 'Checking the build'
    $build = Get-BuildInfo -Dir $BuildDir -Version $version
    Write-Info "stage   : $($build.Stage)"
    Write-Info "vcpkg   : $($build.Installed)"
    if ($build.Cuda) {
        Write-Info "CUDA    : $($build.Cuda.Architectures)"
    }
    $commit = Get-SourceCommit

    # ---- 3/4. assemble ------------------------------------------------------
    New-Directory $OutDir

    # The Premiere Pro package.
    $premiere = New-EmptyPackage (Join-Path $OutDir $premiereName)
    Write-Step "Assembling $premiere"
    $pluginDlls = Copy-PluginModules -Build $build -Package $premiere
    $premiereCliDlls = Copy-CommandLineTool -Build $build -Package $premiere
    Test-PackagedCli -Package $premiere -Version $version
    New-Directory (Join-Path $premiere 'scripts')
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'install_plugins.ps1') -Destination (Join-Path $premiere 'scripts') -Force

    Write-Step 'Generating the LUTs with the packaged osvtool'
    New-PackageLuts -Package $premiere
    Copy-Presets -Package $premiere
    Write-Step 'Staging the panel and building its .ccx with the packaged installer'
    $ccxName = Copy-Panel -Package $premiere

    # The DaVinci Resolve package, whenever the build made the OpenFX bundle.
    $resolve = $null
    $ofxDlls = @()
    $resolveCliDlls = @()
    if ($build.OfxBundle) {
        $resolve = New-EmptyPackage (Join-Path $OutDir $resolveName)
        Write-Step "Assembling $resolve"
        $ofxDlls = Copy-OfxBundle -Build $build -Package $resolve
        $resolveCliDlls = Copy-CommandLineTool -Build $build -Package $resolve
        Test-PackagedCli -Package $resolve -Version $version
        New-Directory (Join-Path $resolve 'scripts')
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'install_ofx.ps1') -Destination (Join-Path $resolve 'scripts') -Force
        # The same tables, generated above by this very build's osvtool.
        Copy-Item -LiteralPath (Join-Path $premiere 'luts') -Destination $resolve -Recurse -Force
        Write-Info 'luts: the tables generated for the Premiere Pro package'
    }
    else {
        Write-Warn 'The build made no OpenFX bundle (OSV_BUILD_OFX off): there is no DaVinci Resolve package.'
    }

    # ---- 5. texts -----------------------------------------------------------
    Write-Step 'Writing the texts and licences'
    $resolveZipName = ''
    if ($resolve) {
        $resolveZipName = "$resolveName.zip"
    }
    Copy-ProjectTexts -Package $premiere
    Write-InstallCommands -Package $premiere
    Write-PackageReadme -Package $premiere -Version $version -Commit $commit -CcxName $ccxName -ResolveZip $resolveZipName
    Write-Licenses -Build $build -Package $premiere -Editor Premiere -PluginDlls $pluginDlls -CliDlls $premiereCliDlls
    Write-Checksums -Package $premiere
    $packages = @($premiere)
    if ($resolve) {
        Copy-ProjectTexts -Package $resolve
        Write-ResolveCommands -Package $resolve
        Write-ResolveReadme -Package $resolve -Version $version -Commit $commit -PremiereZip "$premiereName.zip"
        Write-Licenses -Build $build -Package $resolve -Editor Resolve -CliDlls $resolveCliDlls -OfxDlls $ofxDlls
        Write-Checksums -Package $resolve
        $packages += $resolve
    }
    $ffmpegVersion = Get-PortVersion -Share $build.Share -Port 'ffmpeg'

    # ---- 6. hygiene -----------------------------------------------------------
    foreach ($package in $packages) {
        Write-Step "Hygiene scan of $(Split-Path -Leaf $package)"
        $shipped = @(Get-ChildItem -LiteralPath $package -File -Recurse | ForEach-Object { $_.FullName })
        $failures = Invoke-HygieneScan -Files $shipped -Root $package -BuildDir $BuildDir
        if ($failures -gt 0) {
            Write-Host ''
            Write-Host "ERROR: the hygiene scan found $failures problem(s); nothing was zipped."
            Write-Host '       The package folder is left in place for inspection:'
            Write-Host "       $package"
            Write-Host '       A source path in the FFmpeg DLLs comes from their configure line: the'
            Write-Host '       build used a vcpkg tree inside the checkout. Package a build this'
            Write-Host '       script made itself, without -BuildDir (docs/RELEASING.md).'
            exit 1
        }
        Write-Info 'clean.'
    }

    # ---- 7. zip and notes -----------------------------------------------------
    $zips = @()
    foreach ($package in $packages) {
        $name = Split-Path -Leaf $package
        $zipPath = Join-Path $OutDir "$name.zip"
        Write-Step "Zipping $zipPath"
        New-ReleaseZip -Package $package -ZipPath $zipPath
        $editor = 'Premiere'
        if ($package -eq $resolve) {
            $editor = 'Resolve'
        }
        $zips += [pscustomobject]@{
            Editor = $editor
            Name   = "$name.zip"
            Path   = $zipPath
            Sha256 = (Get-Sha256 $zipPath)
            Bytes  = (Get-Item -LiteralPath $zipPath).Length
        }
    }

    $notes = Join-Path $OutDir 'RELEASE_NOTES.md'
    Write-ReleaseNotes -Path $notes -Version $version -Commit $commit -Zips $zips -FfmpegVersion $ffmpegVersion

    # The notes are published too, so they pass the same scan.
    Write-Step 'Hygiene scan of the release notes'
    $notesFailures = Invoke-HygieneScan -Files @($notes) -Root $OutDir -BuildDir $BuildDir
    if ($notesFailures -gt 0) {
        Write-Host ''
        Write-Host "ERROR: $notes has $notesFailures problem(s); fix CHANGELOG.md or README.md and run again."
        exit 1
    }
    Write-Info 'clean.'
    Write-Host ''
    Write-Step 'Done'
    foreach ($zip in $zips) {
        Write-Info ("{0}  {1:N1} MB" -f $zip.Path, ($zip.Bytes / 1MB))
        Write-Info "SHA-256 $($zip.Sha256)"
    }
    Write-Info "release notes draft: $notes"
    exit 0
}
catch {
    Write-Host ''
    Write-Host "ERROR: $($_.Exception.Message)"
    exit 1
}
