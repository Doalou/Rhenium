#Requires -Version 7.0
<#
.SYNOPSIS
  Certificat de développement local qui signe les paquets (sonde, app Rhénium) : créé s'il manque
  ou a expiré, hors du magasin de certificats Windows. Rend le chemin du .pfx.
.EXAMPLE
  $pfx = & "$PSScriptRoot\dev-cert.ps1"   # appelé par build.ps1 et ..\j2\package-shell.ps1
#>
param(
  [string] $Dir = "$PSScriptRoot\certs",
  [int] $WarnDays = 30  # avertir quand l'expiration approche
)

$ErrorActionPreference = 'Stop'
$subject = 'CN=Rhenium Dev'  # Publisher des manifestes : en changer changerait l'identité des paquets
$pfx = "$Dir\RheniumDev.pfx"

if (Test-Path $pfx) {
  $cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($pfx)
  if ($cert.Subject -ne $subject) { throw "$pfx est au nom de $($cert.Subject), pas de $subject." }
  $left = ($cert.NotAfter - (Get-Date)).TotalDays
  if ($left -gt 0) {
    if ($left -lt $WarnDays) {
      Write-Warning "Le certificat de développement expire le $($cert.NotAfter.ToString('d')) : le supprimer de $Dir pour en créer un autre."
    }
    return $pfx
  }
  # Expiré : on garde l'ancien à côté, le nouveau devra être installé sur le téléphone s'il le demande.
  $stamp = $cert.NotAfter.ToString('yyyyMMdd')
  Move-Item $pfx "$Dir\RheniumDev-expire-$stamp.pfx" -Force
  if (Test-Path "$Dir\RheniumDev.cer") { Move-Item "$Dir\RheniumDev.cer" "$Dir\RheniumDev-expire-$stamp.cer" -Force }
  Write-Warning "Certificat de développement expiré le $($cert.NotAfter.ToString('d')) : remplacé."
}

New-Item -ItemType Directory -Force $Dir | Out-Null
$rsa = [System.Security.Cryptography.RSA]::Create(2048)
$req = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new($subject, $rsa,
  [System.Security.Cryptography.HashAlgorithmName]::SHA256, [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
$req.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension]::new($false, $false, 0, $true))
$req.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509KeyUsageExtension]::new(
    [System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature, $true))
$eku = [System.Security.Cryptography.OidCollection]::new()
[void] $eku.Add([System.Security.Cryptography.Oid]::new('1.3.6.1.5.5.7.3.3'))  # signature de code
$req.CertificateExtensions.Add([System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension]::new($eku, $false))
$cert = $req.CreateSelfSigned([DateTimeOffset]::Now.AddDays(-1), [DateTimeOffset]::Now.AddYears(3))
[IO.File]::WriteAllBytes($pfx, $cert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Pfx))
[IO.File]::WriteAllBytes("$Dir\RheniumDev.cer", $cert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert))
Write-Host "Certificat de développement créé : $pfx (jusqu'au $($cert.NotAfter.ToString('d')))" -ForegroundColor Cyan
$pfx
