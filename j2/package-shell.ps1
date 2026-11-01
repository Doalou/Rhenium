#Requires -Version 7.0
<#
.SYNOPSIS
  Empaquette l'app Rhénium (rhenium_shell.exe et ses données) en .appx signé, avec le certificat
  de développement de la sonde (..\sonde\certs, créé au besoin par ..\sonde\dev-cert.ps1).
  J4 : uBlock Origin Lite (..\j4\fetch-ubol.ps1) dans Extensions\uBOLite, installé au premier
  lancement.
.EXAMPLE
  ../j4/fetch-ubol.ps1 ; ./build-content.ps1 -KeepGoing ; ./package-shell.ps1
  ..\sonde\deploy.ps1 -Ip 127.0.0.1 -Appx ..\j2\out\RheniumShell_arm.appx
#>
param(
  [string] $OutDir = "$PSScriptRoot\..\chromium\src\out\content-arm",
  [string] $UbolDir = "$PSScriptRoot\..\j4\out\ubol\uBOLite"
)

$ErrorActionPreference = 'Stop'
$kits = "${env:ProgramFiles(x86)}\Windows Kits\10"

function Invoke-Tool {
  param([string] $Exe, [string[]] $Arguments)
  $output = & $Exe @Arguments 2>&1
  if ($LASTEXITCODE -ne 0) {
    $output | Out-Host
    throw "$(Split-Path $Exe -Leaf) a échoué (code $LASTEXITCODE)"
  }
}

$sdkBin = (Get-ChildItem "$kits\bin" -Directory | Where-Object { Test-Path "$($_.FullName)\x64\makeappx.exe" } |
  Sort-Object { [version]$_.Name } | Select-Object -Last 1).FullName + '\x64'
$pfx = & "$PSScriptRoot\..\sonde\dev-cert.ps1"

$out = "$PSScriptRoot\out"
$pkg = "$out\pkg"
Remove-Item $pkg -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $pkg | Out-Null

# Exécutable, ressources de content_shell et des extensions, ICU et instantané de V8.
$files = 'rhenium_shell.exe', 'content_shell.pak', 'rhenium_extensions.pak', 'icudtl.dat', 'snapshot_blob.bin'
foreach ($f in $files) {
  if (-not (Test-Path "$OutDir\$f")) { throw "$f absent de $OutDir : lancer build-content.ps1." }
  Copy-Item "$OutDir\$f" $pkg
}
Copy-Item "$PSScriptRoot\..\sonde\Assets" "$pkg\Assets" -Recurse

# Relais d'appels virtuels de clang ARM32 qui écrasent r1 (voir fix_vcall_thunks.py).
$python = (Get-Command python -ErrorAction Stop).Source
& $python -I "$PSScriptRoot\fix_vcall_thunks.py" "$pkg\rhenium_shell.exe"
if ($LASTEXITCODE -ne 0) { throw 'fix_vcall_thunks.py a échoué.' }

# Version croissante à chaque paquet (jour de l'année, minute du jour) :
# deploy.ps1 met alors l'app à jour sans la désinstaller, et ses données
# (historique, onglets, extensions, réglages) restent.
$now = Get-Date
$version = "0.4.$($now.Year % 100 * 1000 + $now.DayOfYear).$($now.Hour * 60 + $now.Minute)"
Write-Host "Version du paquet : $version"

@'
<?xml version="1.0" encoding="utf-8"?>
<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
         xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"
         IgnorableNamespaces="uap">
  <Identity Name="Rhenium.Shell" Publisher="CN=Rhenium Dev" Version="0.4.0.0" ProcessorArchitecture="arm" />
  <Properties>
    <DisplayName>Rhénium</DisplayName>
    <PublisherDisplayName>Rhénium</PublisherDisplayName>
    <Logo>Assets\StoreLogo.png</Logo>
  </Properties>
  <Dependencies>
    <TargetDeviceFamily Name="Windows.Universal" MinVersion="10.0.15063.0" MaxVersionTested="10.0.15254.0" />
  </Dependencies>
  <Resources>
    <Resource Language="fr-FR" />
  </Resources>
  <Applications>
    <Application Id="App" Executable="rhenium_shell.exe" EntryPoint="Rhenium.App">
      <uap:VisualElements DisplayName="Rhénium" Description="Chromium pour Windows 10 Mobile"
                          BackgroundColor="#1E1E1E" Square150x150Logo="Assets\Square150x150Logo.png"
                          Square44x44Logo="Assets\Square44x44Logo.png">
        <uap:SplashScreen Image="Assets\SplashScreen.png" />
      </uap:VisualElements>
    </Application>
  </Applications>
  <Capabilities>
    <Capability Name="internetClient" />
    <Capability Name="codeGeneration" />
  </Capabilities>
</Package>
'@ -replace 'Version="0\.4\.0\.0"', "Version=`"$version`"" | Set-Content "$pkg\AppxManifest.xml" -Encoding utf8

Invoke-Tool "$sdkBin\makepri.exe" @('createconfig', '/cf', "$out\priconfig.xml", '/dq', 'fr-FR', '/pv', '10.0.0', '/o')
Invoke-Tool "$sdkBin\makepri.exe" @('new', '/pr', $pkg, '/cf', "$out\priconfig.xml", '/mn', "$pkg\AppxManifest.xml",
  '/of', "$pkg\resources.pri", '/o')

# Extensions installées au premier lancement. Copiées après makepri : leurs dossiers _locales\fr,
# etc. n'ont pas à passer pour des ressources qualifiées par langue.
if (-not (Test-Path "$UbolDir\manifest.json")) { throw "uBO Lite absent de $UbolDir : lancer ..\j4\fetch-ubol.ps1." }
New-Item -ItemType Directory -Force "$pkg\Extensions" | Out-Null
Copy-Item $UbolDir "$pkg\Extensions\uBOLite" -Recurse

$appx = "$out\RheniumShell_arm.appx"
Invoke-Tool "$sdkBin\makeappx.exe" @('pack', '/d', $pkg, '/p', $appx, '/o')
Invoke-Tool "$sdkBin\signtool.exe" @('sign', '/q', '/fd', 'SHA256', '/f', $pfx, $appx)
Write-Host "Paquet prêt : $appx ($([math]::Round((Get-Item $appx).Length / 1MB)) Mo)" -ForegroundColor Green
