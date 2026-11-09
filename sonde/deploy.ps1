#Requires -Version 7.0
<#
.SYNOPSIS
  Installe un .appx (la sonde par défaut, ou l'app Rhénium) sur un téléphone Windows 10 Mobile en
  mode développeur.
.EXAMPLE
  ./deploy.ps1                                  # liste les téléphones visibles sur le réseau
  ./deploy.ps1 -Ip 192.168.1.42 -Pin ABC123     # premier déploiement (code d'appairage du téléphone)
  ./deploy.ps1 -Ip 192.168.1.42                 # redéploiement : mise à jour si la version monte
  ./deploy.ps1 -Ip 192.168.1.42 -Clean          # désinstalle d'abord (efface les données de l'app)
  ./deploy.ps1 -Ip 192.168.1.42 -Appx out\RheniumSonde_arm-msvc_sans-codegen.appx
  ./deploy.ps1 -Ip 192.168.1.42 -Uninstall
#>
param(
  [string] $Ip,
  [string] $Pin,
  [string] $Appx,
  [switch] $Clean,
  [switch] $Uninstall
)

$ErrorActionPreference = 'Stop'
$kits = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
# Les WinAppDeployCmd des SDK Windows 11 échouent avec Windows 10 Mobile (0x80070003, même pour « list ») :
# on prend le plus récent jusqu'au SDK 10.0.19041 inclus.
$tool = Get-ChildItem $kits -Directory | Where-Object { $_.Name -match '^\d+\.\d+' -and [version]$_.Name -le [version]'10.0.19041.0' } |
  Sort-Object { [version]$_.Name } | ForEach-Object { "$($_.FullName)\x86\WinAppDeployCmd.exe", "$($_.FullName)\x64\WinAppDeployCmd.exe" } |
  Where-Object { Test-Path $_ } | Select-Object -Last 1
if (-not $tool) { throw 'WinAppDeployCmd compatible W10M introuvable : installer le Windows 10 SDK 10.0.19041.' }

if (-not $Ip) {
  & $tool devices 5
  Write-Host "Relancer avec -Ip <adresse> (et -Pin <code> au premier appairage)." -ForegroundColor Yellow
  return
}

$target = @('-ip', $Ip)
if ($Pin) { $target += @('-pin', $Pin) }

if (-not $Appx -and -not $Uninstall) {
  $Appx = Get-ChildItem "$PSScriptRoot\out\RheniumSonde_arm-*.appx" -ErrorAction SilentlyContinue |
    Where-Object Name -notmatch 'sans-codegen' | Sort-Object LastWriteTime | Select-Object -Last 1 -ExpandProperty FullName
}
if (-not $Uninstall -and (-not $Appx -or -not (Test-Path $Appx))) { throw 'Aucun paquet ARM trouvé : lancer d''abord ./build.ps1.' }

# Nom et titre du paquet, lus dans son manifeste (la sonde par défaut, l'app Rhénium de ..\j2).
$identity = 'Rhenium.Sonde'; $title = 'Sonde Rhénium'
if ($Appx) {
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  $zip = [IO.Compression.ZipFile]::OpenRead((Resolve-Path $Appx))
  try {
    $reader = [IO.StreamReader]::new($zip.GetEntry('AppxManifest.xml').Open())
    [xml] $manifest = $reader.ReadToEnd()
    $reader.Dispose()
  } finally { $zip.Dispose() }
  $identity = $manifest.Package.Identity.Name
  $title = $manifest.Package.Properties.DisplayName
}

# Une version plus ancienne est mise à jour sur place, données de l'app comprises. La même version
# est laissée telle quelle, une plus récente refusée : les retirer (WinAppDeployCmd ne réinstalle
# pas un paquet de même version) efface les données, ce que seul -Clean demande.
$newVersion = if ($Appx) { [version] $manifest.Package.Identity.Version } else { $null }
$list = & $tool list @target
if ($LASTEXITCODE -ne 0) {
  $list | Out-Host
  throw "Liste des apps refusée (code $LASTEXITCODE) : téléphone verrouillé ?"
}
$installed = $list | Where-Object { $_ -match "^$([regex]::Escape($identity))_" } | ForEach-Object { $_.Trim() }
foreach ($name in $installed) {
  $oldVersion = [version] ($name -split '_')[1]
  if (-not ($Uninstall -or $Clean) -and $newVersion -and $oldVersion -eq $newVersion) {
    Write-Host "$name est déjà installé." -ForegroundColor Green
    return
  }
  if (-not ($Uninstall -or $Clean) -and $newVersion -and $oldVersion -gt $newVersion) {
    throw "$name est plus récent que le paquet : -Clean pour le remplacer (données effacées)."
  }
  if ($Uninstall -or $Clean -or -not $newVersion) {
    Write-Host "Désinstallation de $name (données de l'app effacées)"
    & $tool uninstall -package $name @target
  } else {
    Write-Host "Mise à jour de $oldVersion vers $newVersion, données gardées"
  }
}
if ($Uninstall) { return }

Write-Host "Installation de $Appx sur $Ip" -ForegroundColor Cyan
& $tool install -file (Resolve-Path $Appx) @target
if ($LASTEXITCODE -ne 0) { throw "Échec de l'installation (code $LASTEXITCODE)" }
Write-Host "Installé. Lancer « $title » depuis la liste des applications du téléphone." -ForegroundColor Green