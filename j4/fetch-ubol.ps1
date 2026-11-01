#Requires -Version 7.0
<#
.SYNOPSIS
  Récupère uBlock Origin Lite 2026.1018.2229, vérifie son empreinte et le décompresse dans
  out\ubol\uBOLite, d'où package-shell.ps1 le copie dans l'app (Extensions\uBOLite). Il déclare
  minimum_chrome_version 122 : Rhénium (Chromium 116) ignore cette clé et fournit les API qu'il
  appelle (voir README.md). uBO Lite est sous GPLv3 : composant distinct, livré avec sa licence
  et l'adresse de ses sources (ubol\SOURCES.txt).
.EXAMPLE
  ./fetch-ubol.ps1
#>

$ErrorActionPreference = 'Stop'

$version = '2026.1018.2229'
$url = "https://github.com/uBlockOrigin/uBOL-home/releases/download/$version/uBOLite_$version.chromium.zip"
$sha256 = 'c48dfa2f925ed31fdbd478d6b504432152ac995ab1e0fc96c3a008a0307dbbe2'

$out = "$PSScriptRoot\out\ubol"
$zip = "$out\uBOLite_$version.chromium.zip"
$dir = "$out\uBOLite"

function Get-Version([string] $Manifest) {
  if (Test-Path $Manifest) { (Get-Content $Manifest -Raw | ConvertFrom-Json).version }
}

if ((Get-Version "$dir\manifest.json") -ne $version) {
  New-Item -ItemType Directory -Force $out | Out-Null
  if (-not (Test-Path $zip)) {
    Write-Host "Téléchargement de $url"
    Invoke-WebRequest -Uri $url -OutFile $zip
  }
  $hash = (Get-FileHash $zip -Algorithm SHA256).Hash
  if ($hash -ne $sha256) {
    Remove-Item $zip
    throw "Empreinte inattendue pour $zip : $hash"
  }
  # Dossier neuf : rien d'une version précédente ne doit rester.
  Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue
  Expand-Archive $zip -DestinationPath $dir
  if ((Get-Version "$dir\manifest.json") -ne $version) { throw "Version inattendue dans $dir\manifest.json" }
}
Copy-Item "$PSScriptRoot\ubol\SOURCES.txt" $dir
Write-Host "uBO Lite $version prêt : $dir" -ForegroundColor Green
