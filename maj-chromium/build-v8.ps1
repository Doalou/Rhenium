#Requires -Version 7.0
<#
.SYNOPSIS
  Compile V8 15.6 (v8_monolith) pour Windows ARM32 avec le clang 24 de Chromium et la STL MSVC 14.44.
.DESCRIPTION
  Applique patches\0001 et 0002 s'ils ne le sont pas, écrit args.gn, lance gn gen puis ninja.
  Arbre : F:\Rhenium\chromium-next\v8 (fetch-v8.ps1), dépôt git local, branche « origine » ;
  les correctifs se régénèrent par git diff --output (-- build, puis -- include src).
  Résultat : chromium-next\v8\out\arm.release\obj\v8_monolith.lib
.EXAMPLE
  ./build-v8.ps1             # gn gen + ninja v8_monolith
  ./build-v8.ps1 -GenOnly    # seulement gn gen
#>
param(
  [string] $V8 = 'F:\Rhenium\chromium-next\v8',
  [string] $OutDir = 'out\arm.release',
  [string] $LlvmDir = 'F:\Rhenium\toolchains\clang-156',
  [string] $ClangVersion = '24',
  [switch] $NoCxx23,
  [switch] $GenOnly,
  [int] $Jobs = 0,
  [switch] $KeepGoing,
  [string[]] $Targets = @('v8_monolith')
)

$ErrorActionPreference = 'Stop'
$V8 = (Resolve-Path $V8).Path

foreach ($patch in Get-ChildItem "$PSScriptRoot\patches\*-v8-*.patch" | Sort-Object Name) {
  git -C $V8 apply --check --reverse $patch.FullName 2>$null
  if ($LASTEXITCODE -eq 0) { continue }  # déjà appliqué
  git -C $V8 apply --whitespace=nowarn $patch.FullName
  if ($LASTEXITCODE -ne 0) { throw "Le correctif $($patch.Name) ne s'applique pas." }
  Write-Host "correctif appliqué : $($patch.Name)" -ForegroundColor Cyan
}

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

if ((Split-Path $LlvmDir -Qualifier) -ne (Split-Path $V8 -Qualifier) -or $LlvmDir -match ' ') {
  throw "Le clang doit être sur le lecteur de V8 et sans espace ($LlvmDir)."
}
$llvmGn = '/' + ((Resolve-Path $LlvmDir).Path -replace '\\', '/')
$vsGn = $vsPath -replace '\\', '/'
$cxx23 = if ($NoCxx23) { 'false' } else { 'true' }

$argsGn = @"
# Généré par Rhénium maj-chromium\build-v8.ps1
target_os = "win"
target_cpu = "arm"
is_debug = false
is_component_build = false
symbol_level = 1
is_clang = true
clang_base_path = "$llvmGn"
clang_version = "$ClangVersion"
clang_use_chrome_plugins = false
use_lld = true
treat_warnings_as_errors = false
use_custom_libcxx = false
use_custom_libcxx_for_host = false
use_clang_modules = false
use_cxx23 = $cxx23
visual_studio_path = "$vsGn"
visual_studio_version = "2022"
windows_sdk_version = "10.0.19041.0"
wdk_path = "$vsGn"

v8_monolithic = true
v8_use_external_startup_data = false
v8_enable_i18n_support = false
v8_depend_on_icu_data_file = false
v8_enable_temporal_support = false
enable_rust = false
v8_use_metagen_instance_types = false
v8_enable_webassembly = false
v8_enable_system_instrumentation = false
v8_enable_etw_stack_walking = false
v8_enable_sandbox = false
v8_enable_pointer_compression = false
v8_enable_gdbjit = false
v8_enable_test_features = false
"@
$out = Join-Path $V8 $OutDir
New-Item -ItemType Directory -Force $out | Out-Null
Set-Content "$out\args.gn" $argsGn -Encoding utf8NoBOM

Push-Location $V8
try {
  & "$V8\buildtools\win\gn.exe" gen $OutDir "--script-executable=$python"
  if ($LASTEXITCODE -ne 0) { throw "gn gen a échoué (code $LASTEXITCODE)" }
  if ($GenOnly) { return }
  $ninjaArgs = @('-C', $OutDir) + $Targets
  if ($Jobs -gt 0) { $ninjaArgs = @('-j', $Jobs) + $ninjaArgs }
  if ($KeepGoing) { $ninjaArgs = @('-k', '0') + $ninjaArgs }
  & "$V8\third_party\ninja\ninja.exe" @ninjaArgs
  if ($LASTEXITCODE -ne 0) { throw "ninja a échoué (code $LASTEXITCODE)" }
} finally { Pop-Location }
Write-Host "Bibliothèque : $out\obj\v8_monolith.lib" -ForegroundColor Green
