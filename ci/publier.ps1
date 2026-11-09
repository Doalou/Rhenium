#Requires -Version 7.0
<#
.SYNOPSIS
  Publie le contenu de -Dir (sortie de paquet.ps1) en Release GitHub pour le tag -Tag, par
  l'API REST et le jeton $env:GITHUB_TOKEN. Relancé sur un tag déjà publié, remplace les
  fichiers de la Release.
.EXAMPLE
  ./ci/publier.ps1 -Dir $env:TEMP\rhenium -Tag v0.5.0 -Repo Doalou/Rhenium
#>
param(
  [Parameter(Mandatory)] [string] $Dir,
  [Parameter(Mandatory)] [string] $Tag,
  [Parameter(Mandatory)] [string] $Repo
)

$ErrorActionPreference = 'Stop'
if (-not $env:GITHUB_TOKEN) { throw 'GITHUB_TOKEN absent.' }
$headers = @{
  Authorization          = "Bearer $env:GITHUB_TOKEN"
  Accept                 = 'application/vnd.github+json'
  'X-GitHub-Api-Version' = '2022-11-28'
}
$api = "https://api.github.com/repos/$Repo"
$files = 'RheniumShell_arm.appx', 'RheniumDev.cer', 'LISEZMOI.txt' | ForEach-Object { Get-Item "$Dir\$_" }

$release = try {
  Invoke-RestMethod "$api/releases/tags/$Tag" -Headers $headers
} catch {
  if ($_.Exception.Response.StatusCode -ne 404) { throw }
  $body = @{
    tag_name = $Tag
    name     = "Rhénium $Tag"
    body     = Get-Content "$Dir\LISEZMOI.txt" -Raw
  } | ConvertTo-Json
  Invoke-RestMethod "$api/releases" -Method Post -Headers $headers -ContentType 'application/json; charset=utf-8' `
    -Body ([Text.Encoding]::UTF8.GetBytes($body))
}

foreach ($file in $files) {
  foreach ($old in @($release.assets | Where-Object name -eq $file.Name)) {
    Invoke-RestMethod "$api/releases/assets/$($old.id)" -Method Delete -Headers $headers | Out-Null
  }
  $upload = "https://uploads.github.com/repos/$Repo/releases/$($release.id)/assets?name=$([uri]::EscapeDataString($file.Name))"
  Invoke-RestMethod $upload -Method Post -Headers $headers -ContentType 'application/octet-stream' -InFile $file.FullName | Out-Null
  Write-Host "envoyé : $($file.Name)"
}
Write-Host "Release publiée : $($release.html_url)" -ForegroundColor Green
