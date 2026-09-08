#Requires -Version 7.2
# Based on the exported manager packer; output and staging are confined to this repo.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDirectory,
    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'package-paths.ps1')

$source = Get-SafeFullPath $SourceDirectory
if (-not (Test-Path -LiteralPath $source -PathType Container)) {
    throw "Module source directory does not exist: $source"
}
$sourceFiles = @(Get-SafeTreeFiles $source)
$manifestPath = Join-Path $source 'module.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "A module source must contain module.json at its root."
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ([int]$manifest.schemaVersion -ne 1) {
    throw "Only module schemaVersion 1 can be packed."
}
if ([int]$manifest.runtimeApiVersion -notin @(1, 2)) {
    throw "Module runtime API must be 1 or 2."
}
if ([int]$manifest.runtimeApiVersion -eq 2) {
    $schemaPath = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot `
        '..\schemas\module-v2.schema.json'))
    if (-not (Test-Path -LiteralPath $schemaPath -PathType Leaf)) {
        throw "Runtime API 2 JSON Schema is missing: $schemaPath"
    }
    $manifestJson = Get-Content -LiteralPath $manifestPath -Raw
    if (-not (Test-Json -Json $manifestJson -SchemaFile $schemaPath `
            -ErrorAction Stop)) {
        throw "module.json does not satisfy the Runtime API 2 JSON Schema."
    }
}
$moduleId = [string]$manifest.id
$moduleVersion = [string]$manifest.moduleVersion
if ($moduleId -notmatch '^[0-9A-Za-z_-]+$') {
    throw "Module id or version is invalid."
}
$semver = '^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-((?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*)(?:\.(?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*))*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$'
if ($moduleVersion -notmatch $semver) {
    throw "Module version must use Semantic Versioning (for example 1.2.3)."
}
$safeVersion = $moduleVersion

$manifests = $sourceFiles | Where-Object {
    $_.Name -ieq 'module.json' -or $_.Name -ilike '*.bridge-module.json'
}
if (@($manifests).Count -ne 1 -or
    $manifests[0].FullName -ine $manifestPath) {
    throw "A module source must contain exactly one root module.json manifest."
}

$boardIds = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($board in @($manifest.boards)) {
    $boardId = [string]$board.id
    if ([string]::IsNullOrWhiteSpace($boardId) -or
        -not $boardIds.Add($boardId)) {
        throw "Board ids must be present and unique."
    }
}

foreach ($firmware in @($manifest.firmware)) {
    $method = [string]$firmware.flashMethod
    if ($method -notin @('None', 'PicoUf2', 'SifliSerial')) {
        throw "Unsupported firmware flashMethod: $method"
    }
    foreach ($boardId in @($firmware.boardIds)) {
        if (-not $boardIds.Contains([string]$boardId)) {
            throw "Firmware references unknown board id: $boardId"
        }
    }
    $relative = [string]$firmware.artifactRelativePath
    if ([string]::IsNullOrWhiteSpace($relative)) {
        if ($method -ne 'None') {
            throw "Automatic firmware $($firmware.id) must include an artifact."
        }
        continue
    }
    $artifact = Join-SafePath $source $relative
    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
        throw "Firmware artifact is missing or outside the module: $relative"
    }
    if ($method -eq 'SifliSerial') {
        $parameters = Get-Content -LiteralPath $artifact -Raw | ConvertFrom-Json
        foreach ($file in @($parameters.write_flash.files)) {
            $secondary = [string]$file.path
            $secondaryPath = Join-SafePath (Split-Path -Parent $artifact) $secondary
            if (-not (Test-Path -LiteralPath $secondaryPath -PathType Leaf)) {
                throw "SiFli package is missing a referenced artifact: $secondary"
            }
        }
    }
}

$hidKeys = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($device in @($manifest.hidDevices)) {
    $key = [string]$device.key
    $serial = if ($null -ne $device.PSObject.Properties['managerSerialNumber']) {
        [string]$device.managerSerialNumber
    } else { '' }
    $identity = "${key}:$($device.vendorId):$($device.productId):$serial"
    if ([string]::IsNullOrWhiteSpace($key) -or
        [string]::IsNullOrWhiteSpace([string]$device.displayName) -or
        [int]$device.vendorId -le 0 -or [int]$device.productId -le 0 -or
        -not $hidKeys.Add($identity)) {
        throw "HID discovery entries must have a unique key/identity and real VID/PID."
    }
}

if (-not (Test-Path -LiteralPath (Join-Path $source 'LICENSE') -PathType Leaf)) {
    throw "A redistributable single-file module must include LICENSE."
}

$repoRoot = Get-SafeFullPath (Join-Path $PSScriptRoot '../..')
$distRoot = Join-SafePath $repoRoot 'dist'
$outputRoot = Join-SafePath $distRoot 'modules'
$stageRoot = Join-SafePath $distRoot 'stage'
$destination = Join-SafePath $outputRoot "$moduleId-$safeVersion.cbmodule"
if ($OutputPath -and (Get-SafeFullPath $OutputPath) -ine $destination) {
    throw "OutputPath must be the repository package destination: $destination"
}
if (Test-Path -LiteralPath $destination) {
    throw "Package already exists; refusing to overwrite: $destination"
}
$stage = Join-SafePath $stageRoot ("pack-" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage -Force | Out-Null
try {
    $payload = Join-SafePath $stage 'payload'
    New-Item -ItemType Directory -Path $payload | Out-Null
    foreach ($file in $sourceFiles) {
        $relative = [IO.Path]::GetRelativePath($source, $file.FullName).Replace('\', '/')
        if ($relative -ieq 'MODULE-SHA256.txt') { continue }
        Copy-StageFile $file.FullName $payload $relative
    }
    $hashFile = Join-SafePath $payload 'MODULE-SHA256.txt'
    $hashLines = Get-SafeTreeFiles $payload |
        Sort-Object FullName |
        ForEach-Object {
            $relative = [IO.Path]::GetRelativePath($payload, $_.FullName).Replace('\', '/')
            $hash = (Get-FileHash -LiteralPath $_.FullName `
                -Algorithm SHA256).Hash.ToLowerInvariant()
            "$hash  $relative"
        }
    Set-Content -LiteralPath $hashFile -Value $hashLines -Encoding utf8NoBOM

    $temporaryZip = Join-SafePath $stage 'package.cbmodule'
    [IO.Compression.ZipFile]::CreateFromDirectory($payload, $temporaryZip,
        [IO.Compression.CompressionLevel]::Optimal, $false)
    Test-ModuleArchive $temporaryZip
    $null = Assert-ContainedPath $outputRoot $destination
    New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null
    [IO.File]::Move($temporaryZip, $destination, $false)
    Write-Host "Single-file firmware compatibility package: $destination"
}
finally {
    Remove-PackageStage $stageRoot $stage
}
