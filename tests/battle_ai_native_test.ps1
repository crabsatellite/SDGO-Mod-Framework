param([string]$Config='Release',[string]$BuildDir='build')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$build=[IO.Path]::GetFullPath($(if([IO.Path]::IsPathRooted($BuildDir)){$BuildDir}else{Join-Path $repo $BuildDir}))
$run=Join-Path $build ('battle_ai_native_runs\'+[DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
$bin=Join-Path $build $Config
foreach($case in 'enabled','disabled','bad-signature','missing-script','config-independence','projectile-lead','generic','easy','medium') {
    $dir=Join-Path $run $case
    $mod=Join-Path $dir 'sdgo\mods\battle_ai'
    New-Item -ItemType Directory -Force -Path $mod,(Join-Path $dir 'sdgo\symbols') | Out-Null
    Copy-Item -LiteralPath (Join-Path $bin 'battle_ai_native_test.exe') -Destination $dir
    Copy-Item -LiteralPath (Join-Path $bin 'core.dll') -Destination (Join-Path $dir 'sdgo')
    Copy-Item -LiteralPath (Join-Path $bin 'battle_ai.dll') -Destination $mod
    $manifest=(Get-Content -LiteralPath (Join-Path $repo 'mods\battle_ai\mod.ini') -Raw).Replace('hosts=GOnline-54BE1568-00C97000','hosts=*')
    [IO.File]::WriteAllText((Join-Path $mod 'mod.ini'),$manifest)
    if($case -ne 'missing-script') {
        Copy-Item -LiteralPath (Join-Path $repo 'mods\battle_ai\battle_ai.lua') -Destination $mod
    } else {
        Copy-Item -LiteralPath (Join-Path $repo 'mods\battle_ai\battle_ai.lua') -Destination (Join-Path $mod 'battle_ai.delayed.lua')
    }
    Copy-Item -LiteralPath (Join-Path $repo 'mods\battle_ai\universal_ai.lua') -Destination $mod
    if($case -eq 'config-independence') {
        [IO.File]::AppendAllText(
            (Join-Path $mod 'universal_ai.lua'),
            "`r`nSDGO_BattleAI_ConfigProbe=config`r`n")
    }
    $policy="[settings]`r`n"
    if($case -eq 'generic' -or $case -eq 'projectile-lead') { $policy="[settings]`r`nDifficulty=Hard`r`nSwingCancelChance=100`r`nReactionMs=0`r`nTargetHoldMs=0`r`nBurstMs=0`r`n" }
    if($case -eq 'config-independence') { $policy="[settings]`r`nDifficulty=Hard`r`nLogDecisions=1`r`nTerrainRayAudit=0`r`n" }
    if($case -eq 'easy') { $policy="[settings]`r`nDifficulty=Easy`r`n" }
    if($case -eq 'medium') { $policy="[settings]`r`nDifficulty=Medium`r`nGenericMode=0`r`nReactionMs=3000`r`nDecisionMs=1000`r`nAttackIntervalMs=10000`r`nBurstMs=1`r`nBurstPauseMs=5000`r`nTargetHoldMs=5000`r`nWeaponHoldMs=5000`r`n" }
    [IO.File]::WriteAllText((Join-Path $mod 'config.ini'),$policy)
    $cfg="[core]`r`nlog_level=1`r`n[mods]`r`n"
    if($case -eq 'disabled') { $cfg+="battle_ai=0`r`n" }
    [IO.File]::WriteAllText((Join-Path $dir 'sdgo\sdgo.ini'),$cfg)
    $resultPath=Join-Path $dir 'result.txt'
    & (Join-Path $dir 'battle_ai_native_test.exe') $case *> $resultPath
    if($LASTEXITCODE -ne 0) {
        Get-Content -LiteralPath $resultPath -Tail 60
        throw "Native AI fixture $case failed; $dir"
    }
    Write-Output "PASS native AI fixture $case"
}
Write-Output "ALL NATIVE ADAPTER CASES PASSED: $run"
