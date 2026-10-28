# auth_pin.ps1 — 로그인 서버(nox_auth)가 쓰는 인증서의 지문(DER 의 SHA-256 · 16진 64자)을 한 줄 찍는다.
#
# 왜 — 다른 PC 클라는 자작 인증서를 그 PC 「믿는 루트」에 넣지 않고 이 지문으로 서버를 가른다(DLL `AuthClient.h` 지문 고정 · ★ 10-28).
#   꾸러미(tools\pack_client.ps1)가 이 값을 2_PLAY 에 넣고, DLL 시험(client_mod\wsock32_proxy\test.bat)이 실제 로그인 서버 시험에 쓴다.
# 찾는 차례 — http.sys 가 0.0.0.0:<포트> 에 묶은 인증서(사람 1회 tools\auth_setup.ps1)의 지문(SHA-1) → 이 PC 인증서 저장소(LocalMachine\My)에서 그 인증서.
#   비밀 키가 아니라 공개 정보라 꾸러미에 넣어도 된다. 인증서를 새로 만들면(auth_setup.ps1 -Undo 뒤 다시) 지문이 바뀌니 꾸러미도 다시
# 못 찾으면 오류 글(stderr) + 종료 1
param([int]$Port = 8443)
$ErrorActionPreference = 'Stop'
$show = (& netsh http show sslcert "ipport=0.0.0.0:$Port") | Out-String
$m = [regex]::Match($show, '\b[0-9a-fA-F]{40}\b')
if (-not $m.Success) {
  [Console]::Error.WriteLine("0.0.0.0:$Port 에 묶인 인증서가 없다 — 사람 1회 tools\auth_setup.ps1 먼저")
  exit 1
}
$cert = Get-ChildItem Cert:\LocalMachine\My | Where-Object { $_.Thumbprint -eq $m.Value.ToUpperInvariant() } | Select-Object -First 1
if (-not $cert) {
  [Console]::Error.WriteLine("묶인 인증서 $($m.Value) 를 LocalMachine\My 에서 못 찾음")
  exit 1
}
$sha = [Security.Cryptography.SHA256]::Create()
-join ($sha.ComputeHash($cert.RawData) | ForEach-Object { $_.ToString('x2') })
