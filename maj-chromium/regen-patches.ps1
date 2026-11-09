#Requires -Version 7.0
<#
.SYNOPSIS
  Régénère les correctifs de l'arbre de la 156 (0002 v8, 0003 build et third_party du palier 2,
  0004 base, content\0005 le reste) depuis F:\Rhenium\chromium-next\src, contre la branche « origine ».
  À lancer après toute retouche de l'arbre, avant build-base.ps1 ou build-content.ps1.
#>
param([string] $Src = 'F:\Rhenium\chromium-next\src')

$ErrorActionPreference = 'Stop'
$p = "$PSScriptRoot\patches"
New-Item -ItemType Directory -Force "$p\content" | Out-Null

# Fichiers neufs de Rhénium : visibles par git diff (hors racine GN, sorties et jonction Rust).
$new = git -C $Src status --short -uall | Where-Object { $_ -match '^\?\?' } |
  ForEach-Object { $_.Substring(3).Trim('"') } |
  Where-Object { $_ -notmatch '^(out/|rhenium/|third_party/rust-toolchain|\.gn$|BUILD\.gn$)' -and $_ -notmatch '\.(rej|orig)$' }
foreach ($file in $new) { git -C $Src add -N -- $file }

$palier2 = @('build', 'third_party/boringssl', 'third_party/googletest', 'third_party/perfetto')
git -C $Src diff --binary --output="$p\0003-chromium-build-winuwp-arm32.patch" -- @palier2
git -C $Src diff --binary --output="$p\0004-base-winuwp-arm32.patch" -- base
git -C $Src diff --binary --relative=v8 --output="$p\0002-v8-win-arm32.patch" -- v8
$exclude = @('base', 'v8', 'third_party/rust-toolchain') + $palier2 | ForEach-Object { ":!$_" }
git -C $Src diff --binary --output="$p\content\0005-content-shell-winuwp.patch" -- . @exclude
Get-ChildItem $p -Recurse -Include '0002*.patch', '0003*.patch', '0004*.patch', '0005*.patch' | ForEach-Object { "{0} : {1:N0} octets" -f $_.Name, $_.Length }
