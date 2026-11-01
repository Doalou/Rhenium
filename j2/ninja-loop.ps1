#Requires -Version 7.0
<#
.SYNOPSIS
  Enchaîne ninja -n rhenium_content et fetch.ps1 -Missing tant que ninja réclame une source
  absente de l'arbre partiel. S'arrête sur la première autre erreur.
#>
param([int] $MaxRounds = 60, [string] $Log = "$env:TEMP\rhenium-ninja-loop.log")

$ErrorActionPreference = 'Continue'
$src = (Resolve-Path "$PSScriptRoot\..\chromium\src").Path
$root = Split-Path (Split-Path $src)
. "$PSScriptRoot\..\j1-base\toolchain-env.ps1"
Set-ToolchainEnv | Out-Null
$seen = @{}
for ($round = 1; $round -le $MaxRounds; $round++) {
  cmd /c "`"$root\toolchains\ninja\ninja.exe`" -C `"$src\out\content-arm`" -n rhenium_content < NUL > `"$Log`" 2>&1"
  $text = Get-Content $Log -Raw
  if ($text -match 'Regenerating ninja files' -and $text -notmatch 'ninja: error') {
    try { & "$PSScriptRoot\build-content.ps1" -GenOnly *> "$Log.gen" } catch { }
    cmd /c "`"$root\toolchains\ninja\ninja.exe`" -C `"$src\out\content-arm`" -n rhenium_content < NUL > `"$Log`" 2>&1"
    $text = Get-Content $Log -Raw
  }
  if ($LASTEXITCODE -eq 0) {
    Write-Host "ninja -n passe (tour $round) : $((Get-Content $Log | Select-Object -Last 1))" -ForegroundColor Green
    return
  }
  $missing = $null
  if ($text -match "ninja: error: '\.\./\.\./([^']+)', needed by") { $missing = $Matches[1] }
  if (-not $missing -or $seen.ContainsKey($missing)) {
    Write-Host "Arrêt au tour $round : erreur autre qu'une source manquante." -ForegroundColor Yellow
    Get-Content $Log | Select-Object -Last 15 | ForEach-Object { Write-Host $_ }
    return
  }
  $seen[$missing] = $true
  Write-Host "tour $round : $missing" -ForegroundColor Cyan
  try { & "$PSScriptRoot\fetch.ps1" -Missing $missing *> "$Log.fetch" } catch { Write-Host "fetch : $_" }
}
Write-Host "Limite de $MaxRounds tours atteinte." -ForegroundColor Yellow
