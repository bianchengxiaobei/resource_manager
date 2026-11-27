param(
    [string]$TestExe = "",
    [switch]$Build
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Work = Join-Path $Root "build\signed-url-smoke"
$ServerLog = Join-Path $Work "server.log"
$ServerErr = Join-Path $Work "server.err.log"
$ServerExe = Join-Path $Work "parcel-server.exe"
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

function New-ManifestBytes([string]$Payload) {
    $data = [Text.Encoding]::UTF8.GetBytes($Payload)
    $manifest = New-Object byte[] (160 + $data.Length)
    [Text.Encoding]::ASCII.GetBytes("PRCM").CopyTo($manifest, 0)
    $manifest[4] = 1
    $manifest[5] = 0
    $manifest[6] = 160
    $manifest[7] = 0
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $hash = $sha.ComputeHash($data)
    } finally {
        $sha.Dispose()
    }
    [Array]::Copy($hash, 0, $manifest, 96, 32)
    [Array]::Copy($data, 0, $manifest, 160, $data.Length)
    return ,$manifest
}

function New-BundleBytes {
    $bundle = New-Object byte[] 4096
    [Text.Encoding]::ASCII.GetBytes("PRCL").CopyTo($bundle, 0)
    $bundle[4] = 1
    $bundle[5] = 0
    $bundle[6] = 128
    $bundle[7] = 0
    for ($i = 128; $i -lt $bundle.Length; $i++) {
        $bundle[$i] = [byte](($i * 31 + 7) % 251)
    }
    return ,$bundle
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
    Write-Host "building parcel-server"
    Push-Location (Join-Path $Root "server")
    try {
        & $go build -buildvcs=false -o $ServerExe ./cmd/parcel-server
        if ($LASTEXITCODE -ne 0) {
            throw "go build parcel-server failed"
        }
    } finally {
        Pop-Location
    }

    if (-not $TestExe) {
        $TestExe = Join-Path $Root "build\windows-debug\bin\Debug\parcel_tests.exe"
    }
    if ($Build) {
        $buildDir = Join-Path $Root "build\windows-debug"
        & cmake --build $buildDir --config Debug --target parcel_tests
        if ($LASTEXITCODE -ne 0) {
            throw "cmake build parcel_tests failed"
        }
    }
    if (-not (Test-Path -LiteralPath $TestExe)) {
        throw "parcel_tests.exe not found: $TestExe (pass -TestExe or -Build)"
    }

    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = $listener.LocalEndpoint.Port
    $listener.Stop()
    $base = "http://127.0.0.1:$port"
    $adminPassword = [Guid]::NewGuid().ToString("N")
    $jwtSecret = [Guid]::NewGuid().ToString("N")
    $db = Join-Path $Work "parcel.db"
    $blobs = Join-Path $Work "blobs"
    Remove-Item -LiteralPath $db -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $blobs -Recurse -Force -ErrorAction SilentlyContinue

    Set-ProcessEnv "PARCEL_LISTEN" "127.0.0.1:$port"
    Set-ProcessEnv "PARCEL_DB" $db
    Set-ProcessEnv "PARCEL_BLOB_ROOT" $blobs
    Set-ProcessEnv "PARCEL_PUBLIC_BASE_URL" $base
    Set-ProcessEnv "PARCEL_ADMIN_USER" "admin"
    Set-ProcessEnv "PARCEL_ADMIN_PASSWORD" $adminPassword
    Set-ProcessEnv "PARCEL_JWT_SECRET" $jwtSecret
    Set-ProcessEnv "PARCEL_HMAC_KEYS" "primary:signed-url-smoke-secret"
    Set-ProcessEnv "PARCEL_HMAC_KEY_ID" "primary"
    Set-ProcessEnv "PARCEL_SIGNED_URL_TTL" "5s"
    Set-ProcessEnv "PARCEL_REQUIRE_SIGNED_FILES" "true"
    Set-ProcessEnv "PARCEL_LOG_FORMAT" "text"
    Set-ProcessEnv "PARCEL_LOG_LEVEL" "warn"
    Set-ProcessEnv "PARCEL_CDN_URLS" ""

    Write-Host "starting parcel-server on $base"
    $Server = Start-Process -FilePath $ServerExe -WorkingDirectory (Join-Path $Root "server") `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput $ServerLog `
        -RedirectStandardError $ServerErr
    Wait-Health $base

    $login = Invoke-RestMethod -Method Post -Uri "$base/api/v1/admin/login" `
        -ContentType "application/json" `
        -Body (@{ username = "admin"; password = $adminPassword } | ConvertTo-Json) `
        -SessionVariable webSession
    $headers = @{ "X-CSRF-Token" = $login.csrf_token }
    $null = Invoke-RestMethod -Method Post -Uri "$base/api/v1/admin/packages" `
        -WebSession $webSession -Headers $headers -ContentType "application/json" `
        -Body (@{ name = "game" } | ConvertTo-Json)
    $keyResponse = Invoke-RestMethod -Method Post -Uri "$base/api/v1/admin/api-keys" `
        -WebSession $webSession -Headers $headers -ContentType "application/json" `
        -Body (@{ name = "signed-url-smoke" } | ConvertTo-Json)
    $apiKey = $keyResponse.api_key
    if (-not $apiKey) {
        throw "server did not return an API key"
    }

    $bundleName = "smoke.bundle"
    $manifestPath = Join-Path $Work "manifest.bytes"
    $bundlePath = Join-Path $Work $bundleName
    [IO.File]::WriteAllBytes($manifestPath, (New-ManifestBytes "signed-url-smoke"))
    [IO.File]::WriteAllBytes($bundlePath, (New-BundleBytes))
    $bundleHash = (Get-FileHash -LiteralPath $bundlePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $bundleSize = (Get-Item -LiteralPath $bundlePath).Length

    $curl = (Get-Command curl.exe -ErrorAction Stop).Source
    $upload = & $curl -sS --fail-with-body `
        -X POST "$base/api/v1/admin/packages/game/versions" `
        -H "X-API-Key: $apiKey" `
        -F "version=1.0.0" `
        -F "manifest=@$manifestPath;filename=manifest.bytes" `
        -F "bundles=@$bundlePath;filename=$bundleName"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "upload response: $upload"
        throw "upload failed"
    }
    $publishPath = Join-Path $Work "publish.json"
    [IO.File]::WriteAllText($publishPath, '{"channel":"prod"}', (New-Object Text.UTF8Encoding($false)))
    $publish = & $curl -sS --fail-with-body -X POST `
        "$base/api/v1/admin/packages/game/versions/1.0.0/publish" `
        -H "X-API-Key: $apiKey" -H "Content-Type: application/json" `
        --data-binary "@$publishPath"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "publish response: $publish"
        throw "publish failed"
    }

    $tokenResponse = Invoke-RestMethod -Method Post -Uri "$base/api/v1/auth/device-token" `
        -ContentType "application/json" `
        -Body (@{ player_id = "signed-url-smoke"; package = "game"; platform = "windows" } |
            ConvertTo-Json)
    if (-not $tokenResponse.token) {
        throw "server did not return a device token"
    }

    Set-ProcessEnv "PARCEL_SIGNED_URL_ENDPOINT" "$base/api/v1/client/url"
    Set-ProcessEnv "PARCEL_SIGNED_URL_TOKEN" $tokenResponse.token
    Set-ProcessEnv "PARCEL_SIGNED_URL_PACKAGE" "game"
    Set-ProcessEnv "PARCEL_SIGNED_URL_VERSION" "1.0.0"
    Set-ProcessEnv "PARCEL_SIGNED_URL_BUNDLE" $bundleName
    Set-ProcessEnv "PARCEL_SIGNED_URL_EXPECTED_SIZE" "$bundleSize"
    Set-ProcessEnv "PARCEL_SIGNED_URL_EXPECTED_SHA256" $bundleHash

    Write-Host "running [.signed] against $base"
    & $TestExe "[.signed]"
    if ($LASTEXITCODE -ne 0) {
        throw "signed URL smoke failed"
    }
    Write-Host "signed URL smoke passed: bytes=$bundleSize sha256=$bundleHash"
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
