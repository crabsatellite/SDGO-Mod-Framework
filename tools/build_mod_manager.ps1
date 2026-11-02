param([string]$BuildDir='build',[string]$Python='python',[string]$UnitBinary,[string]$CapsuleBinary)
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$build=[IO.Path]::GetFullPath($(if([IO.Path]::IsPathRooted($BuildDir)){$BuildDir}else{Join-Path $repo $BuildDir}))
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
$env:LIB=$oldCompilerLib
$oldLibrary=$env:SDGO_MANAGER_SERVICES
$oldCache=$env:PYINSTALLER_CONFIG_DIR
$oldUnitBinary=$env:SDGO_UNIT_CATALOG_BINARY
$oldCapsuleBinary=$env:SDGO_CAPSULE_REGISTRY_BINARY
try {
    if(-not $UnitBinary){$UnitBinary=Join-Path $build 'Release\unit_catalog.dll'}
    if(-not (Test-Path -LiteralPath $UnitBinary -PathType Leaf)){throw 'Build unit_catalog.dll before building Manager'}
    $env:SDGO_UNIT_CATALOG_BINARY=[IO.Path]::GetFullPath($UnitBinary)
    if(-not $CapsuleBinary){$CapsuleBinary=Join-Path $build 'Release\capsule_catalog.dll'}
    if(-not (Test-Path -LiteralPath $CapsuleBinary -PathType Leaf)){throw 'Build capsule_registry_support before building Manager'}
    $env:SDGO_CAPSULE_REGISTRY_BINARY=[IO.Path]::GetFullPath($CapsuleBinary)
    $env:SDGO_MANAGER_SERVICES=$library
    $env:PYINSTALLER_CONFIG_DIR=Join-Path $build 'manager-cache'
    & $Python -m PyInstaller --noconfirm --distpath (Join-Path $build 'manager') --workpath (Join-Path $build 'manager-freeze') (Join-Path $repo 'manager\SDGOModManager.spec')
    if($LASTEXITCODE -ne 0){throw 'Manager EXE build failed; see the concrete error above'}
} finally {$env:SDGO_MANAGER_SERVICES=$oldLibrary;$env:PYINSTALLER_CONFIG_DIR=$oldCache;$env:SDGO_UNIT_CATALOG_BINARY=$oldUnitBinary;$env:SDGO_CAPSULE_REGISTRY_BINARY=$oldCapsuleBinary}
Get-Item -LiteralPath (Join-Path $build 'manager\SDGO Mod Manager.exe') | Select-Object FullName,Length
