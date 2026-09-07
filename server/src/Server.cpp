// Server.cpp — 접속 절차 세 자리에 답한다.
//
//   0x0C SERVER_DISCOVER  →  0x0D SERVER_INFO      (녹음 재생, 토큰만 바꿔서)
//   0x0E SERVER_TRY_JOIN  →  0x14 SERVER_JOIN_OK   (지금은 무조건 받아준다)
//   0x00 SERVER_CONNECT   →  0x1F ACCEPTED + 0x01 SERVER_ACCEPT (여기서 XOR 키를 준다)
//
// ★ 마지막 자리가 **한 조각에 메시지 둘**이다. 예전에 이 10바이트를 통째로
//   "무늬"로 외웠는데, 표대로 쪼개보니 두 메시지가 붙은 것이었다.
#include "nox/Server.h"

#include <cmath>
#include <cstring>

#include "nox/Messages.h"
#include "nox/Opcode.h"

namespace nox {

void Server::SetInfoTemplate(const uint8_t* body, size_t n) {
    infoBody_.assign(body, body + n);
}

void Server::SetJoinTemplate(const uint8_t* body, size_t n) {
    joinBody_.assign(body, body + n);
}

void Server::SetUseMapTemplate(const uint8_t* body, size_t n) {
    useMapBody_.assign(body, body + n);
}

namespace {

void PutU16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

void PutU32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

}  // namespace

Session& Server::SessionFor(const std::string& addr) {
    auto it = sessions_.find(addr);
    if (it != sessions_.end()) return it->second;
    Session s;
    s.addr = addr;
    return sessions_.emplace(addr, s).first->second;
}

void Server::Emit(Session& s, bool reliable, uint8_t stream,
                  const uint8_t* msg, size_t n, std::vector<OutPacket>& out) {
    OutPacket p;
    p.to = s.addr;
    p.data.resize(kWireHeaderSize + n);
    WireHeader h;
    h.reliable = reliable;
    h.stream = stream;

    // 순번 규칙 — StreamSeq 주석 참조. 신뢰는 쓰고 올리고, 비신뢰는 직전 값을 싣는다.
    StreamSeq& q = s.seq[stream & 0x7F];
    if (reliable) {
        h.seq = q.next;
        q.last = q.next;
        ++q.next;
    } else {
        h.seq = q.last;
    }
    p.data[0] = h.Byte0();
    p.data[1] = h.seq;
    std::memcpy(p.data.data() + kWireHeaderSize, msg, n);

    // 키를 준 뒤로는 조각 전체가 뒤집힌다 — 머리도 포함이다.
    // (덤프의 `70 70 74` 가 XOR 0x70 을 풀면 `00 00 04` 인 것이 그 증거다)
    if (s.keyed) ApplyXor(p.data.data(), p.data.size(), s.xorKey);
    out.push_back(std::move(p));
}

// `0xcc SEQ_IMPORTANT` 로 싸서 내보낸다 — 머리 `cc | 번호 2 | 속길이 1` 뒤에 속 메시지.
//
// 실측 원문: `cc 20 00 07 | 6a cb 02 00 00 00 00`  (속 7바이트짜리 REPORT_CLIENT_STATUS)
void Server::EmitSeq(Session& s, const uint8_t* msg, size_t n,
                     std::vector<OutPacket>& out) {
    if (n == 0 || n > 0xFF) return;
    std::vector<uint8_t> buf;
    buf.reserve(4 + n);
    buf.push_back(static_cast<uint8_t>(Op::SEQ_IMPORTANT));
    buf.push_back(static_cast<uint8_t>(s.seqImpId));
    buf.push_back(static_cast<uint8_t>(s.seqImpId >> 8));
    buf.push_back(static_cast<uint8_t>(n));
    buf.insert(buf.end(), msg, msg + n);
    ++s.seqImpId;
    // 껍데기가 신뢰를 맡으므로 패킷 자체는 비신뢰다 (Server.h 의 EmitSeq 주석 참조).
    Emit(s, /*reliable=*/false, /*stream=*/0, buf.data(), buf.size(), out);
}

// 공격 주문과 그 위력.
//
// **어느 주문이 공격이냐**는 이름표에서 갈랐다(`SpellNames.cpp`, 기계로 뽑은 것).
// **위력 숫자는 실측이 아니다** — Server.h 의 SpellDamage 주석 참조.
int Server::SpellDamage(uint8_t spell) {
    switch (spell) {
        case 27: return 30;   // SPELL_FIREBALL       — 09-07 조작 실측에서 F 키
        case 43: return 25;   // SPELL_CHAIN_LIGHTNING — D 키. 표시 이름이 "Lightning"
        case 24: return 15;   // SPELL_LIGHTNING       — 표시 이름은 "Energy Bolt"
        default: return 0;    // 그 밖(점멸·보호막·치유 …)은 피해가 없다
    }
}

// 이 주문이 날리는 것의 와이어 종류 번호.
//
// ⚠ **실측이 아니라 이름 짐작이다** (09-07 정정, 까닭은 Server.h 의 ProjectileType 주석).
// 항목 하나를 채운다 — **처음 보는 것은 전체 번호로, 그 뒤로는 별칭으로.**
//
// ★★ 2026-09-07 에 찾은 결함이다. 전에는 첫 등장부터 별칭으로 보냈다.
//    그러면 클라는 **그 별칭이 무엇인지 알 길이 없다** — 별칭에는 종류가 안 실린다.
//
//    「서버가 `0xa5 NEW_ALIAS` 로 알려 주면 되지 않나」 싶지만 **방향이 반대다** —
//    실측에서 `0xa5` 는 서버 SEND **0건**, 클라→서버로만 왔다(0907 덤프 187건,
//    클라 덤프 839건). 별칭은 **클라가 스스로 굴리고 서버에 알려 주는 것**이고,
//    서버는 같은 규칙으로 따라 굴린다(Alias.h). 그러니 클라가 (번호,종류)를
//    이미 알고 있어야 같은 자리를 고른다. **그 앎은 전체 번호로 온 첫 등장에서 온다.**
static void FillEntry(AliasTable& tbl, uint16_t netCode, uint16_t type,
                      uint32_t frame, int x, int y, uint8_t facing, ObjEntry& e,
                      bool complex = true) {
    uint16_t hadNet = 0, hadType = 0;
    const int slot = tbl.Generate(netCode, type, frame);
    const bool known = slot > 0 && tbl.Lookup(slot, hadNet, hadType) &&
                       hadNet == netCode && hadType == type;
    const int alias = tbl.Assign(netCode, type, frame);
    if (known && alias > 0) {
        e.alias = static_cast<uint8_t>(alias);
    } else {
        e.alias = 0xff;          // 처음이다 — 번호와 종류를 실어 보낸다
        e.netCode = netCode;
    }
    e.type = type;
    e.absolute = true;           // 항목마다 절대 좌표 — 앞의 것에 안 얽힌다
    e.x = x;
    e.y = y;
    // ⚠ 복합이 아니면 **한 바이트도 붙지 않는다.** 깃발이 그렇다(종류표에서 확인).
    //   붙이면 클라가 그 바이트를 다음 항목의 시작으로 읽어 그 뒤가 다 어긋난다.
    e.extra = complex ? 1 : 0;
    if (complex) {
        e.extraBytes[0] = facing;
        // ★★ 방향 바이트의 최상위 비트는 값이 아니라 **신호**다 —
        //   클라는 그게 서 있으면 「스프라이트 프레임이 한 바이트 더 온다」로 읽는다
        //   (UpdateStream.cpp 의 Walk: `if (p[0] & 0x80) ++base`).
        //   그런데 한 바이트만 실으면 다음 항목을 한 칸 밀려 읽고
        //   그 뒤가 전부 어긋난다. 2026-09-08 실측 — 사람이 0x80 넘는 쪽을
        //   보는 순간 두 클라가 동시에 `0xae` 를 보내고 서버 목록으로 나갔다.
        if (facing & 0x80) { e.extra = 2; e.extraBytes[1] = 0; }
    }
}

uint16_t Server::ProjectileType(uint8_t spell) {
    switch (spell) {
        case 27: return 695;   // ⚠ 추정. 09-06 클라 덤프에는 693 TitanFireball 이 보였다
        case 43: return 710;   // LightningBolt
        case 24: return 703;   // SmallEnergyBolt
        default: return 0;
    }
}

// 이 주문이 불러내는 것의 와이어 종류 번호.
//
// ⚠ **실측이 아니라 이름 짝짓기다** (Server.h 의 SummonType 주석 참조).
//   `SpellNames.cpp` 의 `SPELL_SUMMON_*` 과 종류표의 같은 이름을 맞춘 것이고,
//   **소환 표본이 덤프에 없어 대조한 적이 없다.** 열둘이 COMPLEX 인 것만 표에서 확인했다.
uint16_t Server::SummonType(uint8_t spell) {
    switch (spell) {
        case 75: return 1357;  // SUMMON_BAT                → Bat
        case 76: return 1366;  // SUMMON_BLACK_BEAR         → BlackBear
        case 77: return 1365;  // SUMMON_BEAR               → Bear
        case 78: return 1382;  // SUMMON_BEHOLDER           → Beholder
        case 79: return 1348;  // SUMMON_BOMBER             → Bomber
        case 80: return 1371;  // SUMMON_CARNIVOROUS_PLANT  → CarnivorousPlant
        case 81: return 1355;  // SUMMON_ALBINO_SPIDER      → AlbinoSpider
        case 82: return 1356;  // SUMMON_SMALL_ALBINO_SPIDER→ SmallAlbinoSpider
        case 83: return 1317;  // SUMMON_EVIL_CHERUB        → EvilCherub
        case 84: return 1337;  // SUMMON_EMBER_DEMON        → EmberDemon
        case 85: return 1325;  // SUMMON_GHOST              → Ghost
        case 86: return 1358;  // SUMMON_GIANT_LEECH        → GiantLeech
        default: return 0;
    }
}

// 사거리 안에서 가장 가까운 다른 사람.
std::string Server::NearestOther(const std::string& self, double range) const {
    auto me = sessions_.find(self);
    if (me == sessions_.end() || !me->second.inGame) return {};

    std::string best;
    double bestD2 = range * range;
    for (const auto& kv : sessions_) {
        if (kv.first == self) continue;
        const Session& o = kv.second;
        if (!o.inGame || o.respawnAtFrame != 0) continue;   // 죽어 있는 사람은 안 맞는다
        const double dx = static_cast<double>(o.x - me->second.x);
        const double dy = static_cast<double>(o.y - me->second.y);
        const double d2 = dx * dx + dy * dy;
        if (d2 <= bestD2) { bestD2 = d2; best = kv.first; }
    }
    return best;
}

Target Server::NearestTarget(const std::string& friendOwner, double x, double y,
                             double range) const {
    Target best;
    double bestD2 = range * range;

    for (const auto& kv : sessions_) {
        if (kv.first == friendOwner) continue;          // 나 자신은 안 때린다
        const Session& o = kv.second;
        if (!o.inGame || o.respawnAtFrame != 0) continue;
        const double dx = o.x - x, dy = o.y - y;
        const double d2 = dx * dx + dy * dy;
        if (d2 <= bestD2) { bestD2 = d2; best.addr = kv.first; best.pet = -1; }
    }
    for (size_t i = 0; i < pets_.size(); ++i) {
        if (pets_[i].owner == friendOwner) continue;    // 내 편은 안 때린다
        const double dx = pets_[i].x - x, dy = pets_[i].y - y;
        const double d2 = dx * dx + dy * dy;
        if (d2 <= bestD2) { bestD2 = d2; best.addr.clear(); best.pet = static_cast<int>(i); }
    }
    return best;
}

// 불려 나온 것 하나를 없애고 모두에게 알린다.
//
// ★ `0x31 DESTROY_OBJECT` 는 실측이다 — `31 d6 02`(번호 2바이트, 모두 3바이트).
//   09-06 덤프에 54건 있었다. **이걸 안 보내면 화면에 시체가 영원히 남는다.**
void Server::KillPet(size_t idx, std::vector<OutPacket>& out) {
    if (idx >= pets_.size()) return;
    const uint16_t netCode = pets_[idx].netCode;
    const bool wild = pets_[idx].owner.empty();   // 주인 없는 것 = 맵의 몬스터
    pets_.erase(pets_.begin() + static_cast<long>(idx));
    ++petsKilled_;
    if (wild && questTotal_ > 0) {
        ++questKilled_;
        if (!roundOver_ && questKilled_ >= questTotal_) {
            roundOver_ = true;
            SendNotice("Quest complete", out);
        }
    }

    uint8_t msg[3];
    msg[0] = static_cast<uint8_t>(Op::DESTROY_OBJECT);
    msg[1] = static_cast<uint8_t>(netCode);
    msg[2] = static_cast<uint8_t>(netCode >> 8);
    for (auto& kv : sessions_) {
        if (kv.second.inGame) {
            Emit(kv.second, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);
        }
    }
}

void Server::HurtTarget(const Target& t, int dmg, std::vector<OutPacket>& out,
                        const std::string& by) {
    if (t.none() || dmg <= 0) return;
    if (t.pet < 0) { Damage(t.addr, dmg, out, by); return; }

    const size_t i = static_cast<size_t>(t.pet);
    if (i >= pets_.size()) return;
    Creature& k = pets_[i];
    if (k.health > dmg) {
        k.health = static_cast<uint16_t>(k.health - dmg);
        return;
    }
    KillPet(i, out);
}

bool Server::Damage(const std::string& addr, int amount, std::vector<OutPacket>& out,
                    const std::string& by) {
    auto it = sessions_.find(addr);
    if (it == sessions_.end()) return false;
    Session& s = it->second;
    if (!s.inGame || s.respawnAtFrame != 0 || amount <= 0) return false;

    const int before = s.health;
    int after = before - amount;
    if (after < 0) after = 0;
    s.health = static_cast<uint16_t>(after);

    // ① 얼마나 깎였나 — 겉에 그냥 실어 보낸다.
    //    실측 `42 d8 02 81 ff` = 번호 728 · −127. 증감은 **부호 있는 16비트**다.
    {
        const int16_t delta = static_cast<int16_t>(after - before);
        const uint16_t raw = static_cast<uint16_t>(delta);
        uint8_t msg[5];
        msg[0] = static_cast<uint8_t>(Op::REPORT_HEALTH_DELTA);
        msg[1] = static_cast<uint8_t>(s.netCode);
        msg[2] = static_cast<uint8_t>(s.netCode >> 8);
        msg[3] = static_cast<uint8_t>(raw);
        msg[4] = static_cast<uint8_t>(raw >> 8);
        Emit(s, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);
    }

    // ② 내 체력의 절대값 — 껍데기 안이다. 실측 `43 4a 00`.
    {
        uint8_t msg[3];
        msg[0] = static_cast<uint8_t>(Op::REPORT_PLAYER_HEALTH);
        msg[1] = static_cast<uint8_t>(s.health);
        msg[2] = static_cast<uint8_t>(s.health >> 8);
        EmitSeq(s, msg, sizeof(msg), out);
    }

    if (s.health > 0) return false;

    // ③ 죽음 — 껍데기 안. 실측 `e8 d8 02`.
    {
        uint8_t msg[3];
        msg[0] = static_cast<uint8_t>(Op::PLAYER_DIED);
        msg[1] = static_cast<uint8_t>(s.netCode);
        msg[2] = static_cast<uint8_t>(s.netCode >> 8);
        EmitSeq(s, msg, sizeof(msg), out);
    }
    s.respawnAtFrame = frame_ + kRespawnDelayTicks;
    ++s.deaths;
    DropFlag(addr, out);   // 들고 있었으면 그 자리에 떨군다

    // ④ 점수 — 때린 쪽이 있고 자기 자신이 아니면 그쪽이 하나 얻는다.
    //    자기가 자기를 죽인 것(자기에게 건 주문)은 점수가 아니다.
    if (!by.empty() && by != addr) {
        auto k = sessions_.find(by);
        if (k != sessions_.end()) {
            ++k->second.kills;
            if (!roundOver_ && k->second.kills >= kArenaKillLimit) {
                roundOver_ = true;
                // ⚠⚠ `0x58 REPORT_DEATHMATCH_WINNER` 의 **속 짜임은 모른다.**
                //    opennox 길이표에 본문 7바이트라고만 있고(codes.go:301) 구조체가 없다.
                //    우리 덤프에도 **0건**이다 — 겨루기 모드로 돌린 적이 없다.
                //    길이는 맞으니 흐름은 안 어긋나지만, **안에 든 값은 확인 안 됐다.**
                //    번호 2 + 점수 2 + 채움 3 으로 두었을 뿐이다. 표본이 생기면 고칠 것.
                uint8_t msg[8];
                msg[0] = static_cast<uint8_t>(Op::REPORT_DEATHMATCH_WINNER);
                msg[1] = static_cast<uint8_t>(k->second.netCode);
                msg[2] = static_cast<uint8_t>(k->second.netCode >> 8);
                msg[3] = static_cast<uint8_t>(k->second.kills);
                msg[4] = static_cast<uint8_t>(k->second.kills >> 8);
                msg[5] = msg[6] = msg[7] = 0;
                for (auto& kv : sessions_) {
                    if (kv.second.inGame) {
                        EmitSeq(kv.second, msg, sizeof(msg), out);
                    }
                }
            }
        }
    }
    return true;
}

// 판 시계 — 짜임은 실측이다(PROTOCOL.md 「판 시계」 절).
//
//   d3 | 번호 4 | 남은 시간 4(밀리초, 부호 있음) | 지금 틱 4
//
// 한도가 없으면 남은 시간이 **음수로 계속 내려간다** — 실측이 그랬다.
void Server::StartQuest(int count, int centerX, int centerY) {
    questTotal_ = count;
    questKilled_ = 0;
    roundOver_ = false;

    // 가운데를 둘러 고르게 세운다. ⚠ 자리 규칙은 실측이 아니다.
    for (int i = 0; i < count; ++i) {
        const double ang = 2.0 * 3.14159265358979 * i / (count > 0 ? count : 1);
        Creature k;
        k.owner.clear();                 // ★ 주인이 없다 — 아무 편도 아니다
        k.type = SummonType(79);         // Bomber. 종류표에서 확인한 번호다
        k.netCode = nextShotNetCode_++;
        if (nextShotNetCode_ == 0) nextShotNetCode_ = 4096;
        k.x = centerX + kQuestSpread * std::cos(ang);
        k.y = centerY + kQuestSpread * std::sin(ang);
        k.health = kPetHealth;
        pets_.push_back(k);
    }
}

// 글자 한 줄 — `0xa8 TEXT_MESSAGE`. 짜임은 실측이고 **문구만 우리 것**이다.
void Server::SendNotice(const char* text, std::vector<OutPacket>& out) {
    const size_t n = std::strlen(text);
    if (n == 0 || n > 200) return;
    std::vector<uint8_t> msg;
    msg.reserve(11 + n);
    msg.push_back(static_cast<uint8_t>(Op::TEXT_MESSAGE));
    for (auto& kv : sessions_) {
        if (!kv.second.inGame) continue;
        Session& s = kv.second;
        msg.resize(1);
        msg.push_back(static_cast<uint8_t>(s.netCode));
        msg.push_back(static_cast<uint8_t>(s.netCode >> 8));
        msg.push_back(0x02);                       // 깃발 = UTF-8 (그래야 한 글자 한 바이트)
        msg.push_back(static_cast<uint8_t>(s.x));
        msg.push_back(static_cast<uint8_t>(s.x >> 8));
        msg.push_back(static_cast<uint8_t>(s.y));
        msg.push_back(static_cast<uint8_t>(s.y >> 8));
        msg.push_back(static_cast<uint8_t>(n));
        msg.push_back(0); msg.push_back(0);        // 지속 = 0
        msg.insert(msg.end(), text, text + n);
        Emit(s, /*reliable=*/false, /*stream=*/0, msg.data(), msg.size(), out);
    }
}

void Server::StartCtf(int redX, int redY, int blueX, int blueY) {
    flags_.clear();
    caps_[0] = caps_[1] = caps_[2] = 0;

    Flag red;
    red.team = 1;
    red.type = kFlagTypeRed;
    red.netCode = nextShotNetCode_++;
    red.homeX = red.x = redX;
    red.homeY = red.y = redY;
    flags_.push_back(red);

    Flag blue;
    blue.team = 2;
    blue.type = kFlagTypeBlue;
    blue.netCode = nextShotNetCode_++;
    blue.homeX = blue.x = blueX;
    blue.homeY = blue.y = blueY;
    flags_.push_back(blue);

    // 팀은 들어온 차례로 번갈아. ⚠ 진짜 녹스가 이렇게 나누는지는 모른다.
    uint8_t next = 1;
    for (auto& kv : sessions_) {
        if (!kv.second.inGame) continue;
        kv.second.team = next;
        next = (next == 1) ? 2 : 1;
    }
}

// 깃발 알림 — `0xa9 INFORM` 의 갈래 5·6·7.
// 속 짜임은 opennox `InformTeamPlayer` 그대로다: PlayerID 4 + TeamID 4.
void Server::SendFlagInform(InformCode code, uint16_t playerNetCode, uint8_t team,
                            std::vector<OutPacket>& out) {
    uint8_t msg[10];
    msg[0] = static_cast<uint8_t>(Op::INFORM);
    msg[1] = static_cast<uint8_t>(code);
    const uint32_t pid = playerNetCode;
    msg[2] = static_cast<uint8_t>(pid);
    msg[3] = static_cast<uint8_t>(pid >> 8);
    msg[4] = msg[5] = 0;
    msg[6] = team;
    msg[7] = msg[8] = msg[9] = 0;
    for (auto& kv : sessions_) {
        if (kv.second.inGame) {
            Emit(kv.second, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);
        }
    }
}

void Server::DropFlag(const std::string& who, std::vector<OutPacket>& out) {
    for (auto& f : flags_) {
        if (f.carrier != who) continue;
        auto it = sessions_.find(who);
        if (it != sessions_.end()) { f.x = it->second.x; f.y = it->second.y; }
        f.carrier.clear();
        f.atHome = false;
        SendFlagInform(InformCode::FlagDrop,
                       it == sessions_.end() ? 0 : it->second.netCode,
                       f.team, out);
    }
}

void Server::UpdateFlags(std::vector<OutPacket>& out) {
    if (flags_.empty()) return;

    for (auto& kv : sessions_) {
        Session& s = kv.second;
        if (!s.inGame || s.respawnAtFrame != 0 || s.team == 0) continue;

        for (auto& f : flags_) {
            const double dx = f.x - s.x, dy = f.y - s.y;
            const double d2 = dx * dx + dy * dy;
            const bool near = d2 <= kFlagTouchRange * kFlagTouchRange;

            if (f.team != s.team) {
                // 적 깃발 — 바닥에 있고 가까우면 든다.
                if (f.carrier.empty() && near) {
                    f.carrier = kv.first;
                    SendFlagInform(InformCode::FlagPickup, s.netCode, s.team, out);
                }
            } else {
                // 내 깃발 — 바닥에 나와 있고 가까우면 제자리로 돌린다.
                if (f.carrier.empty() && !f.atHome && near) {
                    f.x = f.homeX; f.y = f.homeY; f.atHome = true;
                    SendFlagInform(InformCode::FlagRetrieve, s.netCode, s.team, out);
                }
            }
        }

        // 적 깃발을 들고 **내 깃발이 제자리에 있을 때** 내 진영에 닿으면 득점.
        // ⚠ 「내 깃발이 제자리에 있어야 한다」는 CTF 의 흔한 규칙이지만 **실측은 아니다.**
        Flag* mine = nullptr;
        Flag* held = nullptr;
        for (auto& f : flags_) {
            if (f.team == s.team) mine = &f;
            if (f.carrier == kv.first) held = &f;
        }
        if (held && mine && mine->atHome) {
            const double dx = mine->homeX - s.x, dy = mine->homeY - s.y;
            if (dx * dx + dy * dy <= kFlagTouchRange * kFlagTouchRange) {
                held->carrier.clear();
                held->x = held->homeX; held->y = held->homeY; held->atHome = true;
                if (s.team < 3) ++caps_[s.team];
                SendFlagInform(InformCode::FlagCapture, s.netCode, s.team, out);
                if (!roundOver_ && caps_[s.team] >= kCaptureLimit) roundOver_ = true;
            }
        }
    }

    // 들려 있는 깃발은 든 사람을 따라간다.
    for (auto& f : flags_) {
        if (f.carrier.empty()) continue;
        auto it = sessions_.find(f.carrier);
        if (it == sessions_.end() || !it->second.inGame) { f.carrier.clear(); continue; }
        f.x = it->second.x;
        f.y = it->second.y;
        f.atHome = false;
    }
}

void Server::SendTimer(std::vector<OutPacket>& out) {
    const int32_t remain = -static_cast<int32_t>(frame_ * tickMs_);
    uint8_t msg[13];
    msg[0] = static_cast<uint8_t>(Op::TIMER_STATUS);
    msg[1] = msg[2] = msg[3] = msg[4] = 0;      // 표본 전수가 0 이었다
    const uint32_t r = static_cast<uint32_t>(remain);
    msg[5] = static_cast<uint8_t>(r);
    msg[6] = static_cast<uint8_t>(r >> 8);
    msg[7] = static_cast<uint8_t>(r >> 16);
    msg[8] = static_cast<uint8_t>(r >> 24);
    msg[9]  = static_cast<uint8_t>(frame_);
    msg[10] = static_cast<uint8_t>(frame_ >> 8);
    msg[11] = static_cast<uint8_t>(frame_ >> 16);
    msg[12] = static_cast<uint8_t>(frame_ >> 24);
    for (auto& kv : sessions_) {
        if (kv.second.inGame) EmitSeq(kv.second, msg, sizeof(msg), out);
    }
}

size_t Server::OnPacket(const std::string& from, const uint8_t* data, size_t n,
                        std::vector<OutPacket>& out) {
    Session& s = SessionFor(from);
    ++s.recvPackets;

    // 받은 것이 암호문이면 먼저 푼다. 사본을 뜬다 — 부르는 쪽 버퍼를 건드리지 않는다.
    // ⚠ 덤프 재생일 때는 이미 평문이라 풀면 안 된다 (SetInboundPlain 주석 참조).
    std::vector<uint8_t> buf(data, data + n);
    if (s.keyed && !inboundPlain_) ApplyXor(buf.data(), buf.size(), s.xorKey);

    WireHeader head;
    if (!ReadWireHeader(buf.data(), buf.size(), head)) return 0;

    const uint8_t* body = buf.data() + kWireHeaderSize;
    const size_t bodyLen = buf.size() - kWireHeaderSize;

    // ★ 먼저 **끝까지 깨끗하게 쪼개지는지** 본다. 안 되면 아무 답도 만들지 않는다.
    //
    // 길이를 모르는 메시지를 만나면 그 뒤를 못 읽는데, 반쯤 읽은 채로 답을 만들면
    // **밀린 바이트에서 우연히 나온 오프코드에 답한다.** 실제로 그렇게 해서
    // 서버 찾기가 10건에서 75건으로 불어난 적이 있다.
    //
    // 「모르면 안 읽는다」가 「읽은 척한다」보다 낫다. 가변 메시지를 하나씩
    // 옮길수록 여기를 통과하는 조각이 늘어난다.
    {
        MessageIter probe(body, bodyLen, /*fromClient=*/true);
        while (probe.Next()) {}
        if (probe.stopped()) {
            ++stopped_;
            ++stoppedByOp_[static_cast<uint8_t>(probe.stopOp())];
            return 0;
        }
    }

    MessageIter it(body, bodyLen, /*fromClient=*/true);
    size_t count = 0;
    while (it.Next()) {
        ++count;
        ++seen_[it.op()];

        switch (static_cast<Op>(it.op())) {
            // 핑에 그대로 되돌린다. 값은 클라의 시각이라 **손대지 않는다.**
            // 실측 = `01 04 06 <4바이트>` 에 `01 00 08 <같은 4바이트>` 로 답했다.
            // 스트림은 받은 것을 그대로 쓴다(핑·퐁은 스트림 1 을 쓰더라).
            case Op::CLIENT_PING: {
                if (it.bodyLen() != 4) break;
                uint8_t msg[5];
                msg[0] = static_cast<uint8_t>(Op::CLIENT_PONG);
                std::memcpy(msg + 1, it.body(), 4);
                Emit(s, /*reliable=*/false, head.stream, msg, sizeof(msg), out);
                ++handled_;
                break;
            }

            // ★★ 신뢰 메시지 되받기 — 클라가 0xAA 로 보내면 0xAB 로 번호를 그대로
            //    돌려줘야 한다. 안 하면 클라는 답이 올 때까지 **영원히 재전송**하고
            //    로딩에서 안 넘어간다 — 화면이 검은 채로 멈춘다.
            //    2026-09-07 실측: 내 서버엔 IMPORTANT 1,378건 · 정품엔 0건,
            //    CLIENT_READY 도 71번(정품 2번) 재전송하고 있었다.
            case Op::IMPORTANT: {
                if (it.bodyLen() < 4) break;
                uint8_t msg[5];
                msg[0] = static_cast<uint8_t>(Op::IMPORTANT_ACK);
                std::memcpy(msg + 1, it.body(), 4);
                Emit(s, /*reliable=*/false, head.stream, msg, sizeof(msg), out);
                ++handled_;
                break;
            }

            // ★ 클라 입력 — 매 틱 온다. 트래픽에서 가장 잦은 메시지다(32,450건).
            //
            //   본문 = 길이 1B + [코드 4B + 데이터]*
            //   코드는 4바이트를 먹고 **첫 바이트가 무엇을 눌렀나**를 가리킨다.
            //   실측 `3f 0a | 01 00 00 00 63 | 01 00 00 00 63` — 방향(1) 두 번, 값 0x63.
            //
            // 데이터 크기 (noxnet/msg_player_input.go 의 `CtrlCode.DataSize()`):
            //   방향(1)·앞뒤좌우(2~5)·주문끝(28) = 1바이트,
            //   주문시전(29 CastQueuedSpell·30 CastMostRecentSpell) = 4바이트, 그 밖 0.
            //
            // ⚠ 2026-09-07 정정 — 여기 `19·21·22` 라고 적혀 있었다. **옮겨 적기 오류였다.**
            //   19=Inventory·21=SpellGestureDown·22=SpellGestureLeft 는 전부 크기 0 이고,
            //   1바이트짜리는 28, 4바이트짜리는 29·30 이다.
            //   ⚠ 09-06 캡처로는 못 가린다 — 그 여섯 코드가 **한 건도 안 잡혔다.**
            //   근거는 opennox 소스뿐이니, 주문 몸짓·시전을 실제로 담은 캡처로 다시 확인할 것.
            //
            // ⚠ 아직 못 읽는 자리 — 코드 4바이트의 **위 3바이트**. 서버 덤프에서 방향(1) 항목의
            //   36.8%(54,254건)가 셋째·넷째 바이트에 0 아닌 값을 담고 있다.
            //   opennox 도 첫 바이트만 읽고 버린다. 무엇인지 미상 (`tools/dumpscan -inputs`).
            case Op::PLAYER_INPUT: {
                if (!s.inGame || it.bodyLen() < 1) break;
                const uint8_t* q = it.body() + 1;
                size_t qn = it.bodyLen() - 1;
                if (qn > static_cast<size_t>(it.body()[0])) qn = it.body()[0];
                while (qn >= 4) {
                    const uint8_t code = q[0];
                    q += 4; qn -= 4;
                    size_t dataLen = 0;
                    switch (code) {
                        case 1: case 2: case 3: case 4: case 5: case 28: dataLen = 1; break;
                        case 29: case 30: dataLen = 4; break;
                        default: dataLen = 0; break;
                    }
                    if (qn < dataLen) break;
                    if (code == 1 && dataLen == 1) {
                        s.facing = q[0];          // 바라보는 쪽
                    } else if (code >= 2 && code <= 5 && dataLen == 1) {
                        // 걸음 — 바라보는 쪽을 기준으로 민다.
                        // 방향은 한 바퀴를 256으로 나눈 값이다(1바이트).
                        // 속도는 아직 실측 못 했다. 눈으로 확인할 만한 값으로 둔다.
                        int dir = s.facing;
                        if (code == 3) dir += 128;        // 뒤
                        else if (code == 4) dir += 192;   // 왼
                        else if (code == 5) dir += 64;    // 오른
                        const double rad = (dir & 0xFF) * 3.14159265358979 / 128.0;
                        s.x += static_cast<int>(kStepPerTick * std::cos(rad));
                        s.y += static_cast<int>(kStepPerTick * std::sin(rad));
                        if (s.x < 0) s.x = 0;
                        if (s.y < 0) s.y = 0;
                        if (s.x > kMaxCoord) s.x = kMaxCoord;
                        if (s.y > kMaxCoord) s.y = kMaxCoord;
                        ++moves_;
                    } else if (code == 6) {
                        // 왼버튼 = `CCAction`(6). 근접 공격이다.
                        // 근거는 둘 — 09-07 조작 실측과 opennox 의 CtrlCode 표.
                        //
                        // ⚠ 버튼을 누르고 있으면 이 코드가 **매 틱** 온다.
                        //   쉬는 시간을 안 두면 초당 서른 대가 들어간다.
                        if (frame_ >= s.nextMeleeFrame) {
                            s.nextMeleeFrame = frame_ + kMeleeCooldownTicks;
                            const Target t = NearestTarget(from, s.x, s.y, kMeleeRange);
                            HurtTarget(t, kMeleeDamage, out, from);
                            ++melees_;
                        }
                    }
                    q += dataLen; qn -= dataLen;
                }
                break;
            }

            // ★ 주문 시전 — 2026-09-06 캡처로 짜임을 확정했다(표본 83개 전수).
            //
            //   79 │ 주문번호 1 │ 0 으로 채운 19 │ 대상 플래그 1
            //
            // 주문번호는 opennox `spell` 패키지 ID 와 그대로 같다 (0x1B = 27 = 파이어볼).
            // ⚠ **겨눈 쪽이 여기 없다** — 가운데 19바이트가 표본 전수에서 0 이었다.
            //    대상도 좌표도 안 실린다. 겨눈 쪽은 따로 오는 마우스(0xAC)와
            //    바라보는 쪽(PLAYER_INPUT 코드 1)에 있다.
            // 마지막 자리는 주문마다 갈렸다 — 남에게 쏘는 것(파이어볼·연쇄번개)이 0,
            // 자기에게 거는 것(점멸·치유·보호막)이 1. 보호막만 18건 중 1건이 0 이라
            // 「자기 대상 여부」로 단정하지는 않는다.
            //
            // 지금은 받아서 세는 데까지다. **효과를 되쏘는 것은 아직 못 한다** —
            // 효과 메시지(0x80 계열)의 짜임을 아직 모른다.
            case Op::TRY_SPELL: {
                if (!s.inGame || it.bodyLen() != kTrySpellBodyLen) break;
                const uint8_t spell = it.body()[0];
                const bool onSelf = it.body()[kTrySpellBodyLen - 1] != 0;
                ++spells_[spell];
                if (onSelf) ++spellsOnSelf_;

                // ① 시전을 알린다 — 실측 `70 1b`(파이어볼) 꼴로 2바이트다.
                //    보는 사람 모두에게 나간다(누가 뭘 걸었는지 그려야 하니까).
                {
                    uint8_t msg[2];
                    msg[0] = static_cast<uint8_t>(Op::REPORT_SPELL_START);
                    msg[1] = spell;
                    for (auto& kv2 : sessions_) {
                        if (kv2.second.inGame) {
                            Emit(kv2.second, /*reliable=*/false, /*stream=*/0,
                                 msg, sizeof(msg), out);
                        }
                    }
                }

                // ② 공격 주문이면 맞힌다.
                //    ⚠ 대상 고르기가 **가장 얇다** — 자기에게 건 것이 아니면
                //      사거리 안에서 가장 가까운 한 사람이다. 진짜 녹스는 날아가는
                //      투사체가 부딪히는 것을 본다. 그건 다음 조각이다.
                // ② 소환 주문이면 부른다.
                if (const uint16_t stype = SummonType(spell)) {
                    Creature k;
                    k.owner = from;
                    k.type = stype;
                    k.netCode = nextShotNetCode_++;
                    if (nextShotNetCode_ == 0) nextShotNetCode_ = 4096;
                    k.facing = s.facing;
                    // 부른 사람 바로 앞에 세운다.
                    const double rad = s.facing * 3.14159265358979 / 128.0;
                    k.x = s.x + kPetReach * std::cos(rad);
                    k.y = s.y + kPetReach * std::sin(rad);
                    k.health = kPetHealth;
                    pets_.push_back(k);
                    ++handled_;
                    break;
                }

                const int dmg = SpellDamage(spell);
                if (dmg > 0) {
                    if (onSelf) {
                        // 자기에게 건 것은 날아갈 데가 없다. 그 자리에서 받는다.
                        Damage(from, dmg, out, from);   // 자기에게 건 것 — 점수는 안 준다
                    } else {
                        // ★ 날려 보낸다. 시전하는 순간 맞으면 **피할 수가 없다** —
                        //   비켜서 안 맞는 것이 있어야 싸움이 싸움이 된다.
                        //   방향은 그 사람이 바라보는 쪽(한 바퀴를 256으로 나눈 값).
                        const double rad = s.facing * 3.14159265358979 / 128.0;
                        Projectile p;
                        p.owner = from;
                        p.spell = spell;
                        p.x = s.x;
                        p.y = s.y;
                        p.vx = kProjectileSpeed * std::cos(rad);
                        p.vy = kProjectileSpeed * std::sin(rad);
                        p.damage = dmg;
                        p.facing = s.facing;
                        p.netCode = nextShotNetCode_++;
                        if (nextShotNetCode_ == 0) nextShotNetCode_ = 4096;  // 한 바퀴 돌면
                        shots_.push_back(p);
                    }
                }
                ++handled_;
                break;
            }

            case Op::SERVER_DISCOVER: {
                // 이미 붙은 상대가 다시 찾을 리 없다. 상태를 지켜야 밀린 바이트에
                // 속지 않는다.
                if (s.keyed) break;
                if (infoBody_.empty()) break;
                MsgServerDiscover req;
                Reader r(it.body(), it.bodyLen());
                if (!req.Decode(r)) break;

                // 원본을 그대로 두고 토큰 자리만 바꾼다.
                std::vector<uint8_t> msg;
                msg.reserve(1 + infoBody_.size());
                msg.push_back(static_cast<uint8_t>(Op::SERVER_INFO));
                msg.insert(msg.end(), infoBody_.begin(), infoBody_.end());
                if (infoBody_.size() >= kInfoTokenOffset + 4) {
                    uint8_t* t = msg.data() + 1 + kInfoTokenOffset;
                    t[0] = static_cast<uint8_t>(req.token);
                    t[1] = static_cast<uint8_t>(req.token >> 8);
                    t[2] = static_cast<uint8_t>(req.token >> 16);
                    t[3] = static_cast<uint8_t>(req.token >> 24);
                }
                Emit(s, /*reliable=*/false, /*stream=*/0, msg.data(), msg.size(), out);
                ++handled_;
                break;
            }

            case Op::SERVER_TRY_JOIN: {
                if (s.keyed) break;
                // 지금은 조건 없이 받아준다. 거절 사유(중복 일련번호·인원 참·버전)는
                // 세션 관리를 만들 때 붙인다.
                const uint8_t msg[1] = {static_cast<uint8_t>(Op::SERVER_JOIN_OK)};
                Emit(s, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);
                ++handled_;
                break;
            }

            case Op::SERVER_CONNECT: {
                // 이미 키를 준 상대가 다시 보내면 재전송이다. 번호를 새로 매기지 않는다.
                // ★ 다만 **답은 한다** — 정품은 이 두 번째 요청에 0x04 를 하나 돌려준다
                //   (3바이트·비신뢰·본문 0). 09-05 정품 덤프의 자리 그대로다.
                //   무엇인지는 아직 모른다(PROTOCOL.md 도 「미상」).
                if (s.keyed) {
                    const uint8_t m4[1] = {static_cast<uint8_t>(Op::CODE4)};
                    Emit(s, /*reliable=*/false, /*stream=*/0, m4, sizeof(m4), out);
                    ++handled_;
                    break;
                }
                if (scriptAt_ < script_.size()) {
                    // 덤프 재생 — 그때 서버가 정한 값들을 그대로 쓴다.
                    const AcceptScript& a = script_[scriptAt_++];
                    s.connectId = a.connectId;
                    s.xorKey = a.xorKey;
                    s.frame = a.frame;
                    s.netCode = a.netCode;
                } else {
                    s.connectId = nextConnectId_++;
                    s.xorKey = xorKey_;
                    // ★★ 서두가 있으면 그 바로 앞 프레임을 준다 — 시계가 한 줄로 이어지게.
                    //   전역 프레임을 주면 늦게 붙은 사람은 서두보다 앞서서, 그 뒤 갱신이
                    //   전부 과거로 보여 버려진다(2026-09-08 둘째 클라 검은 화면의 원인).
                    s.frame = preludeFirstFrame_ ? preludeFirstFrame_ - 1 : frame_;
                    s.netCode = netCode_;
                }

                uint8_t msg[8];
                size_t k = 0;
                msg[k++] = static_cast<uint8_t>(Op::ACCEPTED);
                msg[k++] = 0;                      // 스트림을 받아들였다는 표시
                msg[k++] = static_cast<uint8_t>(Op::SERVER_ACCEPT);
                msg[k++] = static_cast<uint8_t>(s.connectId);
                msg[k++] = static_cast<uint8_t>(s.connectId >> 8);
                msg[k++] = static_cast<uint8_t>(s.connectId >> 16);
                msg[k++] = static_cast<uint8_t>(s.connectId >> 24);
                msg[k++] = s.xorKey;

                // ★ 이 조각까지는 평문이다. 키는 이 안에 실려 나간다.
                Emit(s, /*reliable=*/true, /*stream=*/0, msg, k, out);
                s.keyed = true;   // 다음 조각부터 뒤집는다
                ++handled_;
                break;
            }

            // 클라가 「받았다」고 알려오면, 서버가 게임에 필요한 것을 한꺼번에 내려준다.
            // 216바이트 묶음(메시지 여섯) → USE_MAP 순서다. 순서를 바꾸면 순번이 어긋난다.
            case Op::CLIENT_ACCEPT: {
                // ★★ 기본값을 **묶음을 만들기 전에** 채운다.
                //    예전에는 Emit 뒤에 채워서 접속 묶음의 오브젝트 번호가 0 으로 나갔다.
                //    클라는 「네 캐릭터 번호는 0」을 받으니 카메라를 붙일 대상이 없어
                //    화면이 검은 채로 멈춘다(2026-09-07, 정품과 바이트 대조로 잡음).
                // ★ 스폰 자리 — 예전엔 (1000,1000) 이라는 지어낸 값이었다.
                //   estate.map 에서 정품이 쓰는 자리를 덤프에서 읽어 넣는다
                //   (2026-09-07 레퍼런스 상태방송 머리: x=2304 y=3498).
                // ★ 스폰 자리는 **맵마다 다르다**. 지금은 덤프의 맵을 그대로 쓰므로
                //   그 덤프에서 정품이 쓴 자리를 그대로 쓴다.
                //   09-05 덤프(bludeath.map)의 상태갱신 머리: x=3864 y=2534.
                //   ⚠ estate 좌표(2304,3498)를 bludeath 에 쓰면 맵 밖이라 아무것도 안 그려진다.
                if (s.x == 0 && s.y == 0) { s.x = 3864; s.y = 2534; }
                // ★ 오브젝트 번호는 **접속마다 달라야 한다**. 전에는 모두 713 이라
                //   둘째 클라가 첫째와 같은 번호를 받아 화면이 검었다(2026-09-08).
                if (s.netCode == 0) s.netCode = nextPlayerNetCode_++;
                if (s.maxHealth == 0) {
                    s.maxHealth = kDefaultMaxHealth;
                    s.health = s.maxHealth;
                }
                if (!joinBody_.empty()) {
                    std::vector<uint8_t> msg = joinBody_;
                    if (msg.size() > kJoinNetCodeB + 1) {
                        PutU32(msg.data() + kJoinFrameA, s.frame);
                        PutU32(msg.data() + kJoinFrameB, s.frame);
                        PutU16(msg.data() + kJoinNetCodeA, s.netCode);
                        PutU16(msg.data() + kJoinNetCodeB, s.netCode);
                    }
                    // ★ 플레이어 정보는 지어내지 않는다 — 방금 받은 것을 그대로 옮긴다.
                    if (msg.size() >= kJoinPlayerInfo + kJoinPlayerInfoLen &&
                        it.bodyLen() >= kJoinPlayerInfoLen) {
                        std::memcpy(msg.data() + kJoinPlayerInfo, it.body(),
                                    kJoinPlayerInfoLen);
                    }
                    Emit(s, /*reliable=*/true, /*stream=*/0, msg.data(), msg.size(), out);
                    ++handled_;
                }
                // 여기서부터 게임에 들어온 것으로 본다 — 매 틱 상태를 받는다.
                s.inGame = true;

                if (!useMapBody_.empty()) {
                    std::vector<uint8_t> msg = useMapBody_;
                    if (msg.size() > kUseMapFrame + 3)
                        PutU32(msg.data() + kUseMapFrame, s.frame);
                    Emit(s, /*reliable=*/true, /*stream=*/0, msg.data(), msg.size(), out);
                    ++handled_;
                }
                // ★ 맵 지정 뒤에 정품이 더 보내는 것들을 그대로 이어 보낸다.
                for (const auto& p : postMap_) {
                    if (p.empty()) continue;
                    Emit(s, /*reliable=*/true, /*stream=*/0, p.data(), p.size(), out);
                    ++handled_;
                }
                break;
            }

            default:
                break;   // 아직 답하지 않는다. 세기만 했다.
        }
    }
    // 여기까지 왔으면 앞의 훑기에서 이미 끝까지 읽혔다. 다시 셀 것이 없다.
    return count;
}

// ── 틱 ───────────────────────────────────────────────────
//
// 매 틱 나가는 것 = `0x27 TIMESTAMP`(프레임) + `0xa4 UPDATE_STREAM`(누가 어디 있나).
// 실측에서도 이 둘이 거의 1:1 로 붙어 나갔다(30,097 대 30,087).
//
// ★ 상태 갱신의 짜임은 왕복 검사로 굳혀 두었다 — 덤프 30,087건을 뜯어 다시 짜면
//   원본과 바이트가 같다(nox_verify). 그러니 여기서는 **무엇을 담을지**만 정하면 된다.
int Server::Tick(uint32_t nowMs, std::vector<OutPacket>& out) {
    if (!tickStarted_) {
        tickStarted_ = true;
        lastTickMs_ = nowMs;
        return 0;
    }
    if (nowMs < lastTickMs_) lastTickMs_ = nowMs;   // 시계가 되감겼다
    int ticks = 0;
    while (nowMs - lastTickMs_ >= tickMs_) {
        lastTickMs_ += tickMs_;
        ++frame_;
        ++ticks;
    }
    if (ticks == 0) return 0;

    // ── 날아가는 것들을 민다 ──
    // 지나간 틱 수만큼 밀어야 한다. 한 번만 밀면 서버가 늦게 돈 만큼 느려진다.
    for (int step = 0; step < ticks && !shots_.empty(); ++step) {
        for (size_t i = 0; i < shots_.size();) {
            Projectile& p = shots_[i];
            // ★★ 한 걸음을 잘게 쪼개 밀면서 매번 본다.
            //   한 틱에 미는 거리(26)가 판정 반경(24)보다 커서 **표적을 뛰어넘고 있었다** —
            //   틱 사이에 지나가 버리면 영영 안 맞는다. 2026-09-08 실측:
            //   시전 12건에 피해 1건. 쪼개면 한 걸음이 반경보다 작아져 그 구멍이 없어진다.
            // ★ 그리고 **밀기 전에 한 번** 본다(sub == 0). 같은 자리에서 쏘면 표적이
            //   코앞인데, 먼저 밀어 버리면 첫 판정 때 이미 26 만큼 멀어져 있다.
            bool gone = false;
            for (int sub = 0; sub <= kProjectileSubSteps && !gone; ++sub) {
                if (sub > 0) {
                    p.x += p.vx / kProjectileSubSteps;
                    p.y += p.vy / kProjectileSubSteps;
                    p.traveled += kProjectileSpeed / kProjectileSubSteps;
                }

                // 사거리를 넘었거나 맵 밖이면 그냥 사라진다.
                if (p.traveled > kSpellRange || p.x < 0 || p.y < 0 ||
                    p.x > kMaxCoord || p.y > kMaxCoord) {
                    shots_.erase(shots_.begin() + static_cast<long>(i));
                    gone = true;
                    break;
                }

                // 누구에게 닿았나. **쏜 사람과 그가 부른 것은 안 맞는다.**
                const Target hit = NearestTarget(p.owner, p.x, p.y, kProjectileHitRadius);
                if (hit.none()) continue;

                // ⚠ 지우기 **전에** 쓸 값을 다 꺼내 둔다. 전에는 지운 뒤에 `p.owner` 를
                //   읽었다 — 주석에는 「p 는 이미 없다」고 적어 놓고 그대로 썼다.
                const int dmg = p.damage;
                const std::string owner = p.owner;
                shots_.erase(shots_.begin() + static_cast<long>(i));
                ++hits_;
                HurtTarget(hit, dmg, out, owner);
                gone = true;
            }
            if (!gone) ++i;
        }
    }

    // ── 불려 나온 것들을 움직인다 ──
    // 부른 사람 편이다 — **부른 사람은 안 때린다.** 그게 없으면 부르자마자 자기가 맞는다.
    for (int step = 0; step < ticks && !pets_.empty(); ++step) {
        for (auto& k : pets_) {
            // 가장 가까운 적을 고른다 — **남의 소환수도 문다.**
            // 아주 멀리까지 보게 해서 「가장 가까운 적을 쫓는다」가 되게 한다.
            const Target t = NearestTarget(k.owner, k.x, k.y, 1e9);
            if (t.none()) continue;

            double tx = 0, ty = 0;
            if (t.pet >= 0) {
                tx = pets_[static_cast<size_t>(t.pet)].x;
                ty = pets_[static_cast<size_t>(t.pet)].y;
            } else {
                tx = sessions_.at(t.addr).x;
                ty = sessions_.at(t.addr).y;
            }
            const double dx = tx - k.x, dy = ty - k.y;
            const double d = std::sqrt(dx * dx + dy * dy);
            if (d < 1e-6) continue;
            if (d > kPetReach) {
                // 아직 멀다 — 다가간다.
                k.x += kPetSpeed * dx / d;
                k.y += kPetSpeed * dy / d;
                k.facing = static_cast<uint8_t>(
                    static_cast<int>(std::atan2(dy, dx) * 128.0 / 3.14159265358979) & 0xFF);
            } else if (frame_ >= k.nextHitFrame) {
                // 닿았다 — 때린다. 쉬는 시간은 사람 근접과 같은 규칙이다.
                k.nextHitFrame = frame_ + kMeleeCooldownTicks;
                HurtTarget(t, kPetDamage, out, k.owner);   // 부른 사람의 점수다
                break;   // 목록이 바뀔 수 있다 — 이번 걸음은 여기서 접는다
            }
        }
    }

    UpdateFlags(out);

    // 판 시계 — 실측 간격이 2초 남짓이었다.
    if (frame_ >= nextTimerFrame_) {
        nextTimerFrame_ = frame_ + kTimerEveryTicks;
        SendTimer(out);
    }

    // 게임에 들어와 있는 사람에게만 보낸다.
    for (auto& kv : sessions_) {
        Session& s = kv.second;
        if (!s.inGame) continue;

        // 죽어 있고 때가 됐으면 되살린다.
        if (s.respawnAtFrame != 0 && frame_ >= s.respawnAtFrame) {
            s.respawnAtFrame = 0;
            s.health = s.maxHealth;

            // 부활 — 껍데기 안. 실측 `e9 d0 02 5d b8 00 00 ff 01`.
            // ★ 4바이트는 **틱 번호**다(표본 다섯의 값차/시간차가 모두 28/초였다).
            //   끝의 `ff 01` 은 표본 전수가 같아 뜻을 모른다. 그대로 싣는다.
            {
                uint8_t msg[9];
                msg[0] = static_cast<uint8_t>(Op::PLAYER_RESPAWN);
                msg[1] = static_cast<uint8_t>(s.netCode);
                msg[2] = static_cast<uint8_t>(s.netCode >> 8);
                msg[3] = static_cast<uint8_t>(frame_);
                msg[4] = static_cast<uint8_t>(frame_ >> 8);
                msg[5] = static_cast<uint8_t>(frame_ >> 16);
                msg[6] = static_cast<uint8_t>(frame_ >> 24);
                msg[7] = 0xFF;
                msg[8] = 0x01;
                EmitSeq(s, msg, sizeof(msg), out);
            }
            // 되살아난 체력 — 실측 `dd d0 02 4b 00 4b 00` (지금/최대).
            {
                uint8_t msg[7];
                msg[0] = static_cast<uint8_t>(Op::REPORT_TOTAL_HEALTH);
                msg[1] = static_cast<uint8_t>(s.netCode);
                msg[2] = static_cast<uint8_t>(s.netCode >> 8);
                msg[3] = static_cast<uint8_t>(s.health);
                msg[4] = static_cast<uint8_t>(s.health >> 8);
                msg[5] = static_cast<uint8_t>(s.maxHealth);
                msg[6] = static_cast<uint8_t>(s.maxHealth >> 8);
                EmitSeq(s, msg, sizeof(msg), out);
            }
        }

        // ★ 재생 모드 — 내가 만든 것 대신 정품이 보냈던 조각을 순서대로 내보낸다.
        //   클라가 그려지면 「내용이 틀렸다」, 안 그려지면 「구조가 틀렸다」로 갈린다.
        // ★ 재생분이 남아 있으면 그것부터. **다 쓰면 내 생성으로 이어받는다** —
        //   「첫 스냅샷만 문제인가, 내 스트림 자체가 문제인가」를 가르는 자리다.
        if (s.preludeAt < mirror_.size()) {
            const std::vector<uint8_t>& m = mirror_[s.preludeAt++];
            if (!m.empty()) {
                // ★ 서두에 실린 프레임을 따라간다. 서두가 끝나면 그 다음 수부터 이어 센다.
                //   안 이으면 시계가 튀고, 앞으로 튄 쪽 클라는 화면이 검다(Server.h 의 대조표).
                if (m.size() >= 3 && m[0] == static_cast<uint8_t>(Op::TIMESTAMP)) {
                    s.tickFrame = static_cast<uint32_t>(m[1]) |
                                  (static_cast<uint32_t>(m[2]) << 8);
                    s.tickFrameSet = true;
                }
                Emit(s, /*reliable=*/false, /*stream=*/0, m.data(), m.size(), out);
            }
            continue;
        }

        // 서두가 없거나 거기에 프레임이 안 실렸으면 접속한 그 시점부터 센다.
        if (!s.tickFrameSet) {
            s.tickFrame = s.frame;
            s.tickFrameSet = true;
        }
        ++s.tickFrame;

        // ① 시각 — 본문 2바이트(LE16). 클라는 이 값으로 프레임을 맞춘다.
        {
            uint8_t msg[3];
            msg[0] = static_cast<uint8_t>(Op::TIMESTAMP);
            msg[1] = static_cast<uint8_t>(s.tickFrame);
            msg[2] = static_cast<uint8_t>(s.tickFrame >> 8);
            Emit(s, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);
        }

        // ② 상태 갱신 — 머리는 「보는 사람」, 항목에는 **게임에 있는 모두**를 싣는다.
        //    이것이 M 의 「서로를 본다」다. 관심영역(멀리 있는 것은 빼기)은 나중에.
        UpdateMsg m;
        m.headAlias = 0xff;                  // 머리는 전체 번호로
        // ★ 정품은 머리에 **빈 오브젝트**(번호 0·종류 0)를 넣는다.
        //   머리는 「보는 자리」일 뿐이고, 플레이어는 아래 항목으로만 싣는다.
        //   여기에 플레이어를 넣으면 항목과 겹쳐 클라가 못 알아본다(2026-09-07 대조).
        m.headNetCode = 0;
        m.headType = 0;
        m.x = s.x;
        m.y = s.y;
        m.flags = 0;
        m.headAnim = 0;

        for (auto& kv2 : sessions_) {
            Session& o = kv2.second;
            if (!o.inGame) continue;

            ObjEntry e;
            // 별칭은 **클라와 똑같은 규칙**으로 잡는다. 한 칸만 어긋나면
            // 그때부터 클라가 엉뚱한 것을 그린다(Alias.h 참조).
            FillEntry(s.aliases, o.netCode, kPlayerType, s.tickFrame, o.x, o.y, o.facing, e);
            m.objects.push_back(e);
        }

        // ★ 날아가는 것도 싣는다. 이게 없으면 **서버만 알고 화면엔 아무것도 안 날아간다.**
        //   ⚠ 종류 번호는 **이름 짐작**이다(실측 아님). 셋 다 복합인 것만 표에서 확인했다.
        for (const auto& p : shots_) {
            const uint16_t ptype = ProjectileType(p.spell);
            if (ptype == 0) continue;

            ObjEntry e;
            FillEntry(s.aliases, p.netCode, ptype, s.tickFrame,
                      static_cast<int>(p.x), static_cast<int>(p.y), p.facing, e);
            m.objects.push_back(e);
        }

        // 깃발도 싣는다. **복합이 아니라 붙는 바이트가 없다**(종류표 확인).
        //   들고 있는 것은 안 싣는다 — 든 사람에 겹쳐 보이면 어지럽다.
        for (const auto& f : flags_) {
            if (!f.carrier.empty()) continue;
            ObjEntry e;
            FillEntry(s.aliases, f.netCode, f.type, s.tickFrame,
                      static_cast<int>(f.x), static_cast<int>(f.y), 0, e,
                      /*complex=*/false);
            m.objects.push_back(e);
        }

        // 불려 나온 것도 싣는다 — 안 그러면 사람을 때리는 게 화면에 안 보인다.
        for (const auto& k : pets_) {
            ObjEntry e;
            FillEntry(s.aliases, k.netCode, k.type, s.tickFrame,
                      static_cast<int>(k.x), static_cast<int>(k.y), k.facing, e);
            m.objects.push_back(e);
        }

        uint8_t body[512];
        body[0] = static_cast<uint8_t>(Op::UPDATE_STREAM);
        const size_t n = EncodeUpdate(m, body + 1, sizeof(body) - 1);
        if (n && sendUpdates_) Emit(s, /*reliable=*/false, /*stream=*/0, body, 1 + n, out);
    }
    return ticks;
}

}  // namespace nox
