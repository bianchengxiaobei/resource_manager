param(
    [string]$BuildDir = "build/windows-ubsan-ninja",
    [ValidateSet("RelWithDebInfo", "Release")]
    [string]$BuildType = "RelWithDebInfo",
    [string]$ClangCL = "",
    [switch]$SkipRun
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$build = Join-Path $root $BuildDir

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
    $name = $line.Substring(0, $separator)
    $value = $line.Substring($separator + 1)
    [Environment]::SetEnvironmentVariable($name, $value, "Process")
}

& cmake -S $root -B $build -G Ninja `
    "-DCMAKE_BUILD_TYPE=$BuildType" `
    -DCMAKE_C_COMPILER="$ClangCL" `
    -DCMAKE_CXX_COMPILER="$ClangCL" `
    -DPARCEL_BUILD_TESTS=ON `
    -DPARCEL_BUILD_FUZZER=OFF `
    -DPARCEL_BUILD_BENCHMARKS=OFF `
    -DPARCEL_ENABLE_UBSAN=ON
if ($LASTEXITCODE -ne 0) {
    throw "UBSan configure failed"
}

& cmake --build $build --target parcel_tests
if ($LASTEXITCODE -ne 0) {
    throw "UBSan build failed"
}

if (-not $SkipRun) {
    $exe = Join-Path $build "bin/$BuildType/parcel_tests.exe"
    if (-not (Test-Path -LiteralPath $exe)) {
        throw "parcel_tests.exe not found: $exe"
    }
    $env:UBSAN_OPTIONS = "print_stacktrace=1:halt_on_error=1"
    & $exe "~[performance]" --reporter compact
    if ($LASTEXITCODE -ne 0) {
        throw "UBSan test run failed with exit code $LASTEXITCODE"
    }
}


