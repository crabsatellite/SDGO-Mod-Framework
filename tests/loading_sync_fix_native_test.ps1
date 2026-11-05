param(
    [Parameter(Mandatory = $true)]
    [string]$BattleServerExe,
    [string]$BuildDir = "build",
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$build = [IO.Path]::GetFullPath(
    $(if ([IO.Path]::IsPathRooted($BuildDir)) {
        $BuildDir
    } else {
        Join-Path $repo $BuildDir
    })
)
$binaries = Join-Path $build $Config
$test = Join-Path $binaries "loading_sync_fix_native_test.exe"
$dll = Join-Path $binaries "loading_sync_fix.dll"
$native = [IO.Path]::GetFullPath($BattleServerExe)

foreach ($path in @($test, $dll, $native)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Missing required native-test input: $path"
    }
}

$expectedHash = "F74D16037FC8F4FE49A8041488AABD1948DF1B1A9F2D125DE9DAD4A2347B63A4"
$actualHash = (Get-FileHash -LiteralPath $native -Algorithm SHA256).Hash
if ($actualHash -ne $expectedHash) {
    throw "Unsupported BattleServer.exe SHA-256: $actualHash"
}

Write-Host "== actual loading_sync_fix.dll with read-only shipped BattleServer instructions"
& $test $dll $native
if ($LASTEXITCODE -ne 0) {
    throw "loading_sync_fix native replay failed with exit $LASTEXITCODE"
}
Write-Host "ALL LOADING-SYNC NATIVE REPLAY CASES PASSED"
