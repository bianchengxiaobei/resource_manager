param(
    [string]$BuildDir = "build/windows-debug",
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Debug",
    [string]$Url = "https://raw.githubusercontent.com/bianchengxiaobei/resource_manager/b066cfc4ac67e838b7569992b1148d925fb01b41/cpp/tests/golden/minimal.prcl"
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$exe = Join-Path $root "$BuildDir/bin/$Config/parcel_tests.exe"
if (-not (Test-Path -LiteralPath $exe)) {
    throw "parcel_tests.exe not found: $exe"
}

$oldUrl = $env:PARCEL_CDN_BUNDLE_URL
try {
    $env:PARCEL_CDN_BUNDLE_URL = $Url
    & $exe "[.cdn-install]" --reporter compact
    if ($LASTEXITCODE -ne 0) {
        throw "CDN bundle smoke failed with exit code $LASTEXITCODE"
    }
} finally {
    $env:PARCEL_CDN_BUNDLE_URL = $oldUrl
}
