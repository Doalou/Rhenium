#Requires -Version 7.0
<#
.SYNOPSIS
  Copie main et le tag -Tag du dépôt privé (copie locale, historique complet) dans le dépôt
  public -Repo, avec le jeton $env:GITHUB_TOKEN. Jamais en force : si le main public a reçu
  d'autres commits, la copie échoue et il faut les reprendre d'abord dans le dépôt privé.
.EXAMPLE
  ./ci/miroir.ps1 -Tag v0.5.0 -Repo Doalou/Rhenium
#>
param(
  [Parameter(Mandatory)] [string] $Tag,
  [string] $Repo
)

$ErrorActionPreference = 'Stop'
if (-not $Repo) { throw 'Variable RHENIUM_PUBLIC_REPO absente du dépôt privé (voir ci/README.md).' }
if (-not $env:GITHUB_TOKEN) { throw 'Secret RHENIUM_PUBLIC_TOKEN absent du dépôt privé (voir ci/README.md).' }

$url = "https://x-access-token:$($env:GITHUB_TOKEN)@github.com/$Repo.git"
git push $url 'refs/remotes/origin/main:refs/heads/main' "refs/tags/${Tag}:refs/tags/$Tag"
if ($LASTEXITCODE -ne 0) {
  throw "Copie refusée par $Repo (main public modifié à part ? jeton sans droit Contents ou Workflows ?)."
}
Write-Host "main et $Tag copiés dans $Repo" -ForegroundColor Green
