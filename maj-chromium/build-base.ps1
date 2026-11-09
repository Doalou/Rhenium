#Requires -Version 7.0
<#
.SYNOPSIS
  Compile Chromium //base (156.0.8078.88) pour Windows 10 Mobile : target_os winuwp, ARM32,
  clang 24 de Chromium, STL MSVC 14.44, SDK 10.0.19041, Rust de Chromium (thumbv7a-uwp-windows-msvc).
.EXAMPLE
  ./build-base.ps1 -GenOnly   # correctifs, racine GN, args.gn, gn gen
  ./build-base.ps1            # + ninja : rhenium_base.lib, rhenium_base_tests.lib, rhenium_base_unittests.lib
.NOTES
  Arbre : F:\Rhenium\chromium-next\src (fetch.ps1). Outils : F:\Rhenium\toolchains\{clang,gn,ninja,rust}-156.
#>
param(
  [string] $Src = 'F:\Rhenium\chromium-next\src',
  [string] $OutDir = 'out\uwp-arm',
  [string] $LlvmDir = 'F:\Rhenium\toolchains\clang-156',
  [string[]] $ExtraArgs = @(),
  [string[]] $Targets = @('rhenium_base', 'rhenium_base_tests', 'rhenium_base_unittests'),
  [string[]] $PatchDirs = @(),
  [string[]] $V8Patches = @(),
  [string] $ExtraRootGn,
  [switch] $NoRust,
  [switch] $NoRustList,
  [switch] $KeepGoing,
  [switch] $GenOnly
)

$ErrorActionPreference = 'Stop'
$Src = (Resolve-Path $Src).Path
$toolchains = 'F:\Rhenium\toolchains'

. "$PSScriptRoot\..\j1-base\toolchain-env.ps1"
$vsPath = (Set-ToolchainEnv) -replace '\\VC\\Tools\\MSVC\\.*$', ''
$python = (Get-Command python -ErrorAction Stop).Source
$gn = "$toolchains\gn-156\gn.exe"
$ninja = "$toolchains\ninja-156\ninja.exe"
foreach ($tool in $gn, $ninja, "$LlvmDir\bin\clang-cl.exe") {
  if (-not (Test-Path $tool)) { throw "$tool absent : lancer $PSScriptRoot\fetch.ps1." }
}

# Correctifs de Chromium (patches\*-chromium-*.patch, *-base-*.patch), puis ceux des paliers suivants.
# Les correctifs de V8 (-V8Patches) s'appliquent dans v8/.
$patches = @(@(Get-ChildItem "$PSScriptRoot\patches\*.patch" -ErrorAction SilentlyContinue |
  Where-Object Name -notmatch '-v8-' | Sort-Object Name) +
  @($PatchDirs | ForEach-Object { Get-ChildItem "$_\*.patch" -ErrorAction SilentlyContinue | Sort-Object Name }) |
  ForEach-Object { @{ File = $_; Options = @() } })
$patches += @($V8Patches | ForEach-Object { @{ File = Get-Item $_; Options = @('--directory=v8') } })
foreach ($entry in $patches) {
  $patch = $entry.File
  $options = $entry.Options
  git -C $Src apply @options --check --reverse $patch.FullName 2>$null
  if ($LASTEXITCODE -eq 0) { continue }
  git -C $Src apply @options --whitespace=nowarn $patch.FullName
  if ($LASTEXITCODE -ne 0) { throw "Le correctif $($patch.Name) ne s'applique pas." }
  Write-Host "correctif appliqué : $($patch.Name)" -ForegroundColor Cyan
}

Copy-Item "$PSScriptRoot\gn\dot.gn" "$Src\.gn" -Force
$rootGn = Get-Content "$PSScriptRoot\gn\BUILD.gn" -Raw
if ($ExtraRootGn) { $rootGn += "`n" + (Get-Content $ExtraRootGn -Raw) }
Set-Content "$Src\BUILD.gn" $rootGn -NoNewline -Encoding utf8
New-Item -ItemType Directory -Force "$Src\rhenium" | Out-Null
Copy-Item "$PSScriptRoot\gn\base_smoke.cc", "$PSScriptRoot\gn\base_tests.cc", "$PSScriptRoot\gn\rust_libs.cc" "$Src\rhenium\" -Force
$fuzztestStub = "$Src\rhenium\fuzztest\third_party\fuzztest\src\fuzztest"
New-Item -ItemType Directory -Force $fuzztestStub | Out-Null
Copy-Item "$PSScriptRoot\gn\fuzztest.h" $fuzztestStub -Force

if ((Split-Path $LlvmDir -Qualifier) -ne (Split-Path $Src -Qualifier) -or $LlvmDir -match ' ') {
  throw "Le clang doit être sur le lecteur de l'arbre et sans espace ($LlvmDir)."
}
$llvmGn = '/' + ((Resolve-Path $LlvmDir).Path -replace '\\', '/')
$vsGn = $vsPath -replace '\\', '/'
$rust = if ($NoRust) { 'false' } else { 'true' }

$argsGn = @"
target_os = "winuwp"
target_cpu = "arm"
target_winuwp_family = "desktop"
is_debug = false
dcheck_always_on = false
is_component_build = false
symbol_level = 1
is_clang = true
clang_base_path = "$llvmGn"
clang_version = "24"
clang_use_chrome_plugins = false
use_lld = true
treat_warnings_as_errors = false
use_custom_libcxx = false
use_custom_libcxx_for_host = false
use_clang_modules = false
visual_studio_path = "$vsGn"
visual_studio_version = "2022"
windows_sdk_version = "10.0.19041.0"
wdk_path = "$vsGn"

enable_nocompile_tests = false
enable_precompiled_headers = false
use_partition_alloc_as_malloc = false
use_allocator_shim = false
enable_backup_ref_ptr_support = false
enable_rust = $rust
"@
if ($ExtraArgs) {
  $keys = $ExtraArgs | ForEach-Object { ($_ -split '=')[0].Trim() }
  $argsGn = ($argsGn -split '\r?\n' | Where-Object { ($_ -split '=')[0].Trim() -notin $keys }) -join "`n"
  $argsGn += "`n" + ($ExtraArgs -join "`n")
}
$out = Join-Path $Src $OutDir
New-Item -ItemType Directory -Force $out | Out-Null
Set-Content "$out\args.gn" $argsGn -Encoding utf8NoBOM

Push-Location $Src
try {
  $output = & $gn gen $OutDir "--script-executable=$python" 2>&1
  if ($LASTEXITCODE -ne 0) {
    $output | ForEach-Object { Write-Host "$_" }
    throw "gn gen a échoué (code $LASTEXITCODE)"
  }
  Write-Host "$($output | Select-Object -Last 1)"
  if ($GenOnly) { return }
  $keep = if ($KeepGoing) { '-k 0' } else { '' }
  cmd /c "`"$ninja`" -C $OutDir $keep $($Targets -join ' ') < NUL"
  if ($LASTEXITCODE -ne 0) { throw "ninja a échoué (code $LASTEXITCODE)" }

  # Liste des .rlib pour la sonde, lue dans le fichier de réponse du lien de rhenium_rust_libs
  # (que l'exécutable lui-même se lie ou non).
  if (-not $NoRust -and -not $NoRustList) {
    $rsp = "$OutDir\rhenium_rust_libs.exe.rsp"
    Remove-Item $rsp, "$OutDir\rhenium_rust_libs.exe" -ErrorAction SilentlyContinue
    cmd /c "`"$ninja`" -C $OutDir -d keeprsp rhenium_rust_libs < NUL" | Out-Null
    if (-not (Test-Path $rsp)) { throw "$rsp absent : la liste des .rlib n'a pas pu être relevée." }
    $rlibs = [regex]::Matches((Get-Content $rsp -Raw), '[^\s"]+\.rlib') | ForEach-Object {
      (Resolve-Path (Join-Path $out $_.Value)).Path } | Select-Object -Unique
    Set-Content "$out\rhenium_rust_libs.txt" $rlibs -Encoding utf8
    Write-Host "Rust : $(@($rlibs).Count) .rlib -> $out\rhenium_rust_libs.txt" -ForegroundColor Cyan
  }
} finally { Pop-Location }
