# auth_setup.ps1 — 로그인 서버(nox_auth) HTTPS 준비. 사람 1회 · 관리자 PowerShell(Docs/LOGIN_AUTH_DESIGN.md 「사람 1회」).
#
# 왜 — nox_auth 는 윈도 HTTP 서버(http.sys)에 TLS 를 맡긴다. http.sys 는 포트에 묶인 인증서와 주소 예약이 있어야 듣는데,
#   둘 다 관리자 권한이 드는 시스템 설정이라 서버가 스스로 못 한다(그래서 사람 1회).
#   · 인증서 = 이 PC 자작(`New-SelfSignedCertificate` · 이름 localhost · 127.0.0.1). 「믿는 루트」에도 넣어 브라우저 · DLL 의 WinHTTP 가
#     검사를 끄지 않고 통과한다. 바깥 공개 때는 공인 인증서로 바꾼다(범위 밖)
#   · `netsh http add sslcert` = 0.0.0.0:<포트> 에 그 인증서 · `netsh http add urlacl` = 관리자가 아닌 내 계정이 https://+:<포트>/ 를 듣게
# 여러 번 돌려도 된다(있는 인증서를 다시 쓰고, 묶음 · 예약은 지우고 다시 건다). 되돌리기: -Undo
param(
  [int]$Port = 8443,
  [string]$User = "$env:USERDOMAIN\$env:USERNAME",
  [switch]$Undo
)
$ErrorActionPreference = 'Stop'
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
  throw '관리자 PowerShell 에서 돌려야 함(시작 → PowerShell 우클릭 → 관리자 권한으로 실행)'
}
$friendly = 'Nox_Origin auth (local)'
$appId = '{6b1e3c52-8f0a-4c11-9a57-2d4e0b7a91c3}'   # 묶음 주인 표시(아무 GUID — 이 스크립트가 고정)
$ipport = "0.0.0.0:$Port"
$url = "https://+:$Port/"

# 묶음 · 예약 지우기(없으면 netsh 가 오류를 내지만 그대로 넘어간다)
& netsh http delete sslcert ipport=$ipport | Out-Null
& netsh http delete urlacl url=$url | Out-Null

$mine = @(Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.FriendlyName -eq $friendly })
if ($Undo) {
  foreach ($c in $mine) {
    Get-ChildItem Cert:\LocalMachine\Root | Where-Object { $_.Thumbprint -eq $c.Thumbprint } | Remove-Item
    $c | Remove-Item
  }
  Write-Host "[되돌림] 묶음 · 예약 · 인증서 $($mine.Count)장 지움"
  return
}

$cert = $mine | Where-Object { $_.NotAfter -gt (Get-Date).AddDays(30) } | Select-Object -First 1
if (-not $cert) {
  $cert = New-SelfSignedCertificate -Subject 'CN=localhost' -FriendlyName $friendly `
    -TextExtension @('2.5.29.17={text}DNS=localhost&IPAddress=127.0.0.1') `
    -CertStoreLocation Cert:\LocalMachine\My -KeyAlgorithm RSA -KeyLength 2048 -HashAlgorithm SHA256 `
    -NotAfter (Get-Date).AddYears(5)
  Write-Host "[인증서] 새로 만듦 $($cert.Thumbprint)"
} else {
  Write-Host "[인증서] 있는 것 다시 씀 $($cert.Thumbprint)"
}
if (-not (Get-ChildItem Cert:\LocalMachine\Root | Where-Object { $_.Thumbprint -eq $cert.Thumbprint })) {
  $cer = Join-Path $env:TEMP 'nox_auth_local.cer'
  Export-Certificate -Cert $cert -FilePath $cer | Out-Null
  Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
  Remove-Item -LiteralPath $cer
  Write-Host '[인증서] 믿는 루트에 넣음'
}

& netsh http add sslcert ipport=$ipport certhash=$($cert.Thumbprint) appid=$appId certstorename=MY
if ($LASTEXITCODE -ne 0) { throw "netsh http add sslcert 실패($LASTEXITCODE)" }
& netsh http add urlacl url=$url user=$User
if ($LASTEXITCODE -ne 0) { throw "netsh http add urlacl 실패($LASTEXITCODE)" }

& netsh http show sslcert ipport=$ipport
& netsh http show urlacl url=$url
Write-Host "[끝] $url · 인증서 $($cert.Thumbprint) · 예약 사용자 $User"