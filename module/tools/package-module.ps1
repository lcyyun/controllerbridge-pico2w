#Requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ExpectedId,
    [Parameter(Mandatory)][ValidateSet('SifliSerial', 'PicoUf2', 'None')][string]$Method,
    [string]$BuildDirectory,
    [string]$Uf2Path
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'package-paths.ps1')
$moduleRoot = Get-SafeFullPath (Join-Path $PSScriptRoot '..')
$repoRoot = Get-SafeFullPath (Join-Path $moduleRoot '..')
$manifestPath = Join-SafePath $moduleRoot 'module.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.id -cne $ExpectedId) { throw "Expected module id: $ExpectedId" }
$firmware = @($manifest.firmware)
if ($firmware.Count -ne 1 -or $firmware[0].flashMethod -cne $Method) {
    throw "This repository's packaging contract requires one '$Method' firmware entry."
}
if ($Method -ne 'SifliSerial' -and $BuildDirectory) { throw 'BuildDirectory is only for SF32.' }
if ($Method -ne 'PicoUf2' -and $Uf2Path) { throw 'Uf2Path is only for Pico.' }

# Resolve and validate every external input before creating a staging directory.
$inputs = [Collections.Generic.List[object]]::new()
$inputs.Add(@{ Source = $manifestPath; Relative = 'module.json' })
foreach ($name in @('LICENSE', 'NOTICE.md')) {
    $source = Join-SafePath $repoRoot $name
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing root $name" }
    $inputs.Add(@{ Source = $source; Relative = $name })
}
$licenseRoot = Join-SafePath $repoRoot 'LICENSES'
foreach ($file in @(Get-SafeTreeFiles $licenseRoot)) {
    $inputs.Add(@{ Source = $file.FullName
        Relative = 'LICENSES/' + [IO.Path]::GetRelativePath($licenseRoot, $file.FullName).Replace('\', '/') })
}

$relativeArtifact = [string]$firmware[0].artifactRelativePath
if ($Method -eq 'None') {
    if ($relativeArtifact) { throw 'ESP32-S3 packaging is metadata-only; no artifact is permitted.' }
} else {
    $relativeArtifact = Get-SafeRelativePath $relativeArtifact
    if (-not $relativeArtifact.StartsWith('artifacts/', [StringComparison]::Ordinal)) {
        throw 'Firmware must be staged below artifacts/.'
    }
    if ($Method -eq 'SifliSerial') {
        if (-not $BuildDirectory) { throw 'An explicit BuildDirectory is required.' }
        $buildRoot = Get-SafeFullPath $BuildDirectory
        $parameterPath = Join-SafePath $buildRoot 'sftool_param.json'
        if ([IO.Path]::GetFileName($relativeArtifact) -cne 'sftool_param.json') {
            throw 'SF32 artifact must be sftool_param.json.'
        }
        $parameters = Get-Content -LiteralPath $parameterPath -Raw | ConvertFrom-Json
        $flashFiles = @($parameters.write_flash.files)
        if ($flashFiles.Count -eq 0) { throw 'SF32 flash parameters must reference files.' }
        $inputs.Add(@{ Source = $parameterPath; Relative = $relativeArtifact })
        $artifactParent = $relativeArtifact.Substring(0, $relativeArtifact.LastIndexOf('/'))
        foreach ($file in $flashFiles) {
            $relative = Get-SafeRelativePath ([string]$file.path)
            $source = Join-SafePath $buildRoot $relative
            if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
                throw "Missing SF32 flash file: $relative"
            }
            $inputs.Add(@{ Source = $source; Relative = "$artifactParent/$relative" })
        }
    } else {
        if (-not $Uf2Path) { throw 'An explicit Uf2Path is required.' }
        $source = Get-SafeFullPath $Uf2Path
        if ([IO.Path]::GetExtension($source) -ine '.uf2' -or
            -not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw 'Uf2Path must be an existing .uf2 file.'
        }
        $stream = [IO.File]::OpenRead($source)
        $reader = [IO.BinaryReader]::new($stream)
        try {
            if ($stream.Length -eq 0 -or $stream.Length % 512 -ne 0) { throw 'Invalid UF2 length.' }
            for ($offset = 0L; $offset -lt $stream.Length; $offset += 512) {
                $stream.Position = $offset
                if ($reader.ReadUInt32() -ne 0x0A324655 -or
                    $reader.ReadUInt32() -ne [Convert]::ToUInt32('9E5D5157', 16)) {
                    throw 'Invalid UF2 block header.'
                }
                $stream.Position = $offset + 508
                if ($reader.ReadUInt32() -ne 0x0AB16F30) { throw 'Invalid UF2 block trailer.' }
            }
        }
        finally { $reader.Dispose() }
        $inputs.Add(@{ Source = $source; Relative = $relativeArtifact })
    }
}

$names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($inputFile in $inputs) {
    $relative = Get-SafeRelativePath $inputFile.Relative
    if (-not $names.Add($relative)) { throw "Duplicate staged path: $relative" }
}
$distRoot = Join-SafePath $repoRoot 'dist'
$stageRoot = Join-SafePath $distRoot 'stage'
$stage = Join-SafePath $stageRoot ("module-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage -Force | Out-Null
try {
    foreach ($inputFile in $inputs) {
        Copy-StageFile $inputFile.Source $stage $inputFile.Relative
    }
    & (Join-Path $PSScriptRoot 'pack-module-directory.ps1') -SourceDirectory $stage
}
finally { Remove-PackageStage $stageRoot $stage }
