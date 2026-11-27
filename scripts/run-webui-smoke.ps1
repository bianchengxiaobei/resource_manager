$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Work = Join-Path $Root "build\webui-smoke"
$ServerLog = Join-Path $Work "server.log"
$ServerErr = Join-Path $Work "server.err.log"
$ServerExe = Join-Path $Work "parcel-server.exe"
$WebDir = Join-Path $Root "server\web"
$ServerDir = Join-Path $Root "server"
$Server = $null
$SavedEnv = @{}

function Set-ProcessEnv([string]$Name, [string]$Value) {
    if (-not $SavedEnv.ContainsKey($Name)) {
        $SavedEnv[$Name] = [Environment]::GetEnvironmentVariable($Name, "Process")
    }
    [Environment]::SetEnvironmentVariable($Name, $Value, "Process")
}

function Restore-ProcessEnv {
    foreach ($name in $SavedEnv.Keys) {
        [Environment]::SetEnvironmentVariable($name, $SavedEnv[$name], "Process")
    }
}

function Wait-Health([string]$BaseUrl) {
    for ($i = 0; $i -lt 150; $i++) {
        try {
            $response = Invoke-WebRequest -UseBasicParsing -Uri "$BaseUrl/healthz" -TimeoutSec 2
            if ($response.StatusCode -eq 200) {
                return
            }
        } catch {
        }
        Start-Sleep -Milliseconds 200
    }
    throw "server did not become healthy: $BaseUrl"
}

try {
    if (Test-Path -LiteralPath $Work) {
        Remove-Item -LiteralPath $Work -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $Work | Out-Null
    $go = $null
    $goCommand = Get-Command go -ErrorAction SilentlyContinue
    if ($null -ne $goCommand) {
        $go = $goCommand.Source
    } elseif (Test-Path -LiteralPath "C:\Program Files\Go\bin\go.exe") {
        $go = "C:\Program Files\Go\bin\go.exe"
    }
    if (-not $go) {
        throw "go executable was not found; add Go to PATH"
    }
    if (-not (Test-Path -LiteralPath (Join-Path $WebDir "node_modules\@playwright\test"))) {
        throw "Playwright is not installed; run npm install in server/web"
    }

    Write-Host "building web UI"
    Push-Location $WebDir
    try {
        & npm run build
        if ($LASTEXITCODE -ne 0) {
            throw "npm run build failed"
        }
    } finally {
        Pop-Location
    }
    Write-Host "building parcel-server"
    Push-Location $ServerDir
    try {
        & $go build -buildvcs=false -o $ServerExe ./cmd/parcel-server
        if ($LASTEXITCODE -ne 0) {
            throw "go build parcel-server failed"
        }
    } finally {
        Pop-Location
    }

    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = $listener.LocalEndpoint.Port
    $listener.Stop()
    $base = "http://127.0.0.1:$port"
    $adminPassword = [Guid]::NewGuid().ToString("N")
    $db = Join-Path $Work "parcel.db"
    $blobs = Join-Path $Work "blobs"

    Set-ProcessEnv "PARCEL_LISTEN" "127.0.0.1:$port"
    Set-ProcessEnv "PARCEL_DB" $db
    Set-ProcessEnv "PARCEL_BLOB_ROOT" $blobs
    Set-ProcessEnv "PARCEL_PUBLIC_BASE_URL" $base
    Set-ProcessEnv "PARCEL_ADMIN_USER" "admin"
    Set-ProcessEnv "PARCEL_ADMIN_PASSWORD" $adminPassword
    Set-ProcessEnv "PARCEL_JWT_SECRET" ([Guid]::NewGuid().ToString("N"))
    Set-ProcessEnv "PARCEL_HMAC_KEYS" "primary:webui-smoke-secret"
    Set-ProcessEnv "PARCEL_HMAC_KEY_ID" "primary"
    Set-ProcessEnv "PARCEL_SECURE_COOKIES" "false"
    Set-ProcessEnv "PARCEL_LOG_FORMAT" "text"
    Set-ProcessEnv "PARCEL_LOG_LEVEL" "warn"
    Set-ProcessEnv "PARCEL_CDN_URLS" ""

    Write-Host "starting parcel-server on $base"
    $Server = Start-Process -FilePath $ServerExe -WorkingDirectory $ServerDir `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput $ServerLog `
        -RedirectStandardError $ServerErr
    Wait-Health $base

    Set-ProcessEnv "PARCEL_WEB_BASE_URL" $base
    Set-ProcessEnv "PARCEL_WEB_ADMIN_PASSWORD" $adminPassword
    Write-Host "running Playwright smoke against $base"
    Push-Location $WebDir
    try {
        & npx playwright test --config=playwright.config.ts
        if ($LASTEXITCODE -ne 0) {
            throw "Playwright smoke failed"
        }
    } finally {
        Pop-Location
    }
    Write-Host "web UI smoke passed"
} catch {
    if (Test-Path -LiteralPath $ServerLog) {
        Write-Host "--- server.log ---"
        Get-Content -LiteralPath $ServerLog -Tail 80
    }
    if (Test-Path -LiteralPath $ServerErr) {
        Write-Host "--- server.err.log ---"
        Get-Content -LiteralPath $ServerErr -Tail 80
    }
    throw
} finally {
    if ($null -ne $Server -and -not $Server.HasExited) {
        Stop-Process -Id $Server.Id -Force -ErrorAction SilentlyContinue
    }
    Restore-ProcessEnv
}
