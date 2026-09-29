param(
    [Parameter(Mandatory=$true)][string]$BuildDir,
    [Parameter(Mandatory=$true)][string]$OutputZip,
    [Parameter(Mandatory=$true)][string]$QtBinDir,
    [Parameter(Mandatory=$true)][string]$VcpkgBinDir
)
$ErrorActionPreference = "Stop"
$ProjectDir = Split-Path -Parent $PSScriptRoot

if (-not [System.IO.Path]::IsPathRooted($OutputZip)) {
    throw "Use an absolute output path"
}

# DLL names a PE file imports (needs dumpbin, from the MSVC developer shell).
function Get-Imports([string]$Path) {
    & dumpbin /nologo /dependents $Path |
        ForEach-Object { $_.Trim() } |
        Where-Object { $_ -match '^[\w.\-]+\.dll$' }
}

$StageDir = Join-Path ([System.IO.Path]::GetTempPath()) ("ynotbit-package-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $StageDir | Out-Null
try {
    Copy-Item "$BuildDir\ynotbit.exe" "$StageDir\ynotbit.exe"

    # A Widgets app draws with the raster engine: no software OpenGL, no
    # Direct3D/DXC shader compilers (those serve Qt Quick). The C++ runtime is
    # shipped as DLLs below rather than as an installer the user must run.
    # Translations are compiled into ynotbit.exe.
    & "$QtBinDir\windeployqt.exe" --release --no-translations --no-opengl-sw `
        --no-system-d3d-compiler --no-system-dxc-compiler --no-compiler-runtime `
        "$StageDir\ynotbit.exe"
    if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }

    # Only the vcpkg DLLs actually loaded, following imports transitively --
    # the vcpkg bin folder also holds build tools' DLLs (tcl, pkgconf, ...).
    $pending = [System.Collections.Generic.Queue[string]]::new()
    $pending.Enqueue("$StageDir\ynotbit.exe")
    Get-ChildItem -Recurse -Filter *.dll $StageDir | ForEach-Object { $pending.Enqueue($_.FullName) }
    while ($pending.Count -gt 0) {
        foreach ($dll in Get-Imports $pending.Dequeue()) {
            $source = Join-Path $VcpkgBinDir $dll
            $target = Join-Path $StageDir $dll
            if ((Test-Path $source) -and -not (Test-Path $target)) {
                Copy-Item $source $target
                $pending.Enqueue($target)
            }
        }
    }

    # The MSVC runtime, app-local, so no Visual C++ Redistributable install is needed.
    if (-not $env:VCToolsRedistDir) { throw "VCToolsRedistDir is not set; run from an MSVC developer shell" }
    $Crt = Get-ChildItem -Directory "$env:VCToolsRedistDir\x64" -Filter "Microsoft.VC*.CRT" | Select-Object -First 1
    if (-not $Crt) { throw "MSVC CRT redistributable folder not found" }
    Copy-Item "$($Crt.FullName)\*.dll" $StageDir

    $LicenseDir = Join-Path $StageDir "licenses"
    New-Item -ItemType Directory -Path $LicenseDir | Out-Null
    Copy-Item "$ProjectDir\licenses\*" $LicenseDir
    Copy-Item "$ProjectDir\third_party\notbit\COPYING" (Join-Path $LicenseDir "notbit.txt")
    Copy-Item "$ProjectDir\LICENSE" $LicenseDir
    Copy-Item "$ProjectDir\THIRD_PARTY.md" $LicenseDir
    # Licences of the Windows-only libraries the notbit engine links, as vcpkg
    # records them.
    $VcpkgShare = Join-Path (Split-Path -Parent $VcpkgBinDir) "share"
    foreach ($port in "pthreads", "getopt-win32") {
        $copyright = Join-Path $VcpkgShare "$port\copyright"
        if (Test-Path $copyright) { Copy-Item $copyright (Join-Path $LicenseDir "$port.txt") }
    }

    Get-ChildItem -Recurse -File $StageDir | Sort-Object FullName |
        ForEach-Object { "{0,10}  {1}" -f $_.Length, $_.FullName.Substring($StageDir.Length + 1) }

    $OutputDir = Split-Path -Parent $OutputZip
    if ($OutputDir -and -not (Test-Path $OutputDir)) {
        New-Item -ItemType Directory -Path $OutputDir | Out-Null
    }
    if (Test-Path $OutputZip) { Remove-Item $OutputZip }
    Compress-Archive -Path "$StageDir\*" -DestinationPath $OutputZip

    Write-Output $OutputZip
} finally {
    Remove-Item -Recurse -Force $StageDir
}
