# Loads the MSVC x64 developer environment and puts Qt on PATH for this PowerShell session.
# Usage (dot-source it):  . .\scripts\dev-env.ps1
$ErrorActionPreference = 'Stop'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found. Install Visual Studio 2022 Build Tools." }

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw "No Visual Studio installation with the MSVC x64 toolset was found." }

# VsDevCmd calls vswhere by name; make it resolvable to avoid a spurious error message.
$env:PATH = "$(Split-Path $vswhere);$env:PATH"
Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = [Environment]::GetEnvironmentVariable('VCPKG_ROOT', 'User') }
if (-not $env:LOKI_QT_DIR) { $env:LOKI_QT_DIR = [Environment]::GetEnvironmentVariable('LOKI_QT_DIR', 'User') }
if (-not $env:VCPKG_DEFAULT_BINARY_CACHE) {
    $env:VCPKG_DEFAULT_BINARY_CACHE = [Environment]::GetEnvironmentVariable('VCPKG_DEFAULT_BINARY_CACHE', 'User')
}
if (-not $env:VCPKG_ROOT) { throw "VCPKG_ROOT is not set." }
if (-not $env:LOKI_QT_DIR) { throw "LOKI_QT_DIR is not set (e.g. F:\dev\Qt\6.8.3\msvc2022_64)." }

$env:PATH = "$env:LOKI_QT_DIR\bin;$env:PATH"
Write-Host "Loki dev environment ready: MSVC $env:VCToolsVersion, Qt $env:LOKI_QT_DIR, vcpkg $env:VCPKG_ROOT"
