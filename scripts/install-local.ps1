# Builds the installer and installs (or updates) Loki on this computer, with a desktop shortcut
# and the .loki file association. Per-user, no administrator rights needed.
#   .\scripts\install-local.ps1                 # install to F:\Programas\Loki
#   .\scripts\install-local.ps1 -Dir D:\Apps\Loki -SkipBuild
param(
    [string]$Dir = 'F:\Programas\Loki',
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

Get-Process loki -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$Dir*" } | ForEach-Object {
    throw "Loki is running from $Dir; close it before updating."
}
if ($SkipBuild) { & (Join-Path $PSScriptRoot 'make-installer.ps1') -SkipBuild } else { & (Join-Path $PSScriptRoot 'make-installer.ps1') }
$setup = Get-ChildItem (Join-Path $root 'build\installer') -Filter 'Loki-*-setup.exe' |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
$p = Start-Process -FilePath $setup.FullName -Wait -PassThru -ArgumentList @(
    '/VERYSILENT', '/CURRENTUSER', '/SUPPRESSMSGBOXES', '/TASKS="desktopicon,associate"', "/DIR=$Dir")
if ($p.ExitCode -ne 0) { throw "Setup failed with exit code $($p.ExitCode)" }
$desktop = [Environment]::GetFolderPath('Desktop')
"Loki installed in $Dir"
"Desktop shortcut: $(Join-Path $desktop 'Loki.lnk')"
