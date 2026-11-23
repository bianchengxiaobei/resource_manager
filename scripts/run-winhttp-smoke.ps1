param(
    [string]$BuildDir = "build/windows-r08-test",
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Debug"
)

$ErrorActionPreference = "Stop"

$exe = Join-Path $BuildDir "bin/$Config/parcel_tests.exe"
if (-not (Test-Path -LiteralPath $exe)) {
    throw "parcel_tests.exe not found: $exe"
}

$python = (Get-Command python -ErrorAction SilentlyContinue)
if ($null -eq $python) {
    throw "python is required for the loopback HTTP fixture"
}
$pythonExe = $python.Source
$pythonMajor = & $pythonExe -c "import sys; print(sys.version_info[0])"
$pythonModule = if ($pythonMajor -eq 3) { "http.server" } else { "SimpleHTTPServer" }

$root = Join-Path ([System.IO.Path]::GetTempPath()) ("parcel-winhttp-" + [guid]::NewGuid().ToString("N"))
$serverOut = Join-Path $root "server.out.log"
$serverErr = Join-Path $root "server.err.log"
New-Item -ItemType Directory -Path $root -Force | Out-Null
$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
$listener.Stop()

$server = $null
$oldBase = $env:PARCEL_WINHTTP_BASE_URL
$oldFixture = $env:PARCEL_WINHTTP_FIXTURE_DIR
try {
    $server = Start-Process -FilePath $pythonExe `
        -ArgumentList @("-m", $pythonModule, [string]$port) `
        -WorkingDirectory $root -PassThru `
        -RedirectStandardOutput $serverOut -RedirectStandardError $serverErr
    Start-Sleep -Milliseconds 750

    $env:PARCEL_WINHTTP_BASE_URL = "http://127.0.0.1:$port"
    $env:PARCEL_WINHTTP_FIXTURE_DIR = $root
    & $exe "[.winhttp]" --reporter compact
    if ($LASTEXITCODE -ne 0) {
        throw "WinHTTP loopback smoke failed with exit code $LASTEXITCODE"
    }
} catch {
    if (Test-Path $serverErr) {
        Get-Content $serverErr -ErrorAction SilentlyContinue | Select-Object -Last 20
    }
    throw
} finally {
    $env:PARCEL_WINHTTP_BASE_URL = $oldBase
    $env:PARCEL_WINHTTP_FIXTURE_DIR = $oldFixture
    if ($null -ne $server -and -not $server.HasExited) {
        Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
        $server.WaitForExit(2000) | Out-Null
    }
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
