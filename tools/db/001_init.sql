-- 001_init.sql — 녹스 MO 7단계 계정 · 판 기록 표(Docs/ACCOUNT_DB_DESIGN.md).
--
-- 왜 SQL 파일인가 — 서버 계정은 표를 못 만든다(SELECT · INSERT · UPDATE 만). 표는 사람이 관리자(root)로 이 파일을 한 번 돌린다.
--   로비는 뜰 때 schema_version 이 자기가 아는 값인지만 본다(다르면 안 뜬다). 나중 암호 · 레이팅 칸은 002_ … 로 잇고 버전을 올린다.
-- 이름 칸이 VARBINARY 인 까닭 — 정품 클라 이름은 코드페이지 949 바이트일 수 있고 로그인(USER) 때는 코드페이지도 모른다.
--   글자로 바꾸지 않고 바이트 그대로 같음으로 찾는다(16바이트 = 입장 토큰 이름 칸 `JoinToken.h`). 대소문자 무시는 정품 확인 뒤.
-- 시각은 유닉스 초(게임 서버 · 로비가 적는다) — DB 시간대에 기대지 않는다.
-- 여러 번 돌려도 된다(없는 것만 만든다).

CREATE TABLE IF NOT EXISTS schema_version (
  id      TINYINT UNSIGNED NOT NULL PRIMARY KEY,   -- 늘 1(한 행)
  version INT UNSIGNED     NOT NULL
) ENGINE=InnoDB;
INSERT IGNORE INTO schema_version (id, version) VALUES (1, 1);

-- 계정 — 처음 보는 이름이면 로비가 만든다(아이디만 · 결정 2). 암호 칸은 작업 4 까지 NULL
CREATE TABLE IF NOT EXISTS accounts (
  id              BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
  name            VARBINARY(16)   NOT NULL,
  pass_salt       VARBINARY(32)   NULL,
  pass_hash       VARBINARY(64)   NULL,
  created_unix    INT UNSIGNED    NOT NULL,
  last_login_unix INT UNSIGNED    NOT NULL,
  UNIQUE KEY uq_accounts_name (name)
) ENGINE=InnoDB;

-- 판 — 게임 서버가 보낸 판 결과(RESULT) 한 줄이 한 행. 판 번호(<게임 서버가 뜬 초>-<방>-<판>)는 게임 서버 하나 안에서만 유일해
--   게임 서버 등록 주소 · 포트와 묶어 겹침을 거른다(같은 판이 다시 와도 한 행 — 로비가 SAVED dup 으로 답)
CREATE TABLE IF NOT EXISTS matches (
  id             BIGINT UNSIGNED   NOT NULL AUTO_INCREMENT PRIMARY KEY,
  server_ip      INT UNSIGNED      NOT NULL,       -- 등록 주소(REGISTER 의 IPv4 · 0 = 로비 쪽 주소)
  server_port    SMALLINT UNSIGNED NOT NULL,
  match_key      VARCHAR(48)       NOT NULL,       -- 판 결과 "match"
  room_id        INT               NOT NULL,
  room_name      VARBINARY(64)     NOT NULL,       -- 만들 때 이름(10-21 결정) · 바이트 그대로
  map_key        VARCHAR(64)       NOT NULL,
  mode           VARCHAR(32)       NOT NULL,
  start_unix     INT UNSIGNED      NOT NULL,
  end_unix       INT UNSIGNED      NOT NULL,
  frames         INT UNSIGNED      NOT NULL,
  end_reason     VARCHAR(32)       NOT NULL,       -- 갈래 이름 · 중단이면 aborted
  winner_kind    VARCHAR(8)        NOT NULL,       -- none · player · team · draw
  winner_id      INT UNSIGNED      NULL,           -- 사람이면 netCode · 팀이면 팀 번호
  winner_name    VARBINARY(16)     NULL,           -- 승자가 사람이면 로비 이름
  score_limit    INT UNSIGNED      NOT NULL,
  time_limit_sec INT UNSIGNED      NOT NULL,
  raw_json       MEDIUMTEXT CHARACTER SET utf8mb4 NOT NULL,   -- 받은 줄 그대로(파일 기록기 줄과 대조 · 팀 점수는 여기에만)
  saved_unix     INT UNSIGNED      NOT NULL,
  UNIQUE KEY uq_matches_server_key (server_ip, server_port, match_key)
) ENGINE=InnoDB;

-- 판의 사람 — 열쇠는 (판, 자리). 같은 이름 둘이 한 방에 들 수 있어 계정으로 열쇠를 삼지 않는다(c 는 암호 때)
CREATE TABLE IF NOT EXISTS match_players (
  match_id   BIGINT UNSIGNED   NOT NULL,
  seat       TINYINT UNSIGNED  NOT NULL,   -- 판 결과 players 차례(0 부터)
  account_id BIGINT UNSIGNED   NULL,       -- 이름으로 찾음 · 로비를 안 거친 사람(이름 null)은 NULL
  name       VARBINARY(16)     NULL,
  net_code   SMALLINT UNSIGNED NOT NULL,
  state      VARCHAR(8)        NOT NULL,   -- finished · left
  team       TINYINT UNSIGNED  NULL,       -- left 면 NULL(판 결과에 안 실림)
  score      INT               NULL,       -- 음수 있음(아레나 자살 −1)
  deaths     INT UNSIGNED      NULL,
  observer   TINYINT(1)        NOT NULL DEFAULT 0,
  PRIMARY KEY (match_id, seat),
  CONSTRAINT fk_match_players_match   FOREIGN KEY (match_id)   REFERENCES matches(id),
  CONSTRAINT fk_match_players_account FOREIGN KEY (account_id) REFERENCES accounts(id)
) ENGINE=InnoDB;
