# =============================================================================
#  run_ci_local.ps1 - reproduce the CI build on a developer machine.
#
#  Usage (from any PowerShell prompt, no developer shell needed):
#     .\scripts\run_ci_local.ps1                # CUDA + OpenCL release build
#     .\scripts\run_ci_local.ps1 -CpuOnly       # CPU-only configuration
#     .\scripts\run_ci_local.ps1 -Clean         # wipe the build directory first
#
#  The script locates Visual Studio (VSDEVCMD override, vswhere when present,
#  otherwise the well-known 2022 edition folders), imports the x64 developer
#  environment, then runs configure / build / ctest with the presets used by
#  GitHub Actions.  Output is plain ASCII so it is safe in any console.
# =============================================================================
[CmdletBinding()]
param(
    [switch]$CpuOnly,
    [switch]$Clean,
    [string]$VcpkgRoot = $(if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { 'C:\vcpkg' })
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

# -----------------------------------------------------------------------------
#  Locate VsDevCmd.bat
# -----------------------------------------------------------------------------
function Find-VsDevCmd {
    if ($env:VSDEVCMD -and (Test-Path $env:VSDEVCMD)) {
        return $env:VSDEVCMD
    }
    $installer = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer'
    $vswhere = Join-Path $installer 'vswhere.exe'
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vsPath) {
            $candidate = Join-Path $vsPath 'Common7\Tools\VsDevCmd.bat'
            if (Test-Path $candidate) { return $candidate }
        }
    }
    foreach ($edition in @('Enterprise', 'Professional', 'Community', 'BuildTools', 'Preview')) {
        $candidate = "C:\Program Files\Microsoft Visual Studio\2022\$edition\Common7\Tools\VsDevCmd.bat"
        if (Test-Path $candidate) { return $candidate }
    }
    throw "Visual Studio 2022 with the C++ toolset was not found (set VSDEVCMD to VsDevCmd.bat)."
}

$devCmd = Find-VsDevCmd
Write-Host "[ci-local] Importing developer environment from $devCmd"
$envDump = cmd /c "`"$devCmd`" -arch=x64 -host_arch=x64 -no_logo && set"
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') {
        [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process')
    }
}
$env:VCPKG_ROOT = $VcpkgRoot

# -----------------------------------------------------------------------------
#  Presets
# -----------------------------------------------------------------------------
$preset = if ($CpuOnly) { 'windows-msvc-cpu-only' } else { 'windows-msvc-cuda-release' }
$testPreset = if ($CpuOnly) { 'cpu-only' } else { 'all' }

if ($Clean -and (Test-Path "build\$preset")) {
    Write-Host "[ci-local] Removing build\$preset"
    Remove-Item -Recurse -Force "build\$preset"
}

Write-Host "[ci-local] Configure ($preset)"
cmake --preset $preset
if ($LASTEXITCODE -ne 0) { throw "configure failed" }

Write-Host "[ci-local] Build"
cmake --build --preset $preset --parallel
if ($LASTEXITCODE -ne 0) { throw "build failed" }

Write-Host "[ci-local] Test ($testPreset)"
ctest --preset $testPreset
if ($LASTEXITCODE -ne 0) { throw "tests failed" }

Write-Host "[ci-local] OK"
