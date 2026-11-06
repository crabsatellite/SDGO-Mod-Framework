param([Parameter(Mandatory=$true)][string]$GameExe,[string]$ServerExe)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$work=Join-Path $repo ('build\battle_ai_install_runs\'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ')+'\game with spaces')
$source=Join-Path $repo 'build\stage\Release\client'
New-Item -ItemType Directory -Force -Path (Join-Path $work 'sdgo\symbols'),(Join-Path $work 'sdgo\mods\net_route'),(Join-Path $work 'sdgo\mods\battle_ai'),(Join-Path $work 'sdgo\mods\match_ai'),(Join-Path $work 'sdgo\mods\ai_catalog') | Out-Null
Copy-Item -LiteralPath $GameExe -Destination (Join-Path $work 'GOnline.exe')
Copy-Item -LiteralPath (Join-Path $source 'sdgo\core.dll') -Destination (Join-Path $work 'sdgo')
$symbols=Join-Path $work 'sdgo\symbols\GOnline-54BE1568-00C97000.ini'
[IO.File]::WriteAllText($symbols,"[host]`r`ntimestamp=0x54BE1568`r`nsize_of_image=0x00C97000`r`n[native.extra]`r`nkind=data`r`nrva=0x123`r`n")
$keep=@{
 'sdgo\mods\net_route\config.ini'="[settings]`nPrivateEndpoint=preserve-this`n";
 'sdgo\mods\battle_ai\config.ini'="[settings]`nStuckMs=2200`n";
 'sdgo\sdgo.ini'="[mods]`nnet_route=1`nmission_ai=1`n";
}
foreach($key in $keep.Keys) { [IO.File]::WriteAllText((Join-Path $work $key),$keep[$key]) }
[IO.File]::WriteAllText((Join-Path $work 'sdgo\mods\match_ai\mod.ini'),"[mod]`nversion=old`n[SETTINGS]`nLogDiagnostics=1`n[private.extra]`nValue=preserved`n")
[IO.File]::WriteAllText((Join-Path $work 'sdgo\mods\ai_catalog\mod.ini'),"[mod]`nid=ai_catalog`nversion=old`nenabled=1`n")
[IO.File]::WriteAllText((Join-Path $work 'sdgo\mods\ai_catalog\catalog.bin'),'owner-catalog-preserved')
for($i=0;$i -lt 2;$i++) { & (Join-Path $repo 'tools\install_battle_ai.ps1') -GameDir $work -Source $source }
foreach($key in $keep.Keys) {
    if([IO.File]::ReadAllText((Join-Path $work $key)) -cne $keep[$key]) { throw "Installer overwrote $key" }
}
$readback=[IO.File]::ReadAllText($symbols)
foreach($name in 'npc.think','npc.init_ai','native.extra') {
    if([regex]::Matches($readback,'(?m)^\['+[regex]::Escape($name)+'\]').Count -ne 1) { throw "Symbol merge failed $name" }
}
if([IO.File]::ReadAllBytes((Join-Path $work 'sdgo\mods\battle_ai\battle_ai.dll')).Length -eq 0) { throw 'No DLL installed' }
$match=[IO.File]::ReadAllText((Join-Path $work 'sdgo\mods\match_ai\mod.ini'))
if(-not $match.Contains('LogDiagnostics=1') -or -not $match.Contains('[private.extra]') -or $match.Contains('version=old')) { throw 'Match AI manifest/user-section preservation failed' }
if(-not (Test-Path -LiteralPath (Join-Path $work 'sdgo\mods\match_ai\match_ai.dll'))) { throw 'Matching loadout bridge missing' }
foreach($id in 'battle_ai','match_ai','mission_ai','ai_catalog') {
    $management=Get-Content -LiteralPath (Join-Path $work "sdgo\mods\$id\manager.json") -Raw -Encoding UTF8 | ConvertFrom-Json
    if($management.id -ne $id) { throw "Missing or wrong Manager registration for $id" }
}
if([IO.File]::ReadAllText((Join-Path $work 'sdgo\mods\ai_catalog\catalog.bin')) -ne 'owner-catalog-preserved' -or
   [IO.File]::ReadAllText((Join-Path $work 'sdgo\mods\ai_catalog\mod.ini')) -notmatch '(?m)^enabled=1\s*$') { throw 'Catalog code update changed owner data or activation' }
if(-not (Test-Path -LiteralPath (Join-Path $work 'sdgo\mods\mission_ai\mission_ai.lua'))) { throw 'Mission search/transition adapter missing' }
Write-Output "PASS additive, idempotent install preserves separate config and unrelated network settings: $work"
foreach($relative in 'd3d9.dll','sdgo\core.dll') {
    if((Get-FileHash -LiteralPath (Join-Path $work $relative)).Hash -ne (Get-FileHash -LiteralPath (Join-Path $source $relative)).Hash) { throw "Matching framework not installed: $relative" }
}
$loader=Join-Path $work 'd3d9.dll'
$loaderBytes=[IO.File]::ReadAllBytes($loader)
$modBefore=(Get-FileHash -LiteralPath (Join-Path $work 'sdgo\mods\battle_ai\battle_ai.dll')).Hash
try {
    [IO.File]::WriteAllText($loader,'foreign-loader-test')
    $rejected=$false
    try { & (Join-Path $repo 'tools\install_battle_ai.ps1') -GameDir $work -Source $source } catch { $rejected=$true }
    if(-not $rejected -or [IO.File]::ReadAllText($loader) -ne 'foreign-loader-test' -or
       (Get-FileHash -LiteralPath (Join-Path $work 'sdgo\mods\battle_ai\battle_ai.dll')).Hash -ne $modBefore) { throw 'Foreign loader rejection did not preserve the install' }
} finally { [IO.File]::WriteAllBytes($loader,$loaderBytes) }
Write-Output 'PASS foreign loader rejected before framework or AI files are changed'
$framework=Join-Path (Split-Path -Parent $work) 'framework template'
New-Item -ItemType Directory -Force -Path (Join-Path $framework 'sdgo\mods\battle_ai') | Out-Null
Copy-Item -LiteralPath (Join-Path $source 'sdgo\core.dll') -Destination (Join-Path $framework 'sdgo')
$frameworkConfig=Join-Path $framework 'sdgo\mods\battle_ai\config.ini'
[IO.File]::WriteAllText($frameworkConfig,"[settings]`nDifficulty=Easy`n")
& (Join-Path $repo 'tools\install_battle_ai.ps1') -GameDir $framework -Source $source -FrameworkOnly
if((Test-Path -LiteralPath (Join-Path $framework 'GOnline.exe')) -or
   -not (Test-Path -LiteralPath (Join-Path $framework 'sdgo\mods\match_ai\match_ai.dll')) -or
   [IO.File]::ReadAllText($frameworkConfig) -cne "[settings]`nDifficulty=Easy`n") { throw 'Framework-only install did not preserve its template boundary/settings' }
$rejected=$false
try { & (Join-Path $repo 'tools\install_battle_ai.ps1') -GameDir $work -Source $source -FrameworkOnly } catch { $rejected=$true }
if(-not $rejected) { throw 'Framework-only mode accepted a live game layout' }
Write-Output 'PASS explicit framework template install preserves settings and rejects game layouts'
$scoped=Join-Path (Split-Path -Parent $work) 'paired combat template'
New-Item -ItemType Directory -Force -Path (Join-Path $scoped 'sdgo\mods\ai_catalog'),(Join-Path $scoped 'sdgo\mods\mission_ai') | Out-Null
Copy-Item -LiteralPath (Join-Path $source 'sdgo\core.dll') -Destination (Join-Path $scoped 'sdgo')
foreach($id in 'ai_catalog','mission_ai') {
    [IO.File]::WriteAllText((Join-Path $scoped "sdgo\mods\$id\mod.ini"),'owner-specific-manifest')
    [IO.File]::WriteAllText((Join-Path $scoped "sdgo\mods\$id\$id.dll"),'owner-specific-runtime')
}
& (Join-Path $repo 'tools\install_battle_ai.ps1') -GameDir $scoped -Source $source -FrameworkOnly -PvpCombatOnly
foreach($id in 'ai_catalog','mission_ai') {
    if([IO.File]::ReadAllText((Join-Path $scoped "sdgo\mods\$id\mod.ini")) -cne 'owner-specific-manifest' -or
       [IO.File]::ReadAllText((Join-Path $scoped "sdgo\mods\$id\$id.dll")) -cne 'owner-specific-runtime') { throw 'Paired combat install replaced an unrelated AI component' }
}
Write-Output 'PASS paired PvP combat install preserves mission and catalog runtimes'
if($ServerExe) {
    $hostWork=Join-Path (Split-Path -Parent $work) 'server with spaces'
    foreach($dir in 'sdgo\symbols','sdgo\mods\udp_pool','sdgo\mods\pvp_ai') { New-Item -ItemType Directory -Force -Path (Join-Path $hostWork $dir) | Out-Null }
    Copy-Item -LiteralPath $ServerExe -Destination (Join-Path $hostWork 'BattleServer.exe')
    Copy-Item -LiteralPath (Join-Path $source 'sdgo\core.dll') -Destination (Join-Path $hostWork 'sdgo')
    $hostSymbols=Join-Path $hostWork 'sdgo\symbols\BattleServer-548A891D-0046C000.ini'
    [IO.File]::WriteAllText($hostSymbols,"[host]`ntimestamp=0x548A891D`nsize_of_image=0x0046C000`n[private.server.symbol]`nkind=data`nrva=0x123`n")
    $hostKeep=@{'BattleServer.ini'="[private.sql]`nDoNotModify=1`n";'sdgo\sdgo.ini'="[mods]`nudp_pool=1`n";'sdgo\mods\udp_pool\config.ini'="[settings]`nPorts=preserve`n"}
    foreach($key in $hostKeep.Keys) { [IO.File]::WriteAllText((Join-Path $hostWork $key),$hostKeep[$key]) }
    [IO.File]::WriteAllText((Join-Path $hostWork 'sdgo\mods\pvp_ai\mod.ini'),"[mod]`nversion=old`n[settings]`nDefaultLevel=4`nLevel4=3604001`n")
    $hostSource=Join-Path $repo 'build\stage\Release\server'
    for($i=0;$i -lt 2;$i++) { & (Join-Path $repo 'tools\install_battle_ai.ps1') -GameDir $hostWork -Source $hostSource -Side Server }
    foreach($key in $hostKeep.Keys) { if([IO.File]::ReadAllText((Join-Path $hostWork $key)) -cne $hostKeep[$key]) { throw "Host installer overwrote $key" } }
    $hostManifest=[IO.File]::ReadAllText((Join-Path $hostWork 'sdgo\mods\pvp_ai\mod.ini'))
    if(-not $hostManifest.Contains('DefaultLevel=4') -or $hostManifest.Contains('version=old')) { throw 'Host manifest/settings preservation failed' }
    if(-not (Test-Path -LiteralPath (Join-Path $hostWork 'sdgo\mods\battle_ai_server\battle_ai_server.dll'))) { throw 'Host fairness component missing' }
    if(-not ([IO.File]::ReadAllText($hostSymbols)).Contains('[private.server.symbol]')) { throw 'Unrelated host symbol missing' }
    Write-Output "PASS host install preserves SQL configuration, UDP pool and NPC pool settings: $hostWork"
    foreach($relative in 'winmm.dll','sdgo\core.dll') {
        if((Get-FileHash -LiteralPath (Join-Path $hostWork $relative)).Hash -ne (Get-FileHash -LiteralPath (Join-Path $hostSource $relative)).Hash) { throw "Matching server framework not installed: $relative" }
    }
}
