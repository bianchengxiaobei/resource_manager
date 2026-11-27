param(
    [string]$NginxVersion = "1.27.5"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Work = Join-Path $Root "build\nginx-smoke"
$Tools = Join-Path $Root "build\tools"
$ServerLog = Join-Path $Work "server.log"
$ServerErr = Join-Path $Work "server.err.log"
$NginxOut = Join-Path $Work "nginx.out.log"
$NginxErr = Join-Path $Work "nginx.err.log"
$ServerExe = Join-Path $Work "parcel-server.exe"
$Server = $null
$Nginx = $null
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

function Get-FreePort {
    $listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = $listener.LocalEndpoint.Port
    $listener.Stop()
    return $port
}

function Wait-LocalHealth([string]$Url) {
    for ($i = 0; $i -lt 150; $i++) {
        try {
            $response = Invoke-WebRequest -UseBasicParsing -Uri $Url -TimeoutSec 2
            if ($response.StatusCode -eq 200) {
                return
            }
        } catch {
        }
        Start-Sleep -Milliseconds 200
    }
    throw "parcel-server did not become healthy: $Url"
}

function Wait-HttpsHealth([string]$Url, [string]$CaCert, [string]$Go) {
    $probeSource = Join-Path $Work "tls_probe.go"
    $probeExe = Join-Path $Work "tls_probe.exe"
    [IO.File]::WriteAllText($probeSource, @'
package main

import (
    "crypto/tls"
    "crypto/x509"
    "fmt"
    "io"
    "net/http"
    "os"
)

func main() {
    if len(os.Args) != 3 {
        os.Exit(2)
    }
    pem, err := os.ReadFile(os.Args[1])
    if err != nil {
        fmt.Fprintln(os.Stderr, err)
        os.Exit(1)
    }
    roots := x509.NewCertPool()
    if !roots.AppendCertsFromPEM(pem) {
        fmt.Fprintln(os.Stderr, "invalid CA certificate")
        os.Exit(1)
    }
    client := &http.Client{Transport: &http.Transport{TLSClientConfig: &tls.Config{
        RootCAs: roots, MinVersion: tls.VersionTLS12,
    }}}
    response, err := client.Get(os.Args[2])
    if err != nil {
        fmt.Fprintln(os.Stderr, err)
        os.Exit(1)
    }
    defer response.Body.Close()
    _, _ = io.Copy(io.Discard, response.Body)
    fmt.Print(response.StatusCode)
}
'@, (New-Object Text.UTF8Encoding($false)))
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "SilentlyContinue"
    try {
        & $Go build -o $probeExe $probeSource 2>$null
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $probeExe)) {
            throw "failed to build TLS CA probe"
        }
        for ($i = 0; $i -lt 150; $i++) {
            $status = & $probeExe $CaCert $Url 2>$null
            if ($LASTEXITCODE -eq 0 -and $status -eq "200") {
                return
            }
            Start-Sleep -Milliseconds 200
        }
    } finally {
        $ErrorActionPreference = $oldPreference
    }
    throw "nginx HTTPS CA chain probe did not become healthy: $Url"
}

try {
    if (Test-Path -LiteralPath $Work) {
        Remove-Item -LiteralPath $Work -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $Work | Out-Null
    New-Item -ItemType Directory -Force -Path $Tools | Out-Null

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
    $openssl = $null
    $opensslCommand = Get-Command openssl -ErrorAction SilentlyContinue
    if ($null -ne $opensslCommand) {
        $openssl = $opensslCommand.Source
    } elseif (Test-Path -LiteralPath "C:\Program Files (x86)\Subversion\bin\openssl.exe") {
        $openssl = "C:\Program Files (x86)\Subversion\bin\openssl.exe"
    }
    if (-not $openssl) {
        throw "openssl executable was not found; add OpenSSL to PATH"
    }

    $nginxRoot = Join-Path $Tools "nginx-$NginxVersion"
    $nginxExe = Join-Path $nginxRoot "nginx.exe"
    if (-not (Test-Path -LiteralPath $nginxExe)) {
        $zip = Join-Path $Tools "nginx-$NginxVersion.zip"
        Write-Host "downloading nginx $NginxVersion"
        Invoke-WebRequest -UseBasicParsing -Uri "https://nginx.org/download/nginx-$NginxVersion.zip" -OutFile $zip
        Expand-Archive -LiteralPath $zip -DestinationPath $Tools -Force
    }
    if (-not (Test-Path -LiteralPath $nginxExe)) {
        throw "nginx executable not found: $nginxExe"
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

    $serverPort = Get-FreePort
    $httpPort = Get-FreePort
    $httpsPort = Get-FreePort
    $localBase = "http://127.0.0.1:$serverPort"
    $httpsBase = "https://127.0.0.1:$httpsPort"
    $adminPassword = [Guid]::NewGuid().ToString("N")
    $caCert = Join-Path $Work "ca.crt"
    $caKey = Join-Path $Work "ca.key"
    $cert = Join-Path $Work "server.crt"
    $key = Join-Path $Work "server.key"
    $csr = Join-Path $Work "server.csr"
    $nginxConf = Join-Path $Work "nginx.conf"
    $db = Join-Path $Work "parcel.db"
    $blobs = Join-Path $Work "blobs"

    Write-Host "generating local CA and nginx server certificate"
    $opensslConf = Join-Path $Work "openssl.cnf"
    [IO.File]::WriteAllText($opensslConf, @"
[req]
distinguished_name = dn
prompt = no
[dn]
CN = 127.0.0.1
[v3_ca]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid:always
[v3_server]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth
subjectAltName = @alt
[alt]
DNS.1 = localhost
IP.1 = 127.0.0.1
"@, (New-Object Text.UTF8Encoding($false)))
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "SilentlyContinue"
    & $openssl req -x509 -newkey rsa:2048 -nodes -keyout $caKey -out $caCert -days 2 `
        -config $opensslConf -extensions v3_ca 2>&1 | Out-Null
    $caExit = $LASTEXITCODE
    & $openssl req -new -newkey rsa:2048 -nodes -keyout $key -out $csr `
        -config $opensslConf 2>&1 | Out-Null
    $csrExit = $LASTEXITCODE
    & $openssl x509 -req -in $csr -CA $caCert -CAkey $caKey -CAcreateserial -days 1 `
        -out $cert -extfile $opensslConf -extensions v3_server 2>&1 | Out-Null
    $signExit = $LASTEXITCODE
    & $openssl verify -CAfile $caCert $cert 2>&1 | Out-Null
    $verifyExit = $LASTEXITCODE
    $ErrorActionPreference = $oldPreference
    if ($caExit -ne 0 -or $csrExit -ne 0 -or $signExit -ne 0 -or $verifyExit -ne 0 `
        -or -not (Test-Path -LiteralPath $caCert) -or -not (Test-Path -LiteralPath $cert) `
        -or -not (Test-Path -LiteralPath $key)) {
        throw "openssl certificate chain generation failed"
    }

    $certNginx = $cert.Replace("\", "/")
    $keyNginx = $key.Replace("\", "/")
    $nginxConfig = @"
daemon off;
worker_processes 1;
error_log $($Work.Replace("\", "/"))/nginx-error.log info;
pid $($Work.Replace("\", "/"))/nginx.pid;

events {
    worker_connections 1024;
}

http {
    access_log $($Work.Replace("\", "/"))/nginx-access.log;
    client_max_body_size 4g;
    client_body_timeout 300s;
    proxy_read_timeout 300s;
    proxy_send_timeout 300s;

    upstream parcel_server {
        server 127.0.0.1:$serverPort;
        keepalive 16;
    }

    server {
        listen 127.0.0.1:$httpPort;
        server_name _;
        return 301 https://127.0.0.1:$httpsPort`$request_uri;
    }

    server {
        listen 127.0.0.1:$httpsPort ssl;
        server_name _;
        ssl_certificate $certNginx;
        ssl_certificate_key $keyNginx;
        ssl_protocols TLSv1.2;
        ssl_session_cache shared:SSL:10m;

        location / {
            proxy_pass http://parcel_server;
            proxy_http_version 1.1;
            proxy_set_header Host `$host;
            proxy_set_header X-Real-IP `$remote_addr;
            proxy_set_header X-Forwarded-For `$proxy_add_x_forwarded_for;
            proxy_set_header X-Forwarded-Proto https;
            proxy_request_buffering off;
            proxy_buffering off;
        }
    }
}
"@
    [IO.File]::WriteAllText($nginxConf, $nginxConfig, (New-Object Text.UTF8Encoding($false)))

    $nginxArgs = @("-p", $nginxRoot, "-c", $nginxConf)
    $oldPreference = $ErrorActionPreference
    $ErrorActionPreference = "SilentlyContinue"
    & $nginxExe @nginxArgs -t 2>&1 | Out-Null
    $nginxTestExit = $LASTEXITCODE
    $ErrorActionPreference = $oldPreference
    if ($nginxTestExit -ne 0) {
        throw "nginx configuration test failed"
    }

    Set-ProcessEnv "PARCEL_LISTEN" "127.0.0.1:$serverPort"
    Set-ProcessEnv "PARCEL_DB" $db
    Set-ProcessEnv "PARCEL_BLOB_ROOT" $blobs
    Set-ProcessEnv "PARCEL_PUBLIC_BASE_URL" $httpsBase
    Set-ProcessEnv "PARCEL_ADMIN_USER" "admin"
    Set-ProcessEnv "PARCEL_ADMIN_PASSWORD" $adminPassword
    Set-ProcessEnv "PARCEL_JWT_SECRET" ([Guid]::NewGuid().ToString("N"))
    Set-ProcessEnv "PARCEL_HMAC_KEYS" "primary:nginx-smoke-secret"
    Set-ProcessEnv "PARCEL_HMAC_KEY_ID" "primary"
    Set-ProcessEnv "PARCEL_SECURE_COOKIES" "true"
    Set-ProcessEnv "PARCEL_TRUST_PROXY" "true"
    Set-ProcessEnv "PARCEL_LOG_FORMAT" "text"
    Set-ProcessEnv "PARCEL_LOG_LEVEL" "warn"
    Set-ProcessEnv "PARCEL_CDN_URLS" ""

    Write-Host "starting parcel-server on $localBase"
    $Server = Start-Process -FilePath $ServerExe -WorkingDirectory (Join-Path $Root "server") `
        -PassThru -WindowStyle Hidden -RedirectStandardOutput $ServerLog `
        -RedirectStandardError $ServerErr
    Wait-LocalHealth "$localBase/healthz"

    Write-Host "starting nginx on $httpPort/$httpsPort"
    $Nginx = Start-Process -FilePath $nginxExe -WorkingDirectory $nginxRoot `
        -ArgumentList $nginxArgs -PassThru -WindowStyle Hidden `
        -RedirectStandardOutput $NginxOut -RedirectStandardError $NginxErr
    Wait-HttpsHealth "$httpsBase/healthz" $caCert $go

    $redirect = & curl.exe -sS -o NUL -w "%{http_code} %{redirect_url}" "http://127.0.0.1:$httpPort/healthz"
    if ($LASTEXITCODE -ne 0 -or $redirect -notmatch '^30[1278] ') {
        throw "nginx HTTP redirect failed: $redirect"
    }
    $index = (& curl.exe -k -sS -f "$httpsBase/" | Out-String)
    if ($LASTEXITCODE -ne 0 -or $index -notmatch "PARCEL") {
        throw "nginx did not serve the embedded admin UI"
    }

    $loginPath = Join-Path $Work "login.json"
    $packagePath = Join-Path $Work "package.json"
    $cookies = Join-Path $Work "cookies.txt"
    [IO.File]::WriteAllText($loginPath,
        (@{ username = "admin"; password = $adminPassword } | ConvertTo-Json),
        (New-Object Text.UTF8Encoding($false)))
    [IO.File]::WriteAllText($packagePath, '{"name":"nginx-smoke"}',
        (New-Object Text.UTF8Encoding($false)))
    $login = & curl.exe -k -sS -f -c $cookies -H "Content-Type: application/json" `
        --data-binary "@$loginPath" "$httpsBase/api/v1/admin/login"
    if ($LASTEXITCODE -ne 0) {
        throw "admin login through nginx failed"
    }
    $loginValue = $login | ConvertFrom-Json
    if (-not $loginValue.csrf_token) {
        throw "admin login through nginx did not return CSRF"
    }
    $created = & curl.exe -k -sS -f -b $cookies -H "X-CSRF-Token: $($loginValue.csrf_token)" `
        -H "Content-Type: application/json" --data-binary "@$packagePath" `
        "$httpsBase/api/v1/admin/packages"
    if ($LASTEXITCODE -ne 0) {
        throw "admin package creation through nginx failed"
    }

    Write-Host "nginx TLS smoke passed: nginx/$NginxVersion http=$httpPort https=$httpsPort"
} catch {
    if (Test-Path -LiteralPath $ServerLog) {
        Write-Host "--- server.log ---"
        Get-Content -LiteralPath $ServerLog -Tail 80
    }
    if (Test-Path -LiteralPath $ServerErr) {
        Write-Host "--- server.err.log ---"
        Get-Content -LiteralPath $ServerErr -Tail 80
    }
    if (Test-Path -LiteralPath $NginxErr) {
        Write-Host "--- nginx.err.log ---"
        Get-Content -LiteralPath $NginxErr -Tail 80
    }
    throw
} finally {
    if ($null -ne $Nginx -and -not $Nginx.HasExited) {
        & taskkill.exe /PID $Nginx.Id /T /F 2>$null | Out-Null
    }
    if ($null -ne $Server -and -not $Server.HasExited) {
        Stop-Process -Id $Server.Id -Force -ErrorAction SilentlyContinue
    }
    Restore-ProcessEnv
}
