[CmdletBinding()]
param([string]$Compiler='g++',[switch]$VerifyBaseline)
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$out=Join-Path $repo 'build/host-tests'
New-Item -ItemType Directory -Force $out | Out-Null
$cc=(Get-Command $Compiler -ErrorAction Stop).Source
$env:PATH=(Split-Path $cc)+[IO.Path]::PathSeparator+$env:PATH
$stack=Join-Path $repo '.deps/pico-sdk/lib/btstack/src'
Push-Location $repo
try {
    $flags=@('-std=c++20','-O1','-Wall','-Wextra','-Werror','-Wno-sign-compare','-Wno-comment','-Wno-misleading-indentation','-DENABLE_CLASSIC','-DENABLE_AUTO_PROFILE=1','-DENABLE_SERIAL=0','-ffunction-sections','-fdata-sections','-Isrc','-Itests/classic_stubs',"-I$stack")
    $sources=@('tests/classic_state.cpp',(Join-Path $stack 'btstack_util.c'),(Join-Path $stack 'hci_cmd.c'))
    $exe=Join-Path $out 'classic_state.exe'
    & $cc @flags @sources '-Wl,--gc-sections' -o $exe
    if($LASTEXITCODE){throw 'Classic production state-machine build failed.'}
    & $exe
    if($LASTEXITCODE){throw 'Classic production state-machine regression failed.'}
    if($VerifyBaseline){
        $baseline=Join-Path $out 'classic-fe6bf09.cpp'
        $source=& git show 'fe6bf09:src/bt.cpp'
        if($LASTEXITCODE){throw 'Cannot read release baseline.'}
        [IO.File]::WriteAllText($baseline,($source -join "`n"),[Text.UTF8Encoding]::new($false))
        $baselineFlag='-DCLASSIC_SOURCE="'+$baseline.Replace('\','/')+'"'
        $old=Join-Path $out 'classic_baseline.exe'
        & $cc @flags '-DCLASSIC_BASELINE_ONLY' '-Wno-unused-variable' '-Wno-unused-parameter' $baselineFlag @sources '-Wl,--gc-sections' -o $old
        if($LASTEXITCODE){throw 'Classic baseline reproduction build failed.'}
        & $old
        if($LASTEXITCODE -ne 1){throw 'Expected released baseline to reproduce lost raw HCI command.'}
        Write-Output 'PASS: released fe6bf09 regression reproduced, fixed production source passes'
    }
} finally {Pop-Location}
