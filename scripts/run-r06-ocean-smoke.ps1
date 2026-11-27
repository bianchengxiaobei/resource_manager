param(
    [string]$ParcelRoot = "",
    [string]$HbgRoot = "E:\Learn\hdrp-bgfx",
    [string]$AssetRoot = "E:\Learn\hdrp-proland\content\proland",
    [string]$Layout = "",
    [switch]$Rebuild,
    [int]$Width = 800,
    [int]$Height = 450,
    [int]$Frames = 5
)

$ErrorActionPreference = "Stop"

if (-not $ParcelRoot) {
    $ParcelRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}
$HbgRoot = (Resolve-Path -LiteralPath $HbgRoot).Path
$AssetRoot = (Resolve-Path -LiteralPath $AssetRoot).Path
if (-not $Layout) {
    $Layout = Join-Path $ParcelRoot "build/proland-ocean-layout"
}
$Layout = [IO.Path]::GetFullPath($Layout)
$Package = Join-Path $Layout "build/windows/proland_ocean/1.0.0"
$HbgBuild = Join-Path $HbgRoot "build/vs2022"
$Smoke = Join-Path $HbgBuild "bin/RelWithDebInfo/parcel_hbg_gpu_smoke.exe"
$Import = Join-Path $ParcelRoot "build/windows-debug/bin/Debug/parcel_import.exe"
$Pack = Join-Path $ParcelRoot "build/windows-debug/bin/Debug/parcel_pack.exe"

if (-not (Test-Path -LiteralPath (Join-Path $AssetRoot "scenes/fractal/ocean_sphere.gltf"))) {
    throw "ocean_sphere.gltf not found under $AssetRoot"
}
if (-not (Test-Path -LiteralPath $Smoke)) {
    throw "parcel_hbg_gpu_smoke.exe not found: $Smoke"
}
if (-not (Test-Path -LiteralPath $Import) -or -not (Test-Path -LiteralPath $Pack)) {
    throw "parcel_import.exe / parcel_pack.exe not found; build parcel_import_cli and parcel_pack_cli"
}

function Build-Package {
    if (Test-Path -LiteralPath $Layout) {
        Remove-Item -LiteralPath $Layout -Recurse -Force
    }
    $scene = Join-Path $Layout "content/proland/scenes/fractal"
    $assets = Join-Path $Layout "content/proland/assets/fractal"
    $cooked = Join-Path $Layout "cooked"
    New-Item -ItemType Directory -Path $scene, $assets, $cooked -Force | Out-Null
    foreach ($name in @("ocean_sphere.gltf", "ocean_sphere.terrain.json", "fractal_lod.bin")) {
        Copy-Item -LiteralPath (Join-Path $AssetRoot "scenes/fractal/$name") -Destination $scene -Force
    }
    foreach ($name in @("albedo.png", "orm.png", "normal.png", "water_normal.png",
                       "tree_deciduous.png", "tree_conifer.png")) {
        Copy-Item -LiteralPath (Join-Path $AssetRoot "assets/fractal/$name") -Destination $assets -Force
    }

    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "SilentlyContinue"
    try {
        & $Import --project $Layout --source (Join-Path $Layout "content/proland") `
            --output $cooked --db (Join-Path $Layout "import.db") --jobs 8 --force
        $importExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldPreference
    }
    if ($importExit -ne 0) {
        throw "parcel_import failed with exit code $importExit"
    }

    $config = @'
{
  "package": "proland_ocean",
  "version": "1.0.0",
  "target": "windows",
  "cooked": "cooked",
  "output": "build",
  "compression": "lz4",
  "collectors": [
    {
      "name": "main",
      "type": "main",
      "path": "",
      "recursive": true,
      "addressRule": "assetPath"
    }
  ]
}
'@
    $configPath = Join-Path $Layout "pack.json"
    [IO.File]::WriteAllText($configPath, $config, (New-Object Text.UTF8Encoding($false)))
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "SilentlyContinue"
    try {
        & $Pack --project $Layout --cooked cooked --output build --package proland_ocean `
            --version 1.0.0 --target windows --compression lz4 --config $configPath --jobs 8
        $packExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldPreference
    }
    if ($packExit -ne 0) {
        throw "parcel_pack failed with exit code $packExit"
    }
}

if ($Rebuild -or -not (Test-Path -LiteralPath (Join-Path $Package "manifest.bytes"))) {
    Write-Host "building proland_ocean package"
    Build-Package
}

foreach ($backend in @("d3d11", "d3d12", "vulkan")) {
    $screenshot = Join-Path $ParcelRoot "build/proland_ocean_smoke_$backend.png"
    Write-Host "ocean GPU smoke: backend=$backend ${Width}x${Height} frames=$Frames"
    $smokeArgs = @("--backend=$backend", "--width=$Width", "--height=$Height",
        "--frames=$Frames", "--package=$Package")
    if ($backend -ne "d3d12") {
        $smokeArgs += "--screenshot=$screenshot"
    }
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "SilentlyContinue"
    try {
        $output = & $Smoke @smokeArgs 2>&1
        $smokeExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldPreference
    }
    if ($smokeExit -ne 0) {
        throw "ocean GPU smoke failed for $backend with exit code $smokeExit"
    }
    $line = ($output | Select-String -Pattern 'parcel_hbg_gpu_smoke: objects=.*draws=.*passes=.*triangles=.*textures=.*avg_frame_ms=' |
        Select-Object -Last 1).Line
    if (-not $line) {
        throw "failed to parse ocean GPU smoke result for $backend"
    }
    $waterLine = ($output | Select-String -Pattern 'water_fft=active mesh=\d+ vertices=\d+ updates=[1-9]\d*' |
        Select-Object -Last 1).Line
    if (-not $waterLine) {
        throw "ocean CPU FFT was not applied for $backend"
    }
    Write-Host $waterLine
    Write-Host $line
}
