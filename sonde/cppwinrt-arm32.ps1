#Requires -Version 7.0
# C++/WinRT pour ARM32, commun à build.ps1 et ..\j2\shell\build-shell.ps1. À charger par
# « . "$PSScriptRoot\...\cppwinrt-arm32.ps1" ».

# C++/WinRT du SDK le plus récent : ceux d'avant 2020 exigent les coroutines expérimentales,
# refusées en C++20.
function Find-CppWinrt([string] $Kits = "${env:ProgramFiles(x86)}\Windows Kits\10") {
  $dir = Get-ChildItem "$Kits\Include" -Directory | Where-Object { Test-Path "$($_.FullName)\cppwinrt\winrt\base.h" } |
    Sort-Object { [version]$_.Name } | Select-Object -Last 1
  if (-not $dir) { throw "Aucun SDK Windows avec C++/WinRT dans $Kits\Include." }
  "$($dir.FullName)\cppwinrt"
}

# C++/WinRT récent a retiré ARM32 : la branche _M_ARM est remise dans une copie de base.h,
# écrite dans $Dest\winrt et à placer devant celle du SDK dans les chemins d'inclusion (le SDK
# n'est pas modifié).
function Write-CppWinrtArm32([string] $CppWinrt, [string] $Dest) {
  $base = Get-Content "$CppWinrt\winrt\base.h" -Raw
  $arm32Read = "#elif defined _M_ARM`n        int32_t const result = __iso_volatile_load32(reinterpret_cast<int32_t const volatile*>(target));`n" +
    "        __dmb(_ARM_BARRIER_ISH);`n        return result;`n`$0"
  $patched = [regex]::new('#else\r?\n#error Unsupported architecture\r?\n').Replace($base, $arm32Read, 1)
  $patched = $patched.Replace('#if !defined _M_IX86 && !defined _M_X64 && !defined _M_ARM64',
    '#if !defined _M_IX86 && !defined _M_X64 && !defined _M_ARM64 && !defined _M_ARM')
  if ($patched -notmatch 'defined _M_ARM\r?\n        int32_t' -or $patched -notmatch '!defined _M_ARM\r?\n') {
    throw "Le correctif ARM32 de C++/WinRT ne s'applique plus à $CppWinrt\winrt\base.h"
  }
  New-Item -ItemType Directory -Force "$Dest\winrt" | Out-Null
  Set-Content "$Dest\winrt\base.h" $patched -NoNewline -Encoding utf8
}
