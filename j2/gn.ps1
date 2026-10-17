<#
.SYNOPSIS
  Lance gn sur out\content-arm avec l'environnement de la toolchain Rhénium, pour interroger le
  graphe (path, refs, desc…).
.EXAMPLE
  ./gn.ps1 path //:rhenium_content //third_party/devtools-frontend/src:devtools_frontend_resources
#>
$ErrorActionPreference = 'Stop'
$src = (Resolve-Path "$PSScriptRoot\..\chromium\src").Path
$root = Split-Path (Split-Path $src)
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msvc = & $vswhere -all -prerelease -products * -property installationPath |
  ForEach-Object { Get-ChildItem "$_\VC\Tools\MSVC" -Directory -ErrorAction SilentlyContinue } |
  Where-Object { Test-Path "$($_.FullName)\lib\arm\libcmt.lib" } | Sort-Object { [version]$_.Name } | Select-Object -Last 1
$env:RHENIUM_MSVC_DIR = $msvc.FullName
$env:RHENIUM_WINSDK_VERSION = '10.0.19041.0'
$env:RHENIUM_WINSDK_DIR = "${env:ProgramFiles(x86)}\Windows Kits\10"
$env:DEPOT_TOOLS_WIN_TOOLCHAIN = '0'
$python = (Get-Command python -ErrorAction Stop).Source
Push-Location $src
try {
  $command, $rest = $args
  & "$root\v8\buildtools\win\gn.exe" $command 'out\content-arm' @rest "--script-executable=$python" 2>&1 |
    Where-Object { "$_" -notmatch 'DeprecationWarning|utcfromtimestamp' }
} finally { Pop-Location }
