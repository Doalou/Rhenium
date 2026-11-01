#Requires -Version 7.0
<#
.SYNOPSIS
  Récupère les sources de Chromium 116.0.5845.228 utiles à content_shell, dans ..\chromium\src
  (archives gitiles, sans gclient ni historique), à la suite de ..\j1-base\fetch.ps1.
.DESCRIPTION
  Arbre chromium/src : les dossiers de $inTree, et third_party sauf $skipThirdParty (Blink sans
  ses web_tests). Dépôts externes : ceux de $externalPaths, aux révisions du DEPS de la balise
  (évalué par deps.py, recursedeps compris). Chaque ajout est figé dans le dépôt git local de
  référence, comme en J1.
.EXAMPLE
  ./fetch.ps1
  ./fetch.ps1 -Missing third_party/libvpx/BUILD.gn   # fichier réclamé par gn gen
#>
param(
  [string[]] $Missing = @(),
  [string] $Src = "$PSScriptRoot\..\chromium\src"
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$tag = '116.0.5845.228'
$chromium = 'https://chromium.googlesource.com'

# Dossiers de premier niveau de chromium/src nécessaires à content_shell.
$inTree = @(
  'cc', 'components', 'content', 'crypto', 'device', 'gin', 'google_apis', 'gpu', 'ipc', 'media',
  'mojo', 'net', 'pdf', 'ppapi', 'printing', 'sandbox', 'services', 'skia', 'sql', 'storage', 'tools',
  'ui', 'url', 'third_party/blink/common', 'third_party/blink/public', 'third_party/blink/renderer',
  'third_party/blink/tools', 'third_party/blink/perf_tests'
)
# Fichiers isolés (chrome/ entier pèse 179 Mo).
$inTreeFiles = @{
  'chrome' = @('VERSION', 'version.gni', 'BUILD.gn')
  'third_party/blink' = @('BUILD.gn', 'LICENSE', 'README.chromium', 'DIR_METADATA')
}
# third_party de l'arbre : tout sauf Android, ChromeOS, iOS, tests et gros inutiles.
$skipThirdParty = '^(blink|android.*|.*chromeos.*|arcore-android-sdk.*|tflite_support|wpt_tools|chromevox|' +
  'kotlin.*|r8|jdk|gradle_wrapper|checkstyle|proguard|robolectric|ijar|byte_buddy|turbine|mockito|' +
  'hamcrest|junit|objenesis|espresso|google_toolbox_for_mac|ocmock|test_fonts|webxr_test_pages|' +
  'speedometer|xcode.*|apple_apsl|ios|crossbench|chromite|dom_distiller_js|web-animations-js|rust|' +
  'blink_.*|gvr-android-sdk|android_.*)$'

# Dépôts externes indispensables (étude du 2026-10-16) ; le reste attend une demande de gn gen.
$externalPaths = @(
  'v8', 'third_party/skia', 'third_party/angle', 'third_party/dawn', 'third_party/icu',
  'third_party/harfbuzz-ng/src', 'third_party/freetype/src', 'net/third_party/quiche/src',
  'third_party/webrtc', 'third_party/libsrtp', 'third_party/openscreen/src', 'third_party/perfetto',
  'third_party/sqlite/src', 'third_party/leveldatabase/src', 'third_party/snappy/src',
  'third_party/re2/src', 'third_party/crc32c/src', 'third_party/ots/src',
  'third_party/emoji-segmenter/src', 'third_party/ced/src', 'third_party/wuffs/src',
  'third_party/highway/src', 'third_party/libjpeg_turbo', 'third_party/libwebp/src',
  'third_party/libyuv', 'third_party/libwebm/source', 'third_party/libavif/src',
  'third_party/libavifinfo/src', 'third_party/vulkan-deps', 'third_party/vulkan_memory_allocator',
  'media/cdm/api', 'third_party/flac', 'third_party/gperf', 'third_party/jsoncpp/source',
  'third_party/material_color_utilities/src', 'third_party/anonymous_tokens/src',
  'third_party/content_analysis_sdk/src', 'third_party/expat/src', 'third_party/libphonenumber/dist',
  'third_party/openscreen/src/third_party/tinycbor/src',
  'third_party/vulkan-deps/vulkan-headers/src', 'third_party/vulkan-deps/spirv-headers/src',
  'third_party/vulkan-deps/spirv-tools/src', 'third_party/vulkan-deps/glslang/src',
  'third_party/vulkan-deps/spirv-cross/src', 'third_party/vulkan-deps/vulkan-loader/src',
  'third_party/vulkan-deps/vulkan-tools/src', 'third_party/vulkan-deps/vulkan-utility-libraries/src'
)
$depsVars = @('checkout_nacl=False', 'checkout_rust=False', 'checkout_openxr=False',
  'checkout_js_coverage_modules=False', 'checkout_src_internal=False')

$Src = (Resolve-Path $Src).Path
$root = Split-Path $Src
$manifest = Join-Path $root 'fetched.txt'
$done = if (Test-Path $manifest) { @(Get-Content $manifest) } else { @() }
$python = (Get-Command python -ErrorAction Stop).Source

function Get-Gitiles([string] $Url) {
  for ($try = 1; ; $try++) {
    try { return Invoke-WebRequest $Url -UseBasicParsing }
    catch { if ($try -ge 4) { throw }; Start-Sleep -Seconds (5 * $try) }
  }
}

function Get-Listing([string] $Url) {
  $content = (Get-Gitiles "$Url/?format=JSON").Content
  return ($content.Substring($content.IndexOf('{')) | ConvertFrom-Json).entries
}

# Archive d'un dossier ; $Key distingue une version complète d'une version partielle de J1.
function Get-Tree([string] $Path, [string] $Url, [string] $Key = $Path) {
  if ($Key -in $script:done) { return $false }
  $dest = Join-Path $Src $Path
  New-Item -ItemType Directory -Force $dest | Out-Null
  $archive = [IO.Path]::GetTempFileName()
  Write-Host "$Path" -ForegroundColor Cyan
  for ($try = 1; ; $try++) {
    try { Invoke-WebRequest $Url -OutFile $archive -UseBasicParsing; break }
    catch { if ($try -ge 4) { throw }; Start-Sleep -Seconds (5 * $try) }
  }
  tar -xzf $archive -C $dest
  if ($LASTEXITCODE -ne 0) { throw "Extraction de $Path impossible" }
  Remove-Item $archive
  Add-Content $manifest $Key
  $script:done += $Key
  return $true
}

function Get-Files([string] $Path, [string] $Url, [string[]] $Files) {
  if ($Path -in $script:done) { return $false }
  $dest = Join-Path $Src $Path
  if ($Files.Count -eq 1 -and $Path.EndsWith("/$($Files[0])")) { $dest = Split-Path $dest }
  $got = $false
  New-Item -ItemType Directory -Force $dest | Out-Null
  foreach ($file in $Files) {
    try { $text = (Get-Gitiles "$Url/$file`?format=TEXT").Content }
    catch { Write-Warning "$Path/$file absent à la balise"; continue }
    [IO.File]::WriteAllBytes((Join-Path $dest $file), [Convert]::FromBase64String($text))
    $got = $true
  }
  if (-not $got) { return $false }
  Add-Content $manifest $Path
  $script:done += $Path
  return $true
}

# Dépôts du DEPS de la balise et de ses recursedeps : chemin -> @{ Url; Rev }.
function Get-DepsRepos {
  $depsFile = Join-Path $root "DEPS-$tag"
  if (-not (Test-Path $depsFile)) {
    $text = (Get-Gitiles "$chromium/chromium/src/+/refs/tags/$tag/DEPS?format=TEXT").Content
    [IO.File]::WriteAllBytes($depsFile, [Convert]::FromBase64String($text))
  }
  $varArgs = $depsVars | ForEach-Object { '--var', $_ }
  $lines = & $python -I "$PSScriptRoot\deps.py" $depsFile @varArgs
  if ($LASTEXITCODE -ne 0) { throw "deps.py a échoué sur $depsFile" }
  $repos = [ordered]@{}
  $pending = [Collections.Generic.Queue[string]]::new()
  foreach ($line in $lines) {
    if ($line -match '^# recursedeps (.+)$') { $pending.Enqueue($Matches[1]) }
    elseif ($line -match '^([^\t]+)\t([^\t]+)\t([0-9a-f]{40})$') { $repos[$Matches[1]] = @{ Url = $Matches[2] -replace '\.git$', ''; Rev = $Matches[3] } }
  }
  while ($pending.Count) {
    $sub = $pending.Dequeue()
    if (-not $repos.Contains($sub)) { continue }  # recursedeps internes à Google
    $subFile = Join-Path $root ("DEPS-" + ($sub -replace '/', '_') + "-$($repos[$sub].Rev.Substring(0, 8))")
    if (-not (Test-Path $subFile)) {
      $text = (Get-Gitiles "$($repos[$sub].Url)/+/$($repos[$sub].Rev)/DEPS?format=TEXT").Content
      [IO.File]::WriteAllBytes($subFile, [Convert]::FromBase64String($text))
    }
    $lines = & $python -I "$PSScriptRoot\deps.py" $subFile --prefix $sub @varArgs
    if ($LASTEXITCODE -ne 0) { throw "deps.py a échoué sur $subFile" }
    foreach ($line in $lines) {
      if ($line -match '^# recursedeps (.+)$') { $pending.Enqueue($Matches[1]) }
      elseif ($line -match '^([^\t]+)\t([^\t]+)\t([0-9a-f]{40})$' -and -not $repos.Contains($Matches[1])) {
        $repos[$Matches[1]] = @{ Url = $Matches[2] -replace '\.git$', ''; Rev = $Matches[3] }
      }
    }
  }
  return $repos
}

$added = @()
$tagUrl = "$chromium/chromium/src/+/refs/tags/$tag"
$archiveUrl = "$chromium/chromium/src/+archive/refs/tags/$tag"

foreach ($dir in $inTree) {
  if (Get-Tree $dir "$archiveUrl/$dir.tar.gz") { $added += $dir }
}
foreach ($dir in $inTreeFiles.Keys) {
  if (Get-Files $dir "$tagUrl/$dir" $inTreeFiles[$dir]) { $added += $dir }
}
foreach ($entry in Get-Listing "$tagUrl/third_party") {
  if ($entry.type -ne 'tree' -or $entry.name -match $skipThirdParty) { continue }
  $dir = "third_party/$($entry.name)"
  if (Get-Tree $dir "$archiveUrl/$dir.tar.gz") { $added += $dir }
}

$repos = Get-DepsRepos
foreach ($path in $externalPaths) {
  if (-not $repos.Contains($path)) { Write-Warning "$path absent du DEPS évalué"; continue }
  $repo = $repos[$path]
  # J1 n'avait pris que les fichiers de build d'ICU : clé distincte pour la version complète.
  $key = if ($path -eq 'third_party/icu') { 'third_party/icu (complet)' } else { $path }
  if (Get-Tree $path "$($repo.Url)/+archive/$($repo.Rev).tar.gz" $key) { $added += $path }
}

# Fichier réclamé par gn gen hors de ce qui précède : dossier de l'arbre ou dépôt du DEPS entier.
foreach ($path in $Missing) {
  if (Test-Path (Join-Path $Src $path)) { continue }
  $repoPath = $repos.Keys | Where-Object { $path.StartsWith("$_/") } | Sort-Object Length -Descending | Select-Object -First 1
  $buildFile = $path -match '\.(gn|gni)$'
  if ($repoPath -and -not $buildFile) {
    # Source réclamée par ninja : le dépôt entier (clé distincte si J1 n'en avait que les
    # fichiers de build).
    $repo = $repos[$repoPath]
    $key = if ($repoPath -in $done) { "$repoPath (complet)" } else { $repoPath }
    if (Get-Tree $repoPath "$($repo.Url)/+archive/$($repo.Rev).tar.gz" $key) { $added += $repoPath }
  } elseif ($repoPath) {
    # gn gen n'a besoin que des fichiers de build : le fichier seul, à la révision du DEPS
    # (des cibles de test réclament des .gni de dépôts entiers qu'on ne construira pas).
    $repo = $repos[$repoPath]
    $inRepo = $path.Substring($repoPath.Length + 1)
    $dir = (Split-Path $inRepo) -replace '\\', '/'
    $url = "$($repo.Url)/+/$($repo.Rev)" + $(if ($dir) { "/$dir" } else { '' })
    if (Get-Files $path $url @(Split-Path $path -Leaf)) { $added += $path }
  } elseif ($buildFile) {
    # Fichier de build de l'arbre : lui seul (certains dossiers, comme chrome/app, pèsent lourd).
    $dir = (Split-Path $path) -replace '\\', '/'
    if (Get-Files $path "$tagUrl/$dir" @(Split-Path $path -Leaf)) { $added += $path }
  } else {
    # Source de l'arbre : son dossier.
    $dir = (Split-Path $path) -replace '\\', '/'
    $key = if ($dir -in $done) { "$dir (complet)" } else { $dir }
    if (Get-Tree $dir "$archiveUrl/$dir.tar.gz" $key) { $added += $dir }
  }
}

# Récupérés lors d'un lancement interrompu, mais pas encore figés.
foreach ($path in $done) {
  if ($path -match ' ' -or $path -in $added) { continue }
  # Seulement un chemin dont rien n'est suivi : git add -A -f prendrait sinon nos
  # correctifs et les fichiers des hooks (node, sorties de midl). Un dossier suivi
  # en partie (comme tools/ l'a été) se fige à la main.
  if (-not (git -C $Src ls-files -- $path | Select-Object -First 1)) { $added += $path }
}
$added = @($added | Where-Object { Test-Path (Join-Path $Src $_) } | Select-Object -Unique)  # git add refuse toute la liste sur un chemin absent
if ($added) {
  git -C $Src add -A -f -- $added  # -f : les .gitignore de Chromium excluent les dossiers du DEPS
  git -C $Src commit -q -m "Chromium $tag (J2) : $($added.Count) dossiers"
  Write-Host "Ajoutés et figés : $($added.Count) dossiers" -ForegroundColor Green
} else {
  Write-Host 'Rien de nouveau.' -ForegroundColor Green
}
