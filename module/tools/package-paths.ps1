#Requires -Version 7.2
Set-StrictMode -Version Latest

function Get-SafeFullPath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) { throw 'A filesystem path is required.' }
    $provider = $null
    $drive = $null
    $full = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath(
        $Path, [ref]$provider, [ref]$drive)
    if ($provider.Name -ne 'FileSystem') { throw "Not a filesystem path: $Path" }
    $full = [IO.Path]::GetFullPath($full)
    for ($cursor = $full; $cursor; $cursor = [IO.Path]::GetDirectoryName($cursor)) {
        $item = Get-Item -LiteralPath $cursor -Force -ErrorAction SilentlyContinue
        if ($null -ne $item -and
            ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Links and reparse points are not allowed: $cursor"
        }
    }
    return $full
}

function Get-SafeRelativePath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path) -or [IO.Path]::IsPathRooted($Path)) {
        throw "Unsafe relative path: $Path"
    }
    $normalized = $Path.Replace('\', '/')
    foreach ($part in $normalized.Split('/')) {
        if ($part -in @('', '.', '..') -or $part -match '[<>:"|?*\x00-\x1f]' -or
            $part -match '[. ]$' -or
            $part -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)') {
            throw "Unsafe relative path: $Path"
        }
    }
    return $normalized
}

function Assert-ContainedPath([string]$Root, [string]$Path) {
    $rootPath = Get-SafeFullPath $Root
    $full = Get-SafeFullPath $Path
    $prefix = $rootPath.TrimEnd([char[]]@('\', '/')) + [IO.Path]::DirectorySeparatorChar
    if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path is outside its allowed directory '$rootPath': $full"
    }
    return $full
}

function Join-SafePath([string]$Root, [string]$Relative) {
    $relativePath = Get-SafeRelativePath $Relative
    return Assert-ContainedPath $Root (Join-Path $Root $relativePath)
}

function Get-SafeTreeFiles([string]$Root) {
    $full = Get-SafeFullPath $Root
    if (-not (Test-Path -LiteralPath $full -PathType Container)) {
        throw "Directory does not exist: $full"
    }
    $pending = [Collections.Generic.Stack[string]]::new()
    $pending.Push($full)
    while ($pending.Count -gt 0) {
        foreach ($item in Get-ChildItem -LiteralPath $pending.Pop() -Force) {
            $relative = [IO.Path]::GetRelativePath($full, $item.FullName)
            $null = Join-SafePath $full $relative
            if ($item.PSIsContainer) { $pending.Push($item.FullName) }
            else { $item }
        }
    }
}

function Copy-StageFile([string]$Source, [string]$Stage, [string]$Relative) {
    $inputPath = Get-SafeFullPath $Source
    if (-not (Test-Path -LiteralPath $inputPath -PathType Leaf)) {
        throw "Missing input file: $inputPath"
    }
    $target = Join-SafePath $Stage $Relative
    if (Test-Path -LiteralPath $target) { throw "Duplicate staged file: $Relative" }
    New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($target)) -Force | Out-Null
    [IO.File]::Copy($inputPath, $target, $false)
    if ((Get-FileHash -LiteralPath $inputPath).Hash -ne
        (Get-FileHash -LiteralPath $target).Hash) {
        throw "Input changed during copy: $inputPath"
    }
}

function Remove-PackageStage([string]$StageRoot, [string]$Stage) {
    $full = Assert-ContainedPath $StageRoot $Stage
    if (Test-Path -LiteralPath $full) {
        $null = @(Get-SafeTreeFiles $full)
        Remove-Item -LiteralPath $full -Recurse -Force
    }
}

function Test-ModuleArchive([string]$Path) {
    $archive = [IO.Compression.ZipFile]::OpenRead((Get-SafeFullPath $Path))
    try {
        $entries = @{}
        foreach ($entry in $archive.Entries) {
            if ($entry.FullName.EndsWith('/')) { continue }
            $name = Get-SafeRelativePath $entry.FullName
            if ($entries.ContainsKey($name)) { throw "Duplicate ZIP entry: $name" }
            $entries.Add($name, $entry)
        }
        if (-not $entries.ContainsKey('MODULE-SHA256.txt')) { throw 'Missing hash inventory.' }
        $reader = [IO.StreamReader]::new($entries['MODULE-SHA256.txt'].Open())
        try { $inventory = $reader.ReadToEnd() }
        finally { $reader.Dispose() }
        $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($line in ($inventory -split '\r?\n')) {
            if (-not $line) { continue }
            if ($line -notmatch '^([a-f0-9]{64})  (.+)$') { throw 'Malformed hash inventory.' }
            $expected = $Matches[1]
            $name = Get-SafeRelativePath $Matches[2]
            if ($name -eq 'MODULE-SHA256.txt' -or -not $seen.Add($name) -or
                -not $entries.ContainsKey($name)) { throw "Invalid hash entry: $name" }
            $stream = $entries[$name].Open()
            $sha = [Security.Cryptography.SHA256]::Create()
            try { $actual = [Convert]::ToHexString($sha.ComputeHash($stream)).ToLowerInvariant() }
            finally { $sha.Dispose(); $stream.Dispose() }
            if ($actual -cne $expected) { throw "Archive hash mismatch: $name" }
        }
        if ($seen.Count -ne $entries.Count - 1) { throw 'Archive has unhashed files.' }
    }
    finally { $archive.Dispose() }
}
