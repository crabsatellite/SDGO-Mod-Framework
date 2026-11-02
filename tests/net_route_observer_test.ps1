param([string]$Config='Release')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$binaries=Join-Path $repo ('build\'+$Config)
$root=[IO.Path]::GetFullPath((Join-Path $repo ('build\net-route-observer-runs\'+[Guid]::NewGuid().ToString('N'))))
$buildRoot=[IO.Path]::GetFullPath((Join-Path $repo 'build'))
if(-not $root.StartsWith($buildRoot+'\',[StringComparison]::OrdinalIgnoreCase)){throw 'Fixture escaped build'}
foreach($case in @('active','inactive')) {
    $work=Join-Path $root $case
    $mod=Join-Path $work 'sdgo\mods\net_route'
    New-Item -ItemType Directory -Path $mod -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $binaries 'net_route_observer_test.exe') -Destination $work
    Copy-Item -LiteralPath (Join-Path $binaries 'core.dll') -Destination (Join-Path $work 'sdgo\core.dll')
    Copy-Item -LiteralPath (Join-Path $binaries 'net_route.dll') -Destination $mod
    Copy-Item -LiteralPath (Join-Path $repo 'mods\net_route\mod.ini') -Destination $mod
    Push-Location $work
    try {
        & (Join-Path $work 'net_route_observer_test.exe') $case
        if($LASTEXITCODE){throw ('Production route observer failed: '+$case)}
    }finally{Pop-Location}
}
Write-Output ('Offline production-core observer receipts: '+$root)
