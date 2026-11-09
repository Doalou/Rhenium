#Requires -Version 7.0
<#
.SYNOPSIS
  Récupère les sources de Chromium 156.0.8078.88 utiles au palier en cours, dans
  F:\Rhenium\chromium-next\src (archives gitiles, sans gclient ni historique).
.DESCRIPTION
  Dossiers de l'arbre de la balise ($inTree), dépôts du DEPS (évalué par ..\j2\deps.py,
  recursedeps compris) et fichiers réclamés par gn gen ou ninja (-Missing). Chaque ajout est
  figé dans le dépôt git local (branche « origine ») ; seuls les chemins récupérés y entrent.
  Pose aussi GN, ninja (CIPD) et le Rust de Chromium (dans F:\Rhenium\toolchains\*-156).
.EXAMPLE
  ./fetch.ps1
  ./fetch.ps1 -Missing third_party/simdutf/BUILD.gn
#>
param(
  [string[]] $Missing = @(),
  [string[]] $Dirs = @(),
  [string] $Src = 'F:\Rhenium\chromium-next\src',
  [switch] $Content
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$tag = '156.0.8078.88'
$chromium = 'https://chromium.googlesource.com'
$toolchains = 'F:\Rhenium\toolchains'

# Palier 2 : //base, ses tests et le build.
$inTree = @(
  'base', 'build', 'build_overrides', 'buildtools', 'testing', 'tools/rust', 'tools/clang/scripts',
  'third_party/abseil-cpp', 'third_party/modp_b64', 'third_party/boringssl', 'third_party/googletest',
  'third_party/rust', 'third_party/simdutf', 'third_party/fast_float', 'third_party/dragonbox',
  'third_party/protobuf', 'third_party/jinja2', 'third_party/markupsafe'
) + $Dirs
$externalPaths = @('third_party/googletest/src', 'third_party/boringssl/src', 'third_party/fast_float/src',
  'third_party/dragonbox/src')

# Palier 3 et suivants (-Content) : ce que ..\j2\fetch.ps1 prenait pour content_shell.
if ($Content) {
  $inTree += @(
    'cc', 'components', 'content', 'crypto', 'device', 'extensions', 'gin', 'google_apis', 'gpu', 'ipc',
    'media', 'mojo', 'net', 'pdf', 'ppapi', 'printing', 'sandbox', 'services', 'skia', 'sql', 'storage',
    'tools', 'ui', 'url', 'third_party/blink/common', 'third_party/blink/public',
    'third_party/blink/renderer', 'third_party/blink/tools', 'third_party/blink/perf_tests'
  )
  $inTreeFiles = @('third_party/blink/BUILD.gn', 'third_party/blink/LICENSE',
    'third_party/blink/README.chromium', 'third_party/blink/DIR_METADATA')
  $skipThirdParty = '^(blink|android.*|.*chromeos.*|arcore-android-sdk.*|tflite_support|wpt_tools|chromevox|' +
    'kotlin.*|r8|jdk|gradle_wrapper|checkstyle|proguard|robolectric|ijar|byte_buddy|turbine|mockito|' +
    'hamcrest|junit|objenesis|espresso|google_toolbox_for_mac|ocmock|test_fonts|webxr_test_pages|' +
    'speedometer|xcode.*|apple_apsl|ios|crossbench|chromite|dom_distiller_js|web-animations-js|' +
    'blink_.*|gvr-android-sdk|android_.*)$'
  $externalPaths += @(
    'v8', 'third_party/skia', 'third_party/angle', 'third_party/dawn', 'third_party/icu',
    'third_party/harfbuzz/src', 'third_party/freetype/src', 'net/third_party/quiche/src',
    'third_party/webrtc', 'third_party/libsrtp', 'third_party/openscreen/src', 'third_party/perfetto',
    'third_party/sqlite/src', 'third_party/leveldatabase/src', 'third_party/snappy/src',
    'third_party/re2/src', 'third_party/crc32c/src', 'third_party/ots/src',
    'third_party/emoji-segmenter/src', 'third_party/ced/src', 'third_party/wuffs/src',
    'third_party/highway/src', 'third_party/libjpeg_turbo', 'third_party/libwebp/src',
    'third_party/libyuv', 'third_party/libwebm/source', 'third_party/libavif/src',
    'third_party/libavifinfo/src', 'third_party/vulkan-deps', 'third_party/vulkan_memory_allocator',
    'media/cdm/api', 'third_party/flac', 'third_party/gperf', 'third_party/jsoncpp/source',
    'third_party/material_color_utilities/src', 'third_party/anonymous_tokens/src',
    'third_party/content_analysis_sdk/src', 'third_party/expat/src', 'third_party/libphonenumber/src',
    'third_party/openscreen/src/third_party/tinycbor/src', 'third_party/libvpx/source/libvpx',
    'third_party/opus/src', 'third_party/ffmpeg', 'third_party/pdfium', 'third_party/crashpad/crashpad',
    'third_party/vulkan-deps/vulkan-headers/src', 'third_party/vulkan-deps/spirv-headers/src',
    'third_party/vulkan-deps/spirv-tools/src', 'third_party/vulkan-deps/glslang/src',
    'third_party/vulkan-deps/vulkan-loader/src', 'third_party/vulkan-deps/vulkan-tools/src',
    'third_party/vulkan-deps/vulkan-utility-libraries/src'
  )
}
$depsVars = @('checkout_nacl=False', 'checkout_rust=True', 'checkout_openxr=False',
  'checkout_js_coverage_modules=False', 'checkout_src_internal=False')

New-Item -ItemType Directory -Force $Src | Out-Null
$Src = (Resolve-Path $Src).Path
$root = Split-Path $Src
$manifest = Join-Path $root 'fetched.txt'
$done = if (Test-Path $manifest) { @(Get-Content $manifest) } else { @() }
$python = (Get-Command python -ErrorAction Stop).Source
$depsPy = "$PSScriptRoot\..\j2\deps.py"

function Get-Gitiles([string] $Url, [string] $OutFile) {
  for ($try = 1; ; $try++) {
    try {
      if ($OutFile) { Invoke-WebRequest $Url -OutFile $OutFile -UseBasicParsing; return }
      return Invoke-WebRequest $Url -UseBasicParsing
    } catch { if ($try -ge 4) { throw }; Start-Sleep -Seconds (5 * $try) }
  }
}

function Get-Tree([string] $Path, [string] $Url, [string] $Key = $Path) {
  if ($Key -in $script:done) { return $false }
  $dest = Join-Path $Src $Path
  New-Item -ItemType Directory -Force $dest | Out-Null
  $archive = [IO.Path]::GetTempFileName()
  Write-Host $Path -ForegroundColor Cyan
  try { Get-Gitiles $Url $archive } catch {
    if ("$_" -notmatch 'not suitable|NOT_FOUND|404') { throw }
    Write-Warning "$Path absent à la balise"
    Remove-Item $archive
    if (-not (Get-ChildItem $dest -Force | Select-Object -First 1)) { Remove-Item $dest }
    return $false
  }
  # -k : un dossier complété ne doit pas écraser les retouches de Rhénium.
  tar -xzkf $archive -C $dest
  if ($LASTEXITCODE -ne 0) { throw "Extraction de $Path impossible" }
  Remove-Item $archive
  Add-Content $manifest $Key
  $script:done += $Key
  return $true
}

function Get-File([string] $Path, [string] $Url) {
  if ($Path -in $script:done) { return $false }
  try { $text = (Get-Gitiles "$Url`?format=TEXT").Content } catch { Write-Warning "$Path absent à la balise"; return $false }
  $file = Join-Path $Src $Path
  New-Item -ItemType Directory -Force (Split-Path $file) | Out-Null
  [IO.File]::WriteAllBytes($file, [Convert]::FromBase64String($text))
  Write-Host $Path -ForegroundColor Cyan
  Add-Content $manifest $Path
  $script:done += $Path
  return $true
}

function Get-DepsRepos {
  $depsFile = Join-Path $root "DEPS-$tag"
  if (-not (Test-Path $depsFile)) {
    $text = (Get-Gitiles "$chromium/chromium/src/+/refs/tags/$tag/DEPS?format=TEXT").Content
    [IO.File]::WriteAllBytes($depsFile, [Convert]::FromBase64String($text))
  }
  $varArgs = $depsVars | ForEach-Object { '--var', $_ }
  $repos = [ordered]@{}
  $pending = [Collections.Generic.Queue[object]]::new()
  $pending.Enqueue(@{ File = $depsFile; Prefix = $null })
  while ($pending.Count) {
    $item = $pending.Dequeue()
    $prefixArgs = if ($item.Prefix) { @('--prefix', $item.Prefix) } else { @() }
    $lines = & $python -I $depsPy $item.File @prefixArgs @varArgs
    if ($LASTEXITCODE -ne 0) { throw "deps.py a échoué sur $($item.File)" }
    foreach ($line in $lines) {
      if ($line -match '^# recursedeps (.+)$') {
        $sub = $Matches[1]
        if (-not $repos.Contains($sub)) { continue }
        $subFile = Join-Path $root ("DEPS-" + ($sub -replace '/', '_') + "-$($repos[$sub].Rev.Substring(0, 8))")
        if (-not (Test-Path $subFile)) {
          try { $text = (Get-Gitiles "$($repos[$sub].Url)/+/$($repos[$sub].Rev)/DEPS?format=TEXT").Content } catch { continue }
          [IO.File]::WriteAllBytes($subFile, [Convert]::FromBase64String($text))
        }
        $pending.Enqueue(@{ File = $subFile; Prefix = $sub })
      } elseif ($line -match '^([^\t]+)\t([^\t]+)\t([0-9a-f]{40})$' -and -not $repos.Contains($Matches[1])) {
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
if ($Content) {
  foreach ($path in $inTreeFiles) {
    if (Get-File $path "$tagUrl/$path") { $added += $path }
  }
  $listing = (Get-Gitiles "$tagUrl/third_party/?format=JSON").Content
  foreach ($entry in ($listing.Substring($listing.IndexOf('{')) | ConvertFrom-Json).entries) {
    if ($entry.type -ne 'tree' -or $entry.name -match $skipThirdParty) { continue }
    $dir = "third_party/$($entry.name)"
    if (Get-Tree $dir "$archiveUrl/$dir.tar.gz") { $added += $dir }
  }
}
$repos = Get-DepsRepos
foreach ($path in $externalPaths) {
  if (-not $repos.Contains($path)) { Write-Warning "$path absent du DEPS évalué"; continue }
  $repo = $repos[$path]
  if (Get-Tree $path "$($repo.Url)/+archive/$($repo.Rev).tar.gz") { $added += $path }
}

foreach ($path in $Missing) {
  if (Test-Path (Join-Path $Src $path)) { continue }
  $repoPath = $repos.Keys | Where-Object { $path.StartsWith("$_/") } | Sort-Object Length -Descending | Select-Object -First 1
  $buildFile = $path -match '\.(gn|gni)$'
  $dir = (Split-Path $path) -replace '\\', '/'
  if ($repoPath -and $buildFile) {
    $repo = $repos[$repoPath]
    if (Get-File $path "$($repo.Url)/+/$($repo.Rev)/$($path.Substring($repoPath.Length + 1))") { $added += $path }
  } elseif ($repoPath) {
    $repo = $repos[$repoPath]
    $key = if ($repoPath -in $done) { "$repoPath (complet)" } else { $repoPath }
    if (Get-Tree $repoPath "$($repo.Url)/+archive/$($repo.Rev).tar.gz" $key) { $added += $repoPath }
  } elseif ($buildFile -or $dir -notmatch '/') {
    # Fichier seul pour les .gn/.gni et pour ce qui est posé à la racine d'un dossier de
    # premier niveau (chrome/VERSION ne doit pas amener tout chrome/).
    if (Get-File $path "$tagUrl/$path") { $added += $path }
  } else {
    $key = if ($dir -in $done) { "$dir (complet)" } else { $dir }
    if (Get-Tree $dir "$archiveUrl/$dir.tar.gz" $key) { $added += $dir }
  }
}

# Écrits normalement par gclient.
$gclientArgs = "$Src\build\config\gclient_args.gni"
if (-not (Test-Path $gclientArgs)) {
  Set-Content $gclientArgs -Encoding ascii @'
# Generated by Rhenium maj-chromium\fetch.ps1 (no gclient)
build_with_chromium = true
checkout_android = false
checkout_android_prebuilts_build_tools = false
checkout_android_native_support = false
checkout_ios_webkit = false
checkout_nacl = false
checkout_openxr = false
checkout_src_internal = false
checkout_rust = true
cros_boards = ""
cros_boards_with_qemu_images = ""
generate_location_tags = true
'@
  $added += 'build/config/gclient_args.gni'
}
$lastChange = "$Src\build\util\LASTCHANGE"
if (-not (Test-Path $lastChange)) {
  $commit = (Invoke-RestMethod "$chromium/chromium/src/+/refs/tags/${tag}?format=JSON").Substring(5) | ConvertFrom-Json
  # Format gitiles : « Mon Nov 02 22:52:40 2026 » (UTC, sans décalage).
  $when = $commit.committer.time -replace '\s+[+-]\d{4}$', ''
  $time = [DateTimeOffset]::ParseExact($when, 'ddd MMM dd HH:mm:ss yyyy',
    [Globalization.CultureInfo]::InvariantCulture, [Globalization.DateTimeStyles]::AssumeUniversal).ToUnixTimeSeconds()
  Set-Content $lastChange -Encoding ascii "LASTCHANGE=$($commit.commit)-refs/tags/$tag`nLASTCHANGE_YEAR=2026"
  Set-Content "$lastChange.committime" -Encoding ascii -NoNewline "$time"
  $added += 'build/util/LASTCHANGE', 'build/util/LASTCHANGE.committime'
}

# GN, ninja (CIPD) et Rust de Chromium, aux versions du DEPS, hors de l'arbre.
function Get-Cipd([string] $Package, [string] $Version, [string] $Dir, [string] $Exe) {
  if (Test-Path "$Dir\$Exe") { return }
  New-Item -ItemType Directory -Force $Dir | Out-Null
  $zip = "$Dir\cipd.zip"
  Write-Host "CIPD : $Package @ $Version" -ForegroundColor Cyan
  Get-Gitiles "https://chrome-infra-packages.appspot.com/dl/$Package/+/$Version" $zip
  Expand-Archive $zip $Dir -Force
  Remove-Item $zip
  if (-not (Test-Path "$Dir\$Exe")) { throw "$Exe absent du paquet $Package" }
}
$deps = Get-Content (Join-Path $root "DEPS-$tag") -Raw
$gnVersion = [regex]::Match($deps, "'gn_version':\s*'([^']+)'").Groups[1].Value
$ninjaVersion = [regex]::Match($deps, "'ninja_version':\s*'([^']+)'").Groups[1].Value
Get-Cipd 'gn/gn/windows-amd64' $gnVersion "$toolchains\gn-156" 'gn.exe'
Get-Cipd 'infra/3pp/tools/ninja/windows-amd64' $ninjaVersion "$toolchains\ninja-156" 'ninja.exe'

$rustDir = "$toolchains\rust-156"
if (-not (Test-Path "$rustDir\bin\rustc.exe")) {
  $object = [regex]::Match($deps, "'object_name':\s*'(Win/rust-toolchain-[^']+)'").Groups[1].Value
  New-Item -ItemType Directory -Force $rustDir | Out-Null
  $archive = "$rustDir\rust.tar.xz"
  Write-Host "Rust de Chromium : $object" -ForegroundColor Cyan
  Get-Gitiles "https://commondatastorage.googleapis.com/chromium-browser-clang/$object" $archive
  tar -xf $archive -C $rustDir
  if ($LASTEXITCODE -ne 0) { throw 'Extraction du Rust de Chromium impossible' }
  Remove-Item $archive
}

if (-not (Test-Path "$Src\.git")) {
  git -C $Src init -q -b origine
  git -C $Src config core.autocrlf false
  git -C $Src config core.longpaths true
}

# Node et ses modules (objets GCS du DEPS, pour WebUI et devtools), hors de git.
function Get-Gcs([string] $DepPath, [string] $Dest) {
  $m = [regex]::Match($deps, "(?s)'src/$([regex]::Escape($DepPath))':\s*\{(.*?)'object_name':\s*'([^']+)'.*?'sha256sum':\s*'([^']+)'.*?'output_file':\s*'([^']+)'")
  if (-not $m.Success) { throw "$DepPath absent du DEPS" }
  $bucket = [regex]::Match($m.Groups[1].Value, "'bucket':\s*'([^']+)'").Groups[1].Value
  if (-not $bucket) { $bucket = 'chromium-nodejs' }
  $object, $sha, $file = $m.Groups[2].Value, $m.Groups[3].Value, $m.Groups[4].Value
  New-Item -ItemType Directory -Force $Dest | Out-Null
  $out = Join-Path $Dest $file
  Write-Host "GCS : $DepPath ($file)" -ForegroundColor Cyan
  Get-Gitiles "https://storage.googleapis.com/$bucket/$object" $out
  if ((Get-FileHash $out -Algorithm SHA256).Hash -ne $sha) { Remove-Item $out; throw "sha256 de $file incorrect" }
  if ($file -like '*.tar.gz') {
    tar -xzkf $out -C $Dest
    if ($LASTEXITCODE -ne 0) { throw "Extraction de $file impossible" }
    Remove-Item $out
  }
}
if ($Content) {
  if (-not (Test-Path "$Src\third_party\node\win\node.exe")) { Get-Gcs 'third_party/node/win' "$Src\third_party\node\win" }
  if (-not (Test-Path "$Src\third_party\node\node_modules\typescript")) { Get-Gcs 'third_party/node/node_modules' "$Src\third_party\node\node_modules" }
  if (-not (Test-Path "$Src\buildtools\win-format\clang-format.exe")) { Get-Gcs 'buildtools/win-format' "$Src\buildtools\win-format" }
  # Go de Dawn : le générateur de sources de tint (third_party/dawn/tools/generate-sources-gn.py).
  $dawnDeps = Get-ChildItem $root -Filter 'DEPS-third_party_dawn-*' | Select-Object -First 1
  if ($dawnDeps) {
    $goVersion = [regex]::Match((Get-Content $dawnDeps.FullName -Raw), "'dawn_go_version':\s*'([^']+)'").Groups[1].Value
    Get-Cipd 'infra/3pp/tools/go/windows-amd64' $goVersion "$Src\third_party\dawn\tools\golang\windows-amd64" 'bin\go.exe'
  }
  $tsVersion = [regex]::Match($deps, "(?s)'chromium/third_party/typescript/windows-amd64',\s*'version':\s*'([^']+)'").Groups[1].Value
  Get-Cipd 'chromium/third_party/typescript/windows-amd64' $tsVersion "$Src\third_party\typescript\windows-amd64\src" 'lib\tsc.exe'
  $exclude = "$Src\.git\info\exclude"
  foreach ($line in '/buildtools/win-format/', '/third_party/dawn/tools/golang/', '/third_party/node/win/', '/third_party/node/node_modules/', '/third_party/typescript/windows-amd64/src/') {
    if (-not (Select-String -Path $exclude -SimpleMatch $line -Quiet -ErrorAction SilentlyContinue)) { Add-Content $exclude $line }
  }
}
# Récupérés lors d'un lancement interrompu, mais pas encore figés : seulement un chemin dont
# rien n'est suivi (un dossier suivi en partie se fige à la main, voir ..\j2\README.md).
foreach ($path in $done) {
  if ($path -match ' ' -or $path -in $added) { continue }
  if (-not (git -C $Src ls-files -- $path | Select-Object -First 1)) { $added += $path }
}
$added = @($added | Where-Object { Test-Path (Join-Path $Src $_) } | Select-Object -Unique)
if ($added) {
  # Seulement les chemins récupérés : les modifications Rhénium restent hors de « origine ».
  git -C $Src add -A -f -- $added
  git -C $Src commit -q -m "Chromium $tag : $($added.Count) chemins"
  Write-Host "Ajoutés et figés : $($added.Count) chemins" -ForegroundColor Green
} else {
  Write-Host 'Rien de nouveau.' -ForegroundColor Green
}
