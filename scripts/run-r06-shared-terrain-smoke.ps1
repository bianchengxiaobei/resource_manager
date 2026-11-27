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

if ($Frames -lt 3) {
    throw "Frames must be at least 3 so D3D12 has a valid frame sample"
}
if (-not $ParcelRoot) {
    $ParcelRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}
$HbgRoot = (Resolve-Path -LiteralPath $HbgRoot).Path
$AssetRoot = (Resolve-Path -LiteralPath $AssetRoot).Path
if (-not $Layout) {
    $Layout = Join-Path $ParcelRoot "build/proland-shared-layout"
}
$Layout = [IO.Path]::GetFullPath($Layout)
$PackageName = "proland_shared"
$Version = "1.0.0"
$Package = Join-Path $Layout "build/windows/$PackageName/$Version"
$HbgBuild = Join-Path $HbgRoot "build/vs2022"
$Smoke = Join-Path $HbgBuild "bin/RelWithDebInfo/parcel_hbg_gpu_smoke.exe"
$Import = Join-Path $ParcelRoot "build/windows-debug/bin/Debug/parcel_import.exe"
$Pack = Join-Path $ParcelRoot "build/windows-debug/bin/Debug/parcel_pack.exe"

if (-not (Test-Path -LiteralPath $Smoke)) {
    throw "parcel_hbg_gpu_smoke.exe not found: $Smoke"
}
if (-not (Test-Path -LiteralPath $Import) -or -not (Test-Path -LiteralPath $Pack)) {
    throw "parcel_import.exe / parcel_pack.exe not found; build parcel_import_cli and parcel_pack_cli"
}

function Invoke-Native {
    param([scriptblock]$Command, [string]$Name)
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "SilentlyContinue"
    try {
        $output = & $Command 2>&1
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldPreference
    }
    if ($code -ne 0) {
        $output | ForEach-Object { Write-Host $_ }
        throw "$Name failed with exit code $code"
    }
    return $output
}

function Build-Package {
    if (Test-Path -LiteralPath $Layout) {
        Remove-Item -LiteralPath $Layout -Recurse -Force
    }
    $scene = Join-Path $Layout "content/proland/scenes/fractal"
    $assets = Join-Path $Layout "content/proland/assets/fractal"
    $cooked = Join-Path $Layout "cooked"
    New-Item -ItemType Directory -Force -Path $scene, $assets, $cooked | Out-Null

    $sourceScene = Join-Path $AssetRoot "scenes/fractal"
    $sourceAssets = Join-Path $AssetRoot "assets/fractal"
    foreach ($name in @("fractal_lod.gltf", "fractal_lod.bin", "fractal_lod.terrain.json")) {
        Copy-Item -LiteralPath (Join-Path $sourceScene $name) -Destination (Join-Path $scene $name) -Force
    }
    foreach ($name in @("albedo.png", "orm.png", "normal.png", "water_normal.png",
                        "tree_conifer.png", "tree_deciduous.png", "heightmap.bin")) {
        Copy-Item -LiteralPath (Join-Path $sourceAssets $name) -Destination (Join-Path $assets $name) -Force
    }

    $configPath = Join-Path $Layout "pack.json"
    $config = @"
{
  "package": "$PackageName",
  "version": "$Version",
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
"@
    [IO.File]::WriteAllText($configPath, $config, (New-Object Text.UTF8Encoding($false)))

    Invoke-Native {
        & $Import --project $Layout --source (Join-Path $Layout "content/proland") `
            --output $cooked --db (Join-Path $Layout "import.db") --jobs 8 --force
    } "parcel_import" | Out-Null
    Invoke-Native {
        & $Pack --project $Layout --cooked cooked --output build --package $PackageName `
            --version $Version --target windows --compression lz4 --config $configPath --jobs 8
    } "parcel_pack" | Out-Null
}

if ($Rebuild -or -not (Test-Path -LiteralPath (Join-Path $Package "manifest.bytes"))) {
    Write-Host "building $PackageName package"
    Build-Package
}

foreach ($backend in @("d3d11", "d3d12", "vulkan")) {
    $screenshot = Join-Path $ParcelRoot "build/proland_shared_terrain_smoke_$backend.png"
    Write-Host "shared terrain GPU smoke: backend=$backend ${Width}x${Height} frames=$Frames"
    $smokeArgs = @("--backend=$backend", "--width=$Width", "--height=$Height",
        "--frames=$Frames", "--package=$Package")
    if ($backend -ne "d3d12") {
        $smokeArgs += "--screenshot=$screenshot"
    }
    $output = Invoke-Native { & $Smoke @smokeArgs } "shared terrain GPU smoke ($backend)"
    $terrainLine = ($output | Select-String -Pattern 'terrain_lod=active leaves=\d+ visible=\d+ objects=\d+ bound=\d+' |
        Select-Object -Last 1).Line
    if (-not $terrainLine -or $terrainLine -notmatch 'bound=341') {
        throw "failed to parse shared terrain LOD result for $backend"
    }
    $sharedLine = ($output | Select-String -Pattern 'terrain_shared=meshes=\d+ vertices=\d+ triangles=\d+ coarse=\d+ fine=\d+ fine_from=\d+' |
        Select-Object -Last 1).Line
    if (-not $sharedLine -or $sharedLine -notmatch 'meshes=341 vertices=1460841 triangles=2833664 coarse=33 fine=65 fine_from=3') {
        throw "shared terrain geometry was not built for $backend"
    }
    $heightmapLine = ($output | Select-String -Pattern 'parcel_hbg_gpu_smoke: terrain_heightmap=.*' |
        Select-Object -Last 1).Line
    if (-not $heightmapLine -or $heightmapLine -match 'disabled') {
        throw "shared terrain heightmap was not loaded for $backend"
    }
    $waterLine = ($output | Select-String -Pattern 'water_fft=active mesh=\d+ vertices=\d+ updates=[1-9]\d*' |
        Select-Object -Last 1).Line
    if (-not $waterLine) {
        throw "shared terrain CPU FFT was not applied for $backend"
    }
    $resultLine = ($output | Select-String -Pattern 'parcel_hbg_gpu_smoke: objects=.*draws=.*passes=.*triangles=.*textures=.*avg_frame_ms=' |
        Select-Object -Last 1).Line
    if (-not $resultLine) {
        throw "failed to parse GPU smoke result for $backend"
    }
    Write-Host $terrainLine
    Write-Host $sharedLine
    Write-Host $heightmapLine
    Write-Host $waterLine
    Write-Host $resultLine
}
