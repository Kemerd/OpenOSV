# Dump the lens luma bands (code space) the parallax analysis sees, and the
# production DIS / SEA-RAFT region scores, for frames 0 / 32 / 64.
#
#   pwsh research\aistitch\dump_flow_bands.ps1 [-Clip <osv>] [-Out <dir>]
#
# Two dumps per frame:
#   * default analysis band (2048 x +-6 deg), with the production parallax
#     grid applied -> the "prod" reference numbers, per region;
#   * a wide band (2048 x +-10 deg, unwarped) that the Python flow models run
#     on, so a warp that points past +-6 deg still finds pixels to sample.
param(
    [string]$Clip = "L:/Dev/premiere_360_reframe/example_footage_dlogm.OSV",
    [string]$Out = "$PSScriptRoot/out/flowbands",
    [int[]]$Frames = @(0, 32, 64)
)
$ErrorActionPreference = "Stop"
$env:PATH = "L:\Dev\premiere_360_reframe\build\windows-msvc-premiere-release\bin;$env:PATH"
New-Item -ItemType Directory -Force $Out | Out-Null

# Regions of the 2048-column band used by NEURAL_STITCHING.md section 1.6
$regions = @{ sky = "410-900"; ground = "1110-1700"; wing = "1880-2040" }

foreach ($f in $Frames) {
    foreach ($backend in @("classical", "neural")) {
        foreach ($name in $regions.Keys) {
            $json = "$Out/prod_f${f}_${backend}_${name}.json"
            if (Test-Path $json) { continue }
            osvtool seam $Clip --frame $f --parallax --flow-backend $backend --region $regions[$name] --json |
                Out-File -Encoding utf8 $json
        }
    }
    # Default band, uncorrected + DIS-corrected lens lumas
    osvtool seam $Clip --frame $f --parallax --flow-backend classical --dump-bands "$Out/f${f}_b6" --json |
        Out-File -Encoding utf8 "$Out/dump_f${f}_b6.json"
    # Wide band for the Python models
    osvtool seam $Clip --frame $f --parallax --flow-backend classical --parallax-band-deg 10 `
        --dump-bands "$Out/f${f}_b10" --json | Out-File -Encoding utf8 "$Out/dump_f${f}_b10.json"
}
