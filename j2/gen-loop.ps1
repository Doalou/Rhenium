#Requires -Version 7.0
<#
.SYNOPSIS
  Enchaîne gn gen (build-content.ps1 -GenOnly) et fetch.ps1 -Missing tant que gn gen réclame
  un fichier absent de l'arbre partiel. S'arrête sur la première autre erreur.
#>
param([int] $MaxRounds = 60, [string] $Log = "$env:TEMP\rhenium-gen-loop.log")

$ErrorActionPreference = 'Continue'
$src = (Resolve-Path "$PSScriptRoot\..\chromium\src").Path -replace '\\', '/'
$seen = @{}
for ($round = 1; $round -le $MaxRounds; $round++) {
  try { & "$PSScriptRoot\build-content.ps1" -GenOnly *> $Log } catch { }
  $text = Get-Content $Log -Raw
  if ($text -match 'Done\. Made \d+ targets') {
    Write-Host "gn gen passe (tour $round) : $($Matches[0])" -ForegroundColor Green
    return
  }
  $missing = $null
  if ($text -match "(?:Unable to load|Can't load input file)[^`"]*`"$([regex]::Escape($src))/([^`"]+)`"") { $missing = $Matches[1] }
  elseif ($text -match "No such file or directory: '\.\./\.\./([^']+)'") { $missing = $Matches[1] }
  elseif ($text -match "Unable to load `"//([^`"]+)`"") { $missing = $Matches[1] }
  if (-not $missing -or $seen.ContainsKey($missing)) {
    Write-Host "Arrêt au tour $round : erreur autre qu'un fichier manquant." -ForegroundColor Yellow
    Get-Content $Log | Where-Object { $_ -notmatch 'DeprecationWarning|utcfromtimestamp' } | Select-Object -First 30 | ForEach-Object { Write-Host $_ }
    return
  }
  $seen[$missing] = $true
  Write-Host "tour $round : $missing" -ForegroundColor Cyan
  try { & "$PSScriptRoot\fetch.ps1" -Missing $missing *> "$Log.fetch" } catch { Write-Host "fetch : $_" }
  if ($LASTEXITCODE -and $LASTEXITCODE -ne 0) { Get-Content "$Log.fetch" | Select-Object -Last 5 | ForEach-Object { Write-Host $_ } }
}
Write-Host "Limite de $MaxRounds tours atteinte." -ForegroundColor Yellow
