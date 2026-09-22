# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 The OpenOSV Contributors
<#
.SYNOPSIS
    Fetch the prebuilt ONNX Runtime GPU package (and cuDNN) the neural flow
    backend needs, into third_party/ where cmake/OsvOnnxRuntime.cmake finds it.

.DESCRIPTION
    Nothing here is committed: third_party/ is gitignored.  After this script,
    re-run CMake configure and the build defines OSV_HAVE_ONNXRUNTIME and
    stages the DLLs next to osvtool.exe / osv_tests.exe.  Without it the
    build still succeeds; the neural backend simply reports itself absent.

    WHICH BUILD, AND WHY (verified on an RTX 5090 / sm_120, driver 616.56)
    ----------------------------------------------------------------------
    ONNX Runtime 1.30.0 ships two Windows GPU packages:

      gpu_cuda12   imports cudart64_12 / cublas64_12 / cublasLt64_12, loads
                   cudnn64_9 dynamically.  Runs on sm_120 with the CUDA 12.9
                   toolkit this project already requires for its own kernels.
                   THIS IS THE DEFAULT.
      gpu_cuda13   imports cublas64_13 / cublasLt64_13.  Needs a CUDA 13
                   runtime and a cuda13 cuDNN, neither of which the project
                   otherwise needs; pass -Cuda cuda13 on a machine that has
                   them.

    cuDNN is NOT part of the CUDA toolkit and ONNX Runtime's CUDA provider
    cannot run a convolution without it.  In order of preference this script
    takes it from: -CudnnDir; an installed pip wheel (nvidia-cudnn-cu12 -
    PyTorch installs one); or NVIDIA's public redistributable archive (large:
    ~1.9 GB for cuda12).  cuDNN is NVIDIA-licensed (its LICENSE is copied
    alongside); ONNX Runtime is MIT.

.PARAMETER Version
    ONNX Runtime release.  Only versions with a pinned SHA-256 below are
    accepted, so a tampered or truncated download cannot be installed.

.PARAMETER Cuda
    cuda12 (default, verified) or cuda13.

.PARAMETER CudnnDir
    A directory that already contains cudnn64_9.dll to copy from.

.PARAMETER SkipCudnn
    Fetch ONNX Runtime only.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\fetch_onnxruntime.ps1
#>
[CmdletBinding()]
param(
    [string]$Version = "1.30.0",
    [ValidateSet("cuda12", "cuda13")][string]$Cuda = "cuda12",
    [string]$CudnnDir = "",
    [switch]$SkipCudnn
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"   # Invoke-WebRequest is 10x slower with the progress bar

# ---------------------------------------------------------------------------
#  Pins.  These are GitHub's own published digests for the release assets.
# ---------------------------------------------------------------------------
$OrtSha256 = @{
    "1.30.0-cuda12" = "d4667ea48eb0a10bc9b96b838f7b8975a6bf18de3bc5edd403a22e15c1458b23"
    "1.30.0-cuda13" = "8fa4b08359af682cd605892cb59077049700b640128bb93fd2c7776cf9f55bdc"
}
# cuDNN redistributable used when no local copy exists.  The digest is read
# from NVIDIA's redistrib manifest at download time and checked.
$CudnnRedistVersion = "9.26.0"
$CudnnRedistBase = "https://developer.download.nvidia.com/compute/cudnn/redist"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$ThirdParty = Join-Path $RepoRoot "third_party"
$Downloads = Join-Path $ThirdParty "_downloads"
New-Item -ItemType Directory -Force -Path $Downloads | Out-Null

function Get-Verified([string]$Url, [string]$Dest, [string]$Sha256) {
    # Re-use a previous download only when it still hashes correctly.
    if ((Test-Path $Dest) -and ((Get-FileHash $Dest -Algorithm SHA256).Hash -ieq $Sha256)) {
        Write-Host "  cached  $Dest"
        return
    }
    Write-Host "  downloading $Url"
    $part = "$Dest.part"
    Invoke-WebRequest -Uri $Url -OutFile $part -UseBasicParsing
    $got = (Get-FileHash $part -Algorithm SHA256).Hash
    if ($got -ine $Sha256) {
        Remove-Item $part -Force
        throw "SHA-256 mismatch for $Url`n  expected $Sha256`n  got      $got"
    }
    Move-Item -Force $part $Dest
}

# ---------------------------------------------------------------------------
#  1. ONNX Runtime
# ---------------------------------------------------------------------------
$key = "$Version-$Cuda"
if (-not $OrtSha256.ContainsKey($key)) {
    throw "No pinned SHA-256 for ONNX Runtime $key.  Add it to `$OrtSha256 after checking the release page."
}
$ortName = "onnxruntime-win-x64-gpu_$Cuda-$Version"
$ortZip = Join-Path $Downloads "$ortName.zip"
Write-Host "[1/2] ONNX Runtime $Version ($Cuda)"
Get-Verified "https://github.com/microsoft/onnxruntime/releases/download/v$Version/$ortName.zip" $ortZip $OrtSha256[$key]

$ortDest = Join-Path $ThirdParty "onnxruntime"
$staging = Join-Path $Downloads "_extract"
if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
Expand-Archive -Path $ortZip -DestinationPath $staging
$inner = Join-Path $staging $ortName
if (-not (Test-Path (Join-Path $inner "lib\onnxruntime.dll"))) { throw "unexpected archive layout in $ortZip" }
if (Test-Path $ortDest) { Remove-Item -Recurse -Force $ortDest }
Move-Item $inner $ortDest
Remove-Item -Recurse -Force $staging
Write-Host "  installed $ortDest"

# ---------------------------------------------------------------------------
#  2. cuDNN 9
# ---------------------------------------------------------------------------
if ($SkipCudnn) { Write-Host "[2/2] cuDNN skipped"; exit 0 }
Write-Host "[2/2] cuDNN 9 ($Cuda)"
$cudnnDest = Join-Path $ThirdParty "cudnn\bin"

# (a) explicit directory, (b) a pip wheel, (c) NVIDIA's redistributable.
$source = $null
if ($CudnnDir) {
    if (-not (Test-Path (Join-Path $CudnnDir "cudnn64_9.dll"))) { throw "-CudnnDir '$CudnnDir' has no cudnn64_9.dll" }
    $source = $CudnnDir
} elseif ($Cuda -eq "cuda12") {
    # nvidia.cudnn is a namespace package, so __file__ is None; __path__ is not.
    $py = Get-Command python -ErrorAction SilentlyContinue
    if ($py) {
        $pipDir = & python -c "import nvidia.cudnn as m, os; print(os.path.join(list(m.__path__)[0], 'bin'))" 2>$null
        if ($LASTEXITCODE -eq 0 -and $pipDir -and (Test-Path (Join-Path $pipDir "cudnn64_9.dll"))) {
            $source = $pipDir
            Write-Host "  using the pip wheel at $source"
        }
    }
}

if (-not $source) {
    $manifestUrl = "$CudnnRedistBase/redistrib_$CudnnRedistVersion.json"
    Write-Host "  no local cuDNN; reading $manifestUrl"
    $manifest = Invoke-RestMethod -Uri $manifestUrl -UseBasicParsing
    $entry = $manifest.cudnn."windows-x86_64".$Cuda
    if (-not $entry) { throw "the cuDNN $CudnnRedistVersion manifest has no windows-x86_64 $Cuda entry" }
    $zip = Join-Path $Downloads (Split-Path -Leaf $entry.relative_path)
    Get-Verified "$CudnnRedistBase/$($entry.relative_path)" $zip $entry.sha256
    $staging = Join-Path $Downloads "_cudnn"
    if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
    Expand-Archive -Path $zip -DestinationPath $staging
    $dll = Get-ChildItem -Path $staging -Recurse -Filter "cudnn64_9.dll" | Select-Object -First 1
    if (-not $dll) { throw "cudnn64_9.dll not found in $zip" }
    $source = $dll.DirectoryName
}

New-Item -ItemType Directory -Force -Path $cudnnDest | Out-Null
Get-ChildItem -Path $cudnnDest -Filter "cudnn*.dll" -ErrorAction SilentlyContinue | Remove-Item -Force
Copy-Item -Path (Join-Path $source "cudnn*64_9.dll") -Destination $cudnnDest
# Keep NVIDIA's licence next to the binaries it governs.
$lic = @(
    (Join-Path $source "..\LICENSE.txt"), (Join-Path $source "..\LICENSE"),
    (Join-Path $source "..\..\LICENSE.txt")
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($lic) { Copy-Item $lic (Join-Path $cudnnDest "..\LICENSE.txt") -Force }
if ($staging -and (Test-Path $staging) -and ($staging -like "*_cudnn")) { Remove-Item -Recurse -Force $staging }
Write-Host "  installed $((Get-ChildItem $cudnnDest -Filter 'cudnn*.dll').Count) cuDNN DLLs into $cudnnDest"
Write-Host "done - re-run CMake configure to pick these up"
