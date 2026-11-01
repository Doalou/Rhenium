#Requires -Version 7.0
<#
.SYNOPSIS
  Compile V8 11.6 (v8_monolith) pour Windows ARM32 avec clang-cl (LLVM 23) et la STL MSVC 14.44.
.DESCRIPTION
  Applique les correctifs de patches\ (s'ils ne le sont pas déjà), écrit args.gn, lance gn gen puis ninja.
  Résultat : ..\v8\out\arm.release\obj\v8_monolith.lib
.EXAMPLE
  ./build-v8.ps1             # gn gen + ninja v8_monolith
  ./build-v8.ps1 -GenOnly    # seulement gn gen
#>
param(
  [string] $V8 = "$PSScriptRoot\..\v8",
  [string] $OutDir = 'out\arm.release',
  [string] $LlvmDir = "$env:ProgramFiles\LLVM",
  [switch] $GenOnly,
  [int] $Jobs = 0
)

$ErrorActionPreference = 'Stop'
$V8 = (Resolve-Path $V8).Path

# --- Toolset MSVC 14.44 (dernier avec ARM32) et Windows SDK 10.0.19041 (dernier avec les libs ARM32)
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

# --- Correctifs (patches\*.patch, appliqués dans l'ordre ; chacun dit à quel dépôt il s'applique)
foreach ($patch in Get-ChildItem "$PSScriptRoot\patches\*.patch" | Sort-Object Name) {
  $repo = if ($patch.Name -match '^\d+-build-') { "$V8\build" } else { $V8 }
  git -C $repo apply --check --reverse $patch.FullName 2>$null
  if ($LASTEXITCODE -eq 0) { continue }  # déjà appliqué
  git -C $repo apply --whitespace=nowarn $patch.FullName
  if ($LASTEXITCODE -ne 0) { throw "Le correctif $($patch.Name) ne s'applique pas." }
  Write-Host "correctif appliqué : $($patch.Name)" -ForegroundColor Cyan
}

# Le GN de 2023 ne sait pas rendre relatif un chemin situé sur un autre lecteur (il produit
# ..\..\..\C:\…) et ninja lance clang-cl sans guillemets. On passe donc par une jonction sans
# espace sur le lecteur de V8 (supprimer la jonction ne touche pas à LLVM).
$llvmLink = Join-Path (Split-Path $V8) 'toolchains\llvm'
if ((Split-Path $LlvmDir -Qualifier) -ne (Split-Path $V8 -Qualifier) -or $LlvmDir -match ' ') {
  if (-not (Test-Path $llvmLink)) {
    New-Item -ItemType Directory -Force (Split-Path $llvmLink) | Out-Null
    New-Item -ItemType Junction -Path $llvmLink -Target $LlvmDir | Out-Null
  }
  $llvmForGn = $llvmLink
} else {
  $llvmForGn = $LlvmDir
}
$llvmGn = '/' + ((Resolve-Path $llvmForGn).Path -replace '\\', '/')  # chemin absolu GN : /F:/…

# googletest n'est pas téléchargé : V8 n'a besoin que de FRIEND_TEST (stubs\gtest_prod.h)
$gtestProd = "$V8\third_party\googletest\src\googletest\include\gtest\gtest_prod.h"
if (-not (Test-Path $gtestProd)) {
  New-Item -ItemType Directory -Force (Split-Path $gtestProd) | Out-Null
  Copy-Item "$PSScriptRoot\stubs\gtest_prod.h" $gtestProd
}
$vsGn = $vsPath -replace '\\', '/'
$argsGn = @"
# Généré par Rhénium j1\build-v8.ps1
target_os = "win"
target_cpu = "arm"
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

v8_monolithic = true
v8_use_external_startup_data = false
v8_enable_i18n_support = false
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
Set-Content "$out\args.gn" $argsGn -Encoding ascii

Push-Location $V8
try {
  & "$V8\buildtools\win\gn.exe" gen $OutDir "--script-executable=$python"
  if ($LASTEXITCODE -ne 0) { throw "gn gen a échoué (code $LASTEXITCODE)" }
  if ($GenOnly) { return }
  $ninjaArgs = @('-C', $OutDir, 'v8_monolith')
  if ($Jobs -gt 0) { $ninjaArgs = @('-j', $Jobs) + $ninjaArgs }
  & "$V8\third_party\ninja\ninja.exe" @ninjaArgs
  if ($LASTEXITCODE -ne 0) { throw "ninja a échoué (code $LASTEXITCODE)" }
} finally { Pop-Location }
Write-Host "Bibliothèque : $out\obj\v8_monolith.lib" -ForegroundColor Green
