#Requires -Version 7.0
<#
.SYNOPSIS
  Récupère V8 15.6.75.36 (Chromium 156.0.8078.88) et ce qu'il faut pour compiler v8_monolith,
  dans ..\..\chromium-next\v8, sans gclient ni historique (archives gitiles).
.DESCRIPTION
  Un dépôt git local à la racine garde l'état d'origine (branche « origine ») : les correctifs
  se produisent ensuite par git diff, chemins relatifs à la racine de V8 (build/… compris).
.EXAMPLE
  ./fetch-v8.ps1
  ./fetch-v8.ps1 -Missing third_party/rust/BUILD.gn   # fichier réclamé par gn gen
#>
param(
  [string[]] $Missing = @(),
  [string] $Dest = 'F:\Rhenium\chromium-next\v8'
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$v8Rev = 'd72e817d59894a332093c48beae5749fde6f320f'  # 15.6.75.36, DEPS de 156.0.8078.88
$chromium = 'https://chromium.googlesource.com'

$wanted = @(
  'build', 'buildtools', 'third_party/abseil-cpp', 'third_party/dragonbox/src',
  'third_party/fast_float/src', 'third_party/fp16/src', 'third_party/highway/src',
  'third_party/simdutf', 'third_party/zlib', 'third_party/jinja2', 'third_party/markupsafe',
  'third_party/partition_alloc', 'third_party/googletest/src', 'tools/clang', 'tools/rust'
)

function Get-Gitiles([string] $Url, [string] $OutFile) {
  for ($try = 1; ; $try++) {
    try {
      if ($OutFile) { Invoke-WebRequest $Url -OutFile $OutFile -UseBasicParsing; return }
      return Invoke-WebRequest $Url -UseBasicParsing
    } catch { if ($try -ge 4) { throw }; Start-Sleep -Seconds (5 * $try) }
  }
}

New-Item -ItemType Directory -Force $Dest | Out-Null
$Dest = (Resolve-Path $Dest).Path
$manifest = Join-Path $Dest '.rhenium-fetched.txt'
$done = if (Test-Path $manifest) { @(Get-Content $manifest) } else { @() }

function Get-Tree([string] $Path, [string] $Url) {
  if ($Path -in $script:done) { return $false }
  $dir = if ($Path -eq '.') { $Dest } else { Join-Path $Dest $Path }
  New-Item -ItemType Directory -Force $dir | Out-Null
  $archive = [IO.Path]::GetTempFileName()
  Write-Host $Path -ForegroundColor Cyan
  Get-Gitiles $Url $archive
  tar -xzf $archive -C $dir
  if ($LASTEXITCODE -ne 0) { throw "Extraction de $Path impossible" }
  Remove-Item $archive
  Add-Content $manifest $Path
  $script:done += $Path
  return $true
}

$added = @()
if (Get-Tree '.' "$chromium/v8/v8/+archive/$v8Rev.tar.gz") { $added += '.' }

$depsFile = Join-Path $Dest 'DEPS'
$lines = & python -I "$PSScriptRoot\..\j2\deps.py" $depsFile --var build_with_chromium=False --var checkout_rust=False
if ($LASTEXITCODE -ne 0) { throw 'deps.py a échoué' }
$repos = @{}
foreach ($line in $lines) {
  if ($line -match '^([^\t]+)\t([^\t]+)\t([0-9a-f]{40})$') { $repos[$Matches[1]] = @{ Url = $Matches[2] -replace '\.git$', ''; Rev = $Matches[3] } }
}

foreach ($path in $wanted + $Missing) {
  if ($path -in $Missing -and (Test-Path (Join-Path $Dest $path))) { continue }
  $repoPath = if ($repos.ContainsKey($path)) { $path } else {
    $repos.Keys | Where-Object { $path.StartsWith("$_/") } | Sort-Object Length -Descending | Select-Object -First 1
  }
  if (-not $repoPath) { Write-Warning "$path absent du DEPS de V8"; continue }
  $repo = $repos[$repoPath]
  if ($path -in $Missing -and $path -match '\.(gn|gni)$') {
    # gn gen n'a besoin que du fichier de build, pas du dépôt entier (ICU, par exemple).
    if ($path -in $script:done) { continue }
    $file = Join-Path $Dest $path
    New-Item -ItemType Directory -Force (Split-Path $file) | Out-Null
    $text = (Get-Gitiles "$($repo.Url)/+/$($repo.Rev)/$($path.Substring($repoPath.Length + 1))?format=TEXT").Content
    [IO.File]::WriteAllBytes($file, [Convert]::FromBase64String($text))
    Write-Host $path -ForegroundColor Cyan
    Add-Content $manifest $path
    $added += $path
    continue
  }
  if (Get-Tree $repoPath "$($repo.Url)/+archive/$($repo.Rev).tar.gz") { $added += $repoPath }
}

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
$deps = Get-Content $depsFile -Raw
$gn = [regex]::Match($deps, "'gn_version':\s*'([^']+)'").Groups[1].Value
$ninja = [regex]::Match($deps, "'ninja_version':\s*'([^']+)'").Groups[1].Value
Get-Cipd 'gn/gn/windows-amd64' $gn "$Dest\buildtools\win" 'gn.exe'
Get-Cipd 'infra/3pp/tools/ninja/windows-amd64' $ninja "$Dest\third_party\ninja" 'ninja.exe'

$lastchange = "$Dest\build\util\LASTCHANGE"
if (-not (Test-Path $lastchange)) {
  Set-Content $lastchange "LASTCHANGE=$v8Rev-refs/heads/15.6`nLASTCHANGE_YEAR=2026" -NoNewline -Encoding ascii
  Set-Content "$lastchange.committime" '1793000000' -NoNewline -Encoding ascii
}
$gclientArgs = "$Dest\build\config\gclient_args.gni"
if (-not (Test-Path $gclientArgs)) {
  Set-Content $gclientArgs "# Generated by Rhenium maj-chromium\fetch-v8.ps1 (no gclient)`ncheckout_rust = false`n" -Encoding ascii
}

if (-not (Test-Path "$Dest\.git")) {
  git -C $Dest init -q -b origine
  git -C $Dest config core.autocrlf false
  git -C $Dest config core.longpaths true
}
if ($added) {
  # Seulement les chemins récupérés : les modifications Rhénium restent hors de « origine ».
  git -C $Dest add -A -f -- (@($added) + '.rhenium-fetched.txt' | ForEach-Object { if ($_ -eq '.') { ':/' } else { $_ } })
  git -C $Dest commit -q -m "V8 15.6.75.36 : $($added -join ', ')"
  Write-Host "Ajoutés et figés : $($added -join ', ')" -ForegroundColor Green
} else {
  Write-Host 'Rien de nouveau.' -ForegroundColor Green
}
