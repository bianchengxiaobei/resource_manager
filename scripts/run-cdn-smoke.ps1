param(
    [string]$BuildDir = "build/windows-debug",
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Debug",
    [string]$Url = "https://speed.cloudflare.com/__down?bytes=8388608",
    [UInt64]$ExpectedBytes = 8388608
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$exe = Join-Path $root "$BuildDir/bin/$Config/parcel_tests.exe"
if (-not (Test-Path -LiteralPath $exe)) {
    throw "parcel_tests.exe not found: $exe"
}

$oldUrl = $env:PARCEL_CDN_URL
$oldBytes = $env:PARCEL_CDN_EXPECTED_BYTES
try {
    $env:PARCEL_CDN_URL = $Url
    $env:PARCEL_CDN_EXPECTED_BYTES = "$ExpectedBytes"
    & $exe "[.cdn]" --reporter compact
    if ($LASTEXITCODE -ne 0) {
        throw "CDN smoke failed with exit code $LASTEXITCODE"
    }
} finally {
    $env:PARCEL_CDN_URL = $oldUrl
    $env:PARCEL_CDN_EXPECTED_BYTES = $oldBytes
}
