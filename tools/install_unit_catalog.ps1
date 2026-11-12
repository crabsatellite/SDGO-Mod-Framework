<#
.SYNOPSIS
  Installs or removes a confirmed SDGO unit-catalog client package.

.DESCRIPTION
  The package is fail-closed and content addressed. Its root manifest.json
  hashes every package file (manifest.json itself is the sole exception).
  Files below package client\ are mapped below -ClientRoot. Loose resources
  may only use the manifest's owned_paths and the known SDGO resource roots.

  A foreign file is never overwritten. A byte-identical foreign file is
  reused without taking ownership. Later updates may replace only files that
  this installer created and whose current hash still matches its receipt.
  Uninstall removes only those owned files and the exact symbol section this
  installer appended.
#>
param(
    [Parameter(Mandatory = $true)]
    [Alias("GameDir")]
    [string]$ClientRoot,
    [string]$Source,
    [switch]$Uninstall
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($Source)) { $Source = $PSScriptRoot }

$Module = "unit_catalog"
$ReceiptRelative = "sdgo\mods\unit_catalog\install-receipt.json"
$ExpectedHost = "GOnline-54BE1568-00C97000"
$ExpectedHostSha256 = "e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92"
$RequiredModuleFiles = @(
    "sdgo\mods\unit_catalog\unit_catalog.dll",
    "sdgo\mods\unit_catalog\mod.ini",
    "sdgo\mods\unit_catalog\catalog.bin",
    "sdgo\mods\unit_catalog\definitions.json",
    "sdgo\mods\unit_catalog\resource-overlay.ini",
    "sdgo\mods\unit_catalog\resources.zdx",
    "sdgo\mods\unit_catalog\resources.zpk"
)
$AllowedModuleFiles = $RequiredModuleFiles + @(
    'sdgo\mods\unit_catalog\manager.json',
    'sdgo\mods\unit_catalog\symbols.fragment.ini',
    # Optional availability data generated with a unit publication. Older
    # packages without these files remain installable.
    'sdgo\mods\unit_catalog\blueprint-availability.json',
    'sdgo\mods\unit_catalog\blueprint-availability.bin',
    # Optional sealed sidecars: native Lua program bundle (SDGOPR1) and skill atlases (SDGOAT1).
    'sdgo\mods\unit_catalog\native-programs.bin',
    'sdgo\mods\unit_catalog\skill-atlases.bin'
) + @(1..31 | ForEach-Object { "sdgo\mods\unit_catalog\resources.$_.zpk" })  # split archive parts
$AllowedResourceRoots = @("anrs", "fxrs", "mdrs", "scp", "sdrs", "sol", "sprs", "trns", "txrs", "uirs")
$BlockedResourceExtensions = @(".bat", ".cmd", ".com", ".dll", ".exe", ".js", ".lnk", ".msi", ".ps1", ".scr", ".vbs")
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

function Get-NormalizedRelativePath([string]$Value) {
    if ([string]::IsNullOrWhiteSpace($Value)) { throw "Empty relative path in manifest" }
    $relative = $Value.Replace("/", "\").Trim()
    if ([IO.Path]::IsPathRooted($relative) -or $relative -match '[\x00-\x1f:*?"<>|]') {
        throw "Unsafe relative path: $Value"
    }
    $parts = @($relative -split "\\")
    if (@($parts | Where-Object {
        $_ -in @("", ".", "..") -or $_.EndsWith(" ") -or $_.EndsWith(".")
    }).Count) {
        throw "Unsafe relative path segment: $Value"
    }
    return $relative
}

function Assert-ResourceOverlay($Copies, $OverlayPaths) {
    # $OverlayPaths is the exact set of archive entries the manifest declares
    # (resource_overlay.paths); older packages declare them through owned_paths,
    # where every entry was also shipped as a loose copy the game never reads.
    $base = 'sdgo\mods\unit_catalog\'
    $config = [IO.File]::ReadAllText($Copies[$base + 'resource-overlay.ini'].source)
    $section = Get-IniSection $config 'runtime'
    $values = @{}
    foreach ($line in ($section -split "`r?`n")) {
        if ($line -match '^([a-z_0-9]+)=(.*)$') {
            if ($values.ContainsKey($Matches[1])) { throw 'Duplicate resource overlay setting' }
            $values[$Matches[1]] = $Matches[2].Trim()
        }
    }
    if ($values.schema -cne '1' -or $values.index -cne 'resources.zdx' -or
        $values.archive -cne 'resources.zpk') { throw 'Unsupported unit resource overlay configuration' }
    # Optional split archive: parts=N (2..32), part_chunks=C; part k is resources.<k>.zpk and
    # archive_sha256 covers all parts in order.
    $parts = 1; $partChunks = 0
    if ($values.ContainsKey('parts') -or $values.ContainsKey('part_chunks')) {
        if ($values.parts -notmatch '^[0-9]{1,2}$' -or $values.part_chunks -notmatch '^[1-9][0-9]{0,8}$') { throw 'Invalid resource overlay archive parts' }
        $parts = [int]$values.parts; $partChunks = [uint64]$values.part_chunks
        if ($parts -lt 2 -or $parts -gt 32 -or $partChunks * 520 -gt 2048MB) { throw 'Invalid resource overlay archive parts' }
    }
    $partNames = @('resources.zpk')
    if ($parts -gt 1) { $partNames += @(1..($parts - 1) | ForEach-Object { "resources.$_.zpk" }) }
    foreach ($name in $partNames) { if (-not $Copies.ContainsKey($base + $name)) { throw "Resource overlay archive part is missing: $name" } }
    for ($k = $parts; $k -le 31; $k++) { if ($Copies.ContainsKey($base + "resources.$k.zpk")) { throw 'Resource overlay ships an undeclared archive part' } }
    if ($values.index_sha256 -cne $Copies[$base + 'resources.zdx'].sha256) { throw 'Resource overlay seal differs from package hash map' }
    $hasher = [Security.Cryptography.IncrementalHash]::CreateHash([Security.Cryptography.HashAlgorithmName]::SHA256)
    $partSizes = @()
    foreach ($name in $partNames) {
        $stream = [IO.File]::OpenRead($Copies[$base + $name].source)
        try {
            $buffer = New-Object byte[] (1MB)
            while (($read = $stream.Read($buffer, 0, $buffer.Length)) -gt 0) { $hasher.AppendData($buffer, 0, $read) }
            $partSizes += [uint64]$stream.Length
        } finally { $stream.Dispose() }
    }
    $archiveHash = ([BitConverter]::ToString($hasher.GetHashAndReset())).Replace('-', '').ToLowerInvariant()
    $hasher.Dispose()
    if ($values.archive_sha256 -cne $archiveHash) { throw 'Resource overlay seal differs from package hash map' }
    $bytes = [IO.File]::ReadAllBytes($Copies[$base + 'resources.zdx'].source)
    if ($bytes.Length -lt 24 -or $bytes.Length -gt 64MB) { throw 'Invalid resource overlay index size' }
    $version = [BitConverter]::ToUInt32($bytes, 0)
    $chunkSize = [BitConverter]::ToUInt32($bytes, 4)
    $chunkCount = [BitConverter]::ToUInt32($bytes, 8)
    $count = [BitConverter]::ToUInt32($bytes, 16)
    $archiveSize = [uint64]0; foreach ($size in $partSizes) { $archiveSize += $size }
    if ($parts -gt 1) {
        for ($k = 0; $k -lt $parts - 1; $k++) { if ($partSizes[$k] -ne $partChunks * 520) { throw 'Resource overlay archive part size differs from part_chunks' } }
        if ($partSizes[$parts - 1] -lt 1 -or $partSizes[$parts - 1] -gt $partChunks * 520) { throw 'Resource overlay archive part size differs from part_chunks' }
    }
    if ($version -ne 1 -or $chunkSize -ne 512 -or $count -lt 1 -or $count -gt 100000 -or
        $archiveSize -gt 2048MB -or [uint64]$chunkCount * 520 -ne $archiveSize) { throw 'Invalid resource overlay archive layout' }
    $cursor = 24
    $entries = @{}
    for ($i = 0; $i -lt $count; $i++) {
        $name = [Text.StringBuilder]::new()
        $terminated = $false
        while ($cursor + 2 -le $bytes.Length -and $name.Length -le 240) {
            $letter = [BitConverter]::ToUInt16($bytes, $cursor) -bxor 0xAA
            $cursor += 2
            if ($letter -eq 0) { $terminated = $true; break }
            if ($letter -lt 32 -or $letter -gt 126) { throw 'Non-ASCII resource overlay path' }
            $null = $name.Append([char]$letter)
        }
        if (-not $terminated -or $name.Length -gt 240 -or $cursor + 36 -gt $bytes.Length) { throw 'Truncated resource overlay entry' }
        $path = Get-NormalizedRelativePath $name.ToString()
        if ($path -cne $path.ToLowerInvariant() -or $entries.ContainsKey($path) -or -not $OverlayPaths.ContainsKey($path)) {
            throw 'Resource overlay paths differ from the declared overlay paths'
        }
        $root = ($path -split '\\', 2)[0]
        $extensions = @{mdrs='.mod';anrs='.ani';txrs='.txr';fxrs='.zfx';sprs='.spr';sdrs='.zsd';scp='.scp'}
        if (-not $extensions.ContainsKey($root) -or [IO.Path]::GetExtension($path) -cne $extensions[$root]) { throw 'Resource overlay entry is outside the unit domain' }
        $stored = [BitConverter]::ToUInt32($bytes, $cursor)
        $first = [BitConverter]::ToUInt32($bytes, $cursor + 4)
        $chunks = [BitConverter]::ToUInt32($bytes, $cursor + 8)
        if ($stored -lt 1 -or $stored -gt 64MB -or $chunks -lt 1 -or
            [uint64]$first + $chunks -gt $chunkCount -or [uint64]$chunks * 512 -lt $stored) { throw 'Resource overlay chunk range is invalid' }
        if ($partChunks -gt 0 -and [math]::Floor($first / $partChunks) -ne [math]::Floor(([uint64]$first + $chunks - 1) / $partChunks)) {
            throw 'Resource overlay entry crosses an archive part'
        }
        $cursor += 36
        $entries[$path] = $true
    }
    if ($cursor -ne $bytes.Length -or $entries.Count -ne $OverlayPaths.Count) { throw 'Resource overlay does not cover the exact declared resource set' }
}

function Get-PathWithin([string]$Root, [string]$Relative) {
    $fullRoot = [IO.Path]::GetFullPath($Root).TrimEnd("\")
    $full = [IO.Path]::GetFullPath((Join-Path $fullRoot $Relative))
    if (-not $full.StartsWith($fullRoot + "\", [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path leaves its root: $Relative"
    }
    return $full
}

function Get-Sha256([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-HostKey([string]$Exe) {
    $stream = [IO.File]::OpenRead($Exe)
    try {
        $reader = New-Object IO.BinaryReader($stream)
        $stream.Position = 0x3C
        $nt = $reader.ReadInt32()
        $stream.Position = $nt
        if ($reader.ReadUInt32() -ne 0x4550) { throw "$Exe is not a Windows PE executable" }
        $stream.Position = $nt + 8
        $timestamp = $reader.ReadUInt32()
        $stream.Position = $nt + 0x18 + 0x38
        $size = $reader.ReadUInt32()
        return "{0}-{1:X8}-{2:X8}" -f [IO.Path]::GetFileNameWithoutExtension($Exe), $timestamp, $size
    } finally {
        $stream.Dispose()
    }
}

function Test-NativeExport([string]$Path, [string]$Name) {
    try {
        $data = [IO.File]::ReadAllBytes($Path)
        if ($data.Length -lt 256 -or [BitConverter]::ToUInt16($data, 0) -ne 0x5A4D) { return $false }
        $pe = [BitConverter]::ToInt32($data, 0x3C)
        if ($pe -lt 0 -or $pe + 24 -gt $data.Length -or [BitConverter]::ToUInt32($data, $pe) -ne 0x4550) { return $false }
        $opt = $pe + 24
        $optSize = [BitConverter]::ToUInt16($data, $pe + 20)
        $sectionCount = [BitConverter]::ToUInt16($data, $pe + 6)
        if ($optSize -lt 104 -or $sectionCount -gt 96 -or $opt + $optSize + $sectionCount * 40 -gt $data.Length -or
            [BitConverter]::ToUInt16($data, $opt) -ne 0x10B) { return $false }
        $headers = [BitConverter]::ToUInt32($data, $opt + 60)
        $sections = @()
        for ($i = 0; $i -lt $sectionCount; $i++) {
            $at = $opt + $optSize + $i * 40
            $sections += @{rva=[BitConverter]::ToUInt32($data,$at+12);size=[BitConverter]::ToUInt32($data,$at+16);offset=[BitConverter]::ToUInt32($data,$at+20)}
        }
        function Resolve-ExportRva([uint32]$Rva, [uint32]$Bytes) {
            if ($Rva -lt $headers -and [uint64]$Rva + $Bytes -le $data.Length) { return [int]$Rva }
            foreach ($section in $sections) {
                if ($Rva -ge $section.rva -and [uint64]$Rva - $section.rva + $Bytes -le $section.size) {
                    $offset = [uint64]$section.offset + $Rva - $section.rva
                    if ($offset + $Bytes -le $data.Length) { return [int]$offset }
                }
            }
            throw 'Unmapped export RVA'
        }
        $directoryRva = [BitConverter]::ToUInt32($data, $opt + 96)
        if (-not $directoryRva) { return $false }
        $directory = Resolve-ExportRva $directoryRva 40
        $count = [BitConverter]::ToUInt32($data, $directory + 24)
        if ($count -lt 1 -or $count -gt 4096) { return $false }
        $names = Resolve-ExportRva ([BitConverter]::ToUInt32($data, $directory + 32)) ($count * 4)
        for ($i = 0; $i -lt $count; $i++) {
            $text = Resolve-ExportRva ([BitConverter]::ToUInt32($data, $names + $i * 4)) 1
            $end = $text
            while ($end -lt $data.Length -and $end - $text -lt 256 -and $data[$end] -ne 0) { $end++ }
            if ($end -lt $data.Length -and $data[$end] -eq 0 -and
                [Text.Encoding]::ASCII.GetString($data, $text, $end - $text) -ceq $Name) { return $true }
        }
    } catch { return $false }
    return $false
}

function Assert-ClientStopped([string]$Exe) {
    $running = Get-CimInstance Win32_Process -Filter "Name='GOnline.exe'" |
        Where-Object { $_.ExecutablePath -and
            [IO.Path]::GetFullPath($_.ExecutablePath) -ieq [IO.Path]::GetFullPath($Exe) }
    if ($running) { throw "GOnline.exe in $ClientRoot is running; close this client copy first" }
}

function Assert-Host([string]$Exe, [string]$ExpectedKey, [string]$Sha256) {
    if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
        throw "$ClientRoot does not contain GOnline.exe"
    }
    $actualHost = Get-HostKey $Exe
    $actualHash = Get-Sha256 $Exe
    if ($actualHost -cne $ExpectedKey -or $actualHash -cne $Sha256.ToLowerInvariant()) {
        throw "Unsupported client build: $actualHost SHA256 $actualHash"
    }
}

function Get-IniSection([string]$Text, [string]$Name) {
    $escaped = [Regex]::Escape($Name)
    $match = [Regex]::Match($Text, "(?ms)^\[$escaped\][^\r\n]*(?:\r?\n.*?)*?(?=^\[|\z)")
    if (-not $match.Success) { return $null }
    return $match.Value.TrimEnd("`r", "`n")
}

function Get-NormalizedSection([string]$Text) {
    if ($null -eq $Text) { return $null }
    $name = $null; $values = @{}
    foreach ($raw in ($Text -split "`r?`n")) {
        $line = $raw.Trim()
        if (-not $line -or $line.StartsWith(';') -or $line.StartsWith('#')) { continue }
        if ($line -match '^\[([^\]]+)\]\s*(?:[;#].*)?$') {
            if ($null -ne $name) { throw 'Symbol comparison expected one section' }
            $name = $Matches[1].Trim().ToLowerInvariant()
            continue
        }
        if ($null -eq $name -or $line -notmatch '^([^=]+)=(.*)$') { throw 'Invalid symbol section' }
        $key = $Matches[1].Trim().ToLowerInvariant()
        if (-not $key -or $values.ContainsKey($key)) { throw 'Duplicate or empty symbol key' }
        $values[$key] = $Matches[2].Trim()
    }
    if ($null -eq $name) { throw 'Missing symbol section header' }
    return ('[' + $name + "]`n" + (($values.Keys | Sort-Object | ForEach-Object { $_ + '=' + $values[$_] }) -join "`n"))
}

function Remove-IniSection([string]$Text, [string]$Name) {
    $escaped = [Regex]::Escape($Name)
    return [Regex]::Replace($Text, "(?ms)^\[$escaped\][^\r\n]*(?:\r?\n.*?)*?(?=^\[|\z)", "")
}

function Assert-CatalogHeader([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        if ($stream.Length -lt 32) { throw "catalog.bin has a short header" }
        $reader = New-Object IO.BinaryReader($stream)
        $magic = [Text.Encoding]::ASCII.GetString($reader.ReadBytes(8)).TrimEnd([char]0)
        $version = $reader.ReadUInt32()
        $headerBytes = $reader.ReadUInt32()
        $count = $reader.ReadUInt32()
        $payloadBytes = $reader.ReadUInt32()
        [void]$reader.ReadUInt32()
        $reserved = $reader.ReadUInt32()
        if ($magic -cne "SDGOUN1" -or $version -ne 1 -or $headerBytes -ne 32 -or
            $count -lt 1 -or $count -gt 8192 -or $payloadBytes -gt 67108864 -or
            $reserved -ne 0 -or $stream.Length -ne 32 + $payloadBytes) {
            throw "catalog.bin header is unsupported or inconsistent"
        }
    } finally {
        $stream.Dispose()
    }
}

function Read-Package([string]$Root) {
    $manifestPath = Join-Path $Root "manifest.json"
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
        throw "$Root has no manifest.json"
    }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.module -cne $Module -or [int]$manifest.schema_version -ne 1) {
        throw "manifest.json is not a unit_catalog schema 1 package"
    }
    if ([string]::IsNullOrWhiteSpace([string]$manifest.revision) -or
        ([string]$manifest.revision).Length -gt 160) {
        throw "manifest.json has no bounded revision"
    }
    if ($manifest.host -cne $ExpectedHost -or
        ([string]$manifest.host_sha256).ToLowerInvariant() -cne $ExpectedHostSha256) {
        throw "manifest.json targets a different client build"
    }
    if ($null -eq $manifest.files) { throw "manifest.json has no files hash whitelist" }

    $hashes = @{}
    foreach ($property in $manifest.files.PSObject.Properties) {
        $relative = Get-NormalizedRelativePath ([string]$property.Name)
        if ($hashes.ContainsKey($relative)) { throw "Duplicate manifest path: $relative" }
        $hash = ([string]$property.Value).ToLowerInvariant()
        if ($hash -notmatch '^[0-9a-f]{64}$') { throw "Invalid SHA256 for $relative" }
        $path = Get-PathWithin $Root $relative
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing package file: $relative" }
        $actual = Get-Sha256 $path
        if ($actual -cne $hash) { throw "Package hash mismatch: $relative" }
        $hashes[$relative] = $hash
    }

    # The hash map is an allowlist, not only a list of files to copy. The
    # manifest cannot hash itself without recursion and is the sole exception.
    foreach ($file in Get-ChildItem -LiteralPath $Root -Recurse -File) {
        $relative = Get-NormalizedRelativePath $file.FullName.Substring($Root.Length).TrimStart("\")
        if ($relative -ieq "manifest.json") { continue }
        if (-not $hashes.ContainsKey($relative)) { throw "Unlisted package file: $relative" }
    }

    $ownedPaths = @{}
    foreach ($raw in @($manifest.owned_paths)) {
        $relative = Get-NormalizedRelativePath ([string]$raw)
        if ($ownedPaths.ContainsKey($relative)) { throw "Duplicate owned_paths entry: $relative" }
        $ownedPaths[$relative] = $true
    }

    $copy = @()
    $copyByTarget = @{}
    foreach ($relative in $hashes.Keys) {
        if (-not $relative.StartsWith("client\", [StringComparison]::OrdinalIgnoreCase)) { continue }
        $targetRelative = Get-NormalizedRelativePath $relative.Substring(7)
        if ($copyByTarget.ContainsKey($targetRelative)) { throw "Duplicate client target: $targetRelative" }

        $isModuleFile = $targetRelative.StartsWith("sdgo\mods\unit_catalog\", [StringComparison]::OrdinalIgnoreCase)
        if ($isModuleFile) {
            if ($AllowedModuleFiles -notcontains $targetRelative) {
                throw "Unexpected file in unit_catalog runtime directory: $targetRelative"
            }
        } else {
            $rootName = ($targetRelative -split "\\", 2)[0]
            if ($AllowedResourceRoots -notcontains $rootName.ToLowerInvariant()) {
                throw "Loose resource root is not allowed: $targetRelative"
            }
            if ([IO.Path]::GetExtension($targetRelative).ToLowerInvariant() -in $BlockedResourceExtensions) {
                throw "Executable loose resource is not allowed: $targetRelative"
            }
            if (-not $ownedPaths.ContainsKey($targetRelative)) {
                throw "Loose resource is absent from owned_paths: $targetRelative"
            }
        }
        $entry = [pscustomobject]@{
            source_relative = $relative
            source = Get-PathWithin $Root $relative
            target_relative = $targetRelative
            target = Get-PathWithin $script:ResolvedClientRoot $targetRelative
            sha256 = $hashes[$relative]
        }
        $copy += $entry
        $copyByTarget[$targetRelative] = $entry
    }
    foreach ($required in $RequiredModuleFiles) {
        if (-not $copyByTarget.ContainsKey($required)) { throw "Package is missing $required" }
    }
    foreach ($owned in $ownedPaths.Keys) {
        if (-not $copyByTarget.ContainsKey($owned) -or
            $owned.StartsWith("sdgo\mods\unit_catalog\", [StringComparison]::OrdinalIgnoreCase)) {
            throw "owned_paths is not an exact loose-resource target: $owned"
        }
    }
    # An overlay-only package lists its archive entries in resource_overlay.paths and
    # ships no loose copies; a package without that list keeps the owned_paths contract.
    $overlayPaths = $ownedPaths
    $overlayInfo = $manifest.resource_overlay
    if ($null -ne $overlayInfo -and ($overlayInfo.PSObject.Properties.Name -contains 'paths')) {
        $overlayPaths = @{}
        foreach ($raw in @($overlayInfo.paths)) {
            $relative = Get-NormalizedRelativePath ([string]$raw)
            if ($overlayPaths.ContainsKey($relative)) { throw "Duplicate resource_overlay.paths entry: $relative" }
            if ($relative.StartsWith("sdgo\", [StringComparison]::OrdinalIgnoreCase)) { throw "resource_overlay.paths entry is not a resource: $relative" }
            $overlayPaths[$relative] = $true
        }
        foreach ($owned in $ownedPaths.Keys) {
            if (-not $overlayPaths.ContainsKey($owned)) { throw "owned_paths entry is absent from resource_overlay.paths: $owned" }
        }
    }
    Assert-ResourceOverlay $copyByTarget $overlayPaths

    if (-not $hashes.ContainsKey("symbols.fragment.ini")) {
        throw "Package hash whitelist has no symbols.fragment.ini"
    }
    $fragmentPath = Get-PathWithin $Root "symbols.fragment.ini"
    $fragmentText = [IO.File]::ReadAllText($fragmentPath)
    # New packages keep their complete symbol fragment next to the Mod, where
    # the framework loads it. Legacy packages only appended the load hook to
    # the global map; retain that path without losing newer per-Mod symbols.
    $localFragment = 'sdgo\mods\unit_catalog\symbols.fragment.ini'
    if ($copyByTarget.ContainsKey($localFragment) -and
        $copyByTarget[$localFragment].sha256 -cne $hashes['symbols.fragment.ini']) {
        throw 'Runtime symbol fragment differs from the package symbol fragment'
    }
    $symbolSection = Get-IniSection $fragmentText "unit_catalog.load_last_table"
    if ($null -eq $symbolSection) { throw "symbols.fragment.ini has no unit_catalog.load_last_table" }
    $hostSection = Get-IniSection $fragmentText "host"
    if ($null -eq $hostSection -or
        $hostSection -notmatch '(?im)^timestamp=0x54BE1568\s*$' -or
        $hostSection -notmatch '(?im)^size_of_image=0x00C97000\s*$') {
        throw "symbols.fragment.ini host identity is incomplete"
    }

    $catalogEntry = $copyByTarget["sdgo\mods\unit_catalog\catalog.bin"]
    Assert-CatalogHeader $catalogEntry.source
    return [pscustomobject]@{
        manifest = $manifest
        copy = @($copy | Sort-Object target_relative)
        symbol_section = $symbolSection
    }
}

function Read-Receipt([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    $receipt = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ($receipt.module -cne $Module -or [int]$receipt.schema_version -ne 1) {
        throw "Existing install receipt is not a supported unit_catalog receipt"
    }
    return $receipt
}

function Get-ProgramDefinition([string]$Text) {
    $sections = @{}; $values = @{}; $keys = @{}; $section = $null
    foreach ($raw in ($Text -split "`r?`n")) {
        $line = $raw.Trim()
        if (-not $line -or $line.StartsWith(';') -or $line.StartsWith('#')) { continue }
        if ($line -match '^\[([^\]]+)\]\s*(?:[;#].*)?$') {
            $section = $Matches[1].ToLowerInvariant()
            if ($sections.ContainsKey($section)) { throw 'Duplicate unit program manifest section' }
            $sections[$section] = $true
            continue
        }
        if ($null -eq $section -or $line -notmatch '^([^=]+)=(.*)$') { throw 'Invalid unit program manifest' }
        $key = $Matches[1].Trim().ToLowerInvariant(); $value = $Matches[2].Trim()
        $identity = $section + ':' + $key
        if ($keys.ContainsKey($identity)) { throw 'Duplicate unit program manifest key' }
        $keys[$identity] = $true
        if ($section -eq 'settings' -or ($section -eq 'mod' -and $key -eq 'enabled')) { continue }
        $values[$identity] = $value
    }
    return (($values.Keys | Sort-Object | ForEach-Object { $_ + '=' + $values[$_] }) -join "`n")
}

function Test-ReceiptFile($Receipt, $File, [string]$Path, [string]$Relative) {
    $expected = ([string]$File.installed_sha256).ToLowerInvariant()
    if ((Get-Sha256 $Path) -ceq $expected) { return $true }
    if ($Relative -ine 'sdgo\mods\unit_catalog\mod.ini' -or
        -not ($Receipt.PSObject.Properties.Name -contains 'framework_mod_ini_base64')) { return $false }
    $encoded = [string]$Receipt.framework_mod_ini_base64
    if (-not $encoded -or $encoded.Length -gt 131072) { throw 'Invalid unit program manifest snapshot' }
    $snapshot = [Convert]::FromBase64String($encoded)
    $sha = [Security.Cryptography.SHA256]::Create()
    try { $hash = [BitConverter]::ToString($sha.ComputeHash($snapshot)).Replace('-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
    if ($hash -cne $expected) { throw 'Unit program manifest snapshot hash differs from receipt' }
    $stream = [IO.MemoryStream]::new($snapshot, $false)
    $reader = [IO.StreamReader]::new($stream, [Text.Encoding]::UTF8, $true)
    try { $original = $reader.ReadToEnd() } finally { $reader.Dispose(); $stream.Dispose() }
    $current = [IO.File]::ReadAllText($Path)
    return (Get-ProgramDefinition $original) -ceq (Get-ProgramDefinition $current)
}

function New-Transaction([string]$Prefix) {
    $path = Join-Path ([IO.Path]::GetTempPath()) ($Prefix + [Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $path | Out-Null
    return [pscustomobject]@{
        root = $path
        backups = New-Object System.Collections.ArrayList
        seen = @{}
    }
}

function Backup-Target($Transaction, [string]$Path) {
    if ($Transaction.seen.ContainsKey($Path)) { return }
    $Transaction.seen[$Path] = $true
    $exists = Test-Path -LiteralPath $Path -PathType Leaf
    $backup = $null
    if ($exists) {
        $backup = Join-Path $Transaction.root ([Guid]::NewGuid().ToString("N") + ".bak")
        Copy-Item -LiteralPath $Path -Destination $backup
    }
    [void]$Transaction.backups.Add([pscustomobject]@{ path = $Path; existed = $exists; backup = $backup })
}

function Restore-Transaction($Transaction) {
    $failures = @()
    for ($index = $Transaction.backups.Count - 1; $index -ge 0; --$index) {
        $item = $Transaction.backups[$index]
        try {
            if ($item.existed) {
                # A failed write can leave the target byte-identical to its
                # backup. Avoid replacing that file, which also lets rollback
                # proceed when a read-only/shared handle caused the failure.
                if ((Test-Path -LiteralPath $item.path -PathType Leaf) -and
                    (Get-Sha256 $item.path) -ceq (Get-Sha256 $item.backup)) {
                    continue
                }
                New-Item -ItemType Directory -Force -Path (Split-Path -Parent $item.path) | Out-Null
                Copy-Item -LiteralPath $item.backup -Destination $item.path -Force
            } elseif (Test-Path -LiteralPath $item.path -PathType Leaf) {
                Remove-Item -LiteralPath $item.path -Force
            }
        } catch {
            $failures += "$($item.path): $($_.Exception.Message)"
        }
    }
    if ($failures.Count) { throw "Rollback could not restore: $($failures -join '; ')" }
}

function Copy-Atomically([string]$SourcePath, [string]$TargetPath) {
    $directory = Split-Path -Parent $TargetPath
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $temporary = Join-Path $directory ([IO.Path]::GetFileName($TargetPath) + ".unitcatalog." + [Guid]::NewGuid().ToString("N") + ".tmp")
    try {
        Copy-Item -LiteralPath $SourcePath -Destination $temporary
        Move-Item -LiteralPath $temporary -Destination $TargetPath -Force
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
    }
}

$ResolvedClientRoot = (Resolve-Path -LiteralPath $ClientRoot).Path.TrimEnd("\")
$exe = Join-Path $ResolvedClientRoot "GOnline.exe"
$receiptPath = Get-PathWithin $ResolvedClientRoot $ReceiptRelative
Assert-ClientStopped $exe

if ($Uninstall) {
    $receipt = Read-Receipt $receiptPath
    if ($null -eq $receipt) { throw "No unit_catalog install receipt exists in this client" }
    Assert-Host $exe ([string]$receipt.host) ([string]$receipt.host_sha256).ToLowerInvariant()

    $remove = @()
    foreach ($file in @($receipt.files)) {
        $relative = Get-NormalizedRelativePath ([string]$file.path)
        $target = Get-PathWithin $ResolvedClientRoot $relative
        if (-not [bool]$file.owned) { continue }
        if (Test-Path -LiteralPath $target -PathType Leaf) {
            $actual = Get-Sha256 $target
            if (-not (Test-ReceiptFile $receipt $file $target $relative)) {
                throw "Owned file changed after installation; refusing to remove it: $relative"
            }
            $remove += [pscustomobject]@{ relative = $relative; target = $target }
        }
    }

    $symbolPath = Get-PathWithin $ResolvedClientRoot ("sdgo\symbols\" + [string]$receipt.host + ".ini")
    $removeSymbol = $false
    if ([bool]$receipt.symbol_section_added -and (Test-Path -LiteralPath $symbolPath -PathType Leaf)) {
        $symbolText = [IO.File]::ReadAllText($symbolPath)
        $currentSection = Get-IniSection $symbolText "unit_catalog.load_last_table"
        if ($null -ne $currentSection) {
            if ((Get-NormalizedSection $currentSection) -cne
                (Get-NormalizedSection ([string]$receipt.symbol_section))) {
                throw "unit_catalog symbol section changed after installation; refusing to remove it"
            }
            $removeSymbol = $true
        }
    }

    $transaction = New-Transaction "sdgo-unit-catalog-uninstall-"
    $removeTransactionBackup = $true
    try {
        foreach ($entry in $remove) {
            Backup-Target $transaction $entry.target
            Remove-Item -LiteralPath $entry.target -Force
            Write-Host "removed  $($entry.relative)"
        }
        if ($removeSymbol) {
            Backup-Target $transaction $symbolPath
            $hasExactBackup = $receipt.PSObject.Properties.Name -contains 'symbol_original_base64'
            if ($hasExactBackup -and $receipt.symbol_original_base64 -and
                $receipt.symbol_installed_sha256 -and
                (Get-Sha256 $symbolPath) -ceq [string]$receipt.symbol_installed_sha256) {
                [IO.File]::WriteAllBytes($symbolPath,[Convert]::FromBase64String([string]$receipt.symbol_original_base64))
            } else {
                $text = [IO.File]::ReadAllText($symbolPath)
                [IO.File]::WriteAllText($symbolPath,
                    (Remove-IniSection $text "unit_catalog.load_last_table"), $Utf8NoBom)
            }
            Write-Host "removed  sdgo\symbols\$($receipt.host).ini [unit_catalog.load_last_table]"
        }
        Backup-Target $transaction $receiptPath
        Remove-Item -LiteralPath $receiptPath -Force
    } catch {
        $operationFailure = $_
        try {
            Restore-Transaction $transaction
        } catch {
            $removeTransactionBackup = $false
            throw "Uninstall failed: $($operationFailure.Exception.Message). " +
                  "Rollback is incomplete; backups were retained at $($transaction.root): " +
                  "$($_.Exception.Message)"
        }
        throw $operationFailure
    } finally {
        if ($removeTransactionBackup) {
            Remove-Item -LiteralPath $transaction.root -Recurse -Force -ErrorAction SilentlyContinue
        } else {
            Write-Warning "Rollback backups retained at $($transaction.root)"
        }
    }
    $modDirectory = Split-Path -Parent $receiptPath
    if ((Test-Path -LiteralPath $modDirectory -PathType Container) -and
        -not (Get-ChildItem -LiteralPath $modDirectory -Force | Select-Object -First 1)) {
        Remove-Item -LiteralPath $modDirectory -Force
    }
    Write-Host "unit_catalog removed; pre-existing byte-identical files were left in place"
    return
}

$ResolvedSource = (Resolve-Path -LiteralPath $Source).Path.TrimEnd("\")
Assert-Host $exe $ExpectedHost $ExpectedHostSha256
$package = Read-Package $ResolvedSource
$oldReceipt = Read-Receipt $receiptPath

$oldByPath = @{}
if ($null -ne $oldReceipt) {
    if ($oldReceipt.host -cne $ExpectedHost -or
        ([string]$oldReceipt.host_sha256).ToLowerInvariant() -cne $ExpectedHostSha256) {
        throw "Existing unit_catalog receipt belongs to a different client build"
    }
    foreach ($file in @($oldReceipt.files)) {
        $relative = Get-NormalizedRelativePath ([string]$file.path)
        $oldByPath[$relative] = $file
    }
}

$plan = @()
$newTargets = @{}
foreach ($entry in $package.copy) {
    $newTargets[$entry.target_relative] = $true
    $old = if ($oldByPath.ContainsKey($entry.target_relative)) { $oldByPath[$entry.target_relative] } else { $null }
    $exists = Test-Path -LiteralPath $entry.target -PathType Leaf
    $currentHash = if ($exists) { Get-Sha256 $entry.target } else { $null }
    $owned = $false
    $copyNeeded = -not $exists -or $currentHash -cne $entry.sha256

    if ($null -ne $old) {
        if ($exists -and -not (Test-ReceiptFile $oldReceipt $old $entry.target $entry.target_relative)) {
            throw "Previously recorded file changed after installation: $($entry.target_relative)"
        }
        if ([bool]$old.owned) {
            $owned = $true
        } elseif ($exists) {
            if ($currentHash -cne $entry.sha256) {
                throw "A reused foreign file differs from this update: $($entry.target_relative)"
            }
            $owned = $false
        } else {
            $owned = $true
        }
    } elseif ($exists) {
        if ($currentHash -cne $entry.sha256) {
            throw "Foreign file collision: $($entry.target_relative)"
        }
        $owned = $false
    } else {
        $owned = $true
    }
    $plan += [pscustomobject]@{
        entry = $entry
        owned = $owned
        copy_needed = $copyNeeded
    }
}

$stale = @()
foreach ($relative in $oldByPath.Keys) {
    if ($newTargets.ContainsKey($relative) -or -not [bool]$oldByPath[$relative].owned) { continue }
    $target = Get-PathWithin $ResolvedClientRoot $relative
    if (Test-Path -LiteralPath $target -PathType Leaf) {
        $actual = Get-Sha256 $target
        if ($actual -cne ([string]$oldByPath[$relative].installed_sha256).ToLowerInvariant()) {
            throw "Stale owned file changed after installation: $relative"
        }
        $stale += [pscustomobject]@{ relative = $relative; target = $target }
    }
}

$symbolPath = Get-PathWithin $ResolvedClientRoot ("sdgo\symbols\" + $ExpectedHost + ".ini")
if (-not (Test-Path -LiteralPath $symbolPath -PathType Leaf)) {
    throw "The mod framework has no symbol file for $ExpectedHost"
}
$symbolText = [IO.File]::ReadAllText($symbolPath)
$capsuleDirectory = Get-PathWithin $ResolvedClientRoot 'sdgo\mods\capsule_catalog'
$capsuleBinary = Join-Path $capsuleDirectory 'capsule_catalog.dll'
if (Test-Path -LiteralPath $capsuleBinary -PathType Leaf) {
    $capsuleEnabled = $true
    $capsuleManifest = Join-Path $capsuleDirectory 'mod.ini'
    if (Test-Path -LiteralPath $capsuleManifest -PathType Leaf) {
        $modSection = Get-IniSection ([IO.File]::ReadAllText($capsuleManifest)) 'mod'
        if ($modSection -match '(?im)^enabled\s*=\s*(0|false)\s*$') { $capsuleEnabled = $false }
    }
    $coreSettings = Get-PathWithin $ResolvedClientRoot 'sdgo\sdgo.ini'
    if (Test-Path -LiteralPath $coreSettings -PathType Leaf) {
        $modsSection = Get-IniSection ([IO.File]::ReadAllText($coreSettings)) 'mods'
        if ($modsSection -match '(?im)^capsule_catalog\s*=\s*(0|1|false|true)\s*$') {
            $capsuleEnabled = $Matches[1] -in @('1', 'true')
        }
    }
    if ($capsuleEnabled -and -not (Test-NativeExport $capsuleBinary 'SdgoGetResourceOverlayBrokerV1')) {
        throw 'The enabled capsule_catalog needs the current Mod framework to load imported unit resources. Update the Mod framework first; no files have been installed.'
    }
}
foreach ($requiredSymbol in @("grid.data", "grid.find", "grid.make_node", "grid.insert_node",
                               "grid.register_record", "grid.map_find", "crt.operator_new", "crt.operator_delete",
                               "capsule.main.open_file", "capsule.main.file_ctor",
                               "capsule.main.operator_new", "capsule.main.wfopen")) {
    if ($null -eq (Get-IniSection $symbolText $requiredSymbol)) {
        throw "Client symbol file is missing [$requiredSymbol]"
    }
}
$existingSymbol = Get-IniSection $symbolText "unit_catalog.load_last_table"
$addSymbol = $null -eq $existingSymbol
if (-not $addSymbol -and
    (Get-NormalizedSection $existingSymbol) -cne (Get-NormalizedSection $package.symbol_section)) {
    throw "Client symbol file has a conflicting [unit_catalog.load_last_table] section"
}
$symbolAddedByInstaller = if ($null -ne $oldReceipt) {
    [bool]$oldReceipt.symbol_section_added
} else {
    $addSymbol
}
$symbolOriginalBase64 = $null
$symbolInstalledHash = $null
if ($null -ne $oldReceipt -and $oldReceipt.PSObject.Properties.Name -contains 'symbol_original_base64') {
    $symbolOriginalBase64 = $oldReceipt.symbol_original_base64
    $symbolInstalledHash = $oldReceipt.symbol_installed_sha256
} elseif ($null -eq $oldReceipt -and $addSymbol) {
    $symbolOriginalBase64 = [Convert]::ToBase64String([IO.File]::ReadAllBytes($symbolPath))
}

$transaction = New-Transaction "sdgo-unit-catalog-install-"
$removeTransactionBackup = $true
try {
    foreach ($item in $plan) {
        if (-not $item.copy_needed) {
            Write-Host "reused   $($item.entry.target_relative)"
            continue
        }
        Backup-Target $transaction $item.entry.target
        Copy-Atomically $item.entry.source $item.entry.target
        if ((Get-Sha256 $item.entry.target) -cne $item.entry.sha256) {
            throw "Installed file readback mismatch: $($item.entry.target_relative)"
        }
        Write-Host "copied   $($item.entry.target_relative)"
    }
    foreach ($entry in $stale) {
        Backup-Target $transaction $entry.target
        Remove-Item -LiteralPath $entry.target -Force
        Write-Host "removed  stale $($entry.relative)"
    }
    if ($addSymbol) {
        Backup-Target $transaction $symbolPath
        $separator = if ($symbolText.EndsWith("`n")) { "`r`n" } else { "`r`n`r`n" }
        [IO.File]::AppendAllText($symbolPath, $separator + $package.symbol_section + "`r`n", $Utf8NoBom)
        $readback = Get-IniSection ([IO.File]::ReadAllText($symbolPath)) "unit_catalog.load_last_table"
        if ((Get-NormalizedSection $readback) -cne (Get-NormalizedSection $package.symbol_section)) {
            throw "Symbol section readback failed"
        }
        Write-Host "added    sdgo\symbols\$ExpectedHost.ini [unit_catalog.load_last_table]"
    }

    $receiptFiles = @()
    foreach ($item in $plan) {
        $receiptFiles += [ordered]@{
            path = $item.entry.target_relative
            installed_sha256 = $item.entry.sha256
            owned = [bool]$item.owned
        }
    }
    $receipt = [ordered]@{
        module = $Module
        schema_version = 1
        revision = [string]$package.manifest.revision
        host = $ExpectedHost
        host_sha256 = $ExpectedHostSha256
        installed_at_utc = [DateTime]::UtcNow.ToString("o")
        symbol_section_added = [bool]$symbolAddedByInstaller
        symbol_section = [string]$package.symbol_section
        symbol_original_base64 = $symbolOriginalBase64
        symbol_installed_sha256 = $(if ($null -eq $oldReceipt -and $addSymbol) { Get-Sha256 $symbolPath } else { $symbolInstalledHash })
        files = $receiptFiles
        framework_mod_ini_base64 = [Convert]::ToBase64String([IO.File]::ReadAllBytes((Get-PathWithin $ResolvedClientRoot 'sdgo\mods\unit_catalog\mod.ini')))
    }
    Backup-Target $transaction $receiptPath
    $receiptTemporary = Join-Path $transaction.root "install-receipt.json"
    [IO.File]::WriteAllText($receiptTemporary, ($receipt | ConvertTo-Json -Depth 8) + "`n", $Utf8NoBom)
    Copy-Atomically $receiptTemporary $receiptPath
    $receiptReadback = Read-Receipt $receiptPath
    if ($receiptReadback.revision -cne [string]$package.manifest.revision) {
        throw "Install receipt readback failed"
    }
} catch {
    $operationFailure = $_
    try {
        Restore-Transaction $transaction
    } catch {
        $removeTransactionBackup = $false
        throw "Install failed: $($operationFailure.Exception.Message). " +
              "Rollback is incomplete; backups were retained at $($transaction.root): " +
              "$($_.Exception.Message)"
    }
    throw $operationFailure
} finally {
    if ($removeTransactionBackup) {
        Remove-Item -LiteralPath $transaction.root -Recurse -Force -ErrorAction SilentlyContinue
    } else {
        Write-Warning "Rollback backups retained at $($transaction.root)"
    }
}

Write-Host "unit_catalog revision $($package.manifest.revision) installed in $ResolvedClientRoot"
Write-Host "No game process was started. Gameplay, model rendering and server compatibility remain manual acceptance checks."
