-- 004_launcher_keys.sql — 런처 자동 로그인 열쇠(Docs/LAUNCHER_PLAN.md · 11-01 사용자 결정 2). 001 · 002 · 003 뒤에 돌린다.
--
-- 왜 — 런처가 암호로 한 번 로그인하면 로그인 서버(nox_auth)가 열쇠를 준다. 게임 DLL 은 로비에 갈 때마다 그 열쇠로 60초 토큰을 받는다.
--   열쇠 글은 두지 않고 SHA-256(32바이트)만 둔다 — DB 가 새도 열쇠로 못 쓴다.
--   행 하나 = 기기 하나(device 는 보이기용) · 만료는 쓸 때마다 「지금 + ttl_sec」으로 밀린다(30일 · 12시간 — `AuthService.h`).
--   로그아웃 = 그 행 지움 · 새 열쇠를 넣을 때 그 계정의 만료된 행도 지운다.
--   쓰는 쪽은 로그인 서버 하나(로비는 이 표를 안 본다 — 토큰만 본다).
--   표 버전을 4 로 올린다 — 로비 · 로그인 서버는 뜰 때 버전이 자기가 아는 값인지만 본다.
-- 여러 번 돌려도 된다(표가 없을 때만 만든다 — 003_rating.sql 과 같은 꼴).

CREATE TABLE IF NOT EXISTS login_keys (
  key_hash       BINARY(32)      NOT NULL PRIMARY KEY,
  account_id     BIGINT UNSIGNED NOT NULL,
  device         VARBINARY(64)   NOT NULL DEFAULT '',
  ttl_sec        INT UNSIGNED    NOT NULL,
  created_unix   INT UNSIGNED    NOT NULL,
  last_used_unix INT UNSIGNED    NOT NULL,
  expires_unix   INT UNSIGNED    NOT NULL,
  KEY ix_login_keys_account (account_id),
  CONSTRAINT fk_login_keys_account FOREIGN KEY (account_id) REFERENCES accounts(id)
) ENGINE=InnoDB;

UPDATE schema_version SET version = 4 WHERE id = 1 AND version < 4;
