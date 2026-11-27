param(
    [string]$BuildDir = "build/windows-debug",
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Debug",
    [int]$Count = 512,
    [int]$Bytes = 65536,
    [int]$Concurrency = 8
)

$ErrorActionPreference = "Stop"

if ($Count -lt 1 -or $Bytes -lt 1 -or $Concurrency -lt 1) {
    throw "Count, Bytes and Concurrency must be positive"
}
$exe = Join-Path $BuildDir "bin/$Config/parcel_tests.exe"
if (-not (Test-Path -LiteralPath $exe)) {
    throw "parcel_tests.exe not found: $exe"
}

$python = (Get-Command python -ErrorAction SilentlyContinue)
if ($null -eq $python) {
    throw "python is required for the loopback HTTP benchmark"
}
$pythonExe = $python.Source
$pythonMajor = & $pythonExe -c "import sys; print(sys.version_info[0])"
$pythonModule = if ($pythonMajor -eq 3) { "http.server" } else { "SimpleHTTPServer" }

$root = Join-Path ([System.IO.Path]::GetTempPath()) ("parcel-download-bench-" + [guid]::NewGuid().ToString("N"))
$serverOut = Join-Path $root "server.out.log"
$serverErr = Join-Path $root "server.err.log"
New-Item -ItemType Directory -Path $root -Force | Out-Null
$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
$listener.Stop()

$server = $null
$envNames = @(
    "PARCEL_DOWNLOAD_BENCH_BASE_URL",
    "PARCEL_DOWNLOAD_BENCH_FIXTURE_DIR",
    "PARCEL_DOWNLOAD_BENCH_COUNT",
    "PARCEL_DOWNLOAD_BENCH_BYTES",
    "PARCEL_DOWNLOAD_BENCH_CONCURRENCY"
)
$oldEnv = @{}
foreach ($name in $envNames) {
    $oldEnv[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
}
try {
    $server = Start-Process -FilePath $pythonExe `
        -ArgumentList @("-m", $pythonModule, [string]$port) `
        -WorkingDirectory $root -PassThru `
        -RedirectStandardOutput $serverOut -RedirectStandardError $serverErr
    Start-Sleep -Milliseconds 750

    $env:PARCEL_DOWNLOAD_BENCH_BASE_URL = "http://127.0.0.1:$port"
    $env:PARCEL_DOWNLOAD_BENCH_FIXTURE_DIR = $root
    $env:PARCEL_DOWNLOAD_BENCH_COUNT = [string]$Count
    $env:PARCEL_DOWNLOAD_BENCH_BYTES = [string]$Bytes
    $env:PARCEL_DOWNLOAD_BENCH_CONCURRENCY = [string]$Concurrency
    & $exe "[.download-bench]" --reporter compact
    if ($LASTEXITCODE -ne 0) {
        throw "large download benchmark failed with exit code $LASTEXITCODE"
    }
} catch {
    if (Test-Path $serverErr) {
        Get-Content $serverErr -ErrorAction SilentlyContinue | Select-Object -Last 20
    }
    throw
} finally {
    foreach ($name in $envNames) {
        [Environment]::SetEnvironmentVariable($name, $oldEnv[$name], "Process")
    }
    if ($null -ne $server -and -not $server.HasExited) {
        Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
        $server.WaitForExit(2000) | Out-Null
    }
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
