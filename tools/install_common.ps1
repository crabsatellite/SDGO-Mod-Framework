# Shared installer contract. Mod-relative config_files, including bounded
# compiled owner configuration, are preserved on updates and uninstall;
# the package's manifest/code and example files still update normally.
function Get-ModConfigPaths([string]$sourceRoot) {
    $modsRoot = Join-Path $sourceRoot 'sdgo\mods'
    if (-not (Test-Path -LiteralPath $modsRoot)) { return @() }
    $paths = @()
    foreach ($dir in Get-ChildItem -LiteralPath $modsRoot -Directory) {
        $manifest = Join-Path $dir.FullName 'mod.ini'
        if (-not (Test-Path -LiteralPath $manifest)) { continue }
        $mod = Get-IniSection $manifest 'mod'
        if (-not $mod.Contains('config_files')) { continue }
        foreach ($item in ($mod['config_files'] -split ',')) {
            $file = $item.Trim().Replace('/', '\')
            if (-not $file) { continue }
            $parts = @($file -split '\\')
            if ([IO.Path]::IsPathRooted($file) -or $file -match '[\x00-\x1f:*?"<>|]' -or
                @($parts | Where-Object { $_ -in @('', '.', '..') -or $_.EndsWith(' ') -or $_.EndsWith('.') }).Count) {
                throw "Unsafe config_files path in $manifest : $file"
            }
            if ([IO.Path]::GetExtension($file).ToLowerInvariant() -notin @('.ini','.json','.toml','.yaml','.yml','.cfg','.txt','.tsv','.bin') -or
                [IO.Path]::GetFileName($file) -ieq 'mod.ini') {
                throw "Not a user config file in $manifest : $file"
            }
            $paths += 'sdgo\mods\' + $dir.Name + '\' + $file
        }
    }
    $paths
}

function Get-ModConfigBytes([string]$root) {
    $saved = [ordered]@{}
    foreach ($relative in @(Get-ModConfigPaths $root)) {
        $path = Join-Path $root $relative
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $saved[$relative] = [IO.File]::ReadAllBytes($path)
        }
    }
    $saved
}

function Restore-ModConfigBytes([string]$root, [Collections.IDictionary]$saved) {
    foreach ($relative in $saved.Keys) {
        $path = Join-Path $root $relative
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
        [IO.File]::WriteAllBytes($path, [byte[]]$saved[$relative])
    }
}
