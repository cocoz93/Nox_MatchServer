-- 007_account_bans.sql — 계정 정지 테이블(Docs/MO_ACCOUNT_RANK_MAP_PLAN_1105.md 5절). 001 ~ 006 뒤에 돌린다.
--
-- 왜 — 운영이 계정을 막을 자리가 없었다. 정지를 행으로 쌓아 앞선 이력이 남게 한다(경고 → 3일 → 14일 → 영구 같은 운영 규칙의 재료).
--   기간이 지나면 끝 시각으로 저절로 풀린다(행은 그대로 · lifted_unix 0). 운영이 풀면 lifted_unix 에 그 시각을 적는다.
--   정지가 여러 행이면 가장 늦은 끝 시각이 이긴다. until_unix 최댓값(4294967295) = 영구(`LoginLine.h` kBanForever).
--   source = 넣은 쪽('admin' 운영 명령 · 'auto' 자동 — `LoginLine.h`).
--   찾기 칸 둘 — 계정 하나의 정지(로그인 · 토큰) · 지금 유효한 정지 전체(`--bans`).
--   표 버전을 7 로 올린다 — 로비 · 로그인 서버는 뜰 때 버전이 자기가 아는 값인지만 본다.
--   GRANT 는 여기 안 넣는다 — 시험이 이 파일을 서버 계정으로 그대로 돌린다. 실DB 는 root 로 config/account_bans_setup.sql(표 + 버전 + 권한).
-- 여러 번 돌려도 된다(없을 때만 — 006_match_totals.sql 과 같은 꼴).

CREATE TABLE IF NOT EXISTS account_bans (
  id           BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  account_id   BIGINT UNSIGNED NOT NULL,
  until_unix   INT UNSIGNED    NOT NULL,
  reason       VARCHAR(200)    NOT NULL,
  source       VARCHAR(16)     NOT NULL,
  created_unix INT UNSIGNED    NOT NULL,
  lifted_unix  INT UNSIGNED    NOT NULL DEFAULT 0,
  KEY idx_account_bans_account (account_id),
  KEY idx_account_bans_active (lifted_unix, until_unix),
  CONSTRAINT fk_account_bans_account FOREIGN KEY (account_id) REFERENCES accounts(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

UPDATE schema_version SET version = 7 WHERE id = 1 AND version < 7;
