<#
.SYNOPSIS
  Empaquette l'app Rhénium (rhenium_shell.exe et ses données) en .appx signé, avec le certificat
  de développement de la sonde (..\sonde\certs, créé par ..\sonde\build.ps1).
.EXAMPLE
  ./build-content.ps1 -KeepGoing ; ./package-shell.ps1
  ..\sonde\deploy.ps1 -Ip 127.0.0.1 -Appx ..\j2\out\RheniumShell_arm.appx
#>
param(
  [string] $OutDir = "$PSScriptRoot\..\chromium\src\out\content-arm"
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
$pfx = "$PSScriptRoot\..\sonde\certs\RheniumDev.pfx"
if (-not (Test-Path $pfx)) { throw "Certificat absent : lancer d'abord ..\sonde\build.ps1 ($pfx)." }

$out = "$PSScriptRoot\out"
$pkg = "$out\pkg"
Remove-Item $pkg -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $pkg | Out-Null

# Exécutable, ressources de content_shell, ICU et instantané de V8.
$files = 'rhenium_shell.exe', 'content_shell.pak', 'shell_resources.pak', 'ui_resources_100_percent.pak',
  'ui_test.pak', 'icudtl.dat', 'snapshot_blob.bin'
foreach ($f in $files) {
  if (-not (Test-Path "$OutDir\$f")) { throw "$f absent de $OutDir : lancer build-content.ps1." }
  Copy-Item "$OutDir\$f" $pkg
}
Copy-Item "$PSScriptRoot\..\sonde\Assets" "$pkg\Assets" -Recurse

# Relais d'appels virtuels de clang ARM32 qui écrasent r1 (voir fix_vcall_thunks.py).
$python = (Get-Command python -ErrorAction Stop).Source
& $python -I "$PSScriptRoot\fix_vcall_thunks.py" "$pkg\rhenium_shell.exe"
if ($LASTEXITCODE -ne 0) { throw 'fix_vcall_thunks.py a échoué.' }

@'
<?xml version="1.0" encoding="utf-8"?>
<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
         xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"
         IgnorableNamespaces="uap">
  <Identity Name="Rhenium.Shell" Publisher="CN=Rhenium Dev" Version="0.2.3.0" ProcessorArchitecture="arm" />
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
'@ | Set-Content "$pkg\AppxManifest.xml" -Encoding utf8

Invoke-Tool "$sdkBin\makepri.exe" @('createconfig', '/cf', "$out\priconfig.xml", '/dq', 'fr-FR', '/pv', '10.0.0', '/o')
Invoke-Tool "$sdkBin\makepri.exe" @('new', '/pr', $pkg, '/cf', "$out\priconfig.xml", '/mn', "$pkg\AppxManifest.xml",
  '/of', "$pkg\resources.pri", '/o')

$appx = "$out\RheniumShell_arm.appx"
Invoke-Tool "$sdkBin\makeappx.exe" @('pack', '/d', $pkg, '/p', $appx, '/o')
Invoke-Tool "$sdkBin\signtool.exe" @('sign', '/q', '/fd', 'SHA256', '/f', $pfx, $appx)
Write-Host "Paquet prêt : $appx ($([math]::Round((Get-Item $appx).Length / 1MB)) Mo)" -ForegroundColor Green
