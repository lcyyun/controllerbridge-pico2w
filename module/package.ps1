#Requires -Version 7.2
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateNotNullOrEmpty()][string]$Uf2Path
)

$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'tools/package-module.ps1') `
    -ExpectedId 'pico-unified-bridge' -Method 'PicoUf2' -Uf2Path $Uf2Path
