param([string]$CompilerDirectory = '')
$ErrorActionPreference='Stop'
if ($CompilerDirectory) { $env:Path="$CompilerDirectory;$env:Path" }
$root=Split-Path $PSScriptRoot -Parent
$out=Join-Path $PSScriptRoot '.out'
New-Item -ItemType Directory -Force $out | Out-Null
Push-Location $root
try {
    foreach($unit in @('bl616_bridge_protocol','bl616_bridge_mapping')) {
        & gcc -std=c11 -Wall -Wextra -Werror -Isrc/parity -c "src/parity/$unit.c" -o "$out/$unit.o"
        if($LASTEXITCODE) { throw "Compile failed: $unit" }
    }
    & gcc -std=c11 -Wall -Wextra -Werror -Isrc/parity tests/parity_mapping.c "$out/bl616_bridge_protocol.o" "$out/bl616_bridge_mapping.o" -o "$out/parity-mapping.exe"
    if($LASTEXITCODE) { throw 'Mapping test compile failed' }
    & "$out/parity-mapping.exe"
    if($LASTEXITCODE) { throw 'Mapping tests failed' }
    & g++ -std=c++20 -Wall -Wextra -Werror -Isrc tests/runtime.cpp src/bridge_runtime.cpp "$out/bl616_bridge_protocol.o" "$out/bl616_bridge_mapping.o" -o "$out/runtime.exe"
    if($LASTEXITCODE) { throw 'Runtime test compile failed' }
    & "$out/runtime.exe"
    if($LASTEXITCODE) { throw 'Runtime tests failed' }
} finally { Pop-Location }
