<#
.SYNOPSIS
  Compile Chromium //base (116.0.5845.228) pour Windows 10 Mobile : target_os winuwp, ARM32,
  clang-cl (LLVM 23), STL MSVC 14.44, SDK 10.0.19041.
.EXAMPLE
  ./build-base.ps1 -GenOnly   # correctifs, racine GN, args.gn, gn gen
  ./build-base.ps1            # + ninja : obj\rhenium_base.lib (//base, dépendances, test de fumée)
#>
param(
  [string] $Src = "$PSScriptRoot\..\chromium\src",
  [string] $OutDir = 'out\uwp-arm',
  [string] $LlvmDir = "$env:ProgramFiles\LLVM",
  [switch] $GenOnly
)

$ErrorActionPreference = 'Stop'
$Src = (Resolve-Path $Src).Path
$root = Split-Path (Split-Path $Src)

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msvc = & $vswhere -all -prerelease -products * -property installationPath |
  ForEach-Object { Get-ChildItem "$_\VC\Tools\MSVC" -Directory -ErrorAction SilentlyContinue } |
  Where-Object { Test-Path "$($_.FullName)\lib\arm\libcmt.lib" } | Sort-Object { [version]$_.Name } | Select-Object -Last 1
if (-not $msvc) { throw 'Aucun toolset MSVC avec les bibliothèques ARM32 (14.44).' }
$vsPath = $msvc.FullName -replace '\\VC\\Tools\\MSVC\\.*$', ''
$env:RHENIUM_MSVC_DIR = $msvc.FullName
$env:RHENIUM_WINSDK_VERSION = '10.0.19041.0'
$env:RHENIUM_WINSDK_DIR = "${env:ProgramFiles(x86)}\Windows Kits\10"
$env:DEPOT_TOOLS_WIN_TOOLCHAIN = '0'
$python = (Get-Command python -ErrorAction Stop).Source
$clangVersion = (& "$LlvmDir\bin\clang-cl.exe" --version | Select-Object -First 1) -replace '^clang version (\d+).*$', '$1'

foreach ($patch in Get-ChildItem "$PSScriptRoot\patches\*.patch" -ErrorAction SilentlyContinue | Sort-Object Name) {
  git -C $Src apply --check --reverse $patch.FullName 2>$null
  if ($LASTEXITCODE -eq 0) { continue }
  git -C $Src apply --whitespace=nowarn $patch.FullName
  if ($LASTEXITCODE -ne 0) { throw "Le correctif $($patch.Name) ne s'applique pas." }
  Write-Host "correctif appliqué : $($patch.Name)" -ForegroundColor Cyan
}

Copy-Item "$PSScriptRoot\gn\dot.gn" "$Src\.gn" -Force
Copy-Item "$PSScriptRoot\gn\BUILD.gn" "$Src\BUILD.gn" -Force
New-Item -ItemType Directory -Force "$Src\rhenium" | Out-Null
Copy-Item "$PSScriptRoot\gn\base_smoke.cc" "$Src\rhenium\base_smoke.cc" -Force

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
  cmd /c "`"$root\v8\third_party\ninja\ninja.exe`" -C $OutDir rhenium_base < NUL"
  if ($LASTEXITCODE -ne 0) { throw "ninja a échoué (code $LASTEXITCODE)" }
} finally { Pop-Location }
