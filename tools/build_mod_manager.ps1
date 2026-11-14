param([string]$BuildDir='build',[string]$Python='python',[string]$UnitBinary,[string]$CapsuleBinary,[string]$RegistryBinary,[switch]$ServicesOnly)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$build=[IO.Path]::GetFullPath($(if([IO.Path]::IsPathRooted($BuildDir)){$BuildDir}else{Join-Path $repo $BuildDir}))
& $Python (Join-Path $PSScriptRoot 'distribution_guard.py') --source-path $repo
if($LASTEXITCODE -ne 0){throw 'Local-only source cannot enter the public Manager build'}
$services=Join-Path $build 'manager-services'
New-Item -ItemType Directory -Force -Path $services | Out-Null
$compiler=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
if(-not (Test-Path -LiteralPath $compiler)){$compiler=Join-Path $env:WINDIR 'Microsoft.NET\Framework\v4.0.30319\csc.exe'}
$library=Join-Path $services 'SdgoShopServices.dll'
$source=Join-Path $repo 'manager\engines\shop'
$domain=Join-Path $repo 'owner\shop'
$oldCompilerLib=$env:LIB
if($env:LIB){$env:LIB=(($env:LIB -split ';') | Where-Object {$_ -and (Test-Path -LiteralPath $_ -PathType Container)}) -join ';'}
& $compiler /nologo /target:library ("/out:"+$library) /r:System.Data.dll /r:System.Web.Extensions.dll `
    (Join-Path $domain 'Models.cs') (Join-Path $domain 'Repository.cs') (Join-Path $domain 'Service.cs') (Join-Path $source 'ManagerApi.cs')
if($LASTEXITCODE -ne 0){throw 'Manager business assembly build failed'}
& $compiler /nologo /target:exe ("/out:"+(Join-Path $services 'ShopUiTests.exe')) ("/r:"+$library) /r:System.Data.dll /r:System.Web.Extensions.dll (Join-Path $repo 'tests/ShopUiTests.cs')
if($LASTEXITCODE -ne 0){throw 'Manager domain test build failed'}
& $compiler /nologo /target:exe ("/out:"+(Join-Path $services 'ShopChineseRequiredTests.exe')) ("/r:"+$library) /r:System.Data.dll /r:System.Web.Extensions.dll (Join-Path $repo 'tests/ShopChineseRequiredTests.cs')
if($LASTEXITCODE -ne 0){throw 'Manager Chinese product contract test build failed'}
& $compiler /nologo /target:exe ("/out:"+(Join-Path $services 'ShopSearchIndexTests.exe')) ("/r:"+$library) /r:System.Data.dll /r:System.Web.Extensions.dll (Join-Path $repo 'tests/ShopSearchIndexTests.cs')
if($LASTEXITCODE -ne 0){throw 'Manager shop search-index contract test build failed'}
& $compiler /nologo /target:exe ("/out:"+(Join-Path $services 'ShopDefaultPresetTests.exe')) ("/r:"+$library) /r:System.Data.dll /r:System.Web.Extensions.dll (Join-Path $repo 'tests/ShopDefaultPresetTests.cs')
if($LASTEXITCODE -ne 0){throw 'Manager shop default-preset contract test build failed'}
$env:LIB=$oldCompilerLib
& $compiler /nologo /target:exe ("/out:"+(Join-Path $services 'ShopCapacityTests.exe')) ("/r:"+$library) /r:System.Data.dll /r:System.Web.Extensions.dll (Join-Path $repo 'tests/ShopCapacityTests.cs')
if($LASTEXITCODE -ne 0){throw 'Manager shop capacity contract test build failed'}
if($ServicesOnly){Write-Output $services;return}
$oldLibrary=$env:SDGO_MANAGER_SERVICES
$oldCache=$env:PYINSTALLER_CONFIG_DIR
$oldUnitBinary=$env:SDGO_UNIT_CATALOG_BINARY
$oldCapsuleBinary=$env:SDGO_CAPSULE_REGISTRY_BINARY
$oldCollectionSupport=$env:SDGO_COLLECTION_SUPPORT_BINARY
$oldMetadata=$env:SDGO_MANAGER_METADATA
try {
    $metadata=Get-Content -LiteralPath (Join-Path $repo 'manager/repository.json') -Raw | ConvertFrom-Json
    $metadata.manager_version=if($env:SDGO_RELEASE_VERSION){$env:SDGO_RELEASE_VERSION}else{(Get-Content -LiteralPath (Join-Path $repo 'VERSION') -TotalCount 1).Trim()}
    $metadataDir=Join-Path $build 'manager-metadata'
    New-Item -ItemType Directory -Force -Path $metadataDir | Out-Null
    $env:SDGO_MANAGER_METADATA=Join-Path $metadataDir 'repository.json'
    [IO.File]::WriteAllText($env:SDGO_MANAGER_METADATA,($metadata | ConvertTo-Json),[Text.UTF8Encoding]::new($false))
    if(-not $UnitBinary){$UnitBinary=Join-Path $build 'Release\unit_catalog.dll'}
    if(-not (Test-Path -LiteralPath $UnitBinary -PathType Leaf)){throw 'Build unit_catalog.dll before building Manager'}
    $env:SDGO_UNIT_CATALOG_BINARY=[IO.Path]::GetFullPath($UnitBinary)
    if(-not $CapsuleBinary){$CapsuleBinary=Join-Path $build 'Release\capsule_catalog.dll'}
    if(-not (Test-Path -LiteralPath $CapsuleBinary -PathType Leaf)){throw 'Build capsule_registry_support before building Manager'}
    $env:SDGO_CAPSULE_REGISTRY_BINARY=[IO.Path]::GetFullPath($CapsuleBinary)
    if(-not $RegistryBinary){$RegistryBinary=Join-Path $build 'stage\Release\support\capsule-registry\capsule_catalog.dll'}
    if(-not (Test-Path -LiteralPath $RegistryBinary -PathType Leaf)){throw 'Build capsule_registry_support before building Manager'}
    $env:SDGO_COLLECTION_SUPPORT_BINARY=[IO.Path]::GetFullPath($RegistryBinary)
    & $Python -c 'import sys;from pathlib import Path;sys.path.insert(0,sys.argv[1]);from manager.distribution import require_redistributable;require_redistributable(sys.argv[2],Path(sys.argv[2]).read_bytes())' $repo $env:SDGO_COLLECTION_SUPPORT_BINARY
    if($LASTEXITCODE -ne 0){throw 'Collection registry provider cannot enter a public Manager build'}
    $env:SDGO_MANAGER_SERVICES=$library
    $env:PYINSTALLER_CONFIG_DIR=Join-Path $build 'manager-cache'
    & $Python -m PyInstaller --noconfirm --distpath (Join-Path $build 'manager') --workpath (Join-Path $build 'manager-freeze') (Join-Path $repo 'manager\SDGOModManager.spec')
    if($LASTEXITCODE -ne 0){throw 'Manager EXE build failed; see the concrete error above'}
} finally {$env:SDGO_MANAGER_SERVICES=$oldLibrary;$env:PYINSTALLER_CONFIG_DIR=$oldCache;$env:SDGO_UNIT_CATALOG_BINARY=$oldUnitBinary;$env:SDGO_CAPSULE_REGISTRY_BINARY=$oldCapsuleBinary;$env:SDGO_COLLECTION_SUPPORT_BINARY=$oldCollectionSupport;$env:SDGO_MANAGER_METADATA=$oldMetadata}
Get-Item -LiteralPath (Join-Path $build 'manager\SDGO Mod Manager.exe') | Select-Object FullName,Length
