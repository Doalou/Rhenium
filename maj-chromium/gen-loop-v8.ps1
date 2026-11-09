#Requires -Version 7.0
<#
.SYNOPSIS
  Enchaîne gn gen (build-v8.ps1 -GenOnly) et fetch-v8.ps1 -Missing tant que gn gen réclame
  un fichier absent. S'arrête sur la première autre erreur.
#>
param([int] $MaxRounds = 40, [string] $Log = "$env:TEMP\rhenium-gen-loop-v8.log")

$ErrorActionPreference = 'Continue'
$root = 'F:/Rhenium/chromium-next/v8'
$seen = @{}
for ($round = 1; $round -le $MaxRounds; $round++) {
  try { & "$PSScriptRoot\build-v8.ps1" -GenOnly *> $Log } catch { }
  $text = Get-Content $Log -Raw
  if ($text -match 'Done\. Made \d+ targets') {
    Write-Host "gn gen passe (tour $round) : $($Matches[0])" -ForegroundColor Green
    return
  }
  $missing = $null
  if ($text -match "(?:Unable to load|Can't load input file)[^`"]*`"$([regex]::Escape($root))/([^`"]+)`"") { $missing = $Matches[1] }
  elseif ($text -match "Unable to load `"//([^`"]+)`"") { $missing = $Matches[1] }
  if (-not $missing -or $seen.ContainsKey($missing)) {
    Write-Host "Arrêt au tour $round : erreur autre qu'un fichier manquant." -ForegroundColor Yellow
    Get-Content $Log | Select-Object -First 30 | ForEach-Object { Write-Host $_ }
    return
  }
  $seen[$missing] = $true
  Write-Host "tour $round : $missing" -ForegroundColor Cyan
  try { & "$PSScriptRoot\fetch-v8.ps1" -Missing $missing *> "$Log.fetch" } catch { Write-Host "fetch : $_" }
}
Write-Host "Limite de $MaxRounds tours atteinte." -ForegroundColor Yellow
