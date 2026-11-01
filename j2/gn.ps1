#Requires -Version 7.0
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
. "$PSScriptRoot\..\j1-base\toolchain-env.ps1"
Set-ToolchainEnv | Out-Null
$python = (Get-Command python -ErrorAction Stop).Source
Push-Location $src
try {
  $command, $rest = $args
  & "$root\toolchains\gn\gn.exe" $command 'out\content-arm' @rest "--script-executable=$python" 2>&1 |
    Where-Object { "$_" -notmatch 'DeprecationWarning|utcfromtimestamp' }
} finally { Pop-Location }
