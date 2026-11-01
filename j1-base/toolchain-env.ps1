#Requires -Version 7.0
# Environnement de la toolchain Rhénium lu par ..\build\toolchain (RHENIUM_*), commun à
# build-base.ps1, ..\j2\gn.ps1 et ..\j2\ninja-loop.ps1 : gn gen le recopie dans
# out\...\environment.arm, il doit donc être identique quel que soit le script qui le relance.
# À charger par « . "$PSScriptRoot\...\toolchain-env.ps1" » ; rend le dossier du toolset MSVC.

function Set-ToolchainEnv {
  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  $toolsets = & $vswhere -all -prerelease -products * -property installationPath |
    ForEach-Object { Get-ChildItem "$_\VC\Tools\MSVC" -Directory -ErrorAction SilentlyContinue } |
    Sort-Object { [version]$_.Name }
  $msvc = $toolsets | Where-Object { Test-Path "$($_.FullName)\lib\arm\libcmt.lib" } | Select-Object -Last 1
  if (-not $msvc) { throw 'Aucun toolset MSVC avec les bibliothèques ARM32 (14.44).' }
  $env:RHENIUM_MSVC_DIR = $msvc.FullName
  # ATL ARM32 peut venir d'un autre toolset (14.30 ici) : compatibilité binaire de MSVC 14.x.
  $atl = $toolsets | Where-Object { Test-Path "$($_.FullName)\atlmfc\lib\arm\atls.lib" } | Select-Object -Last 1
  if ($atl) { $env:RHENIUM_ATL_DIR = "$($atl.FullName)\atlmfc" }
  $env:RHENIUM_WINSDK_VERSION = '10.0.19041.0'
  $env:RHENIUM_WINSDK_DIR = "${env:ProgramFiles(x86)}\Windows Kits\10"
  $env:DEPOT_TOOLS_WIN_TOOLCHAIN = '0'
  $msvc.FullName
}
