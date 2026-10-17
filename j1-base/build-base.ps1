<#
.SYNOPSIS
  Compile Chromium //base (116.0.5845.228) pour Windows 10 Mobile : target_os winuwp, ARM32,
  clang-cl (LLVM 23), STL MSVC 14.44, SDK 10.0.19041.
.EXAMPLE
  ./build-base.ps1 -GenOnly   # correctifs, racine GN, args.gn, gn gen
  ./build-base.ps1            # + ninja : obj\rhenium_base.lib (//base, dépendances, test de fumée)
                              #   et obj\rhenium_base_tests.lib + rhenium_base_unittests.lib (sous-ensemble de base_unittests)
  Utilisé aussi par ..\j2\build-content.ps1 (-ExtraArgs, -Targets, -PatchDirs, -ExtraRootGn).
#>
param(
  [string] $Src = "$PSScriptRoot\..\chromium\src",
  [string] $OutDir = 'out\uwp-arm',
  [string] $LlvmDir = "$env:ProgramFiles\LLVM",
  [string[]] $ExtraArgs = @(),  # lignes args.gn en plus
  [string[]] $Targets = @('rhenium_base', 'rhenium_base_tests', 'rhenium_base_unittests'),
  [string[]] $PatchDirs = @(),  # dossiers de correctifs appliqués après ceux de J1
  [string] $ExtraRootGn,       # ajouté à la racine GN (gn\BUILD.gn)
  [switch] $KeepGoing,         # ninja -k 0 : compiler tout ce qui peut l'être
  [switch] $GenOnly
)

$ErrorActionPreference = 'Stop'
$Src = (Resolve-Path $Src).Path
$root = Split-Path (Split-Path $Src)

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$toolsets = & $vswhere -all -prerelease -products * -property installationPath |
  ForEach-Object { Get-ChildItem "$_\VC\Tools\MSVC" -Directory -ErrorAction SilentlyContinue } |
  Sort-Object { [version]$_.Name }
$msvc = $toolsets | Where-Object { Test-Path "$($_.FullName)\lib\arm\libcmt.lib" } | Select-Object -Last 1
if (-not $msvc) { throw 'Aucun toolset MSVC avec les bibliothèques ARM32 (14.44).' }
$vsPath = $msvc.FullName -replace '\\VC\\Tools\\MSVC\\.*$', ''
$env:RHENIUM_MSVC_DIR = $msvc.FullName
# ATL ARM32 peut venir d'un autre toolset (14.30 ici) : compatibilité binaire de MSVC 14.x.
$atl = $toolsets | Where-Object { Test-Path "$($_.FullName)\atlmfc\lib\arm\atls.lib" } | Select-Object -Last 1
if ($atl) { $env:RHENIUM_ATL_DIR = "$($atl.FullName)\atlmfc" }
$env:RHENIUM_WINSDK_VERSION = '10.0.19041.0'
$env:RHENIUM_WINSDK_DIR = "${env:ProgramFiles(x86)}\Windows Kits\10"
$env:DEPOT_TOOLS_WIN_TOOLCHAIN = '0'
$python = (Get-Command python -ErrorAction Stop).Source
$clangVersion = (& "$LlvmDir\bin\clang-cl.exe" --version | Select-Object -First 1) -replace '^clang version (\d+).*$', '$1'

$patches = @(Get-ChildItem "$PSScriptRoot\patches\*.patch" -ErrorAction SilentlyContinue | Sort-Object Name) +
  @($PatchDirs | ForEach-Object { Get-ChildItem "$_\*.patch" -ErrorAction SilentlyContinue | Sort-Object Name })
foreach ($patch in $patches) {
  git -C $Src apply --check --reverse $patch.FullName 2>$null
  if ($LASTEXITCODE -eq 0) { continue }
  git -C $Src apply --whitespace=nowarn $patch.FullName
  if ($LASTEXITCODE -ne 0) { throw "Le correctif $($patch.Name) ne s'applique pas." }
  Write-Host "correctif appliqué : $($patch.Name)" -ForegroundColor Cyan
}

Copy-Item "$PSScriptRoot\gn\dot.gn" "$Src\.gn" -Force
$rootGn = Get-Content "$PSScriptRoot\gn\BUILD.gn" -Raw
if ($ExtraRootGn) { $rootGn += "`n" + (Get-Content $ExtraRootGn -Raw) }
Set-Content "$Src\BUILD.gn" $rootGn -NoNewline -Encoding utf8
New-Item -ItemType Directory -Force "$Src\rhenium" | Out-Null
Copy-Item "$PSScriptRoot\gn\base_smoke.cc", "$PSScriptRoot\gn\base_tests.cc" "$Src\rhenium\" -Force

# Jonction vers LLVM sur le lecteur du projet : le GN de 2023 ne gère pas un autre lecteur.
$llvmLink = Join-Path $root 'toolchains\llvm'
if (-not (Test-Path $llvmLink)) {
  New-Item -ItemType Directory -Force (Split-Path $llvmLink) | Out-Null
  New-Item -ItemType Junction -Path $llvmLink -Target $LlvmDir | Out-Null
}
$llvmGn = '/' + ((Resolve-Path $llvmLink).Path -replace '\\', '/')
$vsGn = $vsPath -replace '\\', '/'

$argsGn = @"
target_os = "winuwp"
target_cpu = "arm"
target_winuwp_family = "desktop"
is_debug = false
is_component_build = false
symbol_level = 1
is_clang = true
clang_base_path = "$llvmGn"
clang_version = "$clangVersion"
clang_use_chrome_plugins = false
use_lld = true
treat_warnings_as_errors = false
use_custom_libcxx = false
visual_studio_path = "$vsGn"
visual_studio_version = "2022"
windows_sdk_version = "10.0.19041.0"
wdk_path = "$vsGn"

enable_base_tracing = false
use_partition_alloc_as_malloc = false
use_allocator_shim = false
enable_backup_ref_ptr_support = false
enable_rust = false
"@
if ($ExtraArgs) { $argsGn += "`n" + ($ExtraArgs -join "`n") }
$out = Join-Path $Src $OutDir
New-Item -ItemType Directory -Force $out | Out-Null
Set-Content "$out\args.gn" $argsGn -Encoding ascii

$gn = "$root\v8\buildtools\win\gn.exe"  # même révision de GN que Chromium M116
Push-Location $Src
try {
  $output = & $gn gen $OutDir "--script-executable=$python" 2>&1
  if ($LASTEXITCODE -ne 0) {
    $output | Where-Object { "$_" -notmatch 'DeprecationWarning|utcfromtimestamp' } | ForEach-Object { Write-Host "$_" }
    throw "gn gen a échoué (code $LASTEXITCODE)"
  }
  Write-Host "$($output | Select-Object -Last 1)"
  if ($GenOnly) { return }
  $keep = if ($KeepGoing) { '-k 0' } else { '' }
  cmd /c "`"$root\v8\third_party\ninja\ninja.exe`" -C $OutDir $keep $($Targets -join ' ') < NUL"
  if ($LASTEXITCODE -ne 0) { throw "ninja a échoué (code $LASTEXITCODE)" }
} finally { Pop-Location }
