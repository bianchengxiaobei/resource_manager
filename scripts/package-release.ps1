param(
    [string]$Version = "0.1.0",
    [string]$BuildDir = "build/windows-release",
    [string]$Configuration = "Release",
    [string]$Output = "dist",
    [switch]$SkipArchive
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$build = Join-Path $root $BuildDir
$stage = Join-Path $root (Join-Path $Output "parcel-$Version")

if (-not (Test-Path -LiteralPath $build)) {
    throw "build directory not found: $build"
}
if (Test-Path -LiteralPath $stage) {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
New-Item -ItemType Directory -Path $stage -Force | Out-Null

function Copy-Required([string]$source, [string]$destination) {
    if (-not (Test-Path -LiteralPath $source)) {
        throw "required release input not found: $source"
    }
    $parent = Split-Path -Parent $destination
    if ($parent) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }
    Copy-Item -LiteralPath $source -Destination $destination -Recurse -Force
}

$bin = Join-Path $build "bin/$Configuration"
foreach ($name in @("parcel_import.exe", "parcel_pack.exe")) {
    Copy-Required (Join-Path $bin $name) (Join-Path $stage "bin/$name")
}
$optionalBinaries = @("parcel_bench.exe", "parcel_import.pdb", "parcel_pack.pdb", "parcel_bench.pdb")
foreach ($name in $optionalBinaries) {
    $source = Join-Path $bin $name
    if (Test-Path -LiteralPath $source) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $stage "bin/$name") -Force
    }
}

$lib = Join-Path $build "lib/$Configuration"
if (Test-Path -LiteralPath $lib) {
    Copy-Item -LiteralPath $lib -Destination (Join-Path $stage "lib") -Recurse -Force
}

Copy-Required (Join-Path $root "cpp/include/parcel") (Join-Path $stage "include/parcel")
Copy-Required (Join-Path $root "docs") (Join-Path $stage "docs")
Copy-Required (Join-Path $root "schemas") (Join-Path $stage "schemas")
Copy-Required (Join-Path $root "examples") (Join-Path $stage "examples")
Copy-Required (Join-Path $root "deploy") (Join-Path $stage "deploy")
Copy-Required (Join-Path $root "VERSION") (Join-Path $stage "VERSION")
Copy-Required (Join-Path $root "README.md") (Join-Path $stage "README.md")
Copy-Required (Join-Path $root "CHANGELOG.md") (Join-Path $stage "CHANGELOG.md")
Copy-Required (Join-Path $root "THIRD_PARTY_NOTICES.md") (Join-Path $stage "THIRD_PARTY_NOTICES.md")

$serverBin = Join-Path $root "server/build"
$serverStage = Join-Path $stage "server"
if (Test-Path -LiteralPath $serverBin) {
    New-Item -ItemType Directory -Path $serverStage -Force | Out-Null
    foreach ($name in @("parcel-server.exe", "parcel-admin.exe")) {
        $source = Join-Path $serverBin $name
        if (Test-Path -LiteralPath $source) {
            Copy-Item -LiteralPath $source -Destination (Join-Path $serverStage $name) -Force
        }
    }
}
$serverWeb = Join-Path $root "server/internal/webui/dist"
if (Test-Path -LiteralPath $serverWeb) {
    Copy-Item -LiteralPath $serverWeb -Destination (Join-Path $serverStage "webui") -Recurse -Force
}

$archive = "$stage.zip"
if (-not $SkipArchive) {
    if (Test-Path -LiteralPath $archive) {
        Remove-Item -LiteralPath $archive -Force
    }
    Compress-Archive -Path $stage -DestinationPath $archive -CompressionLevel Optimal
}

Write-Output "release staged: $stage"
if (-not $SkipArchive) {
    Write-Output "archive: $archive"
}




