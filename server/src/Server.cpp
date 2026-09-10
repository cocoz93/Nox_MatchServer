// Server.cpp — 접속 절차 세 자리에 답한다.
//
//   0x0C SERVER_DISCOVER  →  0x0D SERVER_INFO      (녹음 재생, 토큰만 바꿔서)
//   0x0E SERVER_TRY_JOIN  →  0x14 SERVER_JOIN_OK   (지금은 무조건 받아준다)
//   0x00 SERVER_CONNECT   →  0x1F ACCEPTED + 0x01 SERVER_ACCEPT (여기서 XOR 키를 준다)
//
// ★ 마지막 자리가 **한 조각에 메시지 둘**이다. 예전에 이 10바이트를 통째로
//   "무늬"로 외웠는데, 표대로 쪼개보니 두 메시지가 붙은 것이었다.
#include "nox/Server.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "nox/Messages.h"
#include "nox/Opcode.h"
#include "nox/ThingFlags.h"
#include "nox/JoinBundle.h"

namespace nox {

// 정의는 아래 틱 앞에 있다. 접속 처리(첫 세계 스냅샷)에서 먼저 부르므로 여기서 알린다.
static bool PatchPreludePlayer(const std::vector<uint8_t>& frag, uint16_t netCode,
                               int x, int y, std::vector<uint8_t>& out,
                               bool forceHudOn = false);

// ★★ 첫 세계 스냅샷을 받아 두고, **그 머리 좌표를 스폰 자리로 삼는다.**
//   서두가 찍힌 맵의 그 자리라야 사람이 스냅샷 속 다른 오브젝트들과 같은 동네에 선다.
void Server::SetFirstSnapshot(const std::vector<uint8_t>& v) {
    firstSnapshot_ = v;
    // 조각 안을 훑어 `0xa4` 를 찾는다. 앞에 `0x27 TIMESTAMP` 같은 게 붙어 있다.
    size_t pos = 0;
    while (pos < firstSnapshot_.size()) {
        const uint8_t op = firstSnapshot_[pos];
        if (op == static_cast<uint8_t>(Op::UPDATE_STREAM)) {
            const UpdateMsg um =
                DecodeUpdate(firstSnapshot_.data() + pos + 1, firstSnapshot_.size() - pos - 1, nullptr);
            if (um.ok && um.consumed > 0) { spawnX_ = um.x; spawnY_ = um.y; }
            return;
        }
        const size_t msz = MessageSize(firstSnapshot_.data() + pos, firstSnapshot_.size() - pos,
                                       /*fromClient=*/false);
        if (msz == 0) return;            // 모르는 것 — 기본값을 쓴다
        pos += msz;
    }
}

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
void Server::EmitNewPlayer(Session& to, const Session& who, uint8_t status,
                           std::vector<OutPacket>& out) {
    // ★★ **녹음이 없으면 조립한다** (2026-09-10 감사에서 잡음).
    //   전에는 여기서 그냥 되돌아갔다. 독립 모드에는 `joinBody_` 가 비어 있어
    //   **남을 소개하는 `2d` 가 한 번도 안 나갔다** — 접속 묶음 안 자기 소개 하나뿐이었다.
    //   소개가 없으면 클라에 그 사람의 playerInfo 가 없어 이름·외모가 안 서고,
    //   상태 알림(`6a`)도 빈손이면 되돌아간다(09-09 기록).
    std::vector<uint8_t> np;
    if (joinBody_.size() >= kJoinNewPlayerAt + kJoinNewPlayerLen) {
        np.assign(joinBody_.begin() + kJoinNewPlayerAt,
                  joinBody_.begin() + kJoinNewPlayerAt + kJoinNewPlayerLen);
    } else if (!who.info97.empty()) {
        np = BuildNewPlayer(who.netCode, who.info97.data());
    } else {
        return;   // 재료가 없다 — 지어내지 않는다
    }
    PutU16(np.data() + 1, who.netCode);
    // ★★★ **이름을 사람마다 갈라 준다.** 서두가 녹음본이라 모두가 같은 "Jack" 이었다 —
    //   이름·외모가 똑같으니 클라가 **같은 사람으로 보고 하나만 그렸을** 수 있다.
    //   (2026-09-09: 자리·좌표 모드·덧붙이·간격을 다 기각하고 남은 후보가 「누구냐」였다.)
    //   이름은 3번 자리부터 UTF-16LE 다 — 실측 `2d <번호2> 4a 00 61 00 63 00 6b 00`("Jack").
    //   첫 글자만 번호에 따라 바꾼다. 길이·짜임은 그대로라 안전하다.
    if (np.size() > 4) np[3] = static_cast<uint8_t>('A' + (who.netCode % 26));
    // ★ 상태는 4바이트지만 우리가 쓰는 값은 하위 한 바이트뿐이다(0x423 안에 든다).
    if (np.size() > kNewPlayerStatusAt + 3) {
        np[kNewPlayerStatusAt] = status;
        np[kNewPlayerStatusAt + 1] = 0;
        np[kNewPlayerStatusAt + 2] = 0;
        np[kNewPlayerStatusAt + 3] = 0;
    }
    EmitSeq(to, np.data(), np.size(), out);
    ++handled_;
}

// `6a REPORT_CLIENT_STATUS <번호2> <상태4>` 한 통. 받는 쪽 한 명에게.
//
// 실측 원문: `cc 20 00 07 | 6a cb 02 00 00 00 00`
// 녹음본 조각의 `cc` 번호를 이 세션 것으로 다시 매긴다. 자세한 까닭은 Server.h 선언 자리에.
void Server::RenumberSeq(Session& s, std::vector<uint8_t>& frag) {
    size_t pos = 0;
    while (pos < frag.size()) {
        const uint8_t op = frag[pos];
        size_t msz = MessageSize(frag.data() + pos, frag.size() - pos, /*fromClient=*/false);
        // ⚠ `a4` 는 길이표에 없다(가변). 이걸 안 다루면 걸음이 **첫 갱신에서 멈춰**
        //   뒤의 `cc` 를 하나도 못 고친다 — 09-09 에 실제로 그렇게 헛돌았다.
        if (msz == 0 && op == static_cast<uint8_t>(Op::UPDATE_STREAM)) {
            const UpdateMsg um = DecodeUpdate(frag.data() + pos + 1, frag.size() - pos - 1, nullptr);
            if (um.ok && um.consumed > 0) msz = 1 + um.consumed;
        }
        if (msz == 0) break;   // 모르는 것 — 여기서 멈춘다. 뒤를 억지로 고치면 더 나쁘다
        if (op == static_cast<uint8_t>(Op::SEQ_IMPORTANT) && msz >= 4) {
            frag[pos + 1] = static_cast<uint8_t>(s.seqImpId);
            frag[pos + 2] = static_cast<uint8_t>(s.seqImpId >> 8);
            ++s.seqImpId;
        }
        pos += msz;
    }
}

void Server::EmitClientStatus(Session& to, uint16_t netCode, uint32_t status,
                              std::vector<OutPacket>& out) {
    const uint8_t st[7] = {
        static_cast<uint8_t>(Op::REPORT_CLIENT_STATUS),
        static_cast<uint8_t>(netCode),
        static_cast<uint8_t>(netCode >> 8),
        static_cast<uint8_t>(status),
        static_cast<uint8_t>(status >> 8),
        static_cast<uint8_t>(status >> 16),
        static_cast<uint8_t>(status >> 24) };
    EmitSeq(to, st, sizeof(st), out);
    ++handled_;
}

void Server::BroadcastClientStatus(uint16_t netCode, uint32_t status,
                                   std::vector<OutPacket>& out) {
    for (auto& kv : sessions_) {
        Session& o = kv.second;
        if (!o.inGame) continue;
        EmitClientStatus(o, netCode, status, out);
    }
}

void Server::Broadcast(bool viaSeq, const uint8_t* msg, size_t n,
                       std::vector<OutPacket>& out) {
    for (auto& kv : sessions_) {
        Session& o = kv.second;
        if (!o.inGame) continue;
        if (viaSeq) EmitSeq(o, msg, n, out);
        else        Emit(o, /*reliable=*/false, /*stream=*/0, msg, n, out);
    }
}

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

// 불려 나온 것의 값 — **종류마다 다르다** (2026-09-10 정본화, ⭕ monster.bin).
//
// 전에는 넷 다 상수 하나였다(체력 40 · 피해 8 · 사거리 40 · 속도 5).
// 이제 표에서 찾는다 — 임프는 체력 25 에 원거리 5, 박쥐는 체력 35 에 근접 4 다.
//
// ⚠ 닿는 거리 — 파일의 `MELEE_ATTACK_RANGE` 는 몬스터 기준이라 **사람 몸 반지름을
//   더해야** 실제로 닿는다. 안 더하면 박쥐(15)가 사람 몸(10) 안으로 못 들어와
//   영영 다가가기만 한다.
// ⚠ 속도가 사람보다 느리다(임프 3.75 · 박쥐 3.28 대 사람 달리기 7). 정본이 그렇다 —
//   도망가는 사람은 못 잡는다. 퀘스트에서 몬스터가 안 닿으면 이 줄을 먼저 볼 것.
struct PetTuning {
    int      health;
    int      damage;
    double   reach;
    double   speed;
    uint32_t delay;
};

static PetTuning TuningFor(uint16_t type) {
    const MonsterStats* m = FindMonster(type);
    if (!m) return { kPetHealth, kPetDamage, kPetReach, kPetSpeed, kMeleeCooldownTicks };
    PetTuning t;
    t.health = m->health;
    // 근접이 비어 있으면 원거리형이다. 지금 발사체 피해를 아는 것은 임프뿐.
    t.damage = (m->meleeDamage > 0) ? m->meleeDamage
             : ((m->wireType == 1328) ? kImpShotDamage : kPetDamage);
    t.reach  = (m->meleeRange > 0.0) ? m->meleeRange + kPlayerRadius : kPetReach;
    t.speed  = (m->speed > 0) ? m->speed / kThingSpeedDivisor : 0.0;   // 0 = 못 움직인다
    t.delay  = static_cast<uint32_t>((m->minDelay > 0) ? m->minDelay : kDefaultMonsterDelay);
    return t;
}

// 공격 주문과 그 위력. **2026-09-10 정본화** — 근거는 `GameBalance.h`.
//
// **어느 주문이 공격이냐**는 이름표에서 갈랐다(`SpellNames.cpp`, 기계로 뽑은 것).
//
// ★ 파이어볼 ⭕ — thing.bin 의 `COLLIDE = SparkExplosionCollide <n>` 인자를 반으로.
//   레벨은 주문을 얼마나 충전했나인데 **와이어에 안 실린다**(`0x79` 본문 21B 가
//   주문 번호와 0 채움뿐). 지금은 최대 세기로 본다 — 아레나 캐릭은 레벨 10 이다.
// ★ 번개·에너지볼트 ⭕ — 파일 값은 **프레임당** 피해다. 우리는 한 방으로 주므로
//   한 번 시전이 얼마나 이어지는지를 곱해야 한다 —
//   `ARENA TargetedSpellLifetime = 45` 프레임(⭕ gamedata.bin)을 쓴다.
constexpr uint8_t kAssumedSpellLevel = 5;      // ⚠ 와이어에 없어 최대로 가정

int Server::SpellDamage(uint8_t spell) {
    switch (spell) {
        case 27:   // SPELL_FIREBALL — 09-07 조작 실측에서 F 키
            return FireballDirectDamage(kAssumedSpellLevel);
        case 43:   // SPELL_CHAIN_LIGHTNING — D 키. 표시 이름이 "Lightning"
            return static_cast<int>(kLightningPerFrame * kTargetedSpellFrames + 0.5);
        case 24:   // SPELL_LIGHTNING — 표시 이름은 "Energy Bolt"
            return static_cast<int>(kEnergyBoltPerFrame[kAssumedSpellLevel - 1] *
                                    kTargetedSpellFrames + 0.5);
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
    // ★★★ **정정(2026-09-09) — 따라 굴리지 않고 클라가 알려 준 것만 쓴다.**
    //   전에는 `Generate`/`Assign` 으로 클라와 같은 규칙을 흉내 냈는데 어긋났다.
    //   별칭으로 보내면 아무것도 안 그려졌고, 그래서 매 틱 전체번호로 다시 소개하다가
    //   클라를 1분 만에 무너뜨렸다. 이제 `0xa5 NEW_ALIAS` 로 **받은 자리만** 쓴다.
    //   못 찾으면 전체 번호(`ff`)로 소개한다 — 그걸 받은 클라가 별칭을 정해 알려 준다.
    const int alias = tbl.FindByObject(netCode, type, frame);
    if (alias > 0) {
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
        // ★★★ **덧붙이 한 바이트는 「방향 4비트 + 애니 4비트」다** (2026-09-09 원본 대조).
        //
        //   클라(`GAME2_3.c:1580`)는 이 바이트를 이렇게 쪼갠다 —
        //     방향 = (값 >> 4) & 7      → 오브젝트+297
        //     애니 = 값 & 0x0F          → 오브젝트+276  (Flags&4 면 다음 한 바이트에서)
        //   그리고 그리는 쪽은 **그 애니가 없으면 아무것도 안 그리고 되돌아간다**
        //   (`drawable_monster.go`: `if ani.Cnt40 == 0 { return 1 }`).
        //
        //   ⚠ 여기에 **방향값(0~255)을 통째로** 싣고 있었다. 그러면 애니 번호가
        //     방향에 따라 0~15 로 아무렇게나 정해진다 — 그래서 **소환수와 몬스터가
        //     화면에 하나도 안 그려졌다**(2026-09-09 실측: 갱신에 25개인데 화면엔 사람 둘뿐).
        //
        //   애니 번호표는 둘로 갈린다(`draw_debug.go` 의 `debugStates` / `debugStates2`) —
        //     사람(클래스 0x4)  0 IDLE · 1 DIE · 2 DEAD · 3 JUMP · 4 WALK …
        //     몬스터(0x40000)   8 IDLE · 9 DIE · 10 DEAD · 11 HURT · 12 MOVE …
        //   몬스터 쪽 8=IDLE 은 `drawable_monster.go:144` 의 `if dr.AnimInd != 8 { // IDLE` 로 재확인.
        e.extraBytes[0] = static_cast<uint8_t>(
            (((facing >> 5) & 0x07) << 4) | kCreatureAnimIdle);
        // ★ 최상위 비트(0x80)는 값이 아니라 **신호**다 — 서 있으면 클라가
        //   「스프라이트 프레임이 한 바이트 더 온다」로 읽는다(같은 자리).
        //   위 식은 방향을 **위 4비트 중 3비트**에만 넣으므로 이 비트가 늘 0 이다.
        //   그래서 전에 있던 `if (facing & 0x80) extra = 2` 는 필요 없어졌다 —
        //   방향값을 통째로 싣던 시절의 뒷수습이었다.
        // ★★★ 사람 항목 — 정품 **원바이트**로 확정했다(2026-09-08):
        //
        //     [첫 소개·전체번호]  … ff <번호2> <종류2> <좌표4> │ 40 00 │ …   덧붙이 **2**
        //     [그 뒤·별칭]        … cb <dx> <dy>              │ 40    │ …   덧붙이 **1**
        //
        //   ⑴ 값은 **늘 `0x40`** 이다. 방향값이 아니다 — 정품은 서 있든 움직이든 0x40 이었다.
        //   ⑵ **첫 소개일 때만 둘째 바이트가 붙는다.** 별칭으로 보내면서 둘째 바이트를
        //      같이 실으면 클라가 그것을 다음 항목의 시작으로 읽어 그 뒤가 밀린다.
        //      전에는 사람이면 **늘 2** 로 보내 그 자리가 어긋나 있었다.
        //   ⚠ 방향을 어디에 싣는지는 아직 모른다 — 여기가 아니다.
        if (type == kPlayerType) {
            e.extraBytes[0] = 0x40;
            if (e.alias == 0xff) { e.extra = 2; e.extraBytes[1] = 0; }
            else                 { e.extra = 1; }
        }
    }
}

// 별칭 항목의 좌표를 상대(직전 항목 기준)로 바꾼다 — 못 하면 전체번호+절대로 후퇴.
//
// ★★ 클라는 `00 <별칭> <절대4>` 를 못 읽는다(2026-09-09 사람·2026-09-10 정적 실측 —
//   둘 다 화면이 검어지고 클라가 죽었다). 규약은 「별칭이면 상대 · `ff` 면 절대」 둘뿐.
//   base 는 「직전 항목의 실제 자리」 — 디코더(UpdateStream.cpp)가 절대 항목에서
//   기준을 리셋하고 상대에서 더해 가는 것과 같은 축이다.
// ⚠ 별칭 임대 연장(Alias.h)을 넣기 전에는 임대가 2초에 끝나 다들 `ff` 로 나갔기
//   때문에 이 지뢰가 안 밟혔다 — 연장 뒤로는 별칭이 유지되므로 **별칭을 쓰는 모든
//   항목**(투사체·깃발·소환수·정적)이 이 길을 지나야 한다.
static void Relativize(ObjEntry& e, uint16_t netCode, int wx, int wy,
                       int& baseX, int& baseY) {
    if (e.alias != 0xff) {
        const int dx = wx - baseX, dy = wy - baseY;
        if (dx >= -128 && dx <= 127 && dy >= -128 && dy <= 127) {
            e.absolute = false;
            e.dx = static_cast<int8_t>(dx);
            e.dy = static_cast<int8_t>(dy);
        } else {
            e.alias = 0xff;
            e.netCode = netCode;
        }
    }
    baseX = wx;
    baseY = wy;
}

// 시야 반경 안 정적 오브젝트를 **가까운 순 상한**으로 갱신에 담는다 (P4-b).
//
// ★ 상한 없이는 EncodeUpdate 가 512B 를 넘겨 **0 을 돌려주고 갱신이 통째로 무산**된다.
//   스냅샷 조립이 그렇게 죽었다(2026-09-10 pa1 실측: 스냅샷이 387B —
//   `27 | a4 | cc 꼬리` 로 **a4 본문이 텅 비어** 사람 몸까지 안 실렸고 화면이 검었다).
//   estate 스폰 주변은 반경 700 안 정적이 40개를 넘는다.
//
// ★★ **별칭이면 좌표도 상대(직전 항목 기준)라야 한다** (2026-09-10 ao3 실측).
//   `00 <별칭> <절대4>` 꼴은 클라가 못 읽는다 — 사람 항목에서 이미 밟은 지뢰
//   (「별칭으로 보냈더니 화면이 검어졌다」, 갱신 루프의 2026-09-09 기록)를 정적이
//   그대로 다시 밟아, 접속 8초에 화면이 검어지고 20초에 클라가 죽었다.
//   상대차가 ±127 을 넘는 자리만 전체번호(ff)+절대로 후퇴한다 — `ff` 는 절대라야
//   한다는 규약도 실측으로 닫혀 있다(ff+상대 기각 기록).
//   base 는 「직전 항목의 실제 자리」로 이어 굴린다 — 사람 루프의 baseX/baseY 와 같은 축.
//
// ★★★ **정적·아이템은 「반경에 새로 들 때 한 번」만 소개하고 다시는 안 싣는다**
//   (2026-09-10 ao1~ao6 여섯 런 + 정품 전수 대조로 확정).
//
//   정품 갱신 1,879건의 항목엔 정적·아이템 **별칭 항목이 0건**이다 — 항상 갱신은
//   `a4 01 <좌표> 00 00 | cb <dx dy> 40 | 끝` 으로 **사람뿐**이고, 아이템은 첫 소개
//   (스냅샷·큰 갱신)에 1번 실리고 끝이다. 갱신 항목은 「움직인 것」의 목록이라,
//   아이템을 매 틱 항목에 넣으면(별칭+상대라도, 문법이 전수 유효해도) 클라 게임
//   루프가 첫 프레임에 멎는다 — ao1~ao6 이 전부 그 죽음이었다.
//   여기서는 introAt(이 사람에게 소개한 물체)에 없는 것만 ff 로 1번 소개한다.
static void AppendAoiStatics(const std::vector<Server::WorldStatic>& statics,
                             AliasTable& tbl, std::map<uint16_t, uint32_t>& introAt,
                             uint32_t frame, int sx, int sy,
                             int& baseX, int& baseY, UpdateMsg& m) {
    struct Near { double d2; const Server::WorldStatic* w; };
    std::vector<Near> near_;
    for (const auto& w : statics) {
        if (introAt.count(w.netCode)) continue;   // 이미 소개했다 — 다시는 안 싣는다
        const double dx = w.x - sx, dy = w.y - sy;
        const double d2 = dx * dx + dy * dy;
        if (d2 > kAoiRadius * kAoiRadius) continue;
        near_.push_back({ d2, &w });
    }
    if (near_.size() > kAoiMaxPerUpdate) {
        std::partial_sort(near_.begin(), near_.begin() + kAoiMaxPerUpdate, near_.end(),
                          [](const Near& a, const Near& b) { return a.d2 < b.d2; });
        near_.resize(kAoiMaxPerUpdate);
    }
    for (const auto& nr : near_) {
        ObjEntry e;
        FillEntry(tbl, nr.w->netCode, nr.w->type, frame, nr.w->x, nr.w->y, 0, e,
                  /*complex=*/ThingIsComplex(nr.w->type));
        e.alias = 0xff;                    // 소개는 늘 전체번호+절대 — 별칭이 있어도
        e.netCode = nr.w->netCode;         // (별칭 재사용은 「움직이는 것」의 규약이다)
        e.absolute = true;
        introAt[nr.w->netCode] = frame;
        baseX = nr.w->x;
        baseY = nr.w->y;
        m.objects.push_back(e);
    }
}

// 소개해 둔 정적 중 **시야에서 벗어난 것**을 골라낸다.
//
// ★ 왜 필요한가 — 지금까지는 「반경에 들 때 한 번 소개」만 있고 나갈 때가 없었다.
//   그래서 클라는 한 번 본 것을 영영 들고 있었고, 멀리 걸어가도 안 지웠다.
//   정품은 `32 OBJECT_OUT_OF_SIGHT` 로 지운다 — 2026-09-10 대조에서 서버→클라
//   방향으로 **156건**(`_evidence/CAP_server_0906.txt`). 지어낸 메시지가 아니다.
//
// ⚠ 골라낸 것은 `introAt` 에서 지운다. 그래야 되돌아왔을 때 다시 소개된다.
static void CollectAoiExits(std::map<uint16_t, uint32_t>& introAt,
                            const std::vector<Server::WorldStatic>& statics,
                            int sx, int sy, std::vector<uint16_t>& gone) {
    for (const auto& w : statics) {
        auto it = introAt.find(w.netCode);
        if (it == introAt.end()) continue;          // 아직 소개도 안 했다
        const double dx = w.x - sx, dy = w.y - sy;
        if (dx * dx + dy * dy <= kAoiDropRadius * kAoiDropRadius) continue;
        gone.push_back(w.netCode);
        introAt.erase(it);
    }
}

// 세계에서 정적 하나를 뺀다. **지우는 길이 아예 없었다**(계획서 벽②).
//
// ⚠ `staticIntroAt` 에서도 지운다 — 안 지우면 「이미 소개했다」가 영영 남아,
//   같은 번호가 나중에 다시 놓일 때(되돌려 떨구기) 아무에게도 소개되지 않는다.
bool Server::TakeStatic(uint16_t netCode) {
    size_t at = statics_.size();
    for (size_t i = 0; i < statics_.size(); ++i)
        if (statics_[i].netCode == netCode) { at = i; break; }
    if (at == statics_.size()) return false;
    statics_.erase(statics_.begin() + static_cast<long>(at));
    for (auto& kv : sessions_) kv.second.staticIntroAt.erase(netCode);
    return true;
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
        // ★ 87 은 표에 없었다 — **컨저러 캐릭터를 만들어 실제로 눌러 보고서야 알았다.**
        //   2026-09-09: 컨저러 주문칸 A 가 `0x79` 로 87 을 보냈는데 여기가 0 을 돌려줘
        //   시전 수만 오르고 **소환수가 하나도 안 생겼다.**
        //   번호 = `dumpscan -things <thing.bin> -find Imp` 의 색인 1327 + 1.
        //   **와이어 번호는 색인 +1** 이다(genthings.go 의 실측 주석). Bat 색인 1356 → 1357 로 검산했다.
        case 87: return 1328;  // SUMMON_IMP                → Imp
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
            restartAtFrame_ = frame_ + kRoundRestartTicks;
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
                        const std::string& by, DamageKind kind) {
    if (t.none() || dmg <= 0) return;
    // ★ **닿은 피해를 여기서 센다.** `hits_` 는 「투사체가 명중한 횟수」만 세는데,
    //   판정 요약이 그것을 「맞음」이라 찍어 왔다. 그래서 근접으로 피해가 122건 오간
    //   실클라 런이 「맞음 0」으로 나와 **싸움이 안 붙은 것으로 오판**했다(09-10).
    //   갈라 세야 「안 맞았다」와 「투사체만 안 맞았다」가 구분된다.
    ++hurts_;
    if (t.pet < 0) {
        // ★ 사람이면 **입은 것으로 깎는다** — 방어합은 직업 기본 장비에서 온다
        //   (⭕ modifier.bin ARMOR_VALUE: 평상복 각 0.01 · 로브 0.20 · 나무방패 0.0).
        //   ⚠ 아직 「입은 것」을 세션이 따로 안 들고 있어 직업 기본 장비로 본다.
        //     주워 입기를 붙이면 그때 세션 값으로 바꾼다.
        auto vi = sessions_.find(t.addr);
        const double armor = (vi != sessions_.end())
                                 ? LoadoutFor(vi->second.charClass).armorSum : 0.0;
        Damage(t.addr, ApplyArmor(dmg, armor, kind), out, by);
        return;
    }

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
    // ★ **부활 직후 무적** — 살아나자마자 또 죽는 것을 막는다.
    //
    //   `kRespawnInvulnTicks`(150 = 5초)는 ◑ 오픈녹스 `playerRespawn` 재구현 값이다.
    //   전에는 **상수만 있고 아무도 안 봤다**(`GameBalance.h:372`, 참조 0건) — 그래서
    //   스폰 자리에 서 있던 사람이 되살아나는 즉시 다시 맞았다.
    //
    //   ⚠ 「없음」을 프레임 0 으로 나타내지 않는다 — 0 은 진짜 프레임이라 첫 틱에
    //     되살아난 사람이 영영 무적이 된다. 오늘 이 함정을 두 번 밟았다(`closing`·`hasAim`).
    if (s.invulnUntilSet && frame_ < s.invulnUntilFrame) {
        ++invulnBlocked_;
        return false;
    }

    const int before = s.health;
    int after = before - amount;
    if (after < 0) after = 0;
    s.health = static_cast<uint16_t>(after);

    // ① 얼마나 깎였나 — 겉에 그냥 실어 보낸다.
    //    실측 `42 d8 02 81 ff` = 번호 728 · −127. 증감은 **부호 있는 16비트**다.
    //    ★ 번호가 실려 있으니 **모두에게** 보낸다 — 그래야 남이 맞는 것이 보인다.
    {
        const int16_t delta = static_cast<int16_t>(after - before);
        const uint16_t raw = static_cast<uint16_t>(delta);
        uint8_t msg[5];
        msg[0] = static_cast<uint8_t>(Op::REPORT_HEALTH_DELTA);
        msg[1] = static_cast<uint8_t>(s.netCode);
        msg[2] = static_cast<uint8_t>(s.netCode >> 8);
        msg[3] = static_cast<uint8_t>(raw);
        msg[4] = static_cast<uint8_t>(raw >> 8);
        Broadcast(/*viaSeq=*/false, msg, sizeof(msg), out);
    }

    // ② 내 체력의 절대값 — 껍데기 안이다. 실측 `43 4a 00`.
    //    ⚠ **이것만은 본인에게만** — 번호가 안 실려서 받는 쪽이 제 것으로 읽는다.
    {
        uint8_t msg[3];
        msg[0] = static_cast<uint8_t>(Op::REPORT_PLAYER_HEALTH);
        msg[1] = static_cast<uint8_t>(s.health);
        msg[2] = static_cast<uint8_t>(s.health >> 8);
        EmitSeq(s, msg, sizeof(msg), out);
    }

    if (s.health > 0) return false;

    // ③ 죽음 — 껍데기 안. 실측 `e8 d8 02`.
    //    ★ **모두에게.** 남이 쓰러지는 것을 봐야 판이 판이 된다.
    {
        uint8_t msg[3];
        msg[0] = static_cast<uint8_t>(Op::PLAYER_DIED);
        msg[1] = static_cast<uint8_t>(s.netCode);
        msg[2] = static_cast<uint8_t>(s.netCode >> 8);
        Broadcast(/*viaSeq=*/true, msg, sizeof(msg), out);
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
                restartAtFrame_ = frame_ + kRoundRestartTicks;
                // `0x58 REPORT_DEATHMATCH_WINNER` — **짜임을 오픈녹스 원본에서 확정했다**
                //   (2026-09-09, `client__network__cdecode.c` case 88. 우리 덤프 표본은 여전히 0건).
                //
                //     58 | 승자 번호 2 | 끝난 까닭 1 | 프레임 4        모두 8바이트
                //
                //   · **승자 번호** — 0 이면 무승부("DM_Tie"). 클라는 이 번호로
                //     `playerInfoGetByID` 를 찾고 **빈손이면 글이 없이 지나간다.**
                //   · **끝난 까닭** — 1 이면 「시간 초과」를 앞에 붙인다. 0 이 보통 승리이고,
                //     0 일 때만 `sub_4947E0`(승자를 점수판에 올리는 것)이 돈다.
                //     ⚠ 전에는 여기에 **죽인 수(10)** 를 실었다. 그래서 시간초과도 아니고
                //       점수판에도 안 올라가는 어정쩡한 값이 됐다.
                //   · **프레임** — ★★★ **맵을 받은 프레임보다 커야 한다.**
                //     클라는 `if (프레임 <= 저장값) return;` 으로 **통째로 버린다.**
                //     그 저장값은 `USE_MAP` 의 프레임에서 온다(`network_client.go`).
                //     우리는 `USE_MAP` 에 `s.frame` 을 실으므로 지금 세고 있는 `s.tickFrame`
                //     을 실으면 늘 그보다 크다.
                //     ⚠ 전에는 0 을 실었다 — 저장값도 0 이라 `0 <= 0` 으로 **매번 버려졌다.**
                //       화면에 승자가 안 뜨던 것이 이것이다.
                for (auto& kv : sessions_) {
                    Session& o = kv.second;
                    if (!o.inGame) continue;
                    uint8_t msg[8];
                    msg[0] = static_cast<uint8_t>(Op::REPORT_DEATHMATCH_WINNER);
                    msg[1] = static_cast<uint8_t>(k->second.netCode);
                    msg[2] = static_cast<uint8_t>(k->second.netCode >> 8);
                    msg[3] = 0;   // 보통 승리 (1 이면 시간 초과)
                    PutU32(msg + 4, o.tickFrame);
                    EmitSeq(o, msg, sizeof(msg), out);
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
// 지금 누가 어디 있나 — 한 줄로. 하네스가 이 줄을 읽어 걸어갈 각을 잰다.
std::string Server::PositionsLine() const {
    std::string s = "[자리]";
    char buf[96];
    for (const auto& kv : sessions_) {
        const Session& o = kv.second;
        if (!o.inGame) continue;
        std::snprintf(buf, sizeof(buf), " 사람 %u=%d,%d,%u,%u",
                      (unsigned)o.netCode, o.x, o.y, (unsigned)o.facing, (unsigned)o.team);
        s += buf;
    }
    for (const auto& f : flags_) {
        // 팀 = 지금자리 · 들렸나 · 집자리. 몰이가 「가서 들고 → 집으로」 하려면 셋 다 있어야 한다.
        std::snprintf(buf, sizeof(buf), " 깃발 %u=%d,%d,%u,%d,%d", (unsigned)f.team,
                      (int)f.x, (int)f.y, (unsigned)(f.carrier.empty() ? 0 : 1),
                      (int)f.homeX, (int)f.homeY);
        s += buf;
    }
    for (const auto& k : pets_) {
        std::snprintf(buf, sizeof(buf), " 몸 %u=%d,%d",
                      (unsigned)k.netCode, (int)k.x, (int)k.y);
        s += buf;
    }
    // ★ **바닥에 놓인 물건** — 하네스가 여기까지 걸어가 줍기 조작을 시험한다.
    //   ⚠ 열두 개가 다 나오면 줄이 길어진다. **첫 사람에게 가장 가까운 셋**만 —
    //     「어디로 걸어갈까」에 필요한 것은 그것뿐이다.
    if (!statics_.empty()) {
        int px = 0, py = 0;
        bool have = false;
        for (const auto& kv : sessions_) {
            if (!kv.second.inGame) continue;
            px = kv.second.x; py = kv.second.y; have = true; break;
        }
        if (have) {
            std::vector<std::pair<double, const WorldStatic*>> near_;
            near_.reserve(statics_.size());
            for (const auto& w : statics_) {
                const double dx = w.x - px, dy = w.y - py;
                near_.push_back({ dx * dx + dy * dy, &w });
            }
            const size_t k = near_.size() < 3 ? near_.size() : 3;
            std::partial_sort(near_.begin(), near_.begin() + k, near_.end(),
                              [](const std::pair<double, const WorldStatic*>& a,
                                 const std::pair<double, const WorldStatic*>& b) {
                                  return a.first < b.first;
                              });
            for (size_t i = 0; i < k; ++i) {
                std::snprintf(buf, sizeof(buf), " 물건 %u=%d,%d,%u",
                              (unsigned)near_[i].second->netCode,
                              near_[i].second->x, near_[i].second->y,
                              (unsigned)near_[i].second->type);
                s += buf;
            }
        }
    }
    return s;
}

void Server::StartQuest(int count, int centerX, int centerY, double spread) {
    questTotal_ = count;
    questKilled_ = 0;
    roundOver_ = false;

    // 가운데를 둘러 고르게 세운다. ⚠ 자리 규칙은 실측이 아니다.
    for (int i = 0; i < count; ++i) {
        const double ang = 2.0 * 3.14159265358979 * i / (count > 0 ? count : 1);
        Creature k;
        k.owner.clear();                 // ★ 주인이 없다 — 아무 편도 아니다
        // ★★ **Bomber 를 쓰면 안 된다** (2026-09-10) — 정본 체력이 **1** 이다.
        //   자폭형이라 그렇게 적혀 있는데(⭕ monster.bin `ARENA HEALTH = 1`),
        //   퀘스트 몬스터로 쓰면 **한 대에 죽어 사냥이 안 된다**.
        //   정본화 전에는 우리가 체력 40 을 억지로 물려 가려져 있었다.
        //   ★ 어느 몬스터를 세울지는 **우리가 정할 값**이다 — 거머리(체력 60·피해 20)로
        //     둔다. 워리어 롱소드(38)로 두 대, 사람 150 을 깎는 데 여덟 대라 판이 선다.
        k.type = SummonType(86);         // GiantLeech (1358)
        k.netCode = nextShotNetCode_++;
        if (nextShotNetCode_ == 0) nextShotNetCode_ = 4096;
        k.x = centerX + spread * std::cos(ang);
        k.y = centerY + spread * std::sin(ang);
        k.health = static_cast<uint16_t>(TuningFor(k.type).health);
        pets_.push_back(k);
    }
}

// 이 사람이 지금 겨누고 있는 쪽(라디안).
//
// 겨눈 자리가 있고 아직 싱싱하면 **거기를 향해**, 아니면 몸이 바라보는 쪽으로.
// ⚠ 겨눈 자리가 제 몸과 겹치면 방향이 안 나온다 — 그때도 몸 방향으로 되돌아간다.
double Server::AimRadians(const Session& s) const {
    const bool fresh = s.hasAim && (frame_ - s.aimAtFrame <= kAimStaleTicks);
    if (fresh) {
        const double dx = s.aimX - s.x, dy = s.aimY - s.y;
        if (dx * dx + dy * dy > 1.0) return std::atan2(dy, dx);
    }
    return s.facing * 3.14159265358979 / 128.0;
}

// 한 사람이 한 말을 **모두에게.** 머리는 서버가 새로 짓는다 —
// 말한 이의 번호와 그때 좌표로. 글자만 받은 것에서 가져온다.
//
// ⚠ 클라가 준 머리를 그대로 쓰면 **남의 번호로 말하기**가 된다.
void Server::BroadcastChat(const Session& who, const uint8_t* text, size_t n,
                           std::vector<OutPacket>& out) {
    if (n == 0 || n > kChatMaxBytes) return;
    std::vector<uint8_t> msg;
    msg.reserve(11 + n);
    msg.push_back(static_cast<uint8_t>(Op::TEXT_MESSAGE));
    msg.push_back(static_cast<uint8_t>(who.netCode));
    msg.push_back(static_cast<uint8_t>(who.netCode >> 8));
    msg.push_back(0x02);                                  // 깃발 = UTF-8 (한 글자 한 바이트)
    msg.push_back(static_cast<uint8_t>(who.x));
    msg.push_back(static_cast<uint8_t>(who.x >> 8));
    msg.push_back(static_cast<uint8_t>(who.y));
    msg.push_back(static_cast<uint8_t>(who.y >> 8));
    msg.push_back(static_cast<uint8_t>(n));
    msg.push_back(0); msg.push_back(0);                   // 미상 2 — 표본 전수가 0
    msg.insert(msg.end(), text, text + n);
    for (auto& kv : sessions_) {
        if (!kv.second.inGame) continue;
        Emit(kv.second, /*reliable=*/false, /*stream=*/0, msg.data(), msg.size(), out);
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

    roundOver_ = false;   // 깃발 판을 다시 차릴 수 있게. 전에는 안 되돌려 한 번 끝나면 끝이었다
    AssignTeams();
}

// 팀을 번갈아 나눈다. ⚠ 진짜 녹스가 이렇게 나누는지는 모른다(➖ 우리 값).
//
// ★★ 전에는 여기가 **두 군데 틀렸다** (2026-09-11 감사).
//   ① 「들어온 차례로」라고 주석에 적혀 있었지만 실제로는 `sessions_` 를 그냥 돌았는데,
//      그 열쇠가 **접속 주소 문자열**이라 순서가 **사전순**이었다. 포트 번호가 어떻게
//      뽑히느냐에 따라 A·B 팀이 뒤집혔다 — 같은 시험을 두 번 돌리면 결과가 달랐다.
//   ② **두 명이 다 들어온 그 순간 한 번만** 불렸다. 셋째부터는 `team == 0` 으로 남아
//      `UpdateFlags` 의 팀 검사에 걸려 **깃발 판정에서 통째로 빠졌다.**
//
// 고침 — `joinOrder`(접속한 차례)로 정렬해 나누고, **사람이 들어올 때마다 다시 부른다.**
void Server::AssignTeams() {
    std::vector<Session*> ins;
    for (auto& kv : sessions_) {
        if (kv.second.inGame) ins.push_back(&kv.second);
    }
    std::sort(ins.begin(), ins.end(), [](const Session* a, const Session* b) {
        return a->joinOrder < b->joinOrder;
    });
    uint8_t next = 1;
    for (Session* s : ins) {
        s->team = next;
        next = (next == 1) ? 2 : 1;
    }
}

// 문과 오벨리스크를 매 틱 돌본다 (P5).
//
// ★ **문은 다가가면 열린다** — 클라가 요청을 안 보낸다.
//   근거: 정품 덤프에 문 관련 **클라→서버 메시지가 0건**이고, 서버→클라 `0xb2` 만
//   201건 흐른다. 즉 여는 판단은 서버가 한다.
//   ⚠ 여는 거리와 각 값은 ➖ 우리 값이다(`Server.h` 주석 참조).
//
// ★ **오벨리스크는 옆에 서면 마나를 채운다** — gamedata 에 Regen 키가 0개이고
//   오벨리스크 항목도 없다. 정품 온라인의 마나 회복 수단이 이것으로 보인다.
//   ⚠ 채우는 속도는 ➖ 우리 값. P4 캠페인에서 정품 시계열이 잡히면 그 값으로 고친다.
void Server::UpdateProps(std::vector<OutPacket>& out) {
    if (props_.empty()) return;
    for (auto& p : props_) {
        double best = 1e18;
        Session* nearest = nullptr;
        for (auto& kv : sessions_) {
            Session& s = kv.second;
            if (!s.inGame || s.respawnAtFrame != 0) continue;
            const double dx = p.x - s.x, dy = p.y - s.y;
            const double d2 = dx * dx + dy * dy;
            if (d2 < best) { best = d2; nearest = &s; }
        }
        if (!nearest) continue;

        if (p.kind == PropKind::Door) {
            const bool want = best <= kDoorOpenDist * kDoorOpenDist;
            if (want == p.open) continue;          // 안 바뀌면 안 보낸다
            p.open = want;
            if (want) ++doorsOpened_;
            uint8_t full[4] = {
                static_cast<uint8_t>(Op::DOOR_ANGLE),
                static_cast<uint8_t>(p.netCode),
                static_cast<uint8_t>(p.netCode >> 8),
                static_cast<uint8_t>(want ? kDoorOpen : kDoorClosed),
            };
            Broadcast(/*viaSeq=*/false, full, sizeof(full), out);
        } else {
            if (best > kObeliskDist * kObeliskDist) continue;
            if (frame_ < p.nextManaFrame) continue;
            p.nextManaFrame = frame_ + kObeliskEveryTicks;
            if (nearest->mana >= nearest->maxMana) continue;
            const int after = nearest->mana + kObeliskManaPerTick;
            nearest->mana = static_cast<uint16_t>(
                after > nearest->maxMana ? nearest->maxMana : after);
            ++obeliskMana_;
            uint8_t m[5] = {
                static_cast<uint8_t>(Op::REPORT_MANA),
                static_cast<uint8_t>(nearest->netCode),
                static_cast<uint8_t>(nearest->netCode >> 8),
                static_cast<uint8_t>(nearest->mana),
                static_cast<uint8_t>(nearest->mana >> 8),
            };
            EmitSeq(*nearest, m, sizeof(m), out);   // 홀로 — 제 마나다
            uint8_t ob[4] = {
                static_cast<uint8_t>(Op::OBELISK_CHARGE),
                static_cast<uint8_t>(p.netCode),
                static_cast<uint8_t>(p.netCode >> 8),
                0x32,                               // 표본 전수가 0x32 였다
            };
            Broadcast(/*viaSeq=*/true, ob, sizeof(ob), out);
        }
    }
}

// 깃발 판 승자 — `0x57 REPORT_FLAG_WINNER`, 총 8바이트.
//
// ⭕ **짜임을 오픈녹스 클라 원본에서 확정했다** (2026-09-11, `cdecode.c:982-1023`).
//   우리 덤프 표본은 0건인데, 그 까닭은 **정품 녹음에 CTF 판이 한 판도 없어서**다.
//
//     57 | 팀 ID 2 | 끝난 까닭 1 | 프레임 4
//
// ★ `0x58`(데스매치 승자)과 **딱 한 필드만 다르다** — `0x58` 은 +1 이 사람 번호
//   (`netClearHighBit` → `playerInfoGetByID`), `0x57` 은 +1 이 **팀 ID**(`getTeamByID`).
//   나머지 세 자리는 완전히 같다.
// · 팀이 0 이거나 없는 팀이면 클라가 무승부(`CTF_Tie`)로 띄운다.
// · 끝난 까닭 1 이면 「시간 초과」를 앞에 붙인다. 0 이 보통 승리.
// ★★ **프레임이 문지기다** — `cdecode.c:1000` 의 `if (프레임 <= 저장값) return 8;`.
//   `0x58` 에서 0 을 실어 매번 버려졌던 그 사고와 같은 자리다. 받는 사람의 `tickFrame` 을 싣는다.
void Server::SendFlagWinner(uint8_t team, std::vector<OutPacket>& out) {
    for (auto& kv : sessions_) {
        Session& o = kv.second;
        if (!o.inGame) continue;
        uint8_t msg[8];
        msg[0] = static_cast<uint8_t>(Op::REPORT_FLAG_WINNER);
        msg[1] = team;
        msg[2] = 0;
        msg[3] = 0;                       // 보통 승리 (1 이면 시간 초과)
        PutU32(msg + 4, o.tickFrame);
        EmitSeq(o, msg, sizeof(msg), out);
    }
}

// 깃발 상태 HUD — `0xd8 REPORT_FLAG_STATUS`, 총 6바이트.
//
// ⭕ **짜임 확정** (2026-09-11, `cdecode.c:2659-2664` · `gui_ctf.c:25-66` · `GAME2_1.c:4159-4177`).
//   그동안 「짜임 미상」으로 **아예 안 보냈다** — 깃발 HUD 가 안 돌던 것이 이것이다.
//
//     d8 | 상태 1 | 팀 칸 1 | 깃발 번호 1 | 든 사람 번호 2
//
// · 상태 — `0`=제자리 · `1`=누가 들었음 · `2`=밖에 나감
// · 팀 칸 — **1~16** (클라 GUI 자식 ID `8810+n`)
// · 깃발 번호 — **1 또는 2**
// · 든 사람 번호 — 안 들렸으면 0
// ⚠ **바뀔 때만 보낸다.** 매 틱 보내면 안 된다 — `0x5e` 를 매 틱 보내다가
//   정품의 270배가 되어 클라가 죽은 적이 있다(2026-09-08). 상태 지문이 달라진
//   깃발만 내보낸다.
void Server::SendFlagStatus(std::vector<OutPacket>& out) {
    for (auto& f : flags_) {
        if (f.team < 1 || f.team > 2) continue;
        uint16_t carrier = 0;
        uint8_t state = f.atHome ? 0 : 2;      // 집에 없고 안 들렸으면 「밖」
        if (!f.carrier.empty()) {
            auto it = sessions_.find(f.carrier);
            if (it != sessions_.end()) { carrier = it->second.netCode; state = 1; }
        }
        const uint32_t mark = (static_cast<uint32_t>(state) << 16) | carrier;
        if (f.sentMark == mark) continue;      // 안 바뀌었으면 안 보낸다
        f.sentMark = mark;

        uint8_t msg[6];
        msg[0] = static_cast<uint8_t>(Op::REPORT_FLAG_STATUS);
        msg[1] = state;
        msg[2] = f.team;                       // 팀 칸 (1~16 중 1·2 만 쓴다)
        msg[3] = f.team;                       // 깃발 번호 (1 또는 2)
        msg[4] = static_cast<uint8_t>(carrier);
        msg[5] = static_cast<uint8_t>(carrier >> 8);
        Broadcast(/*viaSeq=*/true, msg, sizeof(msg), out);
        ++flagStatusSent_;
    }
}

// 깃발 알림 — `0xa9 INFORM` 의 갈래 5·6·7.
// 속 짜임은 opennox `InformTeamPlayer` 그대로다: PlayerID 4 + TeamID 4.
void Server::SendFlagInform(InformCode code, uint16_t playerNetCode, uint8_t team,
                            std::vector<OutPacket>& out) {
    // ★★ **본문 길이는 갈래가 정한다** — 늘 10바이트로 보내면 안 된다(2026-09-10 감사).
    //   `Wire.h` 의 길이표: 깃발 회수(4)는 본문 **4**, 줍기·놓기·득점(5·6·7)은 **8**.
    //   회수에 8을 실으면 남는 4바이트를 클라가 **다음 메시지의 시작으로 읽어** 그 뒤가
    //   통째로 밀린다. 우리가 보내는 넷 중 회수 하나만 어긋나 있었다.
    uint8_t msg[10];
    msg[0] = static_cast<uint8_t>(Op::INFORM);
    msg[1] = static_cast<uint8_t>(code);
    const uint32_t pid = playerNetCode;
    msg[2] = static_cast<uint8_t>(pid);
    msg[3] = static_cast<uint8_t>(pid >> 8);
    msg[4] = msg[5] = 0;
    msg[6] = team;
    msg[7] = msg[8] = msg[9] = 0;
    const size_t body = (code == InformCode::FlagRetrieve) ? 4 : 8;
    const size_t n = 2 + body;
    for (auto& kv : sessions_) {
        if (kv.second.inGame) {
            Emit(kv.second, /*reliable=*/false, /*stream=*/0, msg, n, out);
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
    // ★ 깃발 HUD 는 **상태가 바뀐 것만** 나간다(`SendFlagStatus` 안에서 지문 대조).
    //   그동안 짜임을 몰라 아예 안 보냈다 — 「깃발 HUD 가 안 돈다」가 이것이었다.
    SendFlagStatus(out);
    // ★ **판이 끝났으면 더 세지 않는다.** 2026-09-09 실측 — 깃발을 둘 다
    //   닿는 자리에 두고 돌렸더니 득점이 매 틱 올라 **1378:1378** 이 됐다.
    //   한도(3)에서 `roundOver_` 는 섰는데 세는 것을 안 멈춘 탓이다.
    if (roundOver_) return;

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
                    f.atHome = false;
                    SendFlagInform(InformCode::FlagPickup, s.netCode, s.team, out);
                }
                // ★ **든 깃발은 든 사람을 따라다닌다.** 안 그러면 화면에서 깃발이
                //   집에 그대로 붙어 있어 「들었는데 왜 안 따라오나」가 된다(2026-09-10).
                if (f.carrier == kv.first) { f.x = s.x; f.y = s.y; }
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
                if (!roundOver_ && caps_[s.team] >= kCaptureLimit) {
                    roundOver_ = true;
                    restartAtFrame_ = frame_ + kRoundRestartTicks;
                    SendFlagWinner(s.team, out);
                }
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

// 판 시계 한 통을 만든다. 보내는 곳이 둘이라 따로 뺐다 —
// 주기 발송(`SendTimer`)과 **클라가 `0xd4` 로 물어봤을 때의 즉답**.
void Server::BuildTimerStatus(uint8_t msg[13]) const {
    const int32_t remain = -static_cast<int32_t>(frame_ * tickMs_);
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
    s.lastSeenFrame = frame_;   // 끊긴 세션을 걷어내는 근거. Tick 의 「자리 비우기」 참조

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
    lastFragOps_.clear();   // 진단 — 이 조각에서 무엇을 읽었는지 남긴다
    while (it.Next()) {
        ++count;
        ++seen_[it.op()];
        if (lastFragOps_.size() < 16) lastFragOps_.push_back(it.op());

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
            // ★★★ **별칭은 클라가 정해서 알려 준다.** `a5 <별칭> <번호2> <종류2> …`
            //   방향이 클라→서버뿐이다(실측: 서버 SEND 0건). 그동안 이걸 **안 받고**
            //   같은 규칙으로 따라 굴리기만 했는데, 실제로는 어긋나서 별칭으로 보내면
            //   아무것도 안 그려졌다. 받아서 그대로 쓰면 그럴 일이 없다.
            case Op::NEW_ALIAS: {
                if (it.bodyLen() < 5) break;
                const uint8_t* b = it.body();
                const uint8_t alias = b[0];
                const uint16_t nc = static_cast<uint16_t>(b[1] | (b[2] << 8));
                const uint16_t ty = static_cast<uint16_t>(b[3] | (b[4] << 8));
                s.aliases.Set(alias, nc, ty, s.tickFrame + kAliasTTL);
                ++aliasHeard_;
                break;
            }

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
                        int nx = s.x + static_cast<int>(kStepPerTick * std::cos(rad));
                        int ny = s.y + static_cast<int>(kStepPerTick * std::sin(rad));
                        if (nx < 0) nx = 0;
                        if (ny < 0) ny = 0;
                        if (nx > kMaxCoord) nx = kMaxCoord;
                        if (ny > kMaxCoord) ny = kMaxCoord;
                        // ★ 벽 충돌 (P3) — 갈 수 없으면 **축을 갈라** 미끄러진다.
                        //   대각으로 벽에 스치면 통째로 서 버리는 것보다, 갈 수 있는
                        //   축만 가는 쪽이 원작의 「벽 타기」 느낌과 맞는다.
                        //   반지름은 thing.bin NewPlayer EXTENT 를 P4 에서 잇기 전까지
                        //   근접 판정과 같은 자리표시를 쓴다.
                        if (CanStand(nx, ny, kBodyRadius)) { s.x = nx; s.y = ny; }
                        else if (CanStand(nx, s.y, kBodyRadius)) { s.x = nx; }
                        else if (CanStand(s.x, ny, kBodyRadius)) { s.y = ny; }
                        // 다 막히면 제자리 — 걸음은 세지 않는다? 아니다,
                        // 클라는 걸었다고 믿으니 서버 집계는 그대로 센다.
                        ++moves_;
                    } else if (code == 6) {
                        ++code6Seen_;   // 진단(sq1): 코드6 도달 수 — 성사 15 vs 클라 송신 231 을 가른다
                        // 왼버튼 = `CCAction`(6). 근접 공격이다.
                        // 근거는 둘 — 09-07 조작 실측과 opennox 의 CtrlCode 표.
                        //
                        // ★★★ **첫 왼클릭은 공격이 아니라 「관전 풀기」다.**
                        //   접속 직후 클라는 관전이라 HUD 가 없다. 정품은 이 클릭에
                        //   `6a REPORT_CLIENT_STATUS <내번호2> 00 00 00 00` 을 신뢰로 보낸다
                        //   (2026-09-09 정품 대조군 와이어). 그전에는 내가 이걸 안 보내
                        //   관전이 영영 안 풀렸다 — 화면 색이 관전 해제 앞뒤로 978 → 978 로
                        //   **한 톨도 안 변했다**(정품은 826 → 1220 으로 HUD 가 떴다).
                        if (s.observer) {
                            s.observer = false;
                            // ★★★ **순서가 중요하다 — 상태를 먼저 갈아 끼우고 알린다.**
                            //   오픈녹스 원본에서 `6a` 처리는 마지막에
                            //   `nox_client_onClientStatusA(playerInfo->Field3680)` 을 부르는데,
                            //   그 필드는 **`2d NEW_PLAYER` 의 112번 자리가 채운다**
                            //   (`nox_xxx_netNewPlayerMakePacket_4DDA90`: `buf[112] = Field3680 & 0x423`).
                            //   즉 `6a` 는 **저장돼 있는 값을 읽어** GUI 를 켤지 정한다.
                            //   내가 서두에서 베껴 쓰던 값이 `01`(= HUD 숨김)이라,
                            //   `6a` 를 먼저 보내면 옛 값을 읽어 그대로 숨긴 채로 끝났다.
                            EmitNewPlayer(s, s, 0, out);
                            for (auto& kv2 : sessions_) {
                                Session& o = kv2.second;
                                if (!o.inGame || o.addr == s.addr) continue;
                                EmitNewPlayer(o, s, 0, out);   // 남들에게도 「이제 논다」
                            }
                            // ★★★ **모두에게 보낸다 — 본인에게만이 아니다.**
                            //   `2d` 가 세운 1번 비트(관전)는 OR 라 안 꺼지고,
                            //   남의 몸을 안 그리게 막는 것이 바로 그 비트다
                            //   (`DrawPlayer`: 자기가 아니면 `Field3680 & 1` 이면 되돌아간다).
                            //   본인에게만 보내던 탓에 **먼저 온 쪽 화면에서만 상대가 사라졌다.**
                            BroadcastClientStatus(s.netCode, 0, out);
                        }
                        // ⚠ 쉬는 시간을 둔다. 없으면 초당 서른 대가 들어갈 수 있다.
                        //   ⚠⚠ **정정(2026-09-09)** — 여기 「버튼을 누르고 있으면 매 틱 온다」고
                        //     적혀 있었는데 실측과 어긋난다. 코드 6 은 **누르는 순간 한 번만** 온다:
                        //     180초를 누른 채 두었더니 클라가 보낸 조각 10,885 개 중 코드 6 은
                        //     **2건**(누른 순간 하나씩)뿐이고 나머지는 전부 방향(코드 1)이었다.
                        //     2026-09-08 기록의 「4초 눌러도 코드 6 단 1회」와도 맞는다.
                        //     그러니 쉬는 시간은 **연타를 막는 장치**로 남는다.
                        //   ⚠ `break` 로 빠져나가지 않는다 — 이 조각에 뒤따르는
                        //     방향(코드 1) 항목까지 같이 버려진다.
                        else if (frame_ >= s.nextMeleeFrame) {
                            s.nextMeleeFrame = frame_ + kMeleeCooldownTicks;
                            // ★★ **직업이 든 것으로 때린다** (2026-09-10 정본화).
                            //   멀티/아레나 기본 장비가 직업마다 다르다 —
                            //     워리어  롱소드 → (힘−30)×0.30 + 9
                            //     위저드·컨저러  맨손 → 힘×0.04 + 5
                            //   발차기(+5)는 **워리어만** 나오는데, 워리어는 무기를 들어
                            //   실제로는 안 쓰인다. 규칙대로 자리는 남겨 둔다.
                            //   전에는 누구나 12 로 고정이었다. 그 값이 맨손 평균(11.25)에
                            //   가까워 맞아 보였지만 **우연이었다.**
                            const bool kick = (s.charClass == CharClass::Warrior) &&
                                              (NextRandom(100) >= 100 - kKickPercent);
                            const int dmg = MeleeDamageFor(s.charClass, s.strength, kick);
                            const Target t = NearestTarget(from, s.x, s.y,
                                                           MeleeRangeFor(s.charClass));
                            HurtTarget(t, dmg, out, from);
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
            // ── 줍기 ──────────────────────────────────────────────────
            //
            // ★ 이 갈래가 **`switch` 까지 오지도 못했다.** `0x73` 의 길이가 `-1` 이라
            //   조각을 훑는 걸음이 거기서 멈추고 **조각을 통째로 버렸기 때문**이다
            //   (`MsgLen.cpp` 를 채워 풀었다). 사람은 눌렀는데 로그에도 안 남았다.
            //
            // 보내는 것 둘 —
            //   `4b <번호2><종류2>`(5B) 또는 수식 붙는 것이면 `4c <번호2><종류2><수식4>`(9B)
            //     → **집는 이에게만.** 첫 필드는 「집는 이」가 아니라 **물건 번호**다
            //       (정품 `0x4c` 표본 10건: 사람은 713 인데 실린 값이 716·717·718…).
            //   `31 <번호2>`(3B) → **보고 있던 모두에게.** 바닥에서 사라지는 것.
            case Op::TRY_GET: {
                if (!s.inGame || it.bodyLen() != 2) break;
                const uint16_t want = static_cast<uint16_t>(
                    it.body()[0] | (it.body()[1] << 8));
                ++tryGets_;

                // 세계에 있나 — 없으면 조용히 넘긴다(남이 먼저 주웠을 수 있다).
                size_t at = statics_.size();
                for (size_t i = 0; i < statics_.size(); ++i)
                    if (statics_[i].netCode == want) { at = i; break; }
                if (at == statics_.size()) { ++handled_; break; }

                const WorldStatic w = statics_[at];
                const double dx = w.x - s.x, dy = w.y - s.y;
                if (dx * dx + dy * dy > kPickupReach * kPickupReach) {
                    ++pickTooFar_;
                    ++handled_;
                    break;      // 손이 안 닿는다
                }
                if (s.inventory.size() >= kInventoryGuard) { ++handled_; break; }

                // ① 집는 이에게 — 무엇을 들었는지
                if (w.modifiable) {
                    uint8_t msg[9];
                    msg[0] = static_cast<uint8_t>(Op::REPORT_MODIFIABLE_PICKUP);
                    msg[1] = static_cast<uint8_t>(w.netCode);
                    msg[2] = static_cast<uint8_t>(w.netCode >> 8);
                    msg[3] = static_cast<uint8_t>(w.type);
                    msg[4] = static_cast<uint8_t>(w.type >> 8);
                    // 수식 네 칸 — **빈 칸은 `ff`(-1)** 다. 정품 표본이 그렇다
                    //   (`ff ff ff ff` 도 있고 `b9 ca ff ff` 처럼 둘만 붙은 것도 있다).
                    //   맵에 놓인 물건에 수식이 붙어 있는지는 아직 안 읽는다 → 전부 빈 칸.
                    msg[5] = msg[6] = msg[7] = msg[8] = 0xFF;
                    EmitSeq(s, msg, sizeof(msg), out);
                } else {
                    uint8_t msg[5];
                    msg[0] = static_cast<uint8_t>(Op::REPORT_PICKUP);
                    msg[1] = static_cast<uint8_t>(w.netCode);
                    msg[2] = static_cast<uint8_t>(w.netCode >> 8);
                    msg[3] = static_cast<uint8_t>(w.type);
                    msg[4] = static_cast<uint8_t>(w.type >> 8);
                    EmitSeq(s, msg, sizeof(msg), out);
                }

                // ② 모두에게 — 바닥에서 사라진다
                {
                    uint8_t msg[3];
                    msg[0] = static_cast<uint8_t>(Op::DESTROY_OBJECT);
                    msg[1] = static_cast<uint8_t>(w.netCode);
                    msg[2] = static_cast<uint8_t>(w.netCode >> 8);
                    Broadcast(/*viaSeq=*/false, msg, sizeof(msg), out);
                }

                // ③ 세계에서 뺀다. **지우는 길이 아예 없었다**(계획서 벽②).
                TakeStatic(w.netCode);

                Session::Carried c;
                c.netCode = w.netCode;
                c.type = w.type;
                c.modifiable = w.modifiable;
                c.useKind = w.useKind;
                c.useAmount = w.useAmount;
                s.inventory.push_back(c);
                ++picked_;
                ++handled_;
                break;
            }

            // ── 쓰기 ──────────────────────────────────────────────────
            //
            // ★ **무슨 일이 나는지가 파일에 있다** — `thing.bin` 의 `USE = PotionUse 50`.
            //   빨강·파랑 물약이 둘 다 50, 작은 물약 3, 음식 10. 지어낸 숫자가 아니다.
            // ⚠ 숫자를 회복량으로 읽어도 되는 갈래는 `PotionUse`·`ConsumeUse` 뿐이라
            //   나머지(`ReadUse` 70종·`WandUse`…)는 **아무 일도 안 일어난다.**
            //   「안 된다」를 「0 만큼 된다」로 꾸미지 않는다.
            case Op::TRY_USE: {
                if (!s.inGame || it.bodyLen() != 2) break;
                const uint16_t nc = static_cast<uint16_t>(
                    it.body()[0] | (it.body()[1] << 8));
                size_t at = s.inventory.size();
                for (size_t i = 0; i < s.inventory.size(); ++i)
                    if (s.inventory[i].netCode == nc) { at = i; break; }
                if (at == s.inventory.size()) { ++handled_; break; }

                const Session::Carried c = s.inventory[at];
                if (c.useKind == UseKind::None) { ++useUnknown_; ++handled_; break; }

                if (c.useKind == UseKind::Health) {
                    const int before = s.health;
                    int after = before + c.useAmount;
                    if (after > s.maxHealth) after = s.maxHealth;
                    s.health = static_cast<uint16_t>(after);
                    // 늘어난 만큼을 `42` 로 — **증감은 부호 있는 16비트**라 양수도 실린다.
                    //   번호가 실리므로 모두에게 보낸다(남이 회복하는 것도 보여야 한다).
                    const uint16_t raw = static_cast<uint16_t>(
                        static_cast<int16_t>(after - before));
                    uint8_t d[5] = {
                        static_cast<uint8_t>(Op::REPORT_HEALTH_DELTA),
                        static_cast<uint8_t>(s.netCode),
                        static_cast<uint8_t>(s.netCode >> 8),
                        static_cast<uint8_t>(raw),
                        static_cast<uint8_t>(raw >> 8),
                    };
                    Broadcast(/*viaSeq=*/false, d, sizeof(d), out);
                    // 절대값은 번호가 없어 **본인에게만**
                    uint8_t abs[3] = {
                        static_cast<uint8_t>(Op::REPORT_PLAYER_HEALTH),
                        static_cast<uint8_t>(s.health),
                        static_cast<uint8_t>(s.health >> 8),
                    };
                    EmitSeq(s, abs, sizeof(abs), out);
                } else {
                    int after = s.mana + c.useAmount;
                    if (after > s.maxMana) after = s.maxMana;
                    s.mana = static_cast<uint16_t>(after);
                    uint8_t m[5] = {
                        static_cast<uint8_t>(Op::REPORT_MANA),
                        static_cast<uint8_t>(s.netCode),
                        static_cast<uint8_t>(s.netCode >> 8),
                        static_cast<uint8_t>(s.mana),
                        static_cast<uint8_t>(s.mana >> 8),
                    };
                    EmitSeq(s, m, sizeof(m), out);   // 홀로
                }

                // 쓴 것은 없어진다. 세계에 되돌려 놓지 않는다.
                s.inventory.erase(s.inventory.begin() + static_cast<long>(at));
                ++used_;
                ++handled_;
                break;
            }

            // ── 입기·벗기 ─────────────────────────────────────────────
            //
            // ⭕ `60 REPORT_EQUIP`(3B, 물건 번호)은 정품 표본 **22건**으로 확인했다
            //   (`60 cc 02` — `4c` 로 준 물건 716 을 그대로 가리킨다).
            // ⚠ **남들에게 겉모습을 알리는 `0x4F`~`0x52` 는 아직 안 보낸다.**
            //   길이는 알지만(6·10) 짜임 안에 **뜻 모르는 u32 한 칸**이 있고
            //   읽을 수 있는 정품 덤프 전부에서 **표본이 0건**이다.
            //   모르는 칸을 채워 보내는 것은 이 클라에게 두 번 당한 짓이라 하지 않는다.
            //   → 실클라로 한 번 입어 보면 표본이 생긴다. 그때 켠다.
            case Op::TRY_EQUIP:
            case Op::TRY_DEQUIP: {
                if (!s.inGame || it.bodyLen() != 2) break;
                const bool wear = (static_cast<Op>(it.op()) == Op::TRY_EQUIP);
                const uint16_t nc = static_cast<uint16_t>(
                    it.body()[0] | (it.body()[1] << 8));
                for (auto& c : s.inventory) {
                    if (c.netCode != nc) continue;
                    if (c.equipped == wear) break;    // 이미 그 상태다
                    c.equipped = wear;
                    uint8_t msg[3] = {
                        static_cast<uint8_t>(wear ? Op::REPORT_EQUIP : Op::REPORT_DEQUIP),
                        static_cast<uint8_t>(nc),
                        static_cast<uint8_t>(nc >> 8),
                    };
                    EmitSeq(s, msg, sizeof(msg), out);   // 본인에게
                    if (wear) ++equipped_; else ++dequipped_;
                    break;
                }
                ++handled_;
                break;
            }

            // ── 인벤토리가 꽉 찼다고 클라가 되돌린 것 ─────────────────
            //
            // ★ **이건 클라가 보낸다.** 우리가 준 물건을 못 받겠다는 뜻이라
            //   **도로 그 자리에 떨궈야** 한다. 안 그러면 물건이 세상에서 사라진다.
            case Op::INVENTORY_FAIL: {
                if (!s.inGame || it.bodyLen() != 2) break;
                const uint16_t nc = static_cast<uint16_t>(
                    it.body()[0] | (it.body()[1] << 8));
                for (size_t i = 0; i < s.inventory.size(); ++i) {
                    if (s.inventory[i].netCode != nc) continue;
                    WorldStatic back;
                    back.netCode = nc;
                    back.type = s.inventory[i].type;
                    back.modifiable = s.inventory[i].modifiable;
                    back.useKind = s.inventory[i].useKind;
                    back.useAmount = s.inventory[i].useAmount;
                    back.x = s.x;
                    back.y = s.y;
                    statics_.push_back(back);
                    s.inventory.erase(s.inventory.begin() + static_cast<long>(i));
                    ++dropped_;
                    break;
                }
                ++handled_;
                break;
            }

            // ── 마우스 조준 ───────────────────────────────────────────
            //
            // ⭕ 짜임은 실측이다 — 본문 4바이트 = **월드 좌표 x2·y2**.
            //   정품 클라 표본이 **18,700건 넘는다**(CTF 실전 포함). 우리는 그동안
            //   **안 읽고 버렸다** — 그래서 투사체가 늘 몸이 바라보는 쪽으로만 났다.
            //
            // ⚠ 아주 자주 온다(한 판에 수천 건). 담아만 두고 계산은 쏠 때 한 번 한다.
            case Op::MOUSE: {
                if (!s.inGame || it.bodyLen() != 4) break;
                const uint8_t* b = it.body();
                s.aimX = static_cast<int16_t>(b[0] | (b[1] << 8));
                s.aimY = static_cast<int16_t>(b[2] | (b[3] << 8));
                s.aimAtFrame = frame_;
                s.hasAim = true;
                ++aims_;
                ++handled_;
                break;
            }

            // ── 채팅 ──────────────────────────────────────────────────
            //
            // ★ 이 갈래가 없어서 **사람이 친 말이 아무 데도 안 갔다.** 서버는
            //   `SendNotice` 로 보내기만 했다(퀘스트 전멸 알림 한 줄).
            //
            // ⭕ **보내는 짜임은 확정이다** — 정품 표본 26건이 전수 같은 꼴이다:
            //     `cb 02 02 20 0c cc 09 fa 00 00` + 글자
            //      번호2 · 깃발1(0x02=UTF-8) · x2 · y2 · 크기1 · 미상2
            //   x,y 는 **말하는 사람의 그때 좌표**를 따라간다(표본에서 말할 때마다 바뀐다).
            //
            // ⚠ **받는 짜임은 표본이 없다** (2026-09-10 전수). 「클라→서버 277건」으로
            //   보이던 것은 조각을 잘못 쪼갠 잡음이었다 — 277건이 전부 **같은 바이트**이고
            //   전부 조각 한가운데(자리14/30)다. `0xd8` 15,700건과 같은 지문이다.
            //   그래서 꼴은 `Wire.h` 의 규칙(opennox `msg_text.go`)에만 근거한다.
            //
            // ★★ 그래서 **되돌릴 때 클라가 준 머리를 안 쓴다.** 글자만 꺼내고
            //   번호·좌표는 **서버가 아는 값으로 새로 짓는다.** 까닭 둘 —
            //     ① 받는 짜임이 표본 없이 소스 근거뿐이라 머리를 믿을 수 없다
            //     ② 머리를 그대로 실으면 **남의 번호로 말하기**가 된다
            case Op::TEXT_MESSAGE: {
                if (!s.inGame || it.bodyLen() < 10) break;
                const uint8_t* b = it.body();
                size_t sz = b[7];
                if ((b[2] & 0x02) == 0 && (b[2] & 0x08) == 0) sz *= 2;   // UTF-16 이면 두 배
                if (sz > it.bodyLen() - 10) sz = it.bodyLen() - 10;
                if (sz == 0 || sz > kChatMaxBytes) { ++handled_; break; }
                BroadcastChat(s, b + 10, sz, out);
                ++chats_;
                ++handled_;
                break;
            }

            // ── 클라가 판 시계를 물어봤다 ────────────────────────────
            //
            // ★ 이 갈래는 **조각을 죽이던 자리**다. 길이표가 `-1`(미상)이라 조각을 훑는
            //   걸음이 여기서 멈추고 **그 조각을 통째로 버렸다** — 같이 실린 정상
            //   메시지까지 같이 죽었다. 09-10 실클라 런의 `[버림]` 228건이 전수 이것이다.
            //   본문 0 은 실측이다(정품 클라 표본 336건이 전수 「남은 1」 = 홑 `d4`).
            //
            // 답은 주기 발송과 **같은 내용을 그 사람에게만** 보낸다.
            case Op::REQUEST_TIMER_STATUS: {
                if (!s.inGame) break;
                uint8_t msg[13];
                BuildTimerStatus(msg);
                EmitSeq(s, msg, sizeof(msg), out);
                ++timerAsks_;
                ++handled_;
                break;
            }

            // ── 정상 종료 알림 ──────────────────────────────────────
            //
            // ⚠ **정품 클라가 이걸 안 보낼 수도 있다** — 코드 주석에 「정품 클라도 `0a` 도
            //   `ae` 도 없이 프로세스가 죽는다」는 실측이 남아 있다. 그래서 이건
            //   **타임아웃(30초)을 대신하는 것이 아니라 앞당기는 것**이다.
            //   실제로 오는지는 `[처음]` 진단 줄로 관측한다.
            //
            // ⚠ 여기서 세션을 지우면 **지금 잡고 있는 `s` 참조가 죽는다.**
            //   표시만 하고 걷어내기는 Tick 의 「자리 비우기」가 한다 — 나감 알림
            //   (`2e`·`31`)과 깃발 떨구기가 이미 거기 있다.
            case Op::CLIENT_CLOSE: {
                s.closing = true;
                ++handled_;
                break;
            }

            case Op::TRY_SPELL: {
                if (!s.inGame || it.bodyLen() != kTrySpellBodyLen) break;
                const uint8_t spell = it.body()[0];
                const bool onSelf = it.body()[kTrySpellBodyLen - 1] != 0;

                // ── 마나 ──────────────────────────────────────────
                //
                // ★ 여기가 없어서 **주문이 무한이었다.** 비용은 `thing.bin` SPEL 섹션
                //   첫 바이트다(정품 콘솔 `list spells` 115줄과 전수 일치).
                // ⚠ 표가 비어 있으면 비용 0 — 「공짜」가 아니라 **「표를 못 읽었다」**다.
                //   기동 로그의 「주문 마나 표 N개」를 볼 것.
                {
                    const uint8_t cost = SpellCost(spell);
                    if (cost > s.mana) {
                        ++spellsNoMana_;
                        ++handled_;
                        break;              // 못 쓴다 — 시전 자체가 안 일어난다
                    }
                    if (cost) {
                        s.mana = static_cast<uint16_t>(s.mana - cost);
                        // `45 <번호2> <지금마나2>` — **홀로 보낸다.**
                        // 정품 서버→클라 방향에 286건(`_evidence/CAP_server_0906.txt`).
                        // ⚠ 두루 보내면 안 된다: 클라는 자기 번호가 아니면 버리므로
                        //   해는 없지만 조각만 헛되이 태운다.
                        uint8_t msg[5];
                        msg[0] = static_cast<uint8_t>(Op::REPORT_MANA);
                        msg[1] = static_cast<uint8_t>(s.netCode);
                        msg[2] = static_cast<uint8_t>(s.netCode >> 8);
                        msg[3] = static_cast<uint8_t>(s.mana);
                        msg[4] = static_cast<uint8_t>(s.mana >> 8);
                        EmitSeq(s, msg, sizeof(msg), out);
                    }
                }

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
                    k.x = s.x + kPetReach * std::cos(rad);   // 세우는 자리는 그대로
                    k.y = s.y + kPetReach * std::sin(rad);
                    k.health = static_cast<uint16_t>(TuningFor(k.type).health);
                    pets_.push_back(k);
                    ++handled_;
                    break;
                }

                const int dmg = SpellDamage(spell);
                if (dmg > 0) {
                    if (onSelf) {
                        // 자기에게 건 것은 날아갈 데가 없다. 그 자리에서 받는다.
                        // ★ 방어구를 안 거치는 것이 맞다 — 주문 피해는 방어를 안 탄다.
                        Damage(from, dmg, out, from);   // 자기에게 건 것 — 점수는 안 준다
                    } else {
                        // ★ 날려 보낸다. 시전하는 순간 맞으면 **피할 수가 없다** —
                        //   비켜서 안 맞는 것이 있어야 싸움이 싸움이 된다.
                        //
                        // ★★ **겨눈 쪽으로 난다** (2026-09-10). 전에는 늘 몸이 바라보는
                        //   쪽으로만 날아갔다 — 녹스는 마우스로 겨누는 게임인데 그게 없었다.
                        //   겨눈 자리는 `0xac MOUSE` 로 온다(정품 클라 표본 18,700+, 4바이트 x,y).
                        //   ⚠ 겨눔이 **낡았으면**(마지막 `0xac` 이 오래됐으면) 몸 방향으로
                        //     되돌아간다 — 안 그러면 옛 자리를 향해 엉뚱하게 쏜다.
                        //     그 시한 `kAimStaleTicks` 는 ➖ 우리 값이다.
                        const double rad = AimRadians(s);
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
                // ★★ 그래서 **상수로 박지 않고 서두에서 읽는다**(`SetFirstSnapshot`).
                //   2026-09-08: 서두를 estate 로 바꿨는데 스폰이 bludeath 값 그대로라
                //   **사람만 세계에서 멀리 떨어져** 있었다. 위 경고가 반대로 실현된 것이다.
                // ★★★ **사람마다 자리를 흩뜨린다.** 전에는 모두 같은 자리에 세웠다 —
                //   둘이 **겹쳐 서면 화면에서 하나로 보인다.** 「서로 안 보인다」의
                //   후보로 오래 붙들고 있던 것이 실은 이것일 수 있다(2026-09-08).
                //   정품 2인 덤프에서도 둘은 (2627,3831) 과 (3174,2468) 로 멀리 떨어져 있었다.
                //   ★ 간격은 `--spawn-spread` 로 바꾼다 — 기본 40 은 근접 사거리(45) 안이라
                //     바로 싸움이 붙지만, **가려서 안 보이는 것**과 **안 그려지는 것**을
                //     가르려면 크게 벌려 봐야 한다(2026-09-09).
                if (s.x == 0 && s.y == 0) {
                    const int n = static_cast<int>(inGameCount());
                    s.x = spawnX_ + (n % 4) * spawnSpread_;
                    s.y = spawnY_ + (n / 4) * spawnSpread_;
                }
                // ★ 오브젝트 번호는 **접속마다 달라야 한다**. 전에는 모두 713 이라
                //   둘째 클라가 첫째와 같은 번호를 받아 화면이 검었다(2026-09-08).
                // ★ 다섯 칸씩 띄운다 — 본인 + 장비 넷. Server.h 의 kNetCodeStride 참조.
                if (s.netCode == 0) {
                    s.netCode = nextPlayerNetCode_;
                    nextPlayerNetCode_ += kNetCodeStride;
                }
                // ★★ **직업과 레벨을 읽어 스탯을 정한다** (2026-09-10 기획데이터 이식).
                //   전에는 누구에게나 75/150 — 녹음본 주인공(위저드) 값이 굳어 있었다.
                //   그래서 워리어가 정본 150 대신 75 로 놀았다. 자리는 `GameBalance.h`.
                if (it.bodyLen() >= kJoinPlayerInfoLen)
                    s.info97.assign(it.body(), it.body() + kJoinPlayerInfoLen);
                if (it.bodyLen() > kInfoLevelAt) {
                    const uint8_t c = it.body()[kInfoClassAt];
                    s.charClass = (c <= 2) ? static_cast<CharClass>(c) : CharClass::Warrior;
                    const uint8_t lv = it.body()[kInfoLevelAt];
                    s.charLevel = (lv > kMaxLevel) ? kMaxLevel : lv;
                }
                if (s.maxHealth == 0) {
                    // ★ **온라인은 레벨을 안 본다** — 그 직업의 최대 스탯을 그대로 준다
                    //   (`GameBalance.h` HealthOnline 주석 · 정품이 접속자 레벨도 10 으로 민다).
                    s.maxHealth = static_cast<uint16_t>(HealthOnline(s.charClass));
                    s.maxMana   = static_cast<uint16_t>(ManaOnline(s.charClass));
                    s.strength  = StrengthOnline(s.charClass);
                    s.health = s.maxHealth;
                    s.mana   = s.maxMana;
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
                } else if (standalone_) {
                    // ★ 독립 모드 — 녹음 없이 조립해 보낸다 (P5-c-2).
                    //   플레이어 정보 97B 는 방금 받은 CLIENT_ACCEPT 에서 그대로.
                    const uint8_t* info97 =
                        (it.bodyLen() >= 97) ? it.body() : nullptr;
                    std::vector<uint8_t> jb = BuildJoinBundle(s.netCode, s.frame, info97);
                    Emit(s, /*reliable=*/true, /*stream=*/0, jb.data(), jb.size(), out);
                    ++handled_;
                }
                // 여기서부터 게임에 들어온 것으로 본다 — 매 틱 상태를 받는다.
                if (!s.inGame) s.joinOrder = ++nextJoinOrder_;
                s.inGame = true;

                // ★ 깃발 판이 이미 서 있으면 **새로 온 사람에게도 팀을 준다.**
                //   전에는 두 명이 찬 그 순간 한 번만 나눠서, 셋째부터 팀 0 으로 남아
                //   깃발 판정에서 통째로 빠졌다(2026-09-11 감사).
                if (!flags_.empty()) AssignTeams();

                if (standalone_ && useMapBody_.empty()) {
                    std::vector<uint8_t> um = BuildUseMap(saMapName_, saMapCrc_, s.frame);
                    Emit(s, /*reliable=*/true, /*stream=*/0, um.data(), um.size(), out);
                    ++handled_;
                    // 스냅샷도 예약 — 발송 자리에서 조립본을 보낸다
                    s.snapshotIn = kSnapshotDelayTicks;
                }

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
                // ★★★ **첫 세계 스냅샷** — 이 한 조각에 사람을 그리는 데 필요한 것이 다 있다.
                //   정품 덤프를 나란히 놓고 재어 확정했다(2026-09-08, 487바이트 조각 하나):
                //
                //     27 a4 b2 a7 aa e4 cc(e4) cc(dd) de cc(e9) cc(4c) cc(60) cc(4c) cc(60)
                //     cc(4c) cc(60) cc(4c) cc(44) cc(60) cc(6a) ef cc(b3) 67 cc(5a) 49
                //     cc(dd) de 48 cc(43)
                //
                //   `0xe9` 살려내기 · `0x60` 장비 넷 · `0x4c` 집을 것 넷 ·
                //   `0xdd`/`0xde` 최대 체력·마나 · `0x43` 현재 체력 · `0x48` 능력치.
                //   이걸 안 보내면 **이름표만 뜨고 몸이 없고 막대가 텅 빈다** — 실측 증상 그대로다.
                //
                // ⚠ 뽑아만 두고 **아무도 안 보내고 있었다**. `SetFirstSnapshot` 은 정의만
                //   있었고 부르는 곳이 없었다 — 이 파일 머리 주석이 경고하던 그 빠진 배선이다.
                // ⚠⚠ **여기서 바로 보내면 안 된다.** 클라가 아직 맵을 안 읽었다 —
                //   같은 묶음에 붙여 보냈더니 화면이 검은 채로 남았다(2026-09-08 실측).
                //   정품처럼 몇 틱 뒤로 미룬다. 실제 발송은 `Tick` 에서.
                if (!firstSnapshot_.empty()) s.snapshotIn = kSnapshotDelayTicks;

                // ★★★ **사람 소개를 서로 주고받는다** — `2d NEW_PLAYER`.
                //   내가 보내던 것은 **접속한 본인 것 하나뿐**이었다. 정품은 다르다
                //   (2026-09-09 대조: 정품 클라 A 가 자기 `cb 02` 와 상대 `d0 02` 를
                //    둘 다 받았고, 내 서버에서 A 는 자기 `c9 02` 하나만 받았다).
                //   이 소개가 없으면 클라에 **그 사람의 playerInfo 가 아예 없다** —
                //   오픈녹스 원본을 보면 `2d` 가 이름·배운 주문(100번 자리)·장비·
                //   상태 플래그(112번 자리)를 통째로 실어 나른다
                //   (`nox_xxx_netNewPlayerMakePacket_4DDA90`). 상태 알림(`6a`)도
                //   `playerInfoGetByID` 가 빈손이면 그 자리에서 되돌아간다.
                //
                //   묶음(216바이트) 안 `2d` 는 87번 자리에서 시작해 129바이트다 —
                //   6+5+7+20+49 로 딱 떨어진다(Server.h 의 kJoin* 주석).
                //   ⚠ **여기서 바로 보내지 않는다.** 정품은 접속 답장에 붙이지 않고
                //     조금 뒤 다른 것들과 묶어 보낸다(2026-09-09 정품 덤프: 208바이트
                //     조각의 79번 자리). 접속 답장에 붙였더니 재생 대조(`replay_connect`)가
                //     「정품은 여기서 다른 것을 보냈다」로 두 자리를 잡아냈다.
                //     실제 발송은 `Tick` 에서 — 첫 스냅샷과 같은 자리다.
                s.pendingIntro = true;

                // ★★★ **먼저 와 있던 사람들에게 이 사람의 장비만 보낸다.**
                //   미니맵에는 둘이 찍히는데 몸이 안 그려진다 = 오브젝트는 있고
                //   **입힐 것이 없다**. 아침에 자기 몸이 안 그려지던 것과 같은 증상이다.
                //   ⚠ 스냅샷을 통째로/껍데기 전부 보내면 **받는 클라가 죽는다**(실측).
                //     정품 순서에서 장비에 해당하는 `cc(4c)`(집을 것) + `cc(60)`(장착)
                //     **네 쌍만** 골라 보낸다.
                //   ★ `--no-newgear` 로 끌 수 있다 — Server.h 의 SetNotifyNewGear 참조.
                if (notifyNewGear_ && !firstSnapshot_.empty()) {
                    std::vector<uint8_t> patched;
                    const std::vector<uint8_t>& src =
                        PatchPreludePlayer(firstSnapshot_, s.netCode, s.x, s.y, patched, forceHudOn_)
                            ? patched : firstSnapshot_;
                    std::vector<uint8_t> gear;
                    size_t q = 0;
                    while (q + 4 <= src.size()) {
                        if (src[q] != static_cast<uint8_t>(Op::SEQ_IMPORTANT)) { ++q; continue; }
                        const size_t inner = src[q + 3];
                        if (q + 4 + inner > src.size()) break;
                        const uint8_t iop = (inner >= 1) ? src[q + 4] : 0;
                        // ★ `4c`(집을 것)+`60`(장착)만 보내니 **몸은 그려지는데 누워 있었다.**
                        //   정품 순서에서 그 앞에 오는 `e9`(살려내기)와
                        //   `dd`/`de`(최대 체력·마나)를 같이 보내 세운다.
                        //   ⚠ 더 넣으면 나빠진다 — 하나씩 재서 확인했다(2026-09-08):
                        //     `e9`+`dd`+`de` 셋 다  → 받는 클라가 **죽는다**
                        //     `e9`(살려내기)만      → 상대가 **아예 안 보인다**(오브젝트가 사라짐)
                        //   **`4c`+`60` 둘만**이 맞다. 다만 상대가 **누워 있다** —
                        //   세우는 것은 다른 자리에서 풀 것(자세/애니 쪽으로 보인다).
                        // ★ 어떤 오프코드를 넘길지는 `--peer-ops` 로 바꿀 수 있다.
                        //   「무엇을 더 보내야 상대가 그려지나」를 다시 컴파일하지 않고
                        //   한 번에 쓸어보려고 손잡이로 뺐다(2026-09-09).
                        if (peerOps_.count(iop) != 0)
                            gear.insert(gear.end(), src.begin() + q, src.begin() + q + 4 + inner);
                        q += 4 + inner;
                    }
                    if (!gear.empty()) {
                        for (auto& kv2 : sessions_) {
                            Session& o = kv2.second;
                            if (!o.inGame || o.addr == s.addr) continue;
                            // ★ 녹음본에서 떼어 온 껍데기라 번호가 그 녹음 때 것이다.
                            //   받는 사람마다 **그 사람의 번호로 다시 매겨** 보낸다.
                            std::vector<uint8_t> g = gear;
                            RenumberSeq(o, g);
                            Emit(o, /*reliable=*/true, /*stream=*/0, g.data(), g.size(), out);
                            ++handled_;
                        }
                    }
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
// 서두(녹음본) 조각 하나에서 **사람 항목만** 이 사람 것으로 갈아 끼운다.
//
// 왜 필요한가 — 서두는 남이 접속했을 때 녹음한 것이라 그 사람의 번호(715)와
// 자리(2600,2578)가 박혀 있다. 그대로 흘리면 클라는 「내가 715, 저기에 있다」로
// 알았다가 내 갱신이 다른 번호·다른 자리를 말해 **딴 오브젝트로 본다**.
//
// ⚠ **무늬 찾기로 하지 않는다.** 뜯어서(DecodeUpdate) 고치고 다시 짠다(EncodeUpdate).
//   바이트열을 눈대중으로 찾으면 우연히 같은 자리를 건드린다.
// 돌려주는 값 = 갈아 끼운 것이 있으면 참(그때만 out 이 채워진다).
// ★★★ 서두에 실린 **남의 번호**를 이 사람 번호로 바꾼다.
//   `0xa4` 안의 사람 항목만 고치면 모자란다 — 장비·체력·부활은 `0xcc` 껍데기 안에
//   **대상 번호를 앞 2바이트로** 달고 온다. 그 번호가 남의 것이면 클라는
//   「713번 사람은 만들었는데 입힐 장비와 체력은 715번 것」이 되어 **몸을 못 그린다**.
//   실측(2026-09-08, 내 번호 713):
//       dd cb 02 4b 00 4b 00        최대 체력 → 0x02cb = 715
//       e9 cb 02 dd 07 00 00 ff 01  살려내기   → 715
//   길이를 안 바꾸는 **자리 치환**이라 뒤 스트림이 밀리지 않는다.
// 사람 하나가 차지하는 번호 칸 수 — 본인 + 장비 넷. Server.h 의 kNetCodeStride 와 같은 값이다.
static constexpr uint16_t kSlotSpan = 5;

static void PatchSeqTargets(std::vector<uint8_t>& buf, uint16_t from, uint16_t to,
                            bool forceHudOn) {
    if (from == to) return;
    size_t q = 0;
    while (q + 4 <= buf.size()) {
        if (buf[q] != static_cast<uint8_t>(Op::SEQ_IMPORTANT)) { ++q; continue; }
        const size_t inner = buf[q + 3];
        if (q + 4 + inner > buf.size()) break;
        // 껍데기 머리 4바이트 뒤가 속 메시지다. `[오프코드][대상번호 2]…` 꼴이면 바꾼다.
        //
        // ★★★ **사람 하나가 아니라 다섯 칸을 통째로 옮긴다** — 본인 + 장비 넷.
        //   정품은 사람마다 장비 번호가 다르다(715→716~719 · 720→721~724).
        //   내 서버는 사람만 옮기고 장비는 녹음본 번호(716~719)를 **둘 다 그대로** 써서
        //   두 사람이 같은 오브젝트를 가리켰다 — 미니맵엔 둘인데 몸은 하나였다(2026-09-09).
        //   ⚠ 한 칸씩 여러 번 훑으면 **먼저 바꾼 값을 다시 바꾼다**(715→713 뒤에 717→715 가
        //     또 걸린다). 그래서 한 번에 범위로 판정하고 한 번만 쓴다.
        if (inner >= 3) {
            const uint16_t id = static_cast<uint16_t>(buf[q + 5] | (buf[q + 6] << 8));
            if (id >= from && id < from + kSlotSpan) {
                const uint16_t nid = static_cast<uint16_t>(to + (id - from));
                buf[q + 5] = static_cast<uint8_t>(nid);
                buf[q + 6] = static_cast<uint8_t>(nid >> 8);
            }
        }
        // ★★★ **서두 안에 「HUD 를 숨겨라」가 들어 있다.**
        //   녹음본에 `cc 00 00 07 | 6a <번호2> 01 00 00 00` 이 그대로 있다(182번 자리).
        //   `01` 은 상태 bit0 = **GUI 안 그림**(오픈녹스 `nox_client_onClientStatusA`:
        //   `v&1` 이면 renderGUI=false). 접속하자마자 이걸 받으니 HUD 가 없는 것이 당연했다.
        //   `--hud-on` 을 주면 그 자리를 0 으로 뒤집는다.
        //   ⚠ 정품은 처음에 `01` 을 주고 놀기 시작할 때 `00` 을 준다 — 그러니 이건
        //     「정품 흉내」가 아니라 **가르는 시험**이다. 켜서 HUD 가 뜨면 이 자리가 맞는 것.
        if (forceHudOn && inner >= 7 &&
            buf[q + 4] == static_cast<uint8_t>(Op::REPORT_CLIENT_STATUS)) {
            buf[q + 7] = 0; buf[q + 8] = 0; buf[q + 9] = 0; buf[q + 10] = 0;
        }
        q += 4 + inner;
    }
}

// 껍데기(`0xcc`) **밖에** 맨몸으로 실린 메시지의 대상 번호를 옮긴다.
//
// ⚠ **아무 자리나 바꾸면 안 된다.** 두 바이트가 우연히 남의 번호와 같은 좌표일 수 있다.
//   그래서 **머리 바로 뒤 두 바이트가 오브젝트 번호인 것이 와이어로 확인된 오프코드**만
//   손댄다. 지금 목록의 근거(2026-09-09 정품·내 서버 덤프에서 눈으로 본 원바이트):
//     de  `de cb 02 96 00 96 00`      최대 마나  = <번호2> <지금2> <최대2>
//     48  `48 cb 02 4b 00 96 00 72`   능력치     = <번호2> …
//   ★ 늘리려면 **그 오프코드의 원바이트를 먼저 볼 것.** 짐작으로 넣지 말 것.
static bool BareTargetOpcode(uint8_t op) {
    return op == static_cast<uint8_t>(Op::REPORT_TOTAL_MANA) ||
           op == static_cast<uint8_t>(Op::REPORT_STATS);
}

// ★ 몇 자리를 고쳤고 **어디서 걷기가 멈췄나**를 같이 돌려준다.
//   「0 자리 고침」만 보면 「고칠 게 없었다」와 「거기까지 못 갔다」가 안 갈린다.
//   실제로 두 번을 그렇게 헛짚었다(2026-09-09).
struct BarePatchStat { int patched = 0; size_t stoppedAt = 0; size_t total = 0; };

static BarePatchStat PatchBareTargets(std::vector<uint8_t>& buf, uint16_t from, uint16_t to) {
    BarePatchStat st;
    st.total = buf.size();
    if (from == to) return st;
    // ★★ **걷지 않고 훑는다.** 처음에는 메시지를 차례로 걸어가며 고치려 했는데
    //   `0xaa MSG_IMPORTANT` 에서 길이가 어긋나 **187/543 바이트에서 섰다** —
    //   고칠 `0xde` 는 302번 자리라 거기까지 가 보지도 못했다(2026-09-09 계측).
    //   `PatchSeqTargets` 도 같은 이유로 훑기를 쓴다.
    //   ⚠ 훑기는 우연히 같은 세 바이트를 만날 수 있다. 그래서 **오프코드 + 남의 번호**가
    //     나란히 있는 자리만 손대고, 몇 자리를 고쳤는지 세어 로그로 남긴다.
    for (size_t q = 0; q + 3 <= buf.size(); ++q) {
        if (!BareTargetOpcode(buf[q])) continue;
        const uint16_t id = static_cast<uint16_t>(buf[q + 1] | (buf[q + 2] << 8));
        if (id < from || id >= from + kSlotSpan) continue;
        const uint16_t nid = static_cast<uint16_t>(to + (id - from));
        buf[q + 1] = static_cast<uint8_t>(nid);
        buf[q + 2] = static_cast<uint8_t>(nid >> 8);
        ++st.patched;
    }
    st.stoppedAt = buf.size();
    return st;
}

static bool PatchPreludePlayer(const std::vector<uint8_t>& frag, uint16_t netCode,
                               int x, int y, std::vector<uint8_t>& out,
                               bool forceHudOn) {
    out.clear();
    out.reserve(frag.size() + 16);
    bool any = false;
    uint16_t origPlayer = 0;      // 서두에 박힌 남의 번호
    size_t pos = 0;
    while (pos < frag.size()) {
        const uint8_t op = frag[pos];
        const size_t msz = MessageSize(frag.data() + pos, frag.size() - pos,
                                       /*fromClient=*/false);
        if (msz > 0) {                       // 길이를 아는 메시지 — 그대로 옮긴다
            if (pos + msz > frag.size()) break;
            out.insert(out.end(), frag.begin() + pos, frag.begin() + pos + msz);
            pos += msz;
            continue;
        }
        if (op != static_cast<uint8_t>(Op::UPDATE_STREAM)) break;   // 모르는 것 — 접는다

        UpdateMsg um = DecodeUpdate(frag.data() + pos + 1, frag.size() - pos - 1, nullptr);
        if (!um.ok || um.consumed == 0) break;
        bool hit = false;
        for (auto& e : um.objects) {
            if (e.type != kPlayerType) continue;
            if (origPlayer == 0) origPlayer = e.netCode;   // 껍데기 치환에 쓸 남의 번호
            e.alias = 0xff;                  // 전체 번호로 다시 소개한다
            e.netCode = netCode;
            e.x = x;
            e.y = y;
            // ⚠ **덧붙이는 건드리지 않는다.** 녹음본의 사람 항목은 `[c0 00]` 인데
            //   정품 지금 덤프는 `[40 00]` 이라, 정품에 맞춰 `[40 00]` 으로 강제해 봤더니
            //   **클라가 접속을 끊었다**(2026-09-08 실측: 화면이 서버 목록으로 되돌아갔다).
            //   `c0` 의 bit 7 이 「한 바이트 더」 신호인 것은 맞지만, 그 다음 `00` 이
            //   그 한 바이트를 채우고 있어 **녹음본 안에서는 짝이 맞는다.**
            //   즉 뜯어 본 「별칭=0f·번호 0」 쓰레기 항목은 원인이 아니라 **내 디코더가
            //   갈래를 넓게 연 결과**일 수 있다 — 관측대기로 남긴다.
            hit = true;
        }
        if (hit) {
            // 머리도 이 사람이 보는 자리로. 안 맞추면 화면이 남의 자리에 선다.
            um.x = x;
            um.y = y;
        }
        uint8_t buf[1024];
        buf[0] = static_cast<uint8_t>(Op::UPDATE_STREAM);
        const size_t n = EncodeUpdate(um, buf + 1, sizeof(buf) - 1);
        if (n == 0) break;                   // 다시 못 짜면 손대지 않는다
        out.insert(out.end(), buf, buf + 1 + n);
        pos += 1 + um.consumed;
        any = any || hit;
    }
    if (!any) { out.clear(); return false; }
    // ⚠ 끝까지 못 읽었으면 **남은 것은 원본 그대로** 붙인다. 통째로 포기하면
    //   갈아 끼우기가 조용히 안 먹는다(2026-09-08: 그래서 첫 시도가 헛돌았다).
    if (pos < frag.size()) out.insert(out.end(), frag.begin() + pos, frag.end());
    // ★★★ 껍데기 속 대상 번호도 함께 옮긴다 — 여기까지 해야 장비·체력이 이 사람에게 붙는다.
    PatchSeqTargets(out, origPlayer, netCode, forceHudOn);
    // ★★★ **껍데기 밖에 맨몸으로 실린 것도 옮긴다.** `PatchSeqTargets` 는 `0xcc` 속만 본다.
    //   2026-09-09 실측 — 같은 조각 안에서 `dd`(최대 체력)는 내 번호로 나가는데
    //   바로 다음 `de`(최대 마나)는 **남의 번호 715 그대로** 나가고 있었다:
    //     ★속 op=0xdd(껍데기 안) | dd c9 02 4b 00 4b 00   ← 갈아끼움
    //     ★   op=0xde(맨몸)      | cb 02 96 00 96 00      ← 남의 번호
    //   마나 막대가 안 뜨는 것과 자리가 같다.
    const BarePatchStat bst = PatchBareTargets(out, origPlayer, netCode);
    // ★ 접속마다 한 줄. 「0 자리 고침」이 「고칠 게 없었다」인지 「거기까지 못 갔다」인지
    //   멈춘 자리를 같이 봐야 갈린다. [[feedback_zero_needs_stall_count]]
    std::printf("서두 맨몸 대상 %d자리 고침 · 걷기 %zu/%zu 바이트 · 멈춘 오프코드 0x%02x\n",
                bst.patched, bst.stoppedAt, bst.total,
                bst.stoppedAt < out.size() ? out[bst.stoppedAt] : 0);
    return true;
}

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

    // ── 판 다시 열기 ────────────────────────────────────────────────
    //
    // ★ 전에는 **판이 한 번 끝나면 그 프로세스에서 다시 못 열었다.**
    //   `roundOver_` 를 되돌리는 곳이 `StartQuest` 하나뿐이었고 아레나·깃발은
    //   되돌리는 데가 아예 없었다(2026-09-11 감사). 사람 둘이 앉아 계속 놀 수가 없었다.
    //
    // ⚠ 다시 여는 시점 `kRoundRestartTicks` 는 ➖ **우리 값이다.** 정품이 몇 초 뒤에
    //   새 판을 여는지는 표본이 없다. 승자 화면을 볼 시간은 줘야 하므로 몇 초로 둔다.
    if (roundOver_ && restartAtFrame_ != 0 && frame_ >= restartAtFrame_) {
        restartAtFrame_ = 0;
        roundOver_ = false;
        // 점수를 지운다 — 셋 다.
        for (auto& kv : sessions_) {
            kv.second.kills = 0;
            kv.second.deaths = 0;
        }
        caps_[0] = caps_[1] = caps_[2] = 0;
        questKilled_ = 0;
        // 깃발은 집으로 되돌린다(들고 있던 사람 손에서도 뺀다).
        for (auto& f : flags_) {
            f.carrier.clear();
            f.x = f.homeX; f.y = f.homeY; f.atHome = true;
        }
        ++roundsPlayed_;
        std::printf("[판] 다시 엶 — %llu번째 판 (점수·깃발 초기화)\n", roundsPlayed_ + 1);
        std::fflush(stdout);
    }

    // ── 자리 비우기 — **조용해진 세션을 걷어낸다** ──
    //
    // 지우는 자리가 아예 없었다. 클라를 죽이면 세션이 그대로 남아 사람 수에 계속
    // 세어졌고, 시험할 때마다 유령이 끼어 「누가 누구를 보는가」 판정이 흐려졌다
    // (2026-09-09 — 두 클라를 붙였는데 서버가 사람 3 이라고 했다).
    // ⚠ 접속 절차 중인 세션도 지운다 — 서버 찾기만 하고 사라진 상대가 쌓인다.
    {
        std::vector<std::string> gone;
        for (const auto& kv : sessions_) {
            const Session& o = kv.second;
            // ★ 정상 종료를 알린 사람은 **아래 문지기보다 먼저** 걷어낸다.
            //   「한 번도 못 받은 세션」 검사에 걸리면 `0x0a` 를 보내고도 안 나간다 —
            //   틱이 아직 0 이면 `lastSeenFrame` 도 0 이라 늘 걸린다(시험이 잡아냈다).
            if (o.closing) { gone.push_back(kv.first); continue; }
            // 아직 한 번도 못 받은 세션(이제 막 만든 것)은 건드리지 않는다.
            if (o.lastSeenFrame == 0) continue;
            if (frame_ - o.lastSeenFrame > kSessionTimeoutTicks) gone.push_back(kv.first);
        }
        for (const std::string& addr : gone) {
            // ★ 나갔다고 **알린다.** 전에는 조용히 지우기만 해서 남은 사람들 화면에
            //   그 몸이 영영 서 있었다. 둘을 같이 보낸다 —
            //     `2e PLAYER_QUIT`(껍데기 안)  = 사람 목록·점수판에서 뺀다
            //     `31 DESTROY_OBJECT`(생)      = 세계에서 몸을 지운다
            //   ⚠ 지우기 **전에** 번호를 떠 둔다. 지운 뒤에는 읽을 자리가 없다.
            uint16_t netCode = 0;
            bool wasInGame = false;
            {
                auto g = sessions_.find(addr);
                if (g != sessions_.end()) {
                    netCode   = g->second.netCode;
                    wasInGame = g->second.inGame;
                    // 깃발을 들고 있었으면 그 자리에 떨군다 — 안 그러면 깃발이
                    // 사라진 사람 손에 남아 판이 영영 안 끝난다.
                    if (wasInGame) DropFlag(addr, out);
                }
            }
            std::printf("조용해서 내보냄: %s\n", addr.c_str());
            sessions_.erase(addr);

            if (wasInGame && netCode != 0) {
                uint8_t quit[3] = {
                    static_cast<uint8_t>(Op::PLAYER_QUIT),
                    static_cast<uint8_t>(netCode),
                    static_cast<uint8_t>(netCode >> 8),
                };
                Broadcast(/*viaSeq=*/true, quit, sizeof(quit), out);

                uint8_t bye[3] = {
                    static_cast<uint8_t>(Op::DESTROY_OBJECT),
                    static_cast<uint8_t>(netCode),
                    static_cast<uint8_t>(netCode >> 8),
                };
                Broadcast(/*viaSeq=*/false, bye, sizeof(bye), out);
            }
        }
    }

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
                // 주문 피해는 **방어를 안 탄다** — 파이어볼 직격이 종류 1,
                // 에너지볼트·연쇄번개가 전기다(GameBalance.h DamageKind 주석).
                HurtTarget(hit, dmg, out, owner, DamageKind::Ignores);
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
            const PetTuning tune = TuningFor(k.type);
            if (d > tune.reach) {
                // 아직 멀다 — 다가간다. 속도는 그 몬스터 것이다.
                if (tune.speed > 0.0) {
                    k.x += tune.speed * dx / d;
                    k.y += tune.speed * dy / d;
                }
                k.facing = static_cast<uint8_t>(
                    static_cast<int>(std::atan2(dy, dx) * 128.0 / 3.14159265358979) & 0xFF);
            } else if (frame_ >= k.nextHitFrame) {
                // 닿았다 — 때린다. 쉬는 시간도 그 몬스터 것이다(파일에 없으면 30).
                k.nextHitFrame = frame_ + tune.delay;
                HurtTarget(t, tune.damage, out, k.owner);   // 부른 사람의 점수다
                break;   // 목록이 바뀔 수 있다 — 이번 걸음은 여기서 접는다
            }
        }
    }

    UpdateFlags(out);
    UpdateProps(out);   // 문·오벨리스크 (P5)

    // 판 시계 — 실측 간격이 2초 남짓이었다.
    if (frame_ >= nextTimerFrame_) {
        nextTimerFrame_ = frame_ + kTimerEveryTicks;
        SendTimer(out);
    }

    // 게임에 들어와 있는 사람에게만 보낸다.
    for (auto& kv : sessions_) {
        Session& s = kv.second;
        if (!s.inGame) continue;

        // ★★★ 첫 세계 스냅샷 — 접속 처리에서 예약해 둔 것을 **몇 틱 뒤** 여기서 보낸다.
        //   이 한 조각에 `0xe9` 살려내기 · `0x60` 장비 넷 · `0x4c` 집을 것 넷 ·
        //   `0xdd`/`0xde` 최대 체력·마나가 다 실려 있다(정품 대조, 2026-09-08).
        //   접속과 같은 묶음으로 보내면 클라가 아직 맵을 안 읽어 그냥 버린다.
        if (s.snapshotIn >= 0 && --s.snapshotIn < 0 &&
            (!firstSnapshot_.empty() || standalone_)) {
            // 서두는 남의 세션 녹음이라 사람 항목의 번호·자리가 남의 것이다.
            std::vector<uint8_t> patched;
            if (firstSnapshot_.empty()) {
                // ★ 독립 모드 (P5-c-2) — 녹음 없이 조립한다:
                //   27 시각 + a4(사람 + 시야 안 정적) + cc 꼬리부(BuildSnapshotTail).
                patched.push_back(static_cast<uint8_t>(Op::TIMESTAMP));
                patched.push_back(static_cast<uint8_t>(s.tickFrame));
                patched.push_back(static_cast<uint8_t>(s.tickFrame >> 8));
                UpdateMsg m;
                m.headAlias = 0xff;
                m.headNetCode = 0;
                m.headType = 0;
                m.x = s.x;
                m.y = s.y;
                m.flags = 0;
                m.headAnim = 0;
                ObjEntry me;
                FillEntry(s.aliases, s.netCode, kPlayerType, s.tickFrame, s.x, s.y,
                          s.facing, me);
                m.objects.push_back(me);
                // 시야 안 정적 — **상한 필수** (AppendAoiStatics 주석: 상한 없이는
                // 스폰 주변 40개↑ 가 480B 를 넘겨 a4 가 통째로 비어 나갔다).
                int snBaseX = s.x, snBaseY = s.y;   // 직전 항목(me)의 자리
                AppendAoiStatics(statics_, s.aliases, s.staticIntroAt,
                                 s.tickFrame, s.x, s.y, snBaseX, snBaseY, m);
                uint8_t ub[480];
                ub[0] = static_cast<uint8_t>(Op::UPDATE_STREAM);
                const size_t un = EncodeUpdate(m, ub + 1, sizeof(ub) - 1);
                patched.insert(patched.end(), ub, ub + 1 + un);
                const std::vector<uint8_t> tail = BuildSnapshotTail(s.netCode, s.charClass);
                patched.insert(patched.end(), tail.begin(), tail.end());
            } else if (!(patchSnapshot_ &&
                  PatchPreludePlayer(firstSnapshot_, s.netCode, s.x, s.y, patched, forceHudOn_))) {
                patched = firstSnapshot_;
            }
            // ★★★ **번호를 다시 매긴 뒤에 보낸다** — 안 그러면 이 뒤에 보내는
            //   신뢰 메시지가 녹음본의 같은 번호에 먹혀 조용히 사라진다(Server.h RenumberSeq 참조).
            RenumberSeq(s, patched);
            const std::vector<uint8_t>& snap = patched;
            Emit(s, /*reliable=*/true, /*stream=*/0, snap.data(), snap.size(), out);
            // 스냅샷 안에 이미 전체번호(ff) 머리의 갱신이 들어 있다 — 그것이 **첫 갱신**이다.
            // 그러니 이 틱에 내 갱신을 또 보태지 않고, 다음 틱부터 별칭 `01` 로 잇는다.
            s.sentFirstUpdate = true;
            // ★★★ **혼자가 아니면 스냅샷 바로 뒤에 전체 갱신을 한 번 더 준다.**
            //   스냅샷은 **이 사람 하나**만 담은 녹음본이라, 이미 와 있던 사람이 거기 없다.
            //   그 뒤로는 별칭 `01` 짜리 작은 갱신만 나가서 **나중에 들어온 쪽은 상대를
            //   소개받을 자리가 없었다**(2026-09-09 사람 눈 판정).
            //   여기서 머리를 되돌리면 다음 틱 갱신이 전체번호로 둘을 다 싣는다.
            if (inGameCount() >= 2) s.sentFirstUpdate = false;
            // ★★★ **소개를 다시 한다 — 스냅샷이 앞서 보낸 소개를 지우기 때문이다.**
            //   이 녹음본 안에 `ea FORGET_DRAWABLES` 와 `b1`(맵 `estate` 로 새 판)이 들어 있다
            //   (2026-09-09 와이어 실측: 545바이트 조각의 210·216번 자리).
            //   접속 순서상 소개(`2d`·`6a`)는 **스냅샷보다 170ms 앞서** 나가므로 통째로 날아간다.
            //   그래서 **나중에 들어온 쪽만** 상대를 못 봤다 — 먼저 온 쪽은 스냅샷을 안 받으니 멀쩡했다.
            //   여기서 다시 세워 두면 다음 틱에 아래 소개 자리가 한 번 더 돈다.
            if (inGameCount() >= 2) s.pendingIntro = true;
            continue;
        }

        // ★★★ **사람 소개를 서로 주고받는다** — `2d NEW_PLAYER`.
        //   내가 보내던 것은 **접속한 본인 것 하나뿐**이었다. 정품은 다르다
        //   (2026-09-09 대조: 정품 클라 A 가 자기 `cb 02` 와 상대 `d0 02` 를 둘 다 받았고,
        //    내 서버에서 A 는 자기 `c9 02` 하나만 받았다).
        //   이 소개가 없으면 클라에 **그 사람의 playerInfo 가 아예 없다** — 오픈녹스
        //   원본을 보면 `2d` 가 이름·배운 주문(100번 자리)·장비·상태 플래그(112번 자리)를
        //   통째로 실어 나르고(`nox_xxx_netNewPlayerMakePacket_4DDA90`),
        //   상태 알림(`6a`)도 `playerInfoGetByID` 가 빈손이면 그 자리에서 되돌아간다.
        //
        //   묶음(216바이트) 안 `2d` 는 87번 자리에서 시작해 129바이트다 — 6+5+7+20+49.
        // ⚠ **여기에 `&& s.snapshotIn < 0` 을 붙였다가 되돌렸다**(2026-09-09).
        //   「나중에 들어온 쪽도 전체 갱신을 받게」 하려던 것인데, 사람 눈으로 재 보니
        //   **먼저 있던 쪽까지 상대를 못 보게 됐다.** 새로 들어온 쪽 몫은 아래
        //   스냅샷 발송 자리에서 따로 챙긴다.
        if (s.pendingIntro) {
            s.pendingIntro = false;
            // ★★★ **사람이 늘면 다음 갱신의 머리를 다시 `ff`(전체)로 되돌린다.**
            //   2026-09-09 사람 눈 판정 — 정품 서버에서는 먼저 들어온 쪽에 상대가
            //   **그려지는데** 내 서버는 양쪽 다 안 그려졌다. 정품 덤프에서 상대 사람이
            //   실려 오는 갱신은 늘 **머리 별칭 `ff`**(번호 0·종류 0)에 세계 오브젝트까지
            //   함께 담긴 큰 갱신이었다(항목 12~15개). 내 것은 접속 첫 틱에 한 번만 `ff` 를
            //   쓰고 그 뒤로는 `01` 짜리 작은 갱신만 보냈다 —
            //   즉 **새로 들어온 사람을 「전체 갱신」에 실어 소개한 적이 없다.**
            for (auto& kv3 : sessions_) {
                if (kv3.second.inGame) kv3.second.sentFirstUpdate = false;
            }
            for (auto& kv2 : sessions_) {
                Session& o = kv2.second;
                if (!o.inGame || o.addr == s.addr) continue;
                // 상태는 **그 사람이 지금 어떤지** 그대로 — 관전 중이면 1, 놀고 있으면 0.
                EmitNewPlayer(o, s, s.observer ? 1 : 0, out);   // 먼저 온 사람에게 새 사람을
                EmitNewPlayer(s, o, o.observer ? 1 : 0, out);   // 새 사람에게 먼저 온 사람을

                // ★★★ **소개 바로 뒤에 상태를 한 번 더 갈아 끼운다.**
                //   `2d` 의 상태 자리는 클라에서 `|=` 로 들어가 **한 번 선 비트가 안 꺼진다.**
                //   그래서 「이 사람은 관전이 아니다」를 `2d` 로는 말할 수 없다 — `6a` 뿐이다.
                //   ⚠ 서로 **상대 것만** 보낸다. 자기 번호로 오면 클라가 HUD 를 다시 켜고 끈다.
                EmitClientStatus(o, s.netCode, s.observer ? 1u : 0u, out);
                EmitClientStatus(s, o.netCode, o.observer ? 1u : 0u, out);

                // ★★★ **상대 몫의 첫 스냅샷을 통째로 보낸다** (`--peer-snapshot`).
                //   2026-09-09 — 자리·좌표 모드·덧붙이·간격·장비·번호·별칭·이름을 다 기각하고
                //   남은 가설 = **스프라이트는 첫 스냅샷에서만 만들어지고 갱신만으로는 안 만들어진다.**
                //   내 몸은 스냅샷에 있어 그려지고, 상대는 갱신에만 있어 미니맵에만 찍힌다.
                //   ⚠ 09-08 에 「스냅샷을 통째로 보내면 받는 클라가 죽는다」고 적었지만
                //     그건 **오픈녹스 클라** 이야기다 — 정품 클라는 `--peer-ops` 를 넓게 줘도 멀쩡했다.
                if (peerSnapshot_ && !firstSnapshot_.empty()) {
                    std::vector<uint8_t> pa, pb;
                    if (!PatchPreludePlayer(firstSnapshot_, s.netCode, s.x, s.y, pa, forceHudOn_)) pa = firstSnapshot_;
                    RenumberSeq(o, pa);   // 받는 쪽은 o 다 — 번호도 o 의 것이라야 한다
                    Emit(o, /*reliable=*/true, /*stream=*/0, pa.data(), pa.size(), out);
                    if (!PatchPreludePlayer(firstSnapshot_, o.netCode, o.x, o.y, pb, forceHudOn_)) pb = firstSnapshot_;
                    RenumberSeq(s, pb);
                    Emit(s, /*reliable=*/true, /*stream=*/0, pb.data(), pb.size(), out);
                    ++handled_;
                }
            }
        }

        // 죽어 있고 때가 됐으면 되살린다.
        if (s.respawnAtFrame != 0 && frame_ >= s.respawnAtFrame) {
            s.respawnAtFrame = 0;
            // ★ 되살아난 뒤 얼마간은 안 맞는다. 상수는 있었는데 아무도 안 봤다.
            s.invulnUntilFrame = frame_ + kRespawnInvulnTicks;
            s.invulnUntilSet = true;
            s.health = s.maxHealth;
            // 마나도 되돌린다. ➖ **우리 값이다** — 정품이 부활에 마나를 채우는지는
            // 표본이 없어 모른다. 0 으로 되살리면 위저드·컨저러가 아무것도 못 하므로
            // 채우는 쪽을 골랐다. 알게 되면 여기부터 고칠 것.
            if (s.mana != s.maxMana) {
                s.mana = s.maxMana;
                uint8_t mp[5];
                mp[0] = static_cast<uint8_t>(Op::REPORT_MANA);
                mp[1] = static_cast<uint8_t>(s.netCode);
                mp[2] = static_cast<uint8_t>(s.netCode >> 8);
                mp[3] = static_cast<uint8_t>(s.mana);
                mp[4] = static_cast<uint8_t>(s.mana >> 8);
                EmitSeq(s, mp, sizeof(mp), out);   // 홀로 — 남에게 갈 값이 아니다
            }

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
                // ★ **모두에게.** 남이 되살아나는 것도 보여야 한다.
                //   ⚠ 4바이트에 싣는 것은 서버 전역 `frame_` 그대로다 — 받는 쪽마다
                //     다시 매기지 않는다. `0x57`(승자)처럼 프레임 문지기가 있는지는
                //     확인된 바 없고, `frame_` 은 늘 커지므로 0 을 실어 버려지던
                //     그 사고와는 모양이 다르다. 안 보이면 여기부터 의심할 것.
                Broadcast(/*viaSeq=*/true, msg, sizeof(msg), out);
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
                Broadcast(/*viaSeq=*/true, msg, sizeof(msg), out);
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
                // ★★ 서두는 **녹음본이라 남의 번호·남의 자리**를 담고 있다.
                //   그 안의 사람 항목을 이 사람 것으로 갈아 끼운다 — 안 그러면 클라가
                //   서두에서 「나는 715, 저기 (2600,2578)」로 알았다가 내 갱신이
                //   다른 번호·다른 자리를 말해 **딴 오브젝트로 본다**(2026-09-08 대조).
                std::vector<uint8_t> patched;
                const std::vector<uint8_t>& send =
                    PatchPreludePlayer(m, s.netCode, s.x, s.y, patched, forceHudOn_) ? patched : m;
                Emit(s, /*reliable=*/false, /*stream=*/0, send.data(), send.size(), out);
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

        // ★★ **첫 세계 스냅샷이 아직 안 나갔으면 갱신을 참는다.** 정품은 USE_MAP 뒤로
        //   시각(`27`)만 몇 번 보내다가 스냅샷을 첫 `a4` 로 내보낸다. 내가 먼저
        //   내 갱신을 흘리면 클라는 **세계를 모르는 채 사람 하나만 받는다**(2026-09-08 대조:
        //   정품 첫 갱신은 항목 14개, 내 첫 갱신은 1개였다).
        if (s.snapshotIn >= 0) continue;

        // ② 상태 갱신 — 머리는 「보는 사람」, 항목에는 **게임에 있는 모두**를 싣는다.
        //    이것이 M 의 「서로를 본다」다. 관심영역(멀리 있는 것은 빼기)은 나중에.
        UpdateMsg m;
        // ★★ 정품은 **첫 갱신만** 머리를 전체번호(ff·번호 0·종류 0)로 보내고,
        //   그 뒤로는 **별칭 `01`** 한 바이트로 보낸다(2026-09-08 대조):
        //     정품  [갱신1] 머리 별칭=ff  [갱신2~] 머리 별칭=01
        //     내 것 계속 ff + 번호 0 + 종류 0
        //   ⚠ **`01` 이 무엇의 별칭인지는 모른다.** 플레이어 별칭은 `cb` 였으니
        //     자기 자신은 아니다. 고정값으로 보고 그대로 맞춘다.
        if (s.sentFirstUpdate) {
            m.headAlias = 0x01;
        } else {
            m.headAlias = 0xff;              // 첫 갱신은 전체 번호로
            m.headNetCode = 0;
            m.headType = 0;
            s.sentFirstUpdate = true;
        }
        m.x = s.x;
        m.y = s.y;
        m.flags = 0;
        m.headAnim = 0;

        lastLoopSeen_ = 0;
        lastSessions_ = sessions_.size();
        // ★★ **보는 사람을 첫 항목으로 놓는다.** 두 가지가 여기에 달려 있다:
        //   ⑴ 정품 2인 갱신이 「자기 먼저(상대좌표) · 남 나중(절대좌표)」 순서다
        //   ⑵ 아래 상대좌표 최적화가 **첫 항목에만** 걸린다(기준이 머리 좌표라서)
        //   `sessions_` 는 주소로 묶인 map 이라 순서가 회차마다 뒤바뀐다 —
        //   맡겨 두면 남이 첫째가 되어 자기 항목이 절대좌표로 나갔다(2026-09-09 실측).
        //   ★ `--peer-first` 로 순서를 뒤집는다 — **가르는 시험용**이다.
        //     자기는 그려지고 남은 안 그려지는데, 둘의 차이가 이제 「목록에서의 자리」와
        //     그에 딸린 좌표 모드(첫째=상대·나머지=절대)뿐이다. 뒤집었을 때
        //     **남이 그려지고 자기가 안 그려지면** 원인은 자리·좌표 모드이고,
        //     그대로면 원인은 「누구냐」쪽이다(2026-09-09).
        std::vector<Session*> order;
        order.reserve(sessions_.size());
        if (peerFirst_) {
            for (auto& kv2 : sessions_) if (kv2.second.inGame && &kv2.second != &s) order.push_back(&kv2.second);
            for (auto& kv2 : sessions_) if (kv2.second.inGame && &kv2.second == &s) order.push_back(&kv2.second);
        } else {
            for (auto& kv2 : sessions_) if (kv2.second.inGame && &kv2.second == &s) order.push_back(&kv2.second);
            for (auto& kv2 : sessions_) if (kv2.second.inGame && &kv2.second != &s) order.push_back(&kv2.second);
        }
        // 상대좌표의 기준. 첫 항목은 머리 좌표에서 출발하고, 그 뒤로는 앞 항목 자리를 잇는다.
        int baseX = m.x, baseY = m.y;
        for (Session* op : order) {
            Session& o = *op;
            ++lastLoopSeen_;

            ObjEntry e;
            // 별칭은 **클라와 똑같은 규칙**으로 잡는다. 한 칸만 어긋나면
            // 그때부터 클라가 엉뚱한 것을 그린다(Alias.h 참조).
            // ★ `--peer-type N` — **남을 사람 아닌 종류로 보내 본다**(가르는 시험).
            //   자기는 그려지고 남은 안 그려지는데, 자리·좌표·덧붙이·이름·스냅샷을 다 기각했다.
            //   남을 박쥐 같은 다른 종류로 보내서 **뭐라도 그려지면** 클라는 남의 오브젝트를
            //   그릴 수 있는 것이고 문제는 「사람 종류」에 있다. 아무것도 안 그려지면
            //   애초에 남 항목으로는 그릴 것을 안 만드는 것이다(2026-09-09).
            const uint16_t useType =
                (&o != &s && peerType_ != 0) ? peerType_ : kPlayerType;
            FillEntry(s.aliases, o.netCode, useType, s.tickFrame, o.x, o.y, o.facing, e);
            // ★★ **별칭으로 보낼 때는 좌표도 상대로.** 정품 원바이트가 그렇다(2026-09-08):
            //     정품  … 01 <머리좌표4> 00 00 │ cb 00 00 │ 40 │ 끝     (14바이트)
            //     내 것 … 01 <머리좌표4> 00 00 │ 00 c9 <절대좌표4> │ 40 │ 끝  (17바이트)
            //   절대 모드는 앞에 `00` 표시가 더 붙는다. 정품은 그 경로를 안 쓴다.
            //   ⚠ 기준은 **머리 좌표**다 — 이 항목이 목록의 첫째일 때만 맞는다.
            //     사람이 첫 항목이라 지금은 맞지만, 앞에 뭘 더 넣으면 같이 손봐야 한다.
            //   ★★★ **정정(2026-09-09) — 기준은 머리가 아니라 「바로 앞 항목」이다.**
            //     뜯는 쪽(`UpdateStream.cpp`)이 `x += e.dx; y += e.dy;` 로 **이어서 더한다.**
            //     첫 항목만 머리 좌표에서 출발할 뿐, 둘째부터는 앞 항목 자리가 기준이다.
            //     그래서 **둘째 항목도 상대좌표를 쓸 수 있다** — 이걸 몰라서 남 항목을
            //     늘 절대좌표로 보냈고, 그 탓에 남을 별칭으로 보낼 길이 막혀 있었다.
            //   ❌ **`ff` 에도 상대좌표를 쓰는 것은 기각됐다**(2026-09-09 실측).
            //     정품 자기 항목이 `ff <번호> <종류> 01 01 40 00` 이라 되는 줄 알았는데,
            //     그렇게 보내니 **화면이 검어지고 두 클라가 다 이탈했다**
            //     (보낸 것 30·100건 만에 조용해짐). `ff` 는 절대좌표라야 한다.
            if (e.alias != 0xff) {
                const int dx = o.x - baseX, dy = o.y - baseY;
                if (dx >= -128 && dx <= 127 && dy >= -128 && dy <= 127) {
                    e.absolute = false;
                    e.dx = static_cast<int8_t>(dx);
                    e.dy = static_cast<int8_t>(dy);
                }
            }
            // 다음 항목의 기준은 이 항목의 자리다(절대로 보냈든 상대로 보냈든 같다).
            baseX = o.x;
            baseY = o.y;
            // ★★★ **사람이 둘 이상이면 정품은 짜임을 통째로 바꾼다** (2026-09-08 실측):
            //
            //   a4 01 <머리4> 00 00 │ ff d0 02 c9 02 │ 01 01 │ 40 00 │
            //                         00 ff cb 02 c9 02 │ 66 0c a4 09 │ 00 │ 00 00 00
            //     보는 사람 : `ff`+번호+종류 + **상대**좌표 + `40 00`(2)
            //     남        : `00`(절대) + `ff`+번호+종류 + **절대**좌표 + `00`(1)
            //
            //   ⚠ **하나씩 떼어 재 봤더니 셋 다 효과가 없었다** — 별칭만·덧붙이만·자리만.
            //     셋이 한 벌인 것으로 보고 통째로 맞춘다.
            //   ⚠ 좌표 모드는 위에서 이미 정해졌다. 별칭만 `ff` 로 올리고 **모드는 건드리지 않는다** —
            //     정품의 보는 사람 항목이 `ff` 인데도 좌표가 상대(`01 01`)이기 때문이다.
            if (inGameCount() >= 2) {
                // ★★★ **소개 형태를 자기와 남이 다르게 간다.** 2026-09-09 에 둘 다 재 봤다:
                //
                //   자기 — 별칭으로. 매 틱 `ff` 로 다시 소개하니 클라가 보낸 `a5 NEW_ALIAS` 가
                //          **갱신 하나당 0.94개**(4,585 / 4,875)였다. 정품은 0.03개다.
                //          별칭으로 돌리니 36개로 떨어졌다. 첫 항목이라 좌표도 상대로 가고,
                //          그 꼴(`cb <dx> <dy> 40`)이 정품 1인 갱신과 같다.
                //   남   — **전체번호(`ff`)로.** 별칭으로 보냈더니 **둘째 클라 화면이 검어졌다**
                //          (색 4종, 34건 보내고 조용해짐). 남 항목은 절대좌표라
                //          `00 <별칭> <좌표4>` 꼴이 되는데 클라가 그걸 못 읽는 것으로 보인다.
                //          정품 표본도 남은 `00 ff <번호> <종류> <좌표4> 30` 이다.
                //   `--intro-every-tick` 은 옛 동작(자기도 매 틱 `ff`)을 되살린다(대조용).
                //   ❌ **자기를 별칭으로 보내는 것은 기각됐다**(2026-09-09). 별칭 churn 은
                //      4,585 → 2,302 으로 줄었지만 **스트림이 어긋났다** — 2인 갱신이
                //      「항목 3개」로 뜯혔다: `ce 00 00` 에서 항목이 끝나고 덧붙이 `40` 이
                //      다음 항목의 별칭으로 읽혔다(정품 2인 표본은 자기도 `ff` 다).
                //      churn 이 큰 진짜 까닭은 따로 있다 — 정품은 2인 갱신을 거의 안 보낸다
                //      (3,488 중 **2건**). 둘이 서로 시야 밖이라 각자 자기만 실린다.
                //      즉 이건 **관심영역(AOI)을 안 만든 탓**이지 소개 형태 탓이 아니다.
                //   ★★★ **정정(2026-09-09 저녁) — 매 틱 `ff` 는 클라를 무너뜨린다.**
                //     사람 눈 판정: 둘이 움직이다 보면 **1분 안에 둘 다 끊긴다.**
                //     끊기기 직전 와이어는 멀쩡했다(갱신 30바이트·좌표 정상). 대신 지표가 튄다 —
                //     `a5 NEW_ALIAS` 3,073 대 갱신 2,129 로 **갱신보다 별칭이 많다**(1.44개/갱신).
                //     정품은 0.03이다. 초당 서른 번 오브젝트를 새로 등록시키고 있었던 것.
                //     그래서 **이미 소개한 사람은 별칭으로 보낸다** — 남 항목도 마찬가지다.
                //     남을 별칭으로 못 보내던 까닭은 좌표를 늘 절대로 보냈기 때문인데,
                //     기준이 「앞 항목」이라는 것을 알고 나니 상대좌표로 보낼 수 있다(위 참조).
                if (introEveryTick_ || e.alias == 0xff) {
                    e.alias = 0xff;
                    e.netCode = o.netCode;
                }
                if (&o != &s) {
                    // ⚠ 남 항목을 **절대좌표로 강제하던 것을 뺐다**(2026-09-09).
                    //   그건 정품 표본을 베낀 것인데 그 표본의 상대는 화면 밖이었다.
                    //   기준이 「앞 항목」인 것을 알고 나서 상대좌표가 가능해졌으므로
                    //   위에서 정해진 모드를 그대로 둔다 — 자기 항목과 같은 길이 된다.
                    e.extra = (e.alias == 0xff) ? 2 : 1;
                    if (e.extra == 2) e.extraBytes[1] = 0x00;
                    // ★★★ **`00` 이 아니라 `0x30` 이다.** 2026-09-09 에 정품 2인 갱신과
                    //   원바이트를 나란히 놓으니 30바이트 중 **이 한 자리만** 달랐다:
                    //     내 것 … 00 ff ce 02 c9 02 <좌표4> **00** │ 00 00
                    //     정품  … 00 ff cb 02 c9 02 <좌표4> **30** │ 00 00
                    //   자기 항목이 늘 `40` 인 것과 같은 자리다(자세·프레임 쪽으로 보인다).
                    //   ⚠ 앞서 「덧붙이만 바꿔 봤다」는 기록이 있지만, 그때는 번호가 겹치고
                    //     HUD 도 꺼져 있어 다른 것이 가리고 있었다.
                    //   ⚠⚠ **`30` 은 「안 그려지는 상태」의 바이트일 수 있다.** 그 값을 뜬
                    //     정품 표본에서 두 사람은 1,400 떨어져 **서로 화면 밖**이었다.
                    //     자기 항목은 `40`(+`00`)인데 그건 그려진다 — `--peer-pose 40` 으로
                    //     남에게도 같은 값을 줘 가른다(2026-09-09).
                    e.extraBytes[0] = peerPose_;
                    if (e.extra == 2) e.extraBytes[1] = 0x00;
                } else {
                    // ★★★ **별칭을 `ff` 로 올렸으면 덧붙이도 첫 소개 규약(2바이트)으로
                    //   같이 올려야 한다.** `FillEntry` 는 「`ff` 면 `40 00`, 별칭이면 `40`」
                    //   으로 정해 놓는데, 여기서 별칭만 `ff` 로 덮고 덧붙이는 1 로 둔 채
                    //   내보내고 있었다. 클라는 `40` 다음 바이트를 **다음 항목의 시작**으로
                    //   읽어 스트림이 통째로 어긋난다.
                    //   지문 = 둘째가 들어온 지 100ms 남짓에 **먼저 있던 클라가 그냥 사라진다**
                    //   (`0x0a CLIENT_CLOSE` 도 `0xae` 도 없이 프로세스가 죽는다).
                    //   2026-09-09 실측 — `--no-update` 로 갱신만 끄면 둘 다 끝까지 살았다.
                    //   ★ 별칭으로 보낼 때는 한 바이트(`40`)다 — 정품 1인 갱신이 그 꼴이다.
                    e.extra = (e.alias == 0xff) ? 2 : 1;
                    e.extraBytes[0] = 0x40;
                    e.extraBytes[1] = 0x00;
                }
            }
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
            // ★ 별칭이면 상대로 (Relativize 주석 — 임대 연장 뒤로는 여기도 별칭이 온다)
            Relativize(e, p.netCode, static_cast<int>(p.x), static_cast<int>(p.y),
                       baseX, baseY);
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
            Relativize(e, f.netCode, static_cast<int>(f.x), static_cast<int>(f.y),
                       baseX, baseY);
            m.objects.push_back(e);
        }

        // 불려 나온 것도 싣는다 — 안 그러면 사람을 때리는 게 화면에 안 보인다.
        for (const auto& k : pets_) {
            ObjEntry e;
            FillEntry(s.aliases, k.netCode, k.type, s.tickFrame,
                      static_cast<int>(k.x), static_cast<int>(k.y), k.facing, e);
            Relativize(e, k.netCode, static_cast<int>(k.x), static_cast<int>(k.y),
                       baseX, baseY);
            m.objects.push_back(e);
        }

        // ── 맵 정적 오브젝트 — **시야 반경 안만, 가까운 순 상한** (독립 서버 P4-b) ──
        //
        // 담는 규칙과 상한의 이유는 AppendAoiStatics 주석에. 드나듦은 별칭 임대(2초)가
        // 알아서 정리하고, 같은 자리에 서 있으면 부분집합이 안 바뀌어 별칭도 안정된다.
        // ⚠ 기본 꺼짐(aoiOn_) — 실클라 검증(P4-c) 전에는 켜지 않는다.
        std::vector<uint16_t> outOfSight;
        if (aoiOn_) {
            AppendAoiStatics(statics_, s.aliases, s.staticIntroAt,
                             s.tickFrame, s.x, s.y, baseX, baseY, m);
            // 멀어진 것은 갱신 **항목이 아니라** 별도 메시지로 알린다 —
            // 갱신에 실을 수 있는 것은 「움직인 것」뿐이라는 규약(P4 8런의 결론) 때문이다.
            CollectAoiExits(s.staticIntroAt, statics_, s.x, s.y, outOfSight);
        }

        uint8_t body[512];
        body[0] = static_cast<uint8_t>(Op::UPDATE_STREAM);
        size_t n = EncodeUpdate(m, body + 1, sizeof(body) - 1);
        // ⚠ **이 대입이 없어서 「갱신에 0」이 나왔다**(2026-09-08). `git checkout` 으로
        //   이분법을 하다가 잃었고, 초기값 0 을 관측으로 읽어 한참을 헤맸다.
        //   ★ 계측을 고칠 때는 **그 계측이 실제로 갱신되는지부터** 볼 것.
        lastUpdateObjects_ = m.objects.size();
        // ★★ 갱신 **바로 뒤에** 사람마다 높이(Z)를 붙인다 — `5e | 번호 2 | z 1`.
        //   정품은 한 조각 안에서 `27 TIMESTAMP` → `a4 UPDATE_STREAM` → `5e` 순으로 보낸다
        //   (2026-09-08 실측: `… a4 cb 42 0a f6 0e 30 00 00 00 00 | 5e cb 02 00`).
        //   녹스는 아이소메트릭이라 그릴 자리를 정하려면 높이가 있어야 하는 것으로 보인다.
        //   ⚠ **z 값은 실측이 아니다** — 정품 표본이 전부 0 이라 0 으로 둔다.
        //   ⚠⚠ **효과는 확인되지 않았다.** 이걸 넣어도 클라 화면에 캐릭터가 안 뜬다
        //      (2026-09-08 AH14 캡처). 깨지지도 않고 정품이 보내는 것이라 남겨 둘 뿐이다.
        //      「이것 때문에 그려진다」고 읽지 말 것.
        // ★★★ **매 틱 보내지 않는다.** 정품은 접속당 **4번**뿐인데 나는 1,081번 보냈다.
        //   그 사이 클라가 죽었다 — 정품 서버 3/3 생존, 내 서버 3/3 사망(2026-09-08 실측).
        //   ⚠ 「깨지지도 않으니 남겨 둔다」고 적어 둔 것이 **270배 과잉**이었다.
        //     정품과 다른 것을 볼 때 **덜 보내는 것만 보고 더 보내는 것을 안 봤다.**
        //   지금은 꺼 둔다. 필요하면 접속 직후 몇 번만 보내는 쪽으로 되살릴 것.
        if (false && n) {
            for (const auto& kv2 : sessions_) {
                const Session& o = kv2.second;
                if (!o.inGame) continue;
                if (1 + n + 4 > sizeof(body)) break;
                uint8_t* p = body + 1 + n;
                p[0] = static_cast<uint8_t>(Op::REPORT_Z_PLUS);
                p[1] = static_cast<uint8_t>(o.netCode);
                p[2] = static_cast<uint8_t>(o.netCode >> 8);
                p[3] = 0;
                n += 4;
            }
        }
        // ★ **시야에서 빠진 것** — `32 <번호2>`. 갱신 뒤에 이어 붙인다.
        //   정품도 한 조각 안에 여러 메시지를 이어 담으므로 꼴이 같다.
        //   ⚠ `n` 이 0(갱신이 통째로 안 나갔다)이면 붙이지 않는다 —
        //     머리 없는 조각을 만들면 클라가 그 뒤를 통째로 오해한다.
        for (uint16_t nc : outOfSight) {
            if (!n) break;
            if (1 + n + 3 > sizeof(body)) break;
            uint8_t* p = body + 1 + n;
            p[0] = static_cast<uint8_t>(Op::OBJECT_OUT_OF_SIGHT);
            p[1] = static_cast<uint8_t>(nc);
            p[2] = static_cast<uint8_t>(nc >> 8);
            n += 3;
        }
        // ★★★ **지연 보고 `0xd7`** — 갱신 조각 끝에 `d7 <번호2> <지연2>` 로 붙는다.
        //   정품 실측(2026-09-08): 본문 `cb 02 00 00`, 갱신 1,287건 대 217건이니
        //   **약 6틱에 한 번**이다. 내가 이걸 **0건** 보내고 있었다.
        //   클라 화면에 「network timeout ... removed from the game」이 반복해서 뜨는데,
        //   이 신호가 없어 클라가 상대를 죽은 것으로 보는 것일 수 있다 → 관측대기에서 시험으로.
        //   ⚠ `0x5e` 처럼 **매 틱 보내면 클라가 죽는다.** 정품 비율을 지킬 것.
        if (n && (frame_ % 6) == 0) {
            for (const auto& kv2 : sessions_) {
                const Session& o = kv2.second;
                if (!o.inGame) continue;
                if (1 + n + 5 > sizeof(body)) break;
                uint8_t* p = body + 1 + n;
                p[0] = static_cast<uint8_t>(Op::REPORT_ALL_LATENCY);
                p[1] = static_cast<uint8_t>(o.netCode);
                p[2] = static_cast<uint8_t>(o.netCode >> 8);
                p[3] = 0;      // 지연 — 정품 표본이 전부 0 이라 0 으로 둔다
                p[4] = 0;
                n += 5;
            }
        }
        if (n && sendUpdates_) Emit(s, /*reliable=*/false, /*stream=*/0, body, 1 + n, out);
    }
    return ticks;
}

}  // namespace nox
