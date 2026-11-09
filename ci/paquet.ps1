#Requires -Version 7.0
<#
.SYNOPSIS
  Compile l'app Rhénium (Chromium 156), l'empaquette et réunit dans -Dest ce qu'on installe
  sur le téléphone : RheniumShell_arm.appx, RheniumDev.cer et LISEZMOI.txt (notice).
  Appelé par le workflow .github/workflows/paquet.yml ; marche aussi à la main.
.DESCRIPTION
  Les sources de l'app viennent de la copie où se trouve ce script ; l'arbre de Chromium
  (Root\chromium-next), les outils (Root\toolchains) et le certificat (Root\sonde\certs) sont
  ceux du dépôt de travail, pour que chaque version soit signée par le même certificat.
.EXAMPLE
  ./ci/paquet.ps1 -Dest $env:TEMP\rhenium
#>
param(
  [string] $Root = 'F:\Rhenium',
  [Parameter(Mandatory)] [string] $Dest
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$repo = (Resolve-Path "$PSScriptRoot\..").Path
$certDir = "$Root\sonde\certs"
$outDir = "$Root\chromium-next\src\out\content-arm"

# Sans le certificat du dépôt de travail, dev-cert.ps1 en créerait un autre : les téléphones
# qui ont l'ancien refuseraient la mise à jour.
foreach ($f in "$certDir\RheniumDev.pfx", "$certDir\RheniumDev.cer") {
  if (-not (Test-Path $f)) { throw "$f absent : le certificat de signature doit rester le même d'une version à l'autre." }
}
if (-not (Test-Path "$Root\chromium-next\src\.gn")) {
  throw "Arbre de Chromium 156 absent de $Root\chromium-next\src (voir maj-chromium\README.md)."
}

# Composants livrés avec l'app, aux versions et empreintes fixées par les scripts.
& "$repo\j4\fetch-ubol.ps1"
& "$repo\j5\fetch-pdfjs.ps1"

# Correctifs, gn, ninja (moteur et coquille XAML). S'arrête à la première erreur ; un correctif
# qui ne s'applique pas signale un arbre modifié à la main depuis le dernier regen-patches.ps1.
& "$repo\maj-chromium\build-content.ps1"

& "$repo\j2\package-shell.ps1" -OutDir $outDir -CertDir $certDir

$manifest = [xml] (Get-Content "$repo\j2\out\pkg\AppxManifest.xml" -Raw)
$version = $manifest.Package.Identity.Version
$chromium = (Get-Content "$Root\chromium-next\src\chrome\VERSION") -replace '^\w+=', '' -join '.'

New-Item -ItemType Directory -Force $Dest | Out-Null
Get-ChildItem $Dest | Remove-Item -Recurse -Force
Copy-Item "$repo\j2\out\RheniumShell_arm.appx", "$certDir\RheniumDev.cer" $Dest
(Get-Content "$PSScriptRoot\notice.txt" -Raw).Replace('@VERSION@', $version).Replace('@CHROMIUM@', $chromium) |
  Set-Content "$Dest\LISEZMOI.txt" -Encoding utf8
Set-Content "$Dest\version.txt" $version -NoNewline -Encoding utf8NoBOM

Write-Host "Rhénium $version (Chromium $chromium) prêt dans $Dest :" -ForegroundColor Green
Get-ChildItem $Dest | ForEach-Object { Write-Host ("  {0,-24} {1,8:N1} Mo" -f $_.Name, ($_.Length / 1MB)) }
