#Requires -Version 7.0
<#
.SYNOPSIS
  Récupère PDF.js 5.4.624 (version « legacy », la dernière qui vise Chrome 110 : les suivantes
  demandent Chrome 118), vérifie son empreinte et en fait l'extension out\pdfjs\PdfViewer, que
  package-shell.ps1 copie dans l'app (Extensions\PdfViewer). Le moteur y ouvre les réponses PDF.
  PDF.js est sous licence Apache 2.0 : livré avec sa licence et l'adresse de ses sources
  (pdfjs\SOURCES.txt).
.EXAMPLE
  ./fetch-pdfjs.ps1
#>

$ErrorActionPreference = 'Stop'

$version = '5.4.624'
$url = "https://github.com/mozilla/pdf.js/releases/download/v$version/pdfjs-$version-legacy-dist.zip"
$sha256 = '8fc4b9ed51aec0ba273b6696c3a6af3de80b3d3b80c7aa463af4389c98f0304b'

$out = "$PSScriptRoot\out\pdfjs"
$zip = "$out\pdfjs-$version-legacy-dist.zip"
$dir = "$out\PdfViewer"
$src = "$PSScriptRoot\pdfjs"

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

# Toujours refait : quelques secondes, et rhenium.js ou le manifeste ont pu changer.
Remove-Item $dir -Recurse -Force -ErrorAction SilentlyContinue
Expand-Archive $zip -DestinationPath $dir

# Allègement : la copie dans le profil, au premier lancement, est lente sur le Lumia.
Get-ChildItem $dir -Recurse -Filter '*.map' | Remove-Item
Remove-Item "$dir\web\debugger.*", "$dir\web\compressed.tracemonkey-pldi-09.pdf", "$dir\build\pdf.sandbox.mjs"
$locales = Get-Content "$dir\web\locale\locale.json" -Raw | ConvertFrom-Json -AsHashtable
$kept = [ordered]@{}
foreach ($name in $locales.Keys) {
  if ($locales[$name] -match '^(en-US|fr)/') { $kept[$name] = $locales[$name] }
}
if ($kept.Count -ne 2) { throw "Langues inattendues dans locale.json : $($kept.Keys -join ', ')" }
Get-ChildItem "$dir\web\locale" -Directory | Where-Object { $_.Name -notin 'en-US', 'fr' } | Remove-Item -Recurse
$kept | ConvertTo-Json -Compress | Set-Content "$dir\web\locale\locale.json" -NoNewline

Copy-Item "$src\manifest.json", "$src\SOURCES.txt" $dir
Copy-Item "$src\rhenium.js", "$src\rhenium.css" "$dir\web"
$viewer = "$dir\web\viewer.html"
$anchor = '<script src="viewer.mjs" type="module"></script>'
$html = Get-Content $viewer -Raw
if (-not $html.Contains($anchor)) { throw "viewer.html inattendu : $anchor introuvable" }
$html = $html.Replace($anchor, "<link rel=`"stylesheet`" href=`"rhenium.css`" />`n  <script src=`"rhenium.js`"></script>`n  $anchor")
Set-Content $viewer $html -NoNewline

$size = (Get-ChildItem $dir -Recurse -File | Measure-Object Length -Sum).Sum
Write-Host "PDF.js $version prêt : $dir ($([math]::Round($size / 1MB, 1)) Mo)" -ForegroundColor Green
