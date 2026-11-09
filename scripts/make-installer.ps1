# Builds the Windows installer (D27): Release build, staging with windeployqt, Inno Setup.
#   .\scripts\make-installer.ps1 [-SkipBuild] [-Iscc F:\dev\InnoSetup\ISCC.exe]
# Output: build\installer\Loki-<version>-setup.exe
param(
    [switch]$SkipBuild,
    [string]$Iscc = 'F:\dev\InnoSetup\ISCC.exe'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'dev-env.ps1') | Out-Null

if (-not $SkipBuild) {
    cmake --preset windows-msvc-release | Out-Null
    cmake --build --preset windows-msvc-release --target loki
    if ($LASTEXITCODE -ne 0) { throw 'Release build failed' }
}

$bin = Join-Path $root 'build\windows-msvc-release\bin'
$out = Join-Path $root 'build\installer'
$stage = Join-Path $out 'stage'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage, (Join-Path $stage 'licenses') | Out-Null

# Application and its own DLLs (ADS, FFmpeg, spdlog, ...); never test or benchmark binaries.
Copy-Item (Join-Path $bin 'loki.exe') $stage
Get-ChildItem $bin -Filter *.dll | Where-Object { $_.Name -notmatch '^gtest' } | Copy-Item -Destination $stage

# Qt (dynamic, LGPL): windeployqt copies the Qt DLLs and plugins next to loki.exe.
$windeployqt = Join-Path $env:LOKI_QT_DIR 'bin\windeployqt.exe'
& $windeployqt --release --no-translations --no-system-d3d-compiler --no-opengl-sw --no-compiler-runtime `
    (Join-Path $stage 'loki.exe') | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'windeployqt failed' }

# MSVC runtime, app-local (no separate redistributable installer needed).
$redist = Get-ChildItem (Join-Path $env:VCINSTALLDIR 'Redist\MSVC') -Directory |
    Where-Object { Test-Path (Join-Path $_.FullName 'x64\Microsoft.VC143.CRT') } |
    Sort-Object Name -Descending | Select-Object -First 1
Copy-Item (Join-Path $redist.FullName 'x64\Microsoft.VC143.CRT\*.dll') $stage

# Licenses: our notice, every vcpkg package's copyright, Qt ADS.
$licenses = Join-Path $stage 'licenses'
Copy-Item (Join-Path $root 'installer\NOTICE.txt') $licenses
Copy-Item (Join-Path $root 'installer\licenses\*.txt') $licenses # LGPL-3.0 / GPL-3.0 (Qt)
Copy-Item (Join-Path $root 'docs\LICENSING.md') $licenses
$share = Join-Path $root 'build\windows-msvc-release\vcpkg_installed\x64-windows\share'
Get-ChildItem $share -Directory | ForEach-Object {
    $copyright = Join-Path $_.FullName 'copyright'
    if ((Test-Path $copyright) -and ($_.Name -notmatch '^(vcpkg|gtest|pkgconf)')) {
        Copy-Item $copyright (Join-Path $licenses "$($_.Name).txt")
    }
}
Copy-Item (Join-Path $root 'build\windows-msvc-release\_deps\qtads-src\LICENSE') (Join-Path $licenses 'qtadvanceddocking.txt')

$cmakeText = Get-Content -Raw (Join-Path $root 'CMakeLists.txt')
$version = [regex]::Match($cmakeText, 'project\(Loki\s+VERSION\s+([0-9.]+)').Groups[1].Value
if (-not $version) { throw 'Cannot read the project version from CMakeLists.txt' }
& $Iscc /Q "/DAppVersion=$version" "/DStageDir=$stage" "/DOutputDir=$out" (Join-Path $root 'installer\loki.iss')
if ($LASTEXITCODE -ne 0) { throw 'Inno Setup failed' }
$setup = Join-Path $out "Loki-$version-setup.exe"
"Installer: $setup ($([math]::Round((Get-Item $setup).Length / 1MB, 1)) MB)"
