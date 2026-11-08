param(
    [string]$Config = 'Release',
    [string]$BuildDir = ''
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $repo 'build' }
$binaries = Join-Path $BuildDir $Config
$work = Join-Path $BuildDir ('fps_limit_runtime_run\' + [guid]::NewGuid().ToString('N'))

foreach ($mode in @('160','120','60','min','max','zero','over','negative',
                     'disabled','badbytes','wrongkind')) {
    $dir = Join-Path $work $mode
    $mod = Join-Path $dir 'sdgo\mods\fps_limit'
    New-Item -ItemType Directory -Force -Path $mod | Out-Null
    Copy-Item -LiteralPath (Join-Path $binaries 'fps_limit_runtime_test.exe') -Destination $dir
    Copy-Item -LiteralPath (Join-Path $binaries 'core.dll') -Destination (Join-Path $dir 'sdgo')
    Copy-Item -LiteralPath (Join-Path $binaries 'fps_limit.dll') -Destination $mod

    $manifest = (Get-Content -LiteralPath (Join-Path $repo 'mods\fps_limit\mod.ini') -Raw) `
        -replace '(?m)^hosts=.*$', 'hosts=*'
    [IO.File]::WriteAllText((Join-Path $mod 'mod.ini'), $manifest)
    $global = "[core]`r`nlog_level=0`r`n"
    if ($mode -eq 'disabled') { $global += "[mods]`r`nfps_limit=0`r`n" }
    [IO.File]::WriteAllText((Join-Path $dir 'sdgo\sdgo.ini'), $global)

    $output = & (Join-Path $dir 'fps_limit_runtime_test.exe') $mode 2>&1
    if ($LASTEXITCODE -ne 0 -or ($output -join "`n") -notmatch
        'ALL FPS LIMIT RUNTIME CHECKS PASSED') {
        throw ($output -join "`n")
    }
    $log = [IO.File]::ReadAllText((Join-Path $dir 'sdgo\logs\sdgo.log'))
    $shouldLoad = $mode -notin @('disabled','badbytes','wrongkind')
    if ($shouldLoad) {
        if (-not $log.Contains('loaded Configured FPS limit 1.0.4')) {
            throw "FPS Mod was not active in $mode"
        }
        if (-not $log.Contains('effective FPS cap=')) {
            throw "FPS Mod did not report the effective cap in $mode"
        }
        $effective = if ($mode -in @('60','zero','over','negative')) { 60 } else {
            [int]$mode.Replace('min','1').Replace('max','1000')
        }
        $policy = "applied native FPS timer: render_cap=$effective update_cap=60"
        if (-not $log.Contains($policy)) {
            throw "FPS Mod did not report the separated Render/update policy in $mode"
        }
        if (-not $log.Contains('render pacing active')) {
            throw "FPS Mod did not report native Present pacing in $mode"
        }
        $expectedApplications = 2
        $applications = [regex]::Matches($log, 'applied native FPS timer:').Count
        if ($applications -ne $expectedApplications) {
            throw "FPS Mod applied the Idle cap $applications times in $mode; expected $expectedApplications"
        }
    } elseif ($log.Contains('loaded Configured FPS limit 1.0.4')) {
        throw "Disabled or rejected FPS Mod unexpectedly loaded in $mode"
    }
    Write-Host "$mode : native site, Idle repair, and Present pacing/ABI/observer checks passed"
}

Write-Host "ALL FPS LIMIT RUNTIME CASES PASSED`nReceipt: $work"
