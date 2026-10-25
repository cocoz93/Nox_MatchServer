-- 002_auth.sql — 로그인 암호(Docs/LOGIN_AUTH_DESIGN.md 차이 4). 001_init.sql 뒤에 돌린다.
--
-- 왜 — 암호 해시를 로그인 서버(nox_auth)가 쓴다. 칸 둘(pass_salt · pass_hash)은 001 에 이미 있고 반복 수 칸만 없다
--   (반복 수를 행마다 두면 나중에 수를 올려도 옛 행을 그대로 확인할 수 있다 — 정할 것 f).
--   표 버전을 2 로 올린다 — 로비 · 로그인 서버는 뜰 때 버전이 자기가 아는 값인지만 본다.
-- 여러 번 돌려도 된다(칸이 없을 때만 더한다 — MySQL 8.0 은 ADD COLUMN IF NOT EXISTS 가 없어 information_schema 로 본다).

SET @has_iter = (SELECT COUNT(*) FROM information_schema.COLUMNS
                 WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'accounts' AND COLUMN_NAME = 'pass_iter');
SET @sql = IF(@has_iter = 0,
              'ALTER TABLE accounts ADD COLUMN pass_iter INT UNSIGNED NULL AFTER pass_hash',
              'DO 0');
PREPARE st FROM @sql;
EXECUTE st;
DEALLOCATE PREPARE st;

UPDATE schema_version SET version = 2 WHERE id = 1 AND version < 2;
