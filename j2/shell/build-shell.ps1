#Requires -Version 7.0
<#
.SYNOPSIS
  Compile la coquille XAML de Rhénium (shell_app.cpp, C++/WinRT) avec MSVC, exceptions
  comprises, en rhenium_xaml_shell.lib, liée ensuite par la cible GN rhenium_shell.
.EXAMPLE
  ./build-shell.ps1 -OutDir ..\..\chromium\src\out\content-arm   # appelé par ..\build-content.ps1
#>
param(
  [Parameter(Mandatory)] [string] $OutDir
)

$ErrorActionPreference = 'Stop'
$kits = "${env:ProgramFiles(x86)}\Windows Kits\10"
$sdkVer = '10.0.19041.0'  # dernier SDK avec les bibliothèques ARM32
$inc = "$kits\Include\$sdkVer"
$OutDir = (Resolve-Path $OutDir).Path

function Get-Newest($items) { $items | Sort-Object { [version]$_.Name } | Select-Object -Last 1 }

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msvc = Get-Newest (& $vswhere -all -prerelease -products * -property installationPath |
  ForEach-Object { Get-ChildItem "$_\VC\Tools\MSVC" -Directory -ErrorAction SilentlyContinue } |
  Where-Object { Test-Path "$($_.FullName)\bin\Hostx64\arm\cl.exe" })
if (-not $msvc) { throw 'Aucun toolset MSVC ne cible ARM32 (14.44).' }
$msvc = $msvc.FullName
$env:PATH = "$msvc\bin\Hostx64\x64;$env:PATH"

# C++/WinRT récent a retiré ARM32 : copie corrigée de base.h, devant celle du SDK.
. "$PSScriptRoot\..\..\sonde\cppwinrt-arm32.ps1"
$cppwinrt = Find-CppWinrt $kits
$work = "$OutDir\rhenium_xaml_shell"
Write-CppWinrtArm32 $cppwinrt "$work\cppwinrt-arm32"

$obj = "$work\shell_app.obj"
# /guard:cf : l'exe est lié avec Control Flow Guard ; sans lui, les fonctions de la coquille
# manquent à la table CFG et le premier rappel de XAML vers elles arrête l'app.
$cl = @('/nologo', '/c', '/Brepro', '/guard:cf', '/std:c++20', '/EHsc', '/O2', '/MT', '/W3', '/permissive-', '/utf-8', '/bigobj',
  '/DWINAPI_FAMILY=WINAPI_FAMILY_APP', '/DUNICODE', '/D_UNICODE', '/DNOMINMAX',
  "/I$work\cppwinrt-arm32", "/I$inc\ucrt", "/I$inc\um", "/I$inc\shared", "/I$inc\winrt", "/I$cppwinrt",
  "/I$msvc\include", "/I$PSScriptRoot\..\app", "/Fo$obj", "$PSScriptRoot\shell_app.cpp")
$output = & "$msvc\bin\Hostx64\arm\cl.exe" @cl 2>&1
if ($LASTEXITCODE -ne 0) { $output | Out-Host; throw "cl.exe a échoué (code $LASTEXITCODE)" }

# Sorties reproductibles (/Brepro) : la bibliothèque n'est remplacée que si elle change, et
# l'exe, que ninja ne sait pas lier à elle, n'est alors supprimé que pour être relié.
$lib = "$OutDir\rhenium_xaml_shell.lib"
$output = & "$msvc\bin\Hostx64\arm\lib.exe" /nologo /Brepro "/OUT:$work\rhenium_xaml_shell.lib" $obj 2>&1
if ($LASTEXITCODE -ne 0) { $output | Out-Host; throw "lib.exe a échoué (code $LASTEXITCODE)" }
$changed = -not (Test-Path $lib) -or
  (Get-FileHash $lib).Hash -ne (Get-FileHash "$work\rhenium_xaml_shell.lib").Hash
if ($changed) {
  Copy-Item "$work\rhenium_xaml_shell.lib" $lib -Force
  $exe = "$OutDir\rhenium_shell.exe"
  if (Test-Path $exe) { Remove-Item $exe }
}
Write-Host "Coquille XAML : $lib" -ForegroundColor Cyan
