#Requires -Version 7.0
<#
.SYNOPSIS
  Enchaîne gn gen (build-base.ps1 -GenOnly, avec les arguments donnés) et fetch.ps1 -Missing
  tant que gn gen réclame un fichier absent de l'arbre partiel. S'arrête sur la première autre erreur.
.EXAMPLE
  ./gen-loop.ps1
  ./gen-loop.ps1 -BuildArgs @{ NoRust = $true }
  ./gen-loop.ps1 -Script build-content.ps1 -MaxRounds 200
#>
param([string] $Script = 'build-base.ps1', [hashtable] $BuildArgs = @{}, [int] $MaxRounds = 60, [string] $Log = "$env:TEMP\rhenium-gen-loop-156.log")

$ErrorActionPreference = 'Continue'
$src = 'F:/Rhenium/chromium-next/src'
$seen = @{}
for ($round = 1; $round -le $MaxRounds; $round++) {
  try { & "$PSScriptRoot\$Script" -GenOnly @BuildArgs *> $Log } catch { }
  $text = Get-Content $Log -Raw
  if ($text -match 'Done\. Made \d+ targets') {
    Write-Host "gn gen passe (tour $round) : $($Matches[0])" -ForegroundColor Green
    return
  }
  $missing = $null
  if ($text -match "(?:Unable to load|Can't load input file|Could not read file)[^`"]*`"$([regex]::Escape($src))/([^`"]+)`"") { $missing = $Matches[1] }
  elseif ($text -match "I resolved this to `"$([regex]::Escape($src))/([^`"]+)`"") { $missing = $Matches[1] }
  elseif ($text -match "Unable to load `"//([^`"]+)`"") { $missing = $Matches[1] }
  elseif ($text -match "No such file or directory: '\.\./\.\./([^']+)'") { $missing = $Matches[1] }
  if (-not $missing -or $seen.ContainsKey($missing)) {
    Write-Host "Arrêt au tour $round : erreur autre qu'un fichier manquant." -ForegroundColor Yellow
    Get-Content $Log | Select-Object -First 30 | ForEach-Object { Write-Host $_ }
    return
  }
  $seen[$missing] = $true
  Write-Host "tour $round : $missing" -ForegroundColor Cyan
  try { & "$PSScriptRoot\fetch.ps1" -Missing $missing *> "$Log.fetch" } catch { Write-Host "fetch : $_" }
}
Write-Host "Limite de $MaxRounds tours atteinte." -ForegroundColor Yellow
