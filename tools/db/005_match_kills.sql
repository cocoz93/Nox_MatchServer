-- 005_match_kills.sql — 대전 기록의 킬(Docs/MO_UX_FIX_PLAN_1104.md 플랜2). 001 · 002 · 003 · 004 뒤에 돌린다.
--
-- 왜 — 대전 기록 표의 킬/데스. 판 점수(score)는 도움 +1 · 자살 −1 이 섞이고 모드마다 뜻이 달라 킬로 못 쓴다. 게임 서버가 새로 센 값을 판 결과 JSON "kills" 로 보낸다.
--   NULL = 나간 사람 · 이 변경 전 판(화면 `-`). 옛 판을 되짚어 채우지 않는다(raw_json 에도 없다).
--   표 버전을 5 로 올린다 — 로비 · 로그인 서버는 뜰 때 버전이 자기가 아는 값인지만 본다. **새 서버를 켜기 전에 사람이 관리자로 한 번** 돌린다(서버 계정은 표를 못 바꾼다):
--     mysql -u root -p nox_mo < tools\db\005_match_kills.sql
-- 여러 번 돌려도 된다(칸이 없을 때만 — 003_rating.sql 과 같은 꼴).

SET @has_kills = (SELECT COUNT(*) FROM information_schema.COLUMNS
                  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'match_players' AND COLUMN_NAME = 'kills');
SET @sql = IF(@has_kills = 0,
              'ALTER TABLE match_players ADD COLUMN kills INT UNSIGNED NULL AFTER deaths',
              'DO 0');
PREPARE st FROM @sql;
EXECUTE st;
DEALLOCATE PREPARE st;

UPDATE schema_version SET version = 5 WHERE id = 1 AND version < 5;
