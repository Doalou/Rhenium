<#
.SYNOPSIS
  Récupère V8 11.6.189.22 (base Chromium M116) et le strict nécessaire pour compiler v8_monolith,
  sans gclient : clones superficiels aux commits du DEPS de V8, GN et ninja depuis CIPD.
.DESCRIPTION
  Environ 200 Mo au lieu des 2 à 4 Go d'un « fetch v8 » complet. Sont volontairement omis :
  tests (test262, mozilla, benchmarks, googletest), Android, Fuchsia, Perfetto/protobuf, ICU
  (J1 compile sans Intl), clang de Chromium (on utilise LLVM 23.1.3), reclient, luci-go.
  Relancer le script est sans effet sur les dépôts déjà présents.
.EXAMPLE
  ./fetch.ps1                     # dans F:\Rhenium\v8
  ./fetch.ps1 -WithIcu            # ajoute third_party/icu (Intl, pour les jalons suivants)
#>
param(
  [string] $Dest = "$PSScriptRoot\..\v8",
  [switch] $WithIcu
)

$ErrorActionPreference = 'Stop'
$cr = 'https://chromium.googlesource.com'
$v8Tag = '11.6.189.22'

# Commits repris du fichier DEPS de V8 au tag 11.6.189.22.
$deps = [ordered]@{
  'build'                    = "$cr/chromium/src/build.git@04466898300bf849d1650cb35dcf2eb353e8e1e1"
  'buildtools'               = "$cr/chromium/src/buildtools.git@3739a3619309af3b788379ad0936ca00b981616e"
  'base/trace_event/common'  = "$cr/chromium/src/base/trace_event/common.git@147f65333c38ddd1ebf554e89965c243c8ce50b3"
  'third_party/zlib'         = "$cr/chromium/src/third_party/zlib.git@e6795474e4885ae55b6c6c0a612d0889e2ebd8d9"
  'third_party/jinja2'       = "$cr/chromium/src/third_party/jinja2.git@264c07d7e64f2874434a3b8039e101ddf1b01e7e"
  'third_party/markupsafe'   = "$cr/chromium/src/third_party/markupsafe.git@13f4e8c9e206567eeb13bf585406ddc574005748"
  'tools/clang'              = "$cr/chromium/src/tools/clang.git@573d371dff6d798388c9b400718e4b30ad683830"
}
if ($WithIcu) { $deps['third_party/icu'] = "$cr/chromium/deps/icu.git@e8c3bc9ea97d4423ad0515e5f1c064f486dae8b1" }

function Get-Shallow([string] $Url, [string] $Ref, [string] $Dir) {
  if (Test-Path "$Dir\.git") { Write-Host "déjà présent : $Dir"; return }
  Write-Host "clone superficiel : $Url @ $Ref" -ForegroundColor Cyan
  New-Item -ItemType Directory -Force $Dir | Out-Null
  git -C $Dir init -q
  git -C $Dir config core.longpaths true
  git -C $Dir remote add origin $Url
  git -C $Dir -c protocol.version=2 fetch -q --depth 1 origin $Ref
  if ($LASTEXITCODE -ne 0) { throw "échec du fetch $Url @ $Ref" }
  git -C $Dir -c advice.detachedHead=false checkout -q FETCH_HEAD
  if ($LASTEXITCODE -ne 0) { throw "échec du checkout dans $Dir" }
}

Get-Shallow "$cr/v8/v8.git" "refs/tags/$v8Tag" $Dest
foreach ($path in $deps.Keys) {
  $url, $sha = $deps[$path] -split '@'
  Get-Shallow $url $sha (Join-Path $Dest $path)
}

# GN et ninja : paquets CIPD (des zip), aux versions du DEPS.
function Get-Cipd([string] $Package, [string] $Version, [string] $Dir, [string] $Exe) {
  if (Test-Path "$Dir\$Exe") { Write-Host "déjà présent : $Dir\$Exe"; return }
  New-Item -ItemType Directory -Force $Dir | Out-Null
  $zip = "$Dir\cipd.zip"
  Write-Host "CIPD : $Package @ $Version" -ForegroundColor Cyan
  Invoke-WebRequest "https://chrome-infra-packages.appspot.com/dl/$Package/+/$Version" -OutFile $zip
  Expand-Archive $zip $Dir -Force
  Remove-Item $zip
  if (-not (Test-Path "$Dir\$Exe")) { throw "$Exe absent du paquet $Package" }
}
Get-Cipd 'gn/gn/windows-amd64' 'git_revision:4bd1a77e67958fb7f6739bd4542641646f264e5d' "$Dest\buildtools\win" 'gn.exe'
Get-Cipd 'infra/3pp/tools/ninja/windows-amd64' 'version:2@1.11.1.chromium.6' "$Dest\third_party\ninja" 'ninja.exe'

# Fichiers que les hooks de gclient auraient produits.
$lastchange = "$Dest\build\util\LASTCHANGE"
if (-not (Test-Path $lastchange)) {
  $rev = git -C $Dest rev-parse HEAD
  Set-Content $lastchange "LASTCHANGE=$rev-refs/tags/$v8Tag`nLASTCHANGE_YEAR=2023" -NoNewline
  Set-Content "$lastchange.committime" '1690000000' -NoNewline
}
$gclientArgs = "$Dest\build\config\gclient_args.gni"
if (-not (Test-Path $gclientArgs)) { Set-Content $gclientArgs '# Généré par Rhénium j1\fetch.ps1 (pas de gclient)' }

$size = (Get-ChildItem $Dest -Recurse -File -Force | Measure-Object Length -Sum).Sum
Write-Host ("V8 $v8Tag prêt dans $((Resolve-Path $Dest).Path) : {0:N0} Mo" -f ($size / 1MB)) -ForegroundColor Green
