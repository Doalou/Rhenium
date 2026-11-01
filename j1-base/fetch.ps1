#Requires -Version 7.0
<#
.SYNOPSIS
  Récupère les sources de Chromium 116.0.5845.228 utiles à //base, dossier par dossier
  (archives gitiles), dans ..\chromium\src, sans gclient ni historique.
.DESCRIPTION
  Un dépôt git local dans ..\chromium\src garde l'état d'origine de chaque dossier ajouté :
  les correctifs Rhénium se produisent ensuite par git diff.
  Pose aussi gn.exe et ninja.exe (CIPD, versions du DEPS) dans ..\toolchains\gn et ..\toolchains\ninja.
.EXAMPLE
  ./fetch.ps1
  ./fetch.ps1 -Dirs tools/win
  ./fetch.ps1 -Missing third_party/catapult/tracing/BUILD.gn   # fichier réclamé par gn gen
#>
param(
  [string[]] $Dirs = @(),
  [string[]] $Missing = @(),
  [string] $Src = "$PSScriptRoot\..\chromium\src"
)

$ErrorActionPreference = 'Stop'
$tag = '116.0.5845.228'
$chromium = 'https://chromium.googlesource.com'

# Dossiers du dépôt chromium/src, à la balise.
$inTree = @(
  'base', 'build', 'build_overrides', 'buildtools', 'testing',
  'third_party/abseil-cpp', 'third_party/modp_b64', 'third_party/boringssl', 'third_party/googletest'
) + $Dirs

# Dépôts externes, aux révisions du DEPS de la balise.
$external = @(
  @{ Path = 'third_party/googletest/src'; Url = "$chromium/external/github.com/google/googletest"; Rev = 'af29db7ec28d6df1c7f0f745186884091e602e07' },
  @{ Path = 'third_party/boringssl/src'; Url = 'https://boringssl.googlesource.com/boringssl'; Rev = 'ae88f198a49d77993e9c44b017d0e69c810dc668' },
  # ICU : seulement ses fichiers de build, pour que GN résolve base_i18n (//base n'en dépend pas).
  @{ Path = 'third_party/icu'; Url = "$chromium/chromium/deps/icu"; Rev = 'e8c3bc9ea97d4423ad0515e5f1c064f486dae8b1';
     Files = @('BUILD.gn', 'config.gni', 'sources.gni', 'version.gni', 'version.json', 'LICENSE', 'README.chromium') }
)

New-Item -ItemType Directory -Force $Src | Out-Null
$Src = (Resolve-Path $Src).Path
$manifest = Join-Path (Split-Path $Src) 'fetched.txt'
$done = if (Test-Path $manifest) { @(Get-Content $manifest) } else { @() }

function Get-Tree([string] $Path, [string] $Url) {
  if ($Path -in $script:done) { return $false }
  $dest = Join-Path $Src $Path
  New-Item -ItemType Directory -Force $dest | Out-Null
  $archive = [IO.Path]::GetTempFileName()
  Write-Host "$Path" -ForegroundColor Cyan
  Invoke-WebRequest $Url -OutFile $archive
  tar -xzf $archive -C $dest
  if ($LASTEXITCODE -ne 0) { throw "Extraction de $Path impossible" }
  Remove-Item $archive
  Add-Content $manifest $Path
  $script:done += $Path
  return $true
}

function Get-Files([string] $Path, [string] $Url, [string[]] $Files) {
  if ($Path -in $script:done) { return $false }
  $dest = Join-Path $Src $Path
  New-Item -ItemType Directory -Force $dest | Out-Null
  Write-Host "$Path ($($Files -join ', '))" -ForegroundColor Cyan
  foreach ($file in $Files) {
    $text = Invoke-RestMethod "$Url/$file`?format=TEXT"
    [IO.File]::WriteAllBytes((Join-Path $dest $file), [Convert]::FromBase64String($text))
  }
  Add-Content $manifest $Path
  $script:done += $Path
  return $true
}

# Fichier réclamé dans un dépôt du DEPS (hors chromium/src) : seulement les fichiers de build
# de la racine du dépôt et du dossier du fichier, assez pour que GN résolve les cibles.
function Get-Deps {
  $depsFile = Join-Path (Split-Path $Src) "DEPS-$tag"
  if (-not (Test-Path $depsFile)) {
    $text = Invoke-RestMethod "$chromium/chromium/src/+/refs/tags/$tag/DEPS?format=TEXT"
    [IO.File]::WriteAllBytes($depsFile, [Convert]::FromBase64String($text))
  }
  Get-Content $depsFile -Raw
}

function Get-DepsBuildFiles([string] $MissingPath) {
  $deps = Get-Deps
  $parts = $MissingPath -split '/'
  for ($n = $parts.Count - 1; $n -ge 1; $n--) {
    $repo = ($parts[0..($n - 1)]) -join '/'
    $m = [regex]::Match($deps, "'src/$([regex]::Escape($repo))':\s*(?:\{\s*'url':\s*)?Var\('chromium_git'\)\s*\+\s*'([^']+)'\s*\+\s*'@'\s*\+\s*(?:'([0-9a-f]{40})'|Var\('(\w+)'\))")
    if ($m.Success) { break }
  }
  if (-not $m.Success) { throw "$MissingPath n'est ni dans chromium/src ni dans un dépôt du DEPS de $tag" }
  $rev = if ($m.Groups[2].Success) { $m.Groups[2].Value } else { [regex]::Match($deps, "'$($m.Groups[3].Value)':\s*'([0-9a-f]{40})'").Groups[1].Value }
  $url = "$chromium$($m.Groups[1].Value -replace '\.git$', '')/+/$rev"
  $added = @()
  $subdir = ($parts[$n..($parts.Count - 2)]) -join '/'
  foreach ($dir in @('', $subdir) | Select-Object -Unique) {
    $listing = (Invoke-WebRequest "$url/$dir/?format=JSON").Content
    $tree = $listing.Substring($listing.IndexOf('{')) | ConvertFrom-Json
    $files = @($tree.entries | Where-Object { $_.type -eq 'blob' -and $_.name -match '\.(gn|gni)$|^LICENSE' } | ForEach-Object { $_.name })
    $local = if ($dir) { "$repo/$dir" } else { $repo }
    if (Get-Files $local "$url/$dir".TrimEnd('/') $files) { $added += $local }
  }
  return $added
}

$added = @()
foreach ($dir in $inTree) {
  if (Get-Tree $dir "$chromium/chromium/src/+archive/refs/tags/$tag/$dir.tar.gz") { $added += $dir }
}
foreach ($path in $Missing) {
  $parts = $path -split '/'
  $dir = if ($parts[0] -eq 'third_party' -and $parts[1] -eq 'rust' -and $parts.Count -gt 3) { ($parts[0..2]) -join '/' }
         elseif ($parts[0] -eq 'third_party' -and $parts.Count -gt 2) { ($parts[0..1]) -join '/' }
         else { ($parts[0..($parts.Count - 2)]) -join '/' }
  if ($dir -notin $done) {
    try {
      if (Get-Tree $dir "$chromium/chromium/src/+archive/refs/tags/$tag/$dir.tar.gz") { $added += $dir }
    } catch {
      Remove-Item (Join-Path $Src $dir) -Recurse -Force -ErrorAction SilentlyContinue
    }
  }
  if (-not (Test-Path (Join-Path $Src $path))) { $added += Get-DepsBuildFiles $path }
}
foreach ($repo in $external) {
  $fetched = if ($repo.Files) { Get-Files $repo.Path "$($repo.Url)/+/$($repo.Rev)" $repo.Files }
             else { Get-Tree $repo.Path "$($repo.Url)/+archive/$($repo.Rev).tar.gz" }
  if ($fetched) { $added += $repo.Path }
}

# Normalement écrit par gclient : valeurs du DEPS de la balise, adaptées à cet arbre partiel.
$gclientArgs = "$Src\build\config\gclient_args.gni"
if (-not (Test-Path $gclientArgs)) {
  Set-Content $gclientArgs -Encoding ascii @'
# Generated by Rhenium j1-base\fetch.ps1 (no gclient)
build_with_chromium = true
checkout_android = false
checkout_android_prebuilts_build_tools = false
checkout_android_native_support = false
checkout_ios_webkit = false
checkout_nacl = false
checkout_openxr = false
checkout_rts_model = false
checkout_src_internal = false
cros_boards = ""
cros_boards_with_qemu_images = ""
generate_location_tags = true
'@
}

# Également écrits par gclient (hook lastchange) : commit de la balise.
$lastChange = "$Src\build\util\LASTCHANGE"
if (-not (Test-Path $lastChange)) {
  $commit = 'b53ecfbe30fbd1731bac998dc191b262f4a1bfb8'
  $time = [DateTimeOffset]::new(2023, 9, 27, 10, 10, 49, [TimeSpan]::Zero).ToUnixTimeSeconds()
  Set-Content $lastChange -Encoding ascii "LASTCHANGE=$commit-refs/tags/$tag`nLASTCHANGE_YEAR=2023"
  Set-Content "$lastChange.committime" -Encoding ascii -NoNewline "$time"
}

# GN et ninja : paquets CIPD (des zip) aux versions du DEPS, dans ..\toolchains (hors de l'arbre).
function Get-Cipd([string] $Package, [string] $Version, [string] $Dir, [string] $Exe) {
  if (Test-Path "$Dir\$Exe") { return }
  New-Item -ItemType Directory -Force $Dir | Out-Null
  $zip = "$Dir\cipd.zip"
  Write-Host "CIPD : $Package @ $Version" -ForegroundColor Cyan
  Invoke-WebRequest "https://chrome-infra-packages.appspot.com/dl/$Package/+/$Version" -OutFile $zip
  Expand-Archive $zip $Dir -Force
  Remove-Item $zip
  if (-not (Test-Path "$Dir\$Exe")) { throw "$Exe absent du paquet $Package" }
}
$deps = Get-Deps
$toolchains = Join-Path (Split-Path (Split-Path $Src)) 'toolchains'
foreach ($tool in @{ Var = 'gn_version'; Package = 'gn/gn/windows-amd64'; Exe = 'gn.exe' },
                  @{ Var = 'ninja_version'; Package = 'infra/3pp/tools/ninja/windows-amd64'; Exe = 'ninja.exe' }) {
  $version = [regex]::Match($deps, "'$($tool.Var)':\s*'([^']+)'").Groups[1].Value
  if (-not $version) { throw "$($tool.Var) absent du DEPS de $tag" }
  Get-Cipd $tool.Package $version (Join-Path $toolchains ($tool.Exe -replace '\.exe$')) $tool.Exe
}

if (-not (Test-Path "$Src\.git")) {
  git -C $Src init -q -b origine
  git -C $Src config core.autocrlf false
}
if ($added) {
  git -C $Src add -A -f -- $added  # -f : les .gitignore de Chromium excluent les dossiers du DEPS
  git -C $Src commit -q -m "Chromium $tag : $($added -join ', ')"
  Write-Host "Ajoutés et figés : $($added -join ', ')" -ForegroundColor Green
} else {
  Write-Host 'Rien de nouveau.' -ForegroundColor Green
}
