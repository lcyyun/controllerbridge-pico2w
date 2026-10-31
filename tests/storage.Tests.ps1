[CmdletBinding()]
param([string]$Compiler = 'g++')
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$out = Join-Path $repo 'build/host-tests'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$compilerCommand = Get-Command $Compiler -ErrorAction Stop
$env:PATH = (Split-Path $compilerCommand.Source) + [IO.Path]::PathSeparator + $env:PATH
Push-Location $repo
try {
    & $Compiler '-std=c++20' '-O1' '-Wall' '-Wextra' '-Werror' '-Isrc' '-Itests/storage_stubs' tests/storage.cpp -o (Join-Path $out 'storage.exe')
    if ($LASTEXITCODE) { throw 'Storage test build failed.' }
    & (Join-Path $out 'storage.exe')
    if ($LASTEXITCODE) { throw 'Storage journal tests failed.' }
} finally { Pop-Location }
