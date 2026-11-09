#Requires -Version 7.0
<#
.SYNOPSIS
  Enchaîne ninja -n (simulation) et fetch.ps1 -Missing tant que ninja réclame un fichier
  absent de l'arbre partiel. S'arrête quand la simulation passe ou sur une autre erreur.
  Avec -Build, boucle sur la vraie compilation (build-content.ps1 -KeepGoing), qui se poursuit
  dès que ninja ne réclame plus de fichier.
.EXAMPLE
  ./ninja-loop.ps1
  ./ninja-loop.ps1 -Build -OutDir out\content-arm -Targets rhenium_content -Log $env:TEMP\ninja-content.log
#>
param(
  [string] $OutDir = 'out\uwp-arm',
  [string[]] $Targets = @('rhenium_base', 'rhenium_base_tests', 'rhenium_base_unittests'),
  [int] $MaxRounds = 100,
  [switch] $Build,
  [string] $Log = "$env:TEMP\rhenium-ninja-build.log"
)

$src = 'F:\Rhenium\chromium-next\src'
$ninja = 'F:\Rhenium\toolchains\ninja-156\ninja.exe'
$seen = @{}
for ($round = 1; $round -le $MaxRounds; $round++) {
  if ($Build) {
    try { & "$PSScriptRoot\build-content.ps1" -Targets $Targets -KeepGoing *> $Log } catch { }
    $all = Get-Content $Log -Raw
    $ok = $all -match 'Entering directory' -and $all -notmatch 'ninja: error|ninja: build stopped|FAILED: '
    $text = Get-Content $Log -Tail 5 | Out-String
  } else {
    $text = (cmd /c "`"$ninja`" -C $src\$OutDir -n $($Targets -join ' ') 2>&1 < NUL") -join "`n"
    $ok = $LASTEXITCODE -eq 0
  }
  if ($ok) {
    Write-Host "ninja passe (tour $round)" -ForegroundColor Green
    return
  }
  if ($text -notmatch "'\.\./\.\./([^']+)', needed by") {
    Write-Host "Arrêt au tour $round : erreur autre qu'un fichier manquant." -ForegroundColor Yellow
    Write-Host $text
    return
  }
  $missing = $Matches[1]
  if ($seen.ContainsKey($missing)) {
    Write-Host "Arrêt au tour $round : $missing toujours absent." -ForegroundColor Yellow
    return
  }
  $seen[$missing] = $true
  Write-Host "tour $round : $missing" -ForegroundColor Cyan
  & "$PSScriptRoot\fetch.ps1" -Missing $missing *> "$env:TEMP\rhenium-ninja-loop-fetch.log"
}
Write-Host "Limite de $MaxRounds tours atteinte." -ForegroundColor Yellow
