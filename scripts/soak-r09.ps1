param(
    [string]$BuildDir = "build/windows-r08-test",
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Debug",
    [int]$Minutes = 60,
    [int]$Seed = 0
)

$ErrorActionPreference = "Stop"

$exe = Join-Path $BuildDir "bin/$Config/parcel_tests.exe"
if (-not (Test-Path -LiteralPath $exe)) {
    throw "parcel_tests.exe not found: $exe"
}

$log = Join-Path (Split-Path -Parent $BuildDir) "soak-r09.log"
$deadline = (Get-Date).AddMinutes($Minutes)
$iteration = 0
$started = Get-Date

"soak start exe=$exe minutes=$Minutes seed=$Seed log=$log" | Tee-Object -FilePath $log
while ((Get-Date) -lt $deadline) {
    ++$iteration
    $iterationSeed = if ($Seed -ne 0) { $Seed + $iteration } else { Get-Random -Minimum 1 -Maximum 2147483647 }
    $line = "iteration=$iteration seed=$iterationSeed at=$((Get-Date).ToString("o"))"
    $line | Tee-Object -FilePath $log -Append

    & $exe "[stress]" --rng-seed $iterationSeed --reporter compact *>&1 |
        Tee-Object -FilePath $log -Append
    if ($LASTEXITCODE -ne 0) {
        throw "soak iteration $iteration failed with exit code $LASTEXITCODE"
    }
}

$elapsed = (Get-Date) - $started
"soak done iterations=$iteration elapsed=$($elapsed.ToString())" |
    Tee-Object -FilePath $log -Append
