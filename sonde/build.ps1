<#
.SYNOPSIS
  Compile et empaquette la sonde Rhénium en .appx signé.
.EXAMPLE
  ./build.ps1                     # ARM32 (Lumia), MSVC, avec la capability codeGeneration
  ./build.ps1 -Arch x64           # build de contrôle sur PC
  ./build.ps1 -Compiler clang     # + module clang-cl sans exceptions (src\clang_probe.cpp), lié par lld-link
  ./build.ps1 -NoCodeGeneration   # variante sans codeGeneration, pour comparer
  ./build.ps1 -WithRust           # lie la bibliothèque Rust de test (rust\), nightly + rust-src requis
  ./build.ps1 -WithA32            # ajoute le test du mode A32 de V8 (..\j1\app\a32_probe.h), ARM32 seulement
  ./build.ps1 -WithV8             # lie V8 (..\v8\out\arm.release\obj\v8_monolith.lib) et exécute du JavaScript ;
                                  # implique -Compiler clang, ARM32 seulement
#>
param(
  [ValidateSet('arm', 'arm64', 'x64')] [string] $Arch = 'arm',
  [ValidateSet('msvc', 'clang')] [string] $Compiler = 'msvc',
  [switch] $NoCodeGeneration,
  [switch] $WithRust,
  [switch] $WithA32,
  [switch] $WithV8,
  [int] $V8First = 0,  # avec -WithV8 : configuration V8 (base 0) du premier lancement après installation
  [string] $ImportCheck,  # exe dont la sonde vérifiera chaque import sur le téléphone (DLL hôte + export)
  [string] $ClangDir  # dossier contenant clang-cl.exe et lld-link.exe, sinon recherche automatique
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$kits = "${env:ProgramFiles(x86)}\Windows Kits\10"
if ($WithV8) {
  if ($Arch -ne 'arm') { throw '-WithV8 ne concerne que -Arch arm.' }
  $Compiler = 'clang'  # la cale V8 est compilée par clang-cl, comme V8
}

function Invoke-Tool {
  param([string] $Exe, [string[]] $Arguments)
  $output = & $Exe @Arguments 2>&1
  if ($LASTEXITCODE -ne 0) {
    $output | Out-Host
    throw "$(Split-Path $Exe -Leaf) a échoué (code $LASTEXITCODE)"
  }
}

function Get-Newest($items) { $items | Sort-Object { [version]$_.Name } | Select-Object -Last 1 }

# --- Windows SDK : le plus récent qui a encore les bibliothèques de l'architecture visée
$sdk = Get-Newest (Get-ChildItem "$kits\Lib" -Directory | Where-Object { Test-Path "$($_.FullName)\um\$Arch\WindowsApp.lib" })
if (-not $sdk) {
  throw "Aucun Windows SDK avec les bibliothèques $Arch. Pour ARM32, installer le Windows 10 SDK 10.0.19041 (voir README)."
}
$sdkVer = $sdk.Name
$inc = "$kits\Include\$sdkVer"
# C++/WinRT : toujours le plus récent (ceux d'avant 2020 exigent les coroutines expérimentales, refusées en C++20)
$cppwinrt = (Get-Newest (Get-ChildItem "$kits\Include" -Directory | Where-Object { Test-Path "$($_.FullName)\cppwinrt\winrt\base.h" })).FullName + '\cppwinrt'
$sdkBin = (Get-Newest (Get-ChildItem "$kits\bin" -Directory | Where-Object { Test-Path "$($_.FullName)\x64\makeappx.exe" })).FullName + '\x64'

# --- Toolset MSVC : le plus récent qui cible encore l'architecture (14.44 pour ARM32)
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsPaths = & $vswhere -all -prerelease -products * -property installationPath
$msvc = Get-Newest ($vsPaths | ForEach-Object { Get-ChildItem "$_\VC\Tools\MSVC" -Directory -ErrorAction SilentlyContinue } |
    Where-Object { (Test-Path "$($_.FullName)\bin\Hostx64\$Arch\cl.exe") -and (Test-Path "$($_.FullName)\lib\onecore\$Arch\libcmt.lib") })
if (-not $msvc) { throw "Aucun toolset MSVC ne cible $Arch avec les bibliothèques OneCore." }
$msvc = $msvc.FullName
$env:PATH = "$msvc\bin\Hostx64\x64;$env:PATH"  # DLL partagées des compilateurs croisés

$target = @{ arm = 'thumbv7-pc-windows-msvc'; arm64 = 'aarch64-pc-windows-msvc'; x64 = 'x86_64-pc-windows-msvc' }[$Arch]
$machine = @{ arm = 'ARM'; arm64 = 'ARM64'; x64 = 'X64' }[$Arch]

if ($Compiler -eq 'clang') {
  if (-not $ClangDir) {
    $candidates = @($vsPaths | ForEach-Object { "$_\VC\Tools\Llvm\x64\bin" }) + "$env:ProgramFiles\LLVM\bin"
    $ClangDir = $candidates | Where-Object { Test-Path "$_\clang-cl.exe" } | Select-Object -First 1
  }
  if (-not $ClangDir) { throw "clang-cl introuvable : installer LLVM ou le composant « C++ Clang » de Visual Studio, ou passer -ClangDir." }
  $clang = "$ClangDir\clang-cl.exe"
  $ld = "$ClangDir\lld-link.exe"
} else {
  $ld = "$msvc\bin\Hostx64\$Arch\link.exe"
}
# La coquille (C++/WinRT) est toujours compilée par MSVC : elle a besoin des exceptions C++,
# que LLVM ne sait pas compiler pour Windows ARM32 (« Funclet EH is not implemented »).
$cl = "$msvc\bin\Hostx64\$Arch\cl.exe"

Write-Host "SDK $sdkVer · MSVC $(Split-Path $msvc -Leaf) · $Compiler · $Arch" -ForegroundColor Cyan

$out = "$root\out\$Arch-$Compiler"
$obj = "$out\obj"
$pkg = "$out\pkg"
Remove-Item $out -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $obj, $pkg | Out-Null

# C++/WinRT récent a retiré ARM32 : on rajoute la branche _M_ARM dans une copie de base.h,
# placée devant celle du SDK dans les chemins d'inclusion (le SDK n'est pas modifié).
$includeFirst = @()
if ($Arch -eq 'arm') {
  $base = Get-Content "$cppwinrt\winrt\base.h" -Raw
  $arm32Read = "#elif defined _M_ARM`n        int32_t const result = __iso_volatile_load32(reinterpret_cast<int32_t const volatile*>(target));`n" +
    "        __dmb(_ARM_BARRIER_ISH);`n        return result;`n`$0"
  $patched = [regex]::new('#else\r?\n#error Unsupported architecture\r?\n').Replace($base, $arm32Read, 1)
  $patched = $patched.Replace('#if !defined _M_IX86 && !defined _M_X64 && !defined _M_ARM64', '#if !defined _M_IX86 && !defined _M_X64 && !defined _M_ARM64 && !defined _M_ARM')
  if ($patched -notmatch 'defined _M_ARM\r?\n        int32_t' -or $patched -notmatch '!defined _M_ARM\r?\n') {
    throw "Le correctif ARM32 de C++/WinRT ne s'applique plus à $cppwinrt\winrt\base.h"
  }
  New-Item -ItemType Directory -Force "$out\cppwinrt-arm32\winrt" | Out-Null
  Set-Content "$out\cppwinrt-arm32\winrt\base.h" $patched -NoNewline -Encoding utf8
  $includeFirst = @("/I$out\cppwinrt-arm32")
}

# --- Rust : cibles UWP de niveau Tier 3, sans std précompilée -> recompilée (nightly + rust-src).
# CRT statique pour correspondre à /MT.
$rustLibs = @()
$rustDefine = '/DSONDE_RUST=0'
if ($WithRust) {
  $rustTarget = @{ arm = 'thumbv7a-uwp-windows-msvc'; arm64 = 'aarch64-uwp-windows-msvc'; x64 = 'x86_64-uwp-windows-msvc' }[$Arch]
  $cargo = (Get-Command cargo -ErrorAction SilentlyContinue).Source
  if (-not $cargo) { $cargo = "$env:USERPROFILE\.cargo\bin\cargo.exe" }
  if (-not (Test-Path $cargo)) { throw 'cargo introuvable : installer rustup puis « rustup toolchain install nightly --component rust-src ».' }
  $env:RUSTFLAGS = '-C target-feature=+crt-static --print native-static-libs'
  $cargoOut = & $cargo +nightly build --release --manifest-path "$root\rust\Cargo.toml" --target $rustTarget `
    -Z build-std=std,panic_abort --target-dir "$root\out\rust" 2>&1
  if ($LASTEXITCODE -ne 0) { $cargoOut | Out-Host; throw "cargo a échoué (code $LASTEXITCODE)" }
  $native = ''
  foreach ($l in $cargoOut) { if ("$l" -match 'native-static-libs: (.*)') { $native = $Matches[1] } }
  $rustLibs = @("$root\out\rust\$rustTarget\release\sonde_rust.lib") + @($native -split '\s+' | Where-Object { $_ })
  $rustDefine = '/DSONDE_RUST=1'
  Write-Host "Rust $rustTarget · bibliothèques natives : $native" -ForegroundColor Cyan
}

# --- Compilation
$codegen = if ($NoCodeGeneration) { 0 } else { 1 }
$clangDefine = "/DSONDE_CLANG=$([int]($Compiler -eq 'clang'))"
if ($WithA32 -and $Arch -ne 'arm') { throw '-WithA32 ne concerne que -Arch arm.' }
$a32Flags = @(if ($WithA32) { '/DSONDE_A32=1', "/I$root\..\j1\app" } else { '/DSONDE_A32=0' })
$a32Flags += "/DSONDE_V8=$([int][bool]$WithV8)", "/DSONDE_V8_FIRST=$V8First"

# Liste des imports (hors chargement différé) d'un autre exe, vérifiés un par un par la sonde :
# une DLL ou un export absent du téléphone empêche cet exe de démarrer, sans aucun message.
if ($ImportCheck) {
  $dumpbin = "$msvc\bin\Hostx64\x64\dumpbin.exe"
  $cur = ''; $delay = $false
  $pairs = foreach ($l in (& $dumpbin /nologo /imports (Resolve-Path $ImportCheck))) {
    if ($l -match 'delay load imports') { $delay = $true }
    if ($delay) { continue }
    if ($l -match '^\s{4}(\S+\.dll)\s*$') { $cur = $Matches[1] }
    elseif ($cur -and $l -match '^\s+[0-9A-F]+\s+(\S+)$') { "    {`"$cur`", `"$($Matches[1])`"}," }
  }
  New-Item -ItemType Directory -Force "$out\gen" | Out-Null
  @"
// Généré par build.ps1 -ImportCheck $(Split-Path $ImportCheck -Leaf) : $($pairs.Count) imports.
#pragma once
struct ImportToCheck { char const* dll; char const* function; };
inline constexpr ImportToCheck kImportsToCheck[] = {
$($pairs -join "`n")
};
"@ | Set-Content "$out\gen\import_check.h" -Encoding utf8
  $a32Flags += @('/DSONDE_IMPORT_CHECK=1', "/I$out\gen")
  Write-Host "Vérification de $($pairs.Count) imports de $(Split-Path $ImportCheck -Leaf)" -ForegroundColor Cyan
}
$cflags = @('/nologo', '/c', '/std:c++20', '/EHsc', '/O2', '/MT', '/W4', '/permissive-', '/utf-8', '/bigobj',
  '/DWINAPI_FAMILY=WINAPI_FAMILY_APP', '/DUNICODE', '/D_UNICODE', '/DNOMINMAX', "/DSONDE_CODEGEN=$codegen", $rustDefine, $clangDefine) + $a32Flags +
  $includeFirst + @("/I$inc\ucrt", "/I$inc\um", "/I$inc\shared", "/I$inc\winrt", "/I$cppwinrt", "/I$msvc\include",
  "/Fo$obj\main.obj", "$root\src\main.cpp")
Invoke-Tool $cl $cflags
$objs = @("$obj\main.obj")

if ($Compiler -eq 'clang') {
  # Module sans exceptions, comme Chromium. Chemins MSVC/SDK explicites : sinon clang-cl prend
  # le toolset le plus récent, qui n'a plus ARM32.
  Invoke-Tool $clang @("--target=$target", '/vctoolsdir', $msvc, '/winsdkdir', $kits, '/winsdkversion', $sdkVer,
    '/nologo', '/c', '/std:c++20', '/EHs-c-', '/D_HAS_EXCEPTIONS=0', '/O2', '/MT', '/W4', '/utf-8',
    '/DWINAPI_FAMILY=WINAPI_FAMILY_APP', "/I$inc\ucrt", "/I$inc\um", "/I$inc\shared", "/I$msvc\include",
    "/Fo$obj\clang_probe.obj", "$root\src\clang_probe.cpp")
  $objs += "$obj\clang_probe.obj"
}

# --- V8 : cale C (..\j1\app\v8_shim.cc) compilée comme V8 (clang-cl, sans exceptions ni RTTI),
# avec les mêmes macros V8_*/CPPGC_* que v8_monolith.lib, lues dans ses fichiers ninja.
$v8Libs = @()
if ($WithV8) {
  $v8 = (Resolve-Path "$root\..\v8").Path
  $v8Out = "$v8\out\arm.release"
  $v8Lib = "$v8Out\obj\v8_monolith.lib"
  if (-not (Test-Path $v8Lib)) { throw "V8 n'est pas compilé : lancer ..\j1\build-v8.ps1 ($v8Lib absent)." }
  $definesLine = Get-Content "$v8Out\obj\v8_libplatform.ninja" | Where-Object { $_ -match '^defines = ' } | Select-Object -First 1
  $v8Defines = @(($definesLine -replace '^defines = ', '') -split ' ' | Where-Object { $_ -match '^-D(V8_|CPPGC_)' })
  Invoke-Tool $clang (@("--target=$target", '/vctoolsdir', $msvc, '/winsdkdir', $kits, '/winsdkversion', $sdkVer,
    '/nologo', '/c', '/std:c++20', '/EHs-c-', '/D_HAS_EXCEPTIONS=0', '/GR-', '/O2', '/MT', '/W3', '/utf-8',
    '-Wno-unused-command-line-argument') + $v8Defines + @("/I$v8", "/I$v8\include",
    "/I$inc\ucrt", "/I$inc\um", "/I$inc\shared", "/I$msvc\include",
    "/Fo$obj\v8_shim.obj", "$root\..\j1\app\v8_shim.cc"))
  $objs += "$obj\v8_shim.obj"
  # Remplace les trois fonctions que le schéma des api sets envoie vers kernel32legacy.dll,
  # absente de Windows 10 Mobile (sinon l'app ne démarre pas).
  Invoke-Tool $clang @("--target=$target", '/vctoolsdir', $msvc, '/winsdkdir', $kits, '/winsdkversion', $sdkVer,
    '/nologo', '/c', '/O2', '/MT', '/W3', '/DWINAPI_FAMILY=WINAPI_FAMILY_APP', '-Wno-unused-command-line-argument',
    "/I$inc\ucrt", "/I$inc\um", "/I$inc\shared", "/I$msvc\include",
    "/Fo$obj\w10m_imports.obj", "$root\..\j1\app\w10m_imports.cc")
  $objs += "$obj\w10m_imports.obj"
  # dbghelp (traces de pile de V8) en chargement différé : rien ne garantit sa présence
  # sur le téléphone, et V8 ne s'en sert qu'en cas de plantage.
  $v8Libs = @($v8Lib, 'dbghelp.lib', 'delayimp.lib', '/DELAYLOAD:dbghelp.dll', "/LIBPATH:$msvc\lib\$Arch", '/ERRORLIMIT:0',
    "/MAP:$out\RheniumSonde.map")  # carte des symboles : situer un PC remonté par le journal V8
  Write-Host "V8 : $([math]::Round((Get-Item $v8Lib).Length / 1MB)) Mo · $($v8Defines.Count) macros" -ForegroundColor Cyan
}

# --- Édition de liens : CRT statique (vcruntime, STL) + UCRT système, sans dépendance à VCLibs
$exe = "$pkg\RheniumSonde.exe"
$ldflags = @('/nologo', '/APPCONTAINER', '/SUBSYSTEM:WINDOWS', "/MACHINE:$machine", '/MANIFEST:NO',
  '/DYNAMICBASE', '/NXCOMPAT', "/OUT:$exe") + $objs + @(
  "/LIBPATH:$msvc\lib\onecore\$Arch", "/LIBPATH:$kits\Lib\$sdkVer\um\$Arch", "/LIBPATH:$kits\Lib\$sdkVer\ucrt\$Arch",
  'WindowsApp.lib', 'd3d11.lib', 'dxgi.lib', 'ucrt.lib', '/NODEFAULTLIB:libucrt.lib', '/NODEFAULTLIB:kernel32.lib') + $rustLibs + $v8Libs
Invoke-Tool $ld $ldflags

# --- Paquet
$caps = if ($NoCodeGeneration) { '' } else { '    <Capability Name="codeGeneration" />' }
(Get-Content "$root\AppxManifest.in.xml" -Raw).Replace('@ARCH@', $Arch).Replace('@CAPABILITIES@', $caps) |
  Set-Content "$pkg\AppxManifest.xml" -Encoding utf8
Copy-Item "$root\Assets" "$pkg\Assets" -Recurse

Invoke-Tool "$sdkBin\makepri.exe" @('createconfig', '/cf', "$out\priconfig.xml", '/dq', 'fr-FR', '/pv', '10.0.0', '/o')
Invoke-Tool "$sdkBin\makepri.exe" @('new', '/pr', $pkg, '/cf', "$out\priconfig.xml", '/mn', "$pkg\AppxManifest.xml",
  '/of', "$pkg\resources.pri", '/o')

$suffix = $(if ($WithRust) { '-rust' } else { '' }) + $(if ($WithA32) { '-a32' } else { '' }) + $(if ($WithV8) { '-v8' } else { '' }) +
  $(if ($NoCodeGeneration) { '_sans-codegen' } else { '' })
$appx = "$root\out\RheniumSonde_$Arch-$Compiler$suffix.appx"
Invoke-Tool "$sdkBin\makeappx.exe" @('pack', '/d', $pkg, '/p', $appx, '/o')

# --- Signature avec un certificat de développement local (créé au premier build, hors magasin Windows)
$pfx = "$root\certs\RheniumDev.pfx"
if (-not (Test-Path $pfx)) {
  New-Item -ItemType Directory -Force "$root\certs" | Out-Null
  $rsa = [System.Security.Cryptography.RSA]::Create(2048)
  $req = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new('CN=Rhenium Dev', $rsa,
    [System.Security.Cryptography.HashAlgorithmName]::SHA256, [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
  $req.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($false, $false, 0, $true))
  $req.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new(
      [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature, $true))
  $eku = [System.Security.Cryptography.OidCollection]::new()
  [void] $eku.Add([System.Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.3'))  # signature de code
  $req.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($eku, $false))
  $cert = $req.CreateSelfSigned([DateTimeOffset]::Now.AddDays(-1), [DateTimeOffset]::Now.AddYears(3))
  [IO.File]::WriteAllBytes($pfx, $cert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Pfx))
  [IO.File]::WriteAllBytes("$root\certs\RheniumDev.cer", $cert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert))
}
Invoke-Tool "$sdkBin\signtool.exe" @('sign', '/q', '/fd', 'SHA256', '/f', $pfx, $appx)

Write-Host "Paquet prêt : $appx" -ForegroundColor Green
