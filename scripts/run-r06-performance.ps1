param(
    [ValidateSet("d3d11", "d3d12", "vulkan")]
    [string]$Backend = "d3d11",
    [int]$Width = 1920,
    [int]$Height = 1080,
    [int]$Frames = 20,
    [int]$Warmup = 10,
    [int]$PreflightFrames = 10,
    [int]$Repeats = 3,
    [double]$MaxRatio = 1.10,
    [double]$MinFps = 0,
    [string]$ParcelRoot = "",
    [string]$HbgRoot = "E:\Learn\hdrp-bgfx",
    [string]$Asset = "E:\Learn\hdrp-proland\content\proland\scenes\fractal\fractal.gltf",
    [string]$Package = "E:\Learn\Parcel\build\proland-original-layout\build\windows\proland_original\1.0.0",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

function Get-Median([double[]]$values) {
    $sorted = @($values | Sort-Object)
    if ($sorted.Count -eq 0) {
        throw "median of an empty set"
    }
    $middle = [int][math]::Floor($sorted.Count / 2.0)
    if (($sorted.Count % 2) -eq 1) {
        return [double]$sorted[$middle]
    }
    return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
}

if (-not $ParcelRoot) {
    $ParcelRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}
$HbgRoot = (Resolve-Path -LiteralPath $HbgRoot).Path
$Asset = (Resolve-Path -LiteralPath $Asset).Path
$Package = (Resolve-Path -LiteralPath $Package).Path
$buildRoot = Join-Path $ParcelRoot "build/r06-performance"
New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null

$hbgBuild = Join-Path $HbgRoot "build/vs2022"
$parcelExe = Join-Path $hbgBuild "bin/RelWithDebInfo/parcel_hbg_gpu_smoke.exe"
$hdrpExe = Join-Path $hbgBuild "RelWithDebInfo/hbg.exe"

if (-not $SkipBuild) {
    & cmake --build $hbgBuild --config RelWithDebInfo `
        --target parcel_hbg_gpu_smoke hbg -- /m:8
    if ($LASTEXITCODE -ne 0) {
        throw "RelWithDebInfo build failed with exit code $LASTEXITCODE"
    }
}
if (-not (Test-Path -LiteralPath $parcelExe)) {
    throw "parcel_hbg_gpu_smoke.exe not found: $parcelExe"
}
if (-not (Test-Path -LiteralPath $hdrpExe)) {
    throw "hbg.exe not found: $hdrpExe"
}

if ($Repeats -lt 1) {
    throw "Repeats must be at least 1"
}
if ($MinFps -gt 0 -and $Frames -lt 120) {
    throw "MinFps requires Frames >= 120: hdrp render-test averages from frame 0 and cannot skip pipeline warmup"
}

$hdrpWarmupLog = Join-Path $buildRoot "hdrp_warmup_${Backend}_${Width}x${Height}.log"
Write-Host "hdrp preflight: backend=$Backend ${Width}x${Height} frames=$PreflightFrames"
& $hdrpExe --backend=$Backend --width=$Width --height=$Height `
    --frames=$PreflightFrames --no-vsync --no-shadows --render-test=$Asset 2>&1 |
    Tee-Object -FilePath $hdrpWarmupLog | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "hdrp preflight failed with exit code $LASTEXITCODE"
}

$hdrpAvgs = @()
$parcelAvgs = @()
$parcelMedians = @()
$parcelP95s = @()
$hdrpDraws = $null
$hdrpPasses = $null
$hdrpTriangles = $null
$parcelObjects = $null

for ($repeat = 1; $repeat -le $Repeats; ++$repeat) {
    $suffix = "r${repeat}"
    $hdrpLog = Join-Path $buildRoot "hdrp_${Backend}_${Width}x${Height}_${suffix}.log"
    $parcelLog = Join-Path $buildRoot "parcel_${Backend}_${Width}x${Height}_${suffix}.log"

    Write-Host "hdrp reference [$repeat/$Repeats]: backend=$Backend ${Width}x${Height} frames=$Frames"
    $hdrpOutput = & $hdrpExe --backend=$Backend --width=$Width --height=$Height `
        --frames=$Frames --no-vsync --no-shadows --render-test=$Asset 2>&1 |
        Tee-Object -FilePath $hdrpLog
    if ($LASTEXITCODE -ne 0) {
        throw "hdrp render-test failed with exit code $LASTEXITCODE"
    }

    Write-Host "parcel package [$repeat/$Repeats]: backend=$Backend ${Width}x${Height} warmup=$Warmup frames=$Frames"
    $parcelOutput = & $parcelExe --backend=$Backend --width=$Width --height=$Height `
        --warmup=$Warmup --frames=$Frames --gpu-light-list --motion-vectors `
        --package=$Package 2>&1 | Tee-Object -FilePath $parcelLog
    if ($LASTEXITCODE -ne 0) {
        throw "parcel GPU smoke failed with exit code $LASTEXITCODE"
    }

    $hdrpText = $hdrpOutput -join "`n"
    $parcelText = $parcelOutput -join "`n"
    $hdrpMatch = [regex]::Match($hdrpText,
        'render performance: frames=\d+ avg_frame_ms=([0-9.]+).*?draw=(\d+) compute=\d+ textures=\d+ triangles=(\d+) passes=(\d+)')
    $parcelMatch = [regex]::Match($parcelText,
        'objects=(\d+) draws=(\d+) passes=(\d+) triangles=(\d+) textures=\d+ avg_frame_ms=([0-9.]+) median_frame_ms=([0-9.]+) p95_frame_ms=([0-9.]+)')
    if (-not $hdrpMatch.Success) {
        throw "failed to parse hdrp render performance from $hdrpLog"
    }
    if (-not $parcelMatch.Success) {
        throw "failed to parse parcel performance from $parcelLog"
    }

    $runHdrpDraws = [int]$hdrpMatch.Groups[2].Value
    $runHdrpTriangles = [int]$hdrpMatch.Groups[3].Value
    $runHdrpPasses = [int]$hdrpMatch.Groups[4].Value
    $runObjects = [int]$parcelMatch.Groups[1].Value
    $runParcelDraws = [int]$parcelMatch.Groups[2].Value
    $runParcelPasses = [int]$parcelMatch.Groups[3].Value
    $runParcelTriangles = [int]$parcelMatch.Groups[4].Value

    if ($null -eq $hdrpDraws) {
        $hdrpDraws = $runHdrpDraws
        $hdrpPasses = $runHdrpPasses
        $hdrpTriangles = $runHdrpTriangles
        $parcelObjects = $runObjects
    } elseif ($hdrpDraws -ne $runHdrpDraws -or $hdrpPasses -ne $runHdrpPasses -or
              $hdrpTriangles -ne $runHdrpTriangles -or $parcelObjects -ne $runObjects) {
        throw "hdrp workload changed between runs"
    }
    if ($runHdrpDraws -ne $runParcelDraws -or $runHdrpPasses -ne $runParcelPasses -or
        $runHdrpTriangles -ne $runParcelTriangles) {
        throw "draw workload differs from the original hdrp render-test"
    }

    $hdrpAvgs += [double]$hdrpMatch.Groups[1].Value
    $parcelAvgs += [double]$parcelMatch.Groups[5].Value
    $parcelMedians += [double]$parcelMatch.Groups[6].Value
    $parcelP95s += [double]$parcelMatch.Groups[7].Value
    Write-Host ("run {0}: hdrp avg={1:F3}ms parcel avg={2:F3}ms median={3:F3}ms p95={4:F3}ms" -f `
        $repeat, $hdrpAvgs[-1], $parcelAvgs[-1], $parcelMedians[-1], $parcelP95s[-1])
}

$hdrpFrameMs = Get-Median $hdrpAvgs
$parcelAvgMs = Get-Median $parcelAvgs
$parcelMedianMs = Get-Median $parcelMedians
$parcelP95Ms = Get-Median $parcelP95s
$ratio = $parcelAvgMs / $hdrpFrameMs
$hdrpFps = 1000.0 / $hdrpFrameMs
$parcelFps = 1000.0 / $parcelAvgMs

Write-Host ("hdrp   : median avg={0:F3}ms fps={1:F1} draws={2} passes={3} triangles={4}" -f `
    $hdrpFrameMs, $hdrpFps, $hdrpDraws, $hdrpPasses, $hdrpTriangles)
Write-Host ("parcel : objects={0} median avg={1:F3}ms fps={2:F1} median={3:F3}ms p95={4:F3}ms draws={5} passes={6} triangles={7}" -f `
    $parcelObjects, $parcelAvgMs, $parcelFps, $parcelMedianMs, $parcelP95Ms, $hdrpDraws, $hdrpPasses, $hdrpTriangles)
Write-Host ("parity : parcel_avg/hdrp_avg={0:F3} max={1:F3}" -f $ratio, $MaxRatio)
if ($MinFps -gt 0) {
    Write-Host ("absolute: hdrp_fps={0:F1} parcel_fps={1:F1} min_fps={2:F1}" -f `
        $hdrpFps, $parcelFps, $MinFps)
}

if ($ratio -gt $MaxRatio) {
    throw ("R06 frame budget exceeded: {0:F3} > {1:F3}" -f $ratio, $MaxRatio)
}
if ($MinFps -gt 0 -and $hdrpFps -lt $MinFps) {
    throw ("R06 absolute baseline unavailable: hdrp {0:F1} fps < {1:F1} fps" -f `
        $hdrpFps, $MinFps)
}
if ($MinFps -gt 0 -and $parcelFps -lt $MinFps) {
    throw ("R06 absolute frame budget exceeded: parcel {0:F1} fps < {1:F1} fps" -f `
        $parcelFps, $MinFps)
}

Write-Host "R06 frame budget PASS"
