<#
.SYNOPSIS
    Builder for the ControllerBridge Pico 2 W unified receiver on Windows 11
    (no WSL required).

.DESCRIPTION
    Installs every prerequisite (winget where possible, portable downloads as a
    fallback), fetches the pinned Raspberry Pi Pico SDK + TinyUSB, and
    configures and builds the firmware with CMake + Ninja. Opus and WDL
    are imported vendor trees, not project submodules. Outputs stay under
    build/<variant> and artifacts/<variant>; this script never flashes.

    The script is idempotent: re-running it skips anything already installed or
    downloaded.

.PARAMETER Variant
    unified (default) - DualSense Classic / NS2Pro BLE unified receiver.
    ns2pro            - optional NS2Pro-only BLE receiver, not unified.
    standard          - inherited DS5-only firmware.
    debug             - unified with USB serial and verbose diagnostics.

.PARAMETER Clean
    Delete the variant's build directory before configuring.

.PARAMETER Repo
    When run standalone (the script is not inside a checkout), the project
    git URL to clone. Defaults to lcyyun/controllerbridge-pico2w (private;
    authenticate Git separately). Override to build a fork.

.PARAMETER UseInstalledTools
    Do not install tools or change SDK checkouts. Requires Git, CMake, Ninja,
    Python and a native C/C++ compiler, plus SdkPath and ArmToolchainPath.

.PARAMETER Ref
    Branch, tag or commit to build when cloned standalone. Empty = the
    repo's default branch.

.EXAMPLE
    # Standalone: download just this file anywhere and run it - it clones
    # the project under %USERPROFILE%\.controllerbridge-pico2w-build.
    powershell -ExecutionPolicy Bypass -File .\build-windows.ps1

.EXAMPLE
    # From inside a cloned repo:
    powershell -ExecutionPolicy Bypass -File tools\build-windows.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File .\build-windows.ps1 -Variant ns2pro
#>

[CmdletBinding()]
param(
    [ValidateSet('unified', 'standard', 'debug', 'ns2pro')]
    [string]$Variant = 'unified',
    [switch]$Clean,
    # Project to build when this script is run standalone (not from inside a
    # checkout). Override to build a fork.
    [string]$Repo = 'https://github.com/lcyyun/controllerbridge-pico2w.git',
    # Branch/tag/SHA to build when cloned standalone. Empty = default branch.
    [string]$Ref = '',
    [switch]$UseInstalledTools,
    [string]$SdkPath = $env:PICO_SDK_PATH,
    [string]$ArmToolchainPath = $env:PICO_TOOLCHAIN_PATH,
    [string]$Version = 'dev',
    [ValidateRange(1, 64)]
    [int]$Jobs = 2
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# Bump on every change so a stale download is obvious in the banner.
$SCRIPT_REV   = '2026-09-08.1'

# --- Pinned versions: keep in sync with .github/workflows/build-firmware.yml ---
$PICO_SDK_REF = '2.2.0'
$TINYUSB_REF  = '0.20.0'
$PICO_SDK_SHA = 'a1438dff1d38bd9c65dbd693f0e5db4b9ae91779'
$TINYUSB_SHA  = '3af1bec1a9161ee8dec29487831f7ac7ade9e189'
$BTSTACK_SHA  = '501e6d2b86e6c92bfb9c390bcf55709938e25ac1'
$CYW43_SHA    = 'dd7568229f3bf7a37737b9e1ef250c26efe75b23'
$ARM_VER      = '14.2.rel1'
$ARM_ZIP      = "arm-gnu-toolchain-$ARM_VER-mingw-w64-x86_64-arm-none-eabi.zip"
$ARM_URL      = "https://developer.arm.com/-/media/Files/downloads/gnu/$ARM_VER/binrel/$ARM_ZIP"
# Portable native host compiler (WinLibs MinGW-w64 UCRT) for pioasm/picotool.
$MINGW_URL    = 'https://github.com/brechtsanders/winlibs_mingw/releases/download/14.2.0posix-19.1.1-12.0.0-ucrt-r2/winlibs-x86_64-posix-seh-gcc-14.2.0-mingw-w64ucrt-12.0.0-r2.zip'

$ToolsHome = Join-Path $env:USERPROFILE '.controllerbridge-pico2w-build'
if (-not $SdkPath) { $SdkPath = Join-Path $ToolsHome 'pico-sdk' }
$ArmRoot = if ($ArmToolchainPath) { $ArmToolchainPath } else { Join-Path $ToolsHome 'arm-gnu-toolchain' }
$ClonePath = Join-Path $ToolsHome 'controllerbridge-pico2w'
# $RepoRoot is resolved at runtime (Resolve-RepoRoot) - either an existing
# checkout this script sits in, or a fresh clone under $ToolsHome.
$RepoRoot  = $null
$GitExit   = 0     # last git exit code, set by Invoke-GitQuiet
$PythonExe = $null # real Python 3 interpreter, set by Resolve-Python

function Info  ($m) { Write-Host "[pico2w] $m"            -ForegroundColor Cyan }
function Ok    ($m) { Write-Host "[pico2w] $m"            -ForegroundColor Green }
function Warn  ($m) { Write-Host "[pico2w] WARNING: $m"   -ForegroundColor Yellow }
function Die   ($m) { throw "[pico2w] $m" }

function Have ($cmd) { [bool](Get-Command $cmd -ErrorAction SilentlyContinue) }

function Add-SessionPath ($dir) {
    if ($dir -and (Test-Path $dir) -and ($env:Path -notlike "*$dir*")) {
        $env:Path = "$dir;$env:Path"
    }
}

# --- Discover already-installed tools that aren't on PATH (common with winget) -
function Add-CommonToolPaths {
    $candidates = @(
        "$env:ProgramFiles\CMake\bin",
        "$env:ProgramFiles\Git\cmd",
        "${env:ProgramFiles(x86)}\Git\cmd"
    )
    foreach ($c in $candidates) { Add-SessionPath $c }
    # winget often installs Ninja/Python under WinGet Links or per-user dirs.
    $wingetLinks = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links'
    Add-SessionPath $wingetLinks
}

# --- Step 0: ensure winget exists, bootstrap if missing, else portable mode ----
function Initialize-PackageManager {
    if (Have winget) { Ok 'winget present.'; return $true }

    Warn 'winget (App Installer) not found. Attempting automatic bootstrap...'
    try {
        $tmp = Join-Path $env:TEMP 'controllerbridge-pico2w-winget'
        New-Item -ItemType Directory -Force -Path $tmp | Out-Null

        $deps = @(
            @{ name = 'VCLibs';
               url  = 'https://aka.ms/Microsoft.VCLibs.x64.14.00.Desktop.appx' },
            @{ name = 'AppInstaller';
               url  = 'https://aka.ms/getwinget' }   # latest DesktopAppInstaller bundle
        )
        foreach ($d in $deps) {
            $dest = Join-Path $tmp ($d.name + [IO.Path]::GetExtension($d.url))
            if (-not $dest.EndsWith('.appx') -and -not $dest.EndsWith('.msixbundle')) {
                $dest = Join-Path $tmp ($d.name + '.msixbundle')
            }
            Info "Downloading $($d.name)..."
            Invoke-WebRequest -Uri $d.url -OutFile $dest -UseBasicParsing
            Add-AppxPackage -Path $dest -ErrorAction Stop
        }
        # Refresh PATH for the App Execution Alias.
        Add-SessionPath (Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps')
        if (Have winget) { Ok 'winget bootstrapped successfully.'; return $true }
    }
    catch {
        Warn "winget bootstrap failed: $($_.Exception.Message)"
    }

    Warn 'Proceeding in PORTABLE mode (no winget) - tools downloaded locally.'
    return $false
}

# --- winget install with portable fallback per tool --------------------------
function Ensure-Tool {
    param(
        [string]$Command,
        [string]$WingetId,
        [bool]$WingetAvailable,
        [scriptblock]$PortableInstall
    )
    if (Have $Command) { Ok "$Command already available."; return }

    if ($WingetAvailable) {
        Info "Installing $Command via winget ($WingetId)..."
        winget install --id $WingetId --exact --silent --accept-source-agreements `
            --accept-package-agreements --disable-interactivity | Out-Host
        Add-CommonToolPaths
        if (Have $Command) { Ok "$Command installed."; return }
        Warn "$Command still not on PATH after winget; trying portable."
    }

    if ($PortableInstall) {
        Info "Installing $Command (portable)..."
        & $PortableInstall
        if (Have $Command) { Ok "$Command installed (portable)."; return }
    }
    Die "Could not install '$Command'. Install it manually and re-run."
}

function Install-PortableArchiveTool {
    param([string]$Name, [string]$Url, [string]$BinSubdir)
    $base = Join-Path $ToolsHome $Name
    $zip  = Join-Path $env:TEMP "$Name.zip"
    if (-not (Test-Path $base)) {
        Invoke-WebRequest -Uri $Url -OutFile $zip -UseBasicParsing
        New-Item -ItemType Directory -Force -Path $base | Out-Null
        Expand-Archive -Path $zip -DestinationPath $base -Force
        Remove-Item $zip -Force
    }
    $bin = if ($BinSubdir) { Join-Path $base $BinSubdir } else { $base }
    # Some archives nest a single top-level folder.
    if (-not (Test-Path $bin)) {
        $inner = Get-ChildItem $base -Directory | Select-Object -First 1
        if ($inner) { $bin = Join-Path $inner.FullName $BinSubdir }
    }
    Add-SessionPath $bin
}

# --- ARM GNU toolchain (always portable: winget package is stale ~10.x) ------
function Ensure-ArmToolchain {
    $armBin = Get-ChildItem -Path $ArmRoot -Recurse -Filter 'arm-none-eabi-gcc.exe' `
        -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($armBin) {
        $script:ArmRoot = Split-Path -Parent $armBin.Directory.FullName
        Add-SessionPath (Split-Path $armBin.FullName)
        Ok "ARM toolchain present: $($armBin.Directory)"
        return
    }
    if ($UseInstalledTools) { Die "ARM GNU $ARM_VER not found in $ArmRoot." }
    Info "Downloading ARM GNU toolchain $ARM_VER (~500 MB, one-time)..."
    New-Item -ItemType Directory -Force -Path $ArmRoot | Out-Null
    $zip = Join-Path $env:TEMP $ARM_ZIP
    Invoke-WebRequest -Uri $ARM_URL -OutFile $zip -UseBasicParsing
    Info 'Extracting ARM toolchain...'
    Expand-Archive -Path $zip -DestinationPath $ArmRoot -Force
    Remove-Item $zip -Force
    $armBin = Get-ChildItem -Path $ArmRoot -Recurse -Filter 'arm-none-eabi-gcc.exe' |
        Select-Object -First 1
    if (-not $armBin) { Die 'ARM toolchain extraction failed.' }
    $script:ArmRoot = Split-Path -Parent $armBin.Directory.FullName
    Add-SessionPath (Split-Path $armBin.FullName)
    Ok "ARM toolchain ready: $($armBin.Directory)"
}

# --- Host C/C++ compiler (for pico-sdk host tools: pioasm, picotool) ---------
# These run on the PC, not the RP2350, so they need a NATIVE compiler - the
# ARM cross-compiler cannot build them. Use MSVC if present, else a portable
# MinGW-w64.
function Ensure-HostCompiler {
    if (Have 'cl') { Ok 'Host compiler: MSVC (cl) on PATH'; return }
    # g++ on PATH that is NOT the arm-none-eabi cross-compiler
    $g = Get-Command 'g++' -ErrorAction SilentlyContinue
    if ($g -and $g.Source -notlike '*arm-none-eabi*') {
        Ok "Host compiler: $($g.Source)"
        $env:CC = 'gcc'; $env:CXX = 'g++'
        return
    }
    $mwRoot = Join-Path $ToolsHome 'mingw64'
    $gpp = Get-ChildItem -Path $mwRoot -Recurse -Filter 'g++.exe' `
        -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $gpp) {
        if ($UseInstalledTools) { Die 'Put a native C/C++ compiler on PATH; installed-tools mode does not download one.' }
        Info 'Downloading portable MinGW-w64 host compiler (~120 MB, one-time)...'
        New-Item -ItemType Directory -Force -Path $mwRoot | Out-Null
        $zip = Join-Path $env:TEMP 'winlibs-mingw.zip'
        Invoke-WebRequest -UseBasicParsing -OutFile $zip -Uri $MINGW_URL
        Info 'Extracting MinGW-w64...'
        Expand-Archive -Path $zip -DestinationPath $mwRoot -Force
        Remove-Item $zip -Force
        $gpp = Get-ChildItem -Path $mwRoot -Recurse -Filter 'g++.exe' |
            Select-Object -First 1
    }
    if (-not $gpp) { Die 'Failed to provide a host C++ compiler.' }
    $binDir = Split-Path $gpp.FullName
    Add-SessionPath $binDir
    # Force host sub-builds (pioasm/picotool ExternalProjects) to use MinGW.
    $env:CC  = (Join-Path $binDir 'gcc.exe')
    $env:CXX = (Join-Path $binDir 'g++.exe')
    Ok "Host compiler: $binDir"
}

# --- Pico SDK 2.2.0 + TinyUSB 0.20.0 (mirrors build-firmware.yml) -------------
function Assert-GitPin ([string]$Path, [string]$Expected) {
    $actual = & git -C $Path rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or "$actual".Trim() -ne $Expected) {
        Die "Dependency pin mismatch at $Path. Expected $Expected; SDK checkouts are not updated automatically."
    }
}

function Ensure-PicoSdk {
    if (-not (Test-Path (Join-Path $SdkPath 'pico_sdk_init.cmake'))) {
        if ($UseInstalledTools) { Die "Installed Pico SDK not found at $SdkPath." }
        Info "Cloning Pico SDK $PICO_SDK_REF..."
        Invoke-GitQuiet clone --depth 1 --branch $PICO_SDK_REF `
            https://github.com/raspberrypi/pico-sdk.git $SdkPath
        if ($GitExit -ne 0) { Die 'Pico SDK clone failed.' }
        Invoke-GitQuiet -C $SdkPath submodule update --init --recursive
        if ($GitExit -ne 0) { Die 'Pico SDK submodule initialization failed.' }
        $tinyusb = Join-Path $SdkPath 'lib\tinyusb'
        Invoke-GitQuiet -C $tinyusb fetch --depth 1 origin "refs/tags/${TINYUSB_REF}:refs/tags/$TINYUSB_REF"
        if ($GitExit -ne 0) { Die 'TinyUSB fetch failed.' }
        Invoke-GitQuiet -C $tinyusb checkout --detach $TINYUSB_SHA
        if ($GitExit -ne 0) { Die 'TinyUSB pin checkout failed.' }
    } else {
        Ok 'Validating installed Pico SDK without modifying it.'
    }
    Assert-GitPin $SdkPath $PICO_SDK_SHA
    Assert-GitPin (Join-Path $SdkPath 'lib\tinyusb') $TINYUSB_SHA
    Assert-GitPin (Join-Path $SdkPath 'lib\btstack') $BTSTACK_SHA
    Assert-GitPin (Join-Path $SdkPath 'lib\cyw43-driver') $CYW43_SHA
}

# --- Locate the project: existing checkout, or clone under $ToolsHome --------
function Test-BridgeCheckout ($dir) {
    if (-not $dir) { return $false }
    $cml = Join-Path $dir 'CMakeLists.txt'
    return (Test-Path $cml) -and (Select-String -Path $cml -Pattern 'ds5-bridge' -Quiet)
}

# Runs git so NOTHING reaches the pipeline: every stream is written to the
# host instead. Two PowerShell hazards handled here:
#  1. Uncaptured native stdout inside a function becomes its return value.
#  2. With $ErrorActionPreference='Stop', git writing to stderr (it uses it
#     for normal progress, e.g. "Already on 'master'") raises a terminating
#     NativeCommandError. So we relax it locally and judge by exit code.
# Sets $script:GitExit to git's real exit code; emits nothing.
function Invoke-GitQuiet {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & git @args 2>&1 | ForEach-Object { Write-Host $_ }
        $script:GitExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prev
    }
}

# Sets $script:RepoRoot (does NOT return it) so stray git output can never
# contaminate the value, regardless of which stream git writes to.
function Resolve-RepoRoot {
    $script:RepoRoot = $null
    # Run from inside a checkout? (script at <repo>/tools/ or at <repo>/)
    foreach ($cand in @((Split-Path -Parent $PSScriptRoot), $PSScriptRoot)) {
        if (Test-BridgeCheckout $cand) {
            Ok "Using existing checkout: $cand"
            $script:RepoRoot = $cand
            return
        }
    }
    # Cached source is never reset or overwritten; updates are explicit Git work.
    if (Test-Path (Join-Path $ClonePath '.git')) {
        $origin = & git -C $ClonePath remote get-url origin
        if ($LASTEXITCODE -ne 0 -or "$origin".Trim() -ne $Repo) { Die "Cached clone origin differs from $Repo." }
        if ($Ref) {
            $wanted = & git -C $ClonePath rev-parse "$Ref^{commit}"
            if ($LASTEXITCODE -ne 0) { Die "Ref $Ref is not present in the cached clone." }
            Assert-GitPin $ClonePath ("$wanted".Trim())
        }
        Ok "Using cached checkout as-is: $ClonePath"
    } else {
        Info "Cloning $Repo into $ClonePath ..."
        if ($Ref) {
            Invoke-GitQuiet clone $Repo $ClonePath
            if ($GitExit -ne 0) { Die "Failed to clone $Repo" }
            Invoke-GitQuiet -C $ClonePath checkout --detach $Ref
        } else {
            Invoke-GitQuiet clone $Repo $ClonePath
        }
        if ($GitExit -ne 0) { Die "Failed to clone or select the requested ref in $Repo" }
    }
    if (-not (Test-BridgeCheckout $ClonePath)) { Die "Clone at $ClonePath is not a ControllerBridge Pico project." }
    $script:RepoRoot = $ClonePath
}

# --- Real Python 3 (CMake's FindPython3 needs one) --------------------------
# Clean Windows / Windows Sandbox ships a Microsoft Store "python.exe" alias
# under \WindowsApps that is NOT an interpreter - Get-Command finds it but
# CMake rejects it. Resolve a genuine interpreter and pass it to CMake.
function Test-RealPython ($exe) {
    if (-not $exe) { return $false }
    if ("$exe" -like '*\WindowsApps\*') { return $false }   # Store alias stub
    try {
        $v = (& $exe --version 2>&1 | Out-String)
        return ($v -match 'Python\s+3\.')
    } catch { return $false }
}

function Resolve-Python {
    if (Have 'py') {
        try {
            $p = (& py -3 -c 'import sys;print(sys.executable)' 2>$null | Select-Object -Last 1)
            if (Test-RealPython $p) { $script:PythonExe = "$p"; Ok "Python: $p"; return }
        } catch {}
    }
    foreach ($name in @('python', 'python3')) {
        $c = Get-Command $name -ErrorAction SilentlyContinue
        if ($c -and (Test-RealPython $c.Source)) {
            $script:PythonExe = $c.Source; Ok "Python: $($c.Source)"; return
        }
    }
    if ($useWinget) {
        Info 'Installing Python 3.12 via winget...'
        winget install --id Python.Python.3.12 --exact --silent --accept-source-agreements `
            --accept-package-agreements --disable-interactivity | Out-Host
        Add-CommonToolPaths
    }
    $globs = @(
        (Join-Path $env:LOCALAPPDATA 'Programs\Python\Python3*\python.exe'),
        (Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages\Python.Python.3.12_*\python.exe'),
        (Join-Path $env:ProgramFiles 'Python3*\python.exe'),
        'C:\Python3*\python.exe'
    )
    foreach ($g in $globs) {
        $hit = Get-ChildItem -Path $g -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit -and (Test-RealPython $hit.FullName)) {
            $script:PythonExe = $hit.FullName
            Add-SessionPath (Split-Path $hit.FullName)
            Ok "Python: $($hit.FullName)"; return
        }
    }
    if ($UseInstalledTools) { Die 'A working Python 3 interpreter is required in installed-tools mode.' }
    # Last resort: portable embeddable build (enough for the SDK's scripts).
    Info 'Installing portable Python (embeddable)...'
    $pyDir = Join-Path $ToolsHome 'python'
    if (-not (Test-Path (Join-Path $pyDir 'python.exe'))) {
        $zip = Join-Path $env:TEMP 'python-embed.zip'
        Invoke-WebRequest -UseBasicParsing -OutFile $zip `
            -Uri 'https://www.python.org/ftp/python/3.12.7/python-3.12.7-embed-amd64.zip'
        New-Item -ItemType Directory -Force -Path $pyDir | Out-Null
        Expand-Archive -Path $zip -DestinationPath $pyDir -Force
        Remove-Item $zip -Force
    }
    $pyExe = Join-Path $pyDir 'python.exe'
    if (Test-RealPython $pyExe) {
        $script:PythonExe = $pyExe; Add-SessionPath $pyDir; Ok "Python: $pyExe"; return
    }
    Die 'Could not provide a working Python 3 interpreter.'
}

# ---------------------------------------------------------------------------- #
#  Main                                                                        #
# ---------------------------------------------------------------------------- #
Info "ControllerBridge Pico 2 W builder (rev $SCRIPT_REV) - variant: $Variant"
if (-not $UseInstalledTools) { New-Item -ItemType Directory -Force -Path $ToolsHome | Out-Null }
Add-CommonToolPaths

$useWinget = $false
if ($UseInstalledTools) {
    foreach ($command in @('git', 'cmake', 'ninja')) {
        if (-not (Have $command)) { Die "$command is required in installed-tools mode." }
    }
} else {
    $useWinget = Initialize-PackageManager

    Ensure-Tool -Command 'git' -WingetId 'Git.Git' -WingetAvailable $useWinget -PortableInstall {
        Install-PortableArchiveTool -Name 'git' `
            -Url 'https://github.com/git-for-windows/git/releases/download/v2.47.1.windows.1/MinGit-2.47.1-64-bit.zip' `
            -BinSubdir 'cmd'
    }
    Ensure-Tool -Command 'cmake' -WingetId 'Kitware.CMake' -WingetAvailable $useWinget -PortableInstall {
        Install-PortableArchiveTool -Name 'cmake' `
            -Url 'https://github.com/Kitware/CMake/releases/download/v3.31.3/cmake-3.31.3-windows-x86_64.zip' `
            -BinSubdir 'cmake-3.31.3-windows-x86_64\bin'
    }
    Ensure-Tool -Command 'ninja' -WingetId 'Ninja-build.Ninja' -WingetAvailable $useWinget -PortableInstall {
        Install-PortableArchiveTool -Name 'ninja' `
            -Url 'https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip' ''
    }
}
Resolve-Python   # sets $script:PythonExe (handles the Store alias stub)

Ensure-ArmToolchain
$compilerVersion = & arm-none-eabi-gcc --version
if ($LASTEXITCODE -ne 0 -or ($compilerVersion -join "`n") -notmatch '14\.2\.Rel1') {
    Die "This repository requires ARM GNU $ARM_VER."
}
Ensure-HostCompiler
Ensure-PicoSdk

# --- Locate / fetch the project source --------------------------------------
Resolve-RepoRoot   # sets $script:RepoRoot
if (-not $RepoRoot -or -not (Test-BridgeCheckout $RepoRoot)) {
    Die "Could not locate the ControllerBridge Pico source (resolved: '$RepoRoot')."
}
Ok "Project source: $RepoRoot"

# These vendor files belong to the source snapshot, not Git submodules.
if ($Variant -ne 'ns2pro') {
    foreach ($vendor in @('lib/WDL/WDL/resample.cpp', 'lib/opus/CMakeLists.txt')) {
        if (-not (Test-Path -LiteralPath (Join-Path $RepoRoot $vendor))) { Die "Missing vendor source: $vendor" }
    }
}

# --- Configure + build -------------------------------------------------------
$buildDir = Join-Path $RepoRoot "build\$Variant"
if ($Clean -and (Test-Path $buildDir)) {
    $expected = [IO.Path]::GetFullPath((Join-Path $RepoRoot "build\$Variant"))
    $resolved = (Resolve-Path -LiteralPath $buildDir).Path
    if ($resolved -ne $expected) {
        Die 'Refusing to clean a redirected or unexpected build directory.'
    }
    $cursor = Get-Item -LiteralPath $resolved
    while ($cursor.FullName -ne $RepoRoot) {
        if (($cursor.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or -not $cursor.Parent) {
            Die 'Refusing to clean through a redirected build parent.'
        }
        $cursor = $cursor.Parent
    }
    Info "Cleaning $buildDir..."
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

$cmakeArgs = @(
    '-S', $RepoRoot, '-B', $buildDir, '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release',
    "-DPICO_SDK_PATH=$SdkPath",
    "-DPICO_TOOLCHAIN_PATH=$ArmRoot",
    '-DCONTROLLERBRIDGE_USE_VSCODE_SDK=OFF',
    '-DPICO_BOARD=pico2_w', '-DPICO_W_BUILD=OFF',
    '-DENABLE_NS2PRO=OFF', '-DENABLE_AUTO_PROFILE=ON',
    '-DENABLE_SERIAL=OFF', '-DENABLE_VERBOSE=OFF', '-DENABLE_WAKE_HID=OFF',
    "-DVERSION=$Version",
    "-DPython3_EXECUTABLE=$($PythonExe -replace '\\','/')"
)
switch ($Variant) {
    'debug' { $cmakeArgs += @('-DENABLE_SERIAL=ON', '-DENABLE_VERBOSE=ON') }
    'standard' { $cmakeArgs += @('-DENABLE_AUTO_PROFILE=OFF') }
    'ns2pro' { $cmakeArgs += @('-DENABLE_NS2PRO=ON', '-DENABLE_AUTO_PROFILE=OFF') }
}

Info "Configuring: cmake $($cmakeArgs -join ' ')"
& cmake @cmakeArgs
if ($LASTEXITCODE -ne 0) { Die 'CMake configure failed.' }

Info 'Building firmware...'
& cmake --build $buildDir --target ds5-bridge --parallel $Jobs
if ($LASTEXITCODE -ne 0) { Die 'Build failed.' }

# --- Collect output ----------------------------------------------------------
$uf2 = Join-Path $buildDir 'ds5-bridge.uf2'
if (-not (Test-Path $uf2)) { Die "Expected $uf2 was not produced." }

$outName = switch ($Variant) {
    'unified' { 'controllerbridge-pico2w-unified.uf2' }
    'ns2pro' { 'ns2pro-bridge-pico2w.uf2' }
    'standard' { 'controllerbridge-pico2w-ds5.uf2' }
    'debug' { 'controllerbridge-pico2w-unified-debug.uf2' }
}
$outputDir = Join-Path $RepoRoot "artifacts\$Variant"
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
$output = Join-Path $outputDir $outName
Copy-Item -LiteralPath $uf2 -Destination $output -Force
$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()

Write-Host ''
Ok  "Build succeeded! Firmware ($Variant):"
Ok  "  $output"
Ok  "  SHA256 $hash"
Write-Host ''
Info 'Build only. No device was accessed or flashed.'
