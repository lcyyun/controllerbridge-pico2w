[CmdletBinding()]
param(
    [string]$Compiler = 'g++',
    [string]$SdkPath = (Join-Path $PSScriptRoot '../.deps/pico-sdk')
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$out = Join-Path $repo 'build/host-tests'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$compilerCommand = Get-Command $Compiler -ErrorAction Stop
$env:PATH = (Split-Path $compilerCommand.Source) + [IO.Path]::PathSeparator + $env:PATH
Push-Location $repo
try {
    $common = @('-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-Isrc')
    & $Compiler @common tests/wireless_input.cpp src/ns2/ns2_input.cpp src/ns2/ns2_gatt.cpp -o (Join-Path $out 'wireless_input.exe')
    if ($LASTEXITCODE) { throw 'Wireless parser/protocol build failed.' }
    & (Join-Path $out 'wireless_input.exe')
    if ($LASTEXITCODE) { throw 'Wireless parser/protocol tests failed.' }
    $stack = Join-Path $SdkPath 'lib/btstack/src'
    if (!(Test-Path (Join-Path $stack 'btstack.h'))) { throw 'Pinned SDK/BTstack sources required for radio-state tests.' }
    & $Compiler @common '-ffunction-sections' '-fdata-sections' '-Itests/wireless_stubs' "-I$stack" '-DENABLE_NS2PRO=1' tests/wireless_ble.cpp src/ns2/ns2_input.cpp src/ns2/ns2_gatt.cpp (Join-Path $stack 'btstack_util.c') '-Wl,--gc-sections' -o (Join-Path $out 'wireless_ble.exe')
    if ($LASTEXITCODE) { throw 'BLE state-machine test build failed.' }
    & (Join-Path $out 'wireless_ble.exe')
    if ($LASTEXITCODE) { throw 'BLE state-machine tests failed.' }
} finally { Pop-Location }
