-- 006_match_totals.sql — 승패 합계 표(Docs/RECORD_SEARCH_PLAN.md 조각 1). 001 ~ 005 뒤에 돌린다.
--
-- 왜 — 대전 기록 · 전적 검색 화면은 최근 20판인데 승패를 세려고 계정의 판을 전부 읽고 있었다(조회 하나의 크기가 판 수를 따라감).
--   승패를 판 저장 트랜잭션 안에서 이 표에 함께 올리면 읽기는 합계 한 행 + 20줄로 끝난다. `ratings` 와 같은 구조.
--   판 행(matches · match_players)이 정본이고 이 표는 파생 값이다 — 판정 규칙이 바뀌거나 틀리면 `nox_match_totals` 로 판 행에서 다시 계산한다.
--   행은 첫 판에서 생긴다(없으면 전부 0 · 관전만 한 계정은 행이 없다).
--   표 버전을 6 으로 올린다 — 로비 · 로그인 서버는 뜰 때 버전이 자기가 아는 값인지만 본다.
--   GRANT 는 여기 안 넣는다 — 시험이 이 파일을 서버 계정으로 그대로 돌린다. 실DB 는 root 로 config/match_totals_setup.sql(표 + 버전 + 권한).
-- 여러 번 돌려도 된다(없을 때만 — 003_rating.sql 과 같은 꼴).

CREATE TABLE IF NOT EXISTS match_totals (
  account_id    BIGINT UNSIGNED NOT NULL PRIMARY KEY,
  games         INT UNSIGNED    NOT NULL,
  ranked_wins   INT UNSIGNED    NOT NULL,
  ranked_losses INT UNSIGNED    NOT NULL,
  normal_wins   INT UNSIGNED    NOT NULL,
  normal_losses INT UNSIGNED    NOT NULL,
  updated_unix  INT UNSIGNED    NOT NULL,
  CONSTRAINT fk_match_totals_account FOREIGN KEY (account_id) REFERENCES accounts(id)
) ENGINE=InnoDB;

UPDATE schema_version SET version = 6 WHERE id = 1 AND version < 6;
