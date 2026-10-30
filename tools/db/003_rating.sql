-- 003_rating.sql — 레이팅(Docs/RATING_DESIGN.md). 001_init.sql · 002_auth.sql 뒤에 돌린다.
--
-- 왜 — 랭크 판 결과로 매긴 점수를 둔다. 점수는 accounts 가 아니라 새 표 — accounts 는 로그인 서버가 쓰고 점수는 로비가 쓴다(쓰는 쪽이 하나씩).
--   행은 첫 랭크 판에서 생긴다(로비 DB 워커가 판 저장 트랜잭션 안에서) · 행이 없는 사람은 1000(`Rating.h` kRatingStart)
--   순위 = 나보다 점수 높은 사람 수 + 1 → 점수 칸에 색인
--   matches.ranked = 랭크 판인가 · match_players.rating_delta = 그 판에서 변한 점수(반영 안 된 사람 · 일반 판 = NULL)
--   표 버전을 3 으로 올린다 — 로비 · 로그인 서버는 뜰 때 버전이 자기가 아는 값인지만 본다.
-- 여러 번 돌려도 된다(표 · 칸이 없을 때만 — 002_auth.sql 과 같은 꼴).

CREATE TABLE IF NOT EXISTS ratings (
  account_id   BIGINT UNSIGNED NOT NULL PRIMARY KEY,
  rating       INT             NOT NULL,
  updated_unix INT UNSIGNED    NOT NULL,
  KEY ix_ratings_rating (rating),
  CONSTRAINT fk_ratings_account FOREIGN KEY (account_id) REFERENCES accounts(id)
) ENGINE=InnoDB;

SET @has_ranked = (SELECT COUNT(*) FROM information_schema.COLUMNS
                   WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'matches' AND COLUMN_NAME = 'ranked');
SET @sql = IF(@has_ranked = 0,
              'ALTER TABLE matches ADD COLUMN ranked TINYINT(1) NOT NULL DEFAULT 0 AFTER time_limit_sec',
              'DO 0');
PREPARE st FROM @sql;
EXECUTE st;
DEALLOCATE PREPARE st;

SET @has_delta = (SELECT COUNT(*) FROM information_schema.COLUMNS
                  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'match_players' AND COLUMN_NAME = 'rating_delta');
SET @sql = IF(@has_delta = 0,
              'ALTER TABLE match_players ADD COLUMN rating_delta INT NULL AFTER observer',
              'DO 0');
PREPARE st FROM @sql;
EXECUTE st;
DEALLOCATE PREPARE st;

UPDATE schema_version SET version = 3 WHERE id = 1 AND version < 3;
