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

$StageDir = Join-Path ([System.IO.Path]::GetTempPath()) ("ynotbit-package-" + [System.Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $StageDir | Out-Null
try {
    Copy-Item "$BuildDir\ynotbit.exe" "$StageDir\ynotbit.exe"

    & "$QtBinDir\windeployqt.exe" --release --no-translations "$StageDir\ynotbit.exe"
    if ($LASTEXITCODE -ne 0) { throw "windeployqt failed" }

    Copy-Item "$VcpkgBinDir\*.dll" $StageDir

    $LicenseDir = Join-Path $StageDir "licenses"
    New-Item -ItemType Directory -Path $LicenseDir | Out-Null
    Copy-Item "$ProjectDir\licenses\*" $LicenseDir
    Copy-Item "$ProjectDir\third_party\notbit\COPYING" (Join-Path $LicenseDir "notbit.txt")
    Copy-Item "$ProjectDir\LICENSE" $LicenseDir
    Copy-Item "$ProjectDir\THIRD_PARTY.md" $LicenseDir

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
