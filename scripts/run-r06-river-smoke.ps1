param(
    [string]$ParcelRoot = "",
    [string]$HbgRoot = "E:\Learn\hdrp-bgfx",
    [string]$AssetRoot = "E:\Learn\hdrp-proland\content\proland",
    [string]$Layout = "",
    [switch]$Rebuild,
    [int]$Width = 800,
    [int]$Height = 450,
    [int]$Frames = 5,
    [int]$Warmup = 1
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
    $Layout = Join-Path $ParcelRoot "build/proland-river-layout"
}
$Layout = [IO.Path]::GetFullPath($Layout)
$PackageName = "proland_river"
$Version = "1.0.0"
$Package = Join-Path $Layout "build/windows/$PackageName/$Version"
$HbgBuild = Join-Path $HbgRoot "build/vs2022"
$Smoke = Join-Path $HbgBuild "bin/RelWithDebInfo/parcel_hbg_gpu_smoke.exe"
$Import = Join-Path $ParcelRoot "build/windows-debug/bin/Debug/parcel_import.exe"
$Pack = Join-Path $ParcelRoot "build/windows-debug/bin/Debug/parcel_pack.exe"

if (-not (Test-Path -LiteralPath (Join-Path $AssetRoot "scenes/river/river.gltf"))) {
    throw "river.gltf not found under $AssetRoot"
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
    $scene = Join-Path $Layout "content/proland/scenes/river"
    $cooked = Join-Path $Layout "cooked"
    New-Item -ItemType Directory -Force -Path $scene, $cooked | Out-Null
    foreach ($name in @("river.gltf", "river.bin", "river.hdrp.json")) {
        Copy-Item -LiteralPath (Join-Path $AssetRoot "scenes/river/$name") `
            -Destination (Join-Path $scene $name) -Force
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
    $screenshot = Join-Path $ParcelRoot "build/proland_river_smoke_$backend.png"
    Write-Host "river GPU smoke: backend=$backend ${Width}x${Height} frames=$Frames warmup=$Warmup"
    $smokeArgs = @("--backend=$backend", "--width=$Width", "--height=$Height",
        "--frames=$Frames", "--warmup=$Warmup", "--package=$Package")
    if ($backend -ne "d3d12") {
        $smokeArgs += "--screenshot=$screenshot"
    }
    $output = Invoke-Native { & $Smoke @smokeArgs } "river GPU smoke ($backend)"
    $riverLine = ($output | Select-String -Pattern 'parcel_hbg_gpu_smoke: river=active mesh=\d+ object=\d+ material=\d+ vertices=\d+ indices=\d+ flow_samples=[1-9]\d*' |
        Select-Object -Last 1).Line
    if (-not $riverLine) {
        throw "river CPU mesh was not applied for $backend"
    }
    $terrainLine = ($output | Select-String -Pattern "parcel_hbg_gpu_smoke: river_terrain=mesh='RiverTerrain' vertices=\d+ changed=[1-9]\d* delta=\[-?[0-9.]+,-?[0-9.]+\]" |
        Select-Object -Last 1).Line
    if (-not $terrainLine) {
        throw "river CPU terrain carve was not applied for $backend"
    }
    $textureLine = ($output | Select-String -Pattern 'parcel_hbg_gpu_smoke: river_texture=size=\d+ base=\d+ normal=\d+ frames=[1-9]\d*' |
        Select-Object -Last 1).Line
    if (-not $textureLine) {
        throw "river CPU material textures were not generated for $backend"
    }
    $normalFrameLine = ($output | Select-String -Pattern 'parcel_hbg_gpu_smoke: river_normal_frame=frame=\d+/[1-9]\d* time=[0-9.]+ step=[0-9.]+' |
        Select-Object -Last 1).Line
    if (-not $normalFrameLine) {
        throw "river wave normal frames were not generated for $backend"
    }
    $foamLine = ($output | Select-String -Pattern 'parcel_hbg_gpu_smoke: river_foam=particles=[1-9]\d* vertices=[1-9]\d* indices=[1-9]\d* texture=\d+ image=\d+ fade=\[[0-9.]+,[0-9.]+\]' |
        Select-Object -Last 1).Line
    if (-not $foamLine) {
        throw "river CPU foam particles were not generated for $backend"
    }
    $foamUpdateLine = ($output | Select-String -Pattern 'parcel_hbg_gpu_smoke: river_foam_update=updates=[1-9]\d* time=[0-9.]+ max_delta=[0-9.]+ fade=\[[0-9.]+,[0-9.]+\]' |
        Select-Object -Last 1).Line
    if (-not $foamUpdateLine) {
        throw "river CPU foam particles were not animated for $backend"
    }
    $foamScreenLine = ($output | Select-String -Pattern 'parcel_hbg_gpu_smoke: river_foam_screen=particles=[1-9]\d*' |
        Select-Object -Last 1).Line
    if (-not $foamScreenLine) {
        throw "river foam particles were not seeded in screen space for $backend"
    }
    $resultLine = ($output | Select-String -Pattern 'parcel_hbg_gpu_smoke: objects=.*draws=.*passes=.*triangles=.*textures=.*avg_frame_ms=' |
        Select-Object -Last 1).Line
    if (-not $resultLine) {
        throw "failed to parse river GPU smoke result for $backend"
    }
    Write-Host $riverLine
    Write-Host $terrainLine
    Write-Host $textureLine
    Write-Host $normalFrameLine
    Write-Host $foamLine
    Write-Host $foamUpdateLine
    Write-Host $foamScreenLine
    Write-Host $resultLine
}
