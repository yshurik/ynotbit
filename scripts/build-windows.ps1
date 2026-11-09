# Build ynotbit on Windows (MSVC + Ninja + vcpkg + Qt), optionally package a zip.
#
# Prerequisites (one-time):
#   - Visual Studio 2022 with "Desktop development with C++"
#   - Qt 6.8.3 msvc2022_64 incl. Qt Multimedia
#   - vcpkg (git clone https://github.com/microsoft/vcpkg; .\bootstrap-vcpkg.bat)
#   - pkgconfiglite (choco install pkgconfiglite -y)
#   - cmake, ninja (both ship with Visual Studio; or install separately)
#
# Usage (any PowerShell):
#   scripts\build-windows.ps1 -QtPrefix C:\Qt\6.8.3\msvc2022_64 -VcpkgRoot C:\vcpkg
#   scripts\build-windows.ps1 -QtPrefix ... -VcpkgRoot ... -Package
#
# Env fallbacks: $env:QT_PREFIX, $env:VCPKG_ROOT / $env:VCPKG_INSTALLATION_ROOT.
param(
    [string]$QtPrefix = $env:QT_PREFIX,
    [string]$VcpkgRoot = $(if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { $env:VCPKG_INSTALLATION_ROOT }),
    [string]$BuildDir = "build",
    [string]$OutputZip = "ynotbit-windows-x86_64.zip",
    [switch]$Package,
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

if (-not $QtPrefix -or -not (Test-Path "$QtPrefix\bin\qmake.exe")) {
    throw "Set -QtPrefix to the Qt msvc2022_64 dir, e.g. C:\Qt\6.8.3\msvc2022_64"
}
if (-not $VcpkgRoot -or -not (Test-Path "$VcpkgRoot\scripts\buildsystems\vcpkg.cmake")) {
    throw "Set -VcpkgRoot to your vcpkg checkout, e.g. C:\vcpkg"
}
# Qt-linked tests load Qt6*.dll from PATH; vcpkg copies only its own DLLs next to the binaries.
$env:Path = "$QtPrefix\bin;$env:Path"

# Submodules (secp256k1, miniupnp) are needed.
git submodule update --init --recursive
if ($LASTEXITCODE) { throw "git submodule failed" }

# Load the MSVC x64 environment if cl.exe is not on PATH.
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw "Visual Studio not found (vswhere missing)" }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw "Visual Studio C++ tools not found" }
    $envDump = cmd /c "`"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul && set"
    foreach ($line in $envDump) {
        if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
    }
}

if (-not (Get-Command pkg-config.exe -ErrorAction SilentlyContinue)) {
    Write-Warning "pkg-config not on PATH; run: choco install pkgconfiglite -y"
}

cmake -S . -B $BuildDir -G Ninja `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_TOOLCHAIN_FILE="$VcpkgRoot\scripts\buildsystems\vcpkg.cmake" `
    -DVCPKG_TARGET_TRIPLET=x64-windows `
    -DCMAKE_PREFIX_PATH="$QtPrefix"
if ($LASTEXITCODE) { throw "cmake configure failed" }

cmake --build $BuildDir --parallel
if ($LASTEXITCODE) { throw "build failed" }

if (-not $SkipTests) {
    ctest --test-dir $BuildDir --output-on-failure
    if ($LASTEXITCODE) { throw "tests failed" }
}

if ($Package) {
    $abs = (Resolve-Path $BuildDir).Path
    $zip = if ([IO.Path]::IsPathRooted($OutputZip)) { $OutputZip } else { Join-Path $repo $OutputZip }
    & "$PSScriptRoot\package-windows.ps1" `
        -BuildDir $abs `
        -OutputZip $zip `
        -QtBinDir "$QtPrefix\bin" `
        -VcpkgBinDir "$abs\vcpkg_installed\x64-windows\bin"
    if ($LASTEXITCODE) { throw "packaging failed" }
    Write-Host "Package: $zip"
} else {
    Write-Host "Built: $repo\$BuildDir\ynotbit.exe"
}
