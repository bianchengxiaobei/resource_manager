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
    $Layout = Join-Path $ParcelRoot "build/proland-terrain-layout"
}
$Layout = [IO.Path]::GetFullPath($Layout)
$PackageName = "proland_terrain"
$Version = "1.0.0"
$Package = Join-Path $Layout "build/windows/$PackageName/$Version"
$HbgBuild = Join-Path $HbgRoot "build/vs2022"
$Smoke = Join-Path $HbgBuild "bin/RelWithDebInfo/parcel_hbg_gpu_smoke.exe"
$Import = Join-Path $ParcelRoot "build/windows-debug/bin/Debug/parcel_import.exe"
$Pack = Join-Path $ParcelRoot "build/windows-debug/bin/Debug/parcel_pack.exe"
$Generator = Join-Path $AssetRoot "tools/gen_terrain_lod.py"

if (-not (Test-Path -LiteralPath $Generator)) {
    throw "gen_terrain_lod.py not found: $Generator"
}
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
    $tools = Join-Path $ParcelRoot "build/terrain-tools"
    New-Item -ItemType Directory -Force -Path $scene, $assets, $cooked, $tools | Out-Null

    $sourceAssets = Join-Path $AssetRoot "assets/fractal"
    foreach ($name in @("albedo.png", "orm.png", "normal.png", "water_normal.png",
                        "tree_conifer.png", "tree_deciduous.png", "heightmap.png", "trees.json")) {
        Copy-Item -LiteralPath (Join-Path $sourceAssets $name) -Destination (Join-Path $assets $name) -Force
    }
    foreach ($name in @("gen_terrain_lod.py", "export_gltf.py", "gen_trees.py", "gen_fractal.py")) {
        Copy-Item -LiteralPath (Join-Path $AssetRoot "tools/$name") -Destination (Join-Path $tools $name) -Force
    }

    $generator = Join-Path $tools "gen_terrain_lod.py"
    $gltf = Join-Path $scene "fractal_lod_baked.gltf"
    Invoke-Native {
        py -3 $generator --params (Join-Path $AssetRoot "scene_params.json") --assets $assets `
            --out $gltf --baked-terrain --no-trees
    } "terrain generation" | Out-Null

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
    $screenshot = Join-Path $ParcelRoot "build/proland_terrain_smoke_$backend.png"
    Write-Host "terrain GPU smoke: backend=$backend ${Width}x${Height} frames=$Frames"
    $smokeArgs = @("--backend=$backend", "--width=$Width", "--height=$Height",
        "--frames=$Frames", "--package=$Package")
    if ($backend -ne "d3d12") {
        $smokeArgs += "--screenshot=$screenshot"
    }
    $output = Invoke-Native { & $Smoke @smokeArgs } "terrain GPU smoke ($backend)"
    $terrainLine = ($output | Select-String -Pattern 'terrain_lod=active leaves=\d+ visible=\d+ objects=\d+ bound=\d+' |
        Select-Object -Last 1).Line
    if (-not $terrainLine) {
        throw "failed to parse terrain LOD result for $backend"
    }
    $heightmapLine = ($output | Select-String -Pattern 'parcel_hbg_gpu_smoke: terrain_heightmap=.*' |
        Select-Object -Last 1).Line
    if (-not $heightmapLine -or $heightmapLine -match 'disabled') {
        throw "terrain heightmap was not loaded for $backend"
    }
    $vegetationLine = ($output | Select-String -Pattern 'parcel_hbg_gpu_smoke: vegetation=\d+ deciduous=\d+ conifer=\d+ meshes=\d+ triangles=\d+ textures=\d+ objects=\d+ tiles=\d+ submeshes=\d+' |
        Select-Object -Last 1).Line
    if (-not $vegetationLine -or $vegetationLine -notmatch 'vegetation=60000 .*meshes=1481 .*textures=2' -or
        $vegetationLine -notmatch 'objects=1481 tiles=1273 submeshes=5003') {
        throw "terrain vegetation was not loaded for $backend"
    }
    $visibilityLine = ($output | Select-String -Pattern 'parcel_hbg_gpu_smoke: vegetation_visibility=objects=\d+ tiles=\d+ submeshes=\d+ visible=\d+ visible_tiles=\d+ visible_submeshes=\d+ frustum_culled=\d+ frustum_culled_submeshes=\d+ distance_culled=\d+ distance_culled_submeshes=\d+' |
        Select-Object -Last 1).Line
    if (-not $visibilityLine -or $visibilityLine -notmatch 'objects=1481 tiles=1273 submeshes=5003' -or
        $visibilityLine -notmatch 'visible_submeshes=[1-9]\d*') {
        throw "terrain vegetation visibility was not applied for $backend"
    }
    $waterLine = ($output | Select-String -Pattern 'water_fft=active mesh=\d+ vertices=\d+ updates=[1-9]\d*' |
        Select-Object -Last 1).Line
    if (-not $waterLine) {
        throw "terrain CPU FFT was not applied for $backend"
    }
    $resultLine = ($output | Select-String -Pattern 'parcel_hbg_gpu_smoke: objects=.*draws=.*passes=.*triangles=.*textures=.*avg_frame_ms=' |
        Select-Object -Last 1).Line
    if (-not $resultLine) {
        throw "failed to parse GPU smoke result for $backend"
    }
    Write-Host $terrainLine
    Write-Host $heightmapLine
    Write-Host $vegetationLine
    Write-Host $visibilityLine
    Write-Host $waterLine
    Write-Host $resultLine
}
