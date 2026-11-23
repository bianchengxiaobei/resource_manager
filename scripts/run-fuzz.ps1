param(
    [string]$BuildDir = "build/windows-fuzz-libfuzzer",
    [ValidateSet("RelWithDebInfo", "Release")]
    [string]$BuildType = "RelWithDebInfo",
    [int]$Minutes = 30,
    [string]$Target = "parcel_fuzz",
    [string]$ClangCL = "",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$build = Join-Path $root $BuildDir
if ($Minutes -le 0) {
    throw "Minutes must be positive"
}
if (-not $ClangCL) {
    $found = Get-Command clang-cl -ErrorAction SilentlyContinue
    if ($found) {
        $ClangCL = $found.Source
    } else {
        $ClangCL = "C:\Program Files\LLVM\bin\clang-cl.exe"
    }
}
if (-not (Test-Path -LiteralPath $ClangCL)) {
    throw "clang-cl not found; install LLVM or pass -ClangCL <path>"
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "vswhere.exe not found: $vswhere"
}
$vsRoot = (& $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath | Select-Object -First 1)
if (-not $vsRoot) {
    throw "Visual Studio with the C++ toolchain was not found"
}
$vcvars = Join-Path $vsRoot "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "vcvars64.bat not found: $vcvars"
}
$envLines = & cmd.exe /d /c "call `"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) {
    throw "failed to initialize the Visual Studio environment"
}
foreach ($line in $envLines) {
    $separator = $line.IndexOf("=")
    if ($separator -le 0) {
        continue
    }
    [Environment]::SetEnvironmentVariable($line.Substring(0, $separator),
        $line.Substring($separator + 1), "Process")
}

if (-not $SkipBuild) {
    & cmake -S $root -B $build -G Ninja `
        "-DCMAKE_BUILD_TYPE=$BuildType" `
        -DCMAKE_C_COMPILER="$ClangCL" `
        -DCMAKE_CXX_COMPILER="$ClangCL" `
        -DPARCEL_BUILD_TESTS=OFF `
        -DPARCEL_BUILD_FUZZER=ON `
        -DPARCEL_FUZZ_LIBFUZZER=ON `
        -DPARCEL_BUILD_BENCHMARKS=OFF
    if ($LASTEXITCODE -ne 0) {
        throw "libFuzzer configure failed"
    }
    & cmake --build $build --target $Target
    if ($LASTEXITCODE -ne 0) {
        throw "libFuzzer build failed"
    }
}

$exe = Join-Path $build "bin/$BuildType/$Target.exe"
if (-not (Test-Path -LiteralPath $exe)) {
    throw "$Target.exe not found: $exe"
}
$corpus = Join-Path $build "fuzz-corpus"
New-Item -ItemType Directory -Path $corpus -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $root "cpp/fuzz/corpus/asset/minimal.prca") `
    -Destination (Join-Path $corpus "minimal.prca") -Force
Copy-Item -LiteralPath (Join-Path $root "cpp/fuzz/corpus/bundle/minimal.prcl") `
    -Destination (Join-Path $corpus "minimal.prcl") -Force
Copy-Item -LiteralPath (Join-Path $root "cpp/fuzz/corpus/manifest/minimal.prcm") `
    -Destination (Join-Path $corpus "minimal.prcm") -Force
Copy-Item -LiteralPath (Join-Path $root "cpp/fuzz/corpus/sidecar/minimal.json") `
    -Destination (Join-Path $corpus "minimal.json") -Force
$artifacts = Join-Path $build "fuzz-artifacts"
New-Item -ItemType Directory -Path $artifacts -Force | Out-Null
$log = Join-Path $build ("fuzz-" + $Target + ".log")
$runLog = Join-Path $build ("fuzz-" + $Target + ".out.log")
$seconds = $Minutes * 60
$artifactPrefix = $artifacts.TrimEnd('\') + '\'
$arguments = @(
    $corpus,
    "-max_total_time=$seconds",
    "-max_len=1048576",
    "-timeout=10",
    "-rss_limit_mb=2048",
    "-artifact_prefix=$artifactPrefix",
    "-print_final_stats=1"
)
"fuzz start exe=$exe minutes=$Minutes corpus=$corpus log=$log" |
    Tee-Object -FilePath $log
$previousErrorAction = $ErrorActionPreference
$ErrorActionPreference = "Continue"
& $exe @arguments *> $runLog
$exit = $LASTEXITCODE

$ErrorActionPreference = $previousErrorAction
Get-Content -LiteralPath $runLog | Tee-Object -FilePath $log -Append

"fuzz exit=$exit artifacts=$artifacts" | Tee-Object -FilePath $log -Append
if ($exit -ne 0) {
    throw "libFuzzer failed with exit code $exit; see $artifacts"
}

