[CmdletBinding()]
param([string]$Compiler = 'g++', [string]$OpusLibrary)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$output = Join-Path $repo 'build/host'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$compilerCommand = Get-Command $Compiler -ErrorAction Stop
$env:Path = (Split-Path -Parent $compilerCommand.Source) + ';' + $env:Path
$common = @('-std=c++20','-Wall','-Wextra','-Werror','-I',(Join-Path $repo 'src'))
$cases = @(
    @{ Name='output_usb_test'; Sources=@('tests/output_usb_test.cpp','src/xinput_usb.cpp','src/profiles/auto_usb.cpp','src/usb_descriptors.cpp'); Flags=@('-Wno-missing-field-initializers','-DENABLE_AUTO_PROFILE=1','-DENABLE_SERIAL=0','-I',(Join-Path $PSScriptRoot 'output_stubs')) },
    @{ Name='output_features_test'; Sources=@('tests/output_features_test.cpp','src/bridge_features.cpp'); Flags=@() },
    @{ Name='output_haptics_test'; Sources=@('tests/output_haptics_test.cpp'); Flags=@() },
    @{ Name='output_mic_test'; Sources=@('tests/output_mic_test.cpp'); Flags=@() },
    @{ Name='output_state_test'; Sources=@('tests/output_state_test.cpp','src/state_mgr.cpp'); Flags=@() }
)
foreach ($case in $cases) {
    $exe = Join-Path $output ($case.Name + '.exe')
    $sources = @($case.Sources | ForEach-Object { Join-Path $repo $_ })
    & $compilerCommand.Source @common @($case.Flags) @sources -o $exe
    if ($LASTEXITCODE -ne 0) { throw "Compile failed: $($case.Name)" }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw "Test failed: $($case.Name)" }
}
if ($OpusLibrary) {
    $opus = (Resolve-Path -LiteralPath $OpusLibrary).Path
    $exe = Join-Path $output 'output_mic_opus_test.exe'
    & $compilerCommand.Source @common -I (Join-Path $repo 'lib/opus/include') (Join-Path $PSScriptRoot 'output_mic_opus_test.cpp') $opus -o $exe
    if ($LASTEXITCODE -ne 0) { throw 'Compile failed: output_mic_opus_test' }
    & $exe
    if ($LASTEXITCODE -ne 0) { throw 'Test failed: output_mic_opus_test' }
}
