<#
.SYNOPSIS
  Installe la sonde Rhénium sur un téléphone Windows 10 Mobile en mode développeur.
.EXAMPLE
  ./deploy.ps1                                  # liste les téléphones visibles sur le réseau
  ./deploy.ps1 -Ip 192.168.1.42 -Pin ABC123     # premier déploiement (code d'appairage du téléphone)
  ./deploy.ps1 -Ip 192.168.1.42                 # redéploiement (désinstalle puis réinstalle)
  ./deploy.ps1 -Ip 192.168.1.42 -Appx out\RheniumSonde_arm-msvc_sans-codegen.appx
  ./deploy.ps1 -Ip 192.168.1.42 -Uninstall
#>
param(
  [string] $Ip,
  [string] $Pin,
  [string] $Appx,
  [switch] $Uninstall
)

$ErrorActionPreference = 'Stop'
$kits = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
# Les WinAppDeployCmd des SDK Windows 11 échouent avec Windows 10 Mobile (0x80070003, même pour « list ») :
# on prend le plus récent jusqu'au SDK 10.0.19041 inclus.
$tool = Get-ChildItem $kits -Directory | Where-Object { $_.Name -match '^\d+\.\d+' -and [version]$_.Name -le [version]'10.0.19041.0' } |
  Sort-Object { [version]$_.Name } | ForEach-Object { "$($_.FullName)\x64\WinAppDeployCmd.exe", "$($_.FullName)\x86\WinAppDeployCmd.exe" } |
  Where-Object { Test-Path $_ } | Select-Object -Last 1
if (-not $tool) { throw 'WinAppDeployCmd compatible W10M introuvable : installer le Windows 10 SDK 10.0.19041.' }

if (-not $Ip) {
  & $tool devices 5
  Write-Host "Relancer avec -Ip <adresse> (et -Pin <code> au premier appairage)." -ForegroundColor Yellow
  return
}

$target = @('-ip', $Ip)
if ($Pin) { $target += @('-pin', $Pin) }

# Retire une version déjà installée : WinAppDeployCmd ne réinstalle pas un paquet de même version.
$installed = (& $tool list @target) | Where-Object { $_ -match '^Rhenium\.Sonde_' } | ForEach-Object { $_.Trim() }
foreach ($name in $installed) {
  Write-Host "Désinstallation de $name"
  & $tool uninstall -package $name @target
}
if ($Uninstall) { return }

if (-not $Appx) {
  $Appx = Get-ChildItem "$PSScriptRoot\out\RheniumSonde_arm-*.appx" -ErrorAction SilentlyContinue |
    Where-Object Name -notmatch 'sans-codegen' | Sort-Object LastWriteTime | Select-Object -Last 1 -ExpandProperty FullName
}
if (-not $Appx -or -not (Test-Path $Appx)) { throw 'Aucun paquet ARM trouvé : lancer d''abord ./build.ps1.' }

Write-Host "Installation de $Appx sur $Ip" -ForegroundColor Cyan
& $tool install -file (Resolve-Path $Appx) @target
if ($LASTEXITCODE -ne 0) { throw "Échec de l'installation (code $LASTEXITCODE)" }
Write-Host 'Installé. Lancer « Sonde Rhénium » depuis la liste des applications du téléphone.' -ForegroundColor Green
