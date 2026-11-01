#Requires -Version 7.0
<#
.SYNOPSIS
  Assemble a32_probe.s (A32) avec LLVM et produit a32_probe.h : octets + décalage de chaque fonction.
  À relancer seulement si a32_probe.s change (le .h généré est versionné à côté).
#>
param([string] $LlvmDir = "$env:ProgramFiles\LLVM\bin")
$ErrorActionPreference = 'Stop'
$src = "$PSScriptRoot\a32_probe.s"
$tmp = New-Item -ItemType Directory -Force "$PSScriptRoot\..\out\a32"
& "$LlvmDir\clang.exe" --target=armv7a-none-eabihf -marm -mfpu=vfpv3 -c $src -o "$tmp\a32.o"
if ($LASTEXITCODE) { throw 'assemblage A32 échoué' }
& "$LlvmDir\llvm-objcopy.exe" -O binary --only-section=.text "$tmp\a32.o" "$tmp\a32.bin"
$bytes = [IO.File]::ReadAllBytes("$tmp\a32.bin")
$syms = & "$LlvmDir\llvm-nm.exe" --defined-only "$tmp\a32.o" | Where-Object { $_ -match '^([0-9a-f]+) t (a32_\w+)$' } |
  ForEach-Object { "constexpr size_t k_$($Matches[2]) = 0x$($Matches[1]);" }
$hex = ($bytes | ForEach-Object { '0x{0:X2}' -f $_ }) -join ', '
@"
// Généré par gen_a32.ps1 depuis a32_probe.s : ne pas modifier à la main.
#pragma once
#include <cstddef>
#include <cstdint>
alignas(4) inline constexpr uint8_t kA32Code[] = {$hex};
$($syms -join "`n")
"@ | Set-Content "$PSScriptRoot\a32_probe.h" -Encoding utf8
& "$LlvmDir\llvm-objdump.exe" -d "$tmp\a32.o"
