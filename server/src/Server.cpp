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
    if (s.importantImmediate) {   // 시험용 — Session::importantImmediate 주석
        // 껍데기가 신뢰를 맡으므로 패킷 자체는 비신뢰다 (Server.h 의 EmitSeq 주석 참조).
        Emit(s, /*reliable=*/false, /*stream=*/0, buf.data(), buf.size(), out);
        return;
    }
    // ★ 바로 안 보낸다 — 대기열에 쌓고 FlushImportant 가 준비 뒤에 싣고, 확인이 올 때까지 다시 싣는다.
    //   (Session::importantQ 주석 — 한 번만 보내던 것이 접속 직후 순번 구멍을 만들어 뒤 cc 가 전부 막혔다)
    if (s.importantQ.size() >= kImportantQueueMax) {
        // 정답지는 넘치면 그 사람을 끊는다(`nox_xxx_importantCheckRate_4E52B0` → playerKickDueToRate).
        // 여기서는 끊지 않고 알리기만 한다 — 끊기 경로는 아직 옮기지 않았다.
        std::printf("[중요] %s 대기열 %zu 개로 넘침 — 새 순번 %u 버림\n", s.addr.c_str(),
                    s.importantQ.size(), static_cast<unsigned>(s.seqImpId - 1));
        return;
    }
    Session::PendingImportant p;
    p.wire = std::move(buf);
    s.importantQ.push_back(std::move(p));
}

// 중요 대기열에 **봉투 없이** 싣는다 — 정답지 `nox_xxx_netSendPacket0_4E5420`(GAME3_3.c:1762, 마지막 인자 0).
//   순번이 없으니 다시 실으면 클라가 **또 처리한다**(정답지도 같다). 사라짐·그늘처럼 두 번 받아도 같은 것만 쓴다.
void Server::EmitImportantRaw(Session& s, const uint8_t* msg, size_t n,
                              std::vector<OutPacket>& out) {
    if (n == 0 || n > 0xFF) return;
    if (s.importantImmediate) {   // 시험용 — Session::importantImmediate 주석
        Emit(s, /*reliable=*/false, /*stream=*/0, msg, n, out);
        return;
    }
    if (s.importantQ.size() >= kImportantQueueMax) {
        std::printf("[중요] %s 대기열 %zu 개로 넘침 — 맨 메시지 0x%02x 버림\n", s.addr.c_str(),
                    s.importantQ.size(), msg[0]);
        return;
    }
    Session::PendingImportant p;
    p.wire.assign(msg, msg + n);
    s.importantQ.push_back(std::move(p));
}

// 중요 메시지 한 조각 — 정답지 `nox_xxx_netImportant_4E5770`(GAME3_3.c:1938)를 옮겼다.
//   ① 준비 전(CLIENT_READY 전)이면 아무것도 안 싣는다(1966줄 `v3[3680] & 0x10`).
//   ② 처음 싣는 것은 다 싣는다 · 이미 실은 것은 기다림(+132)이 끝났을 때 **틱당 1개까지**만 다시 싣는다
//      (2001줄 `v14 >= byte(1565124+12*pl)` — 그 값은 처음 1, `nox_xxx_playerResetImportantCtr_4E4F40`).
//   ③ 기다림 = FPS × 2 / 전송률(1) = 60틱(2039줄 · 1565125 자리 초깃값 2).
//   ④ 머리 `aa` 는 조각마다 한 번(2011~2022줄 — 호스트가 아닌 사람에게는 `aa` 1바이트).
//   ⑤ 확인은 조각의 시각으로 온다 — 클라가 같은 조각의 `27` 로 localFrame 을 잡아 `ab <그 프레임>` 을 돌려준다
//      (`network_client.go:302~318`). 그래서 이 조각 머리에 `27` 을 **따로 붙인다.**
void Server::FlushImportant(Session& s, std::vector<OutPacket>& out) {
    if (!s.importantReady || s.importantQ.empty()) return;
    std::vector<uint8_t> pkt;
    pkt.reserve(kImportantPacketBudget + 8);
    pkt.push_back(static_cast<uint8_t>(Op::TIMESTAMP));
    pkt.push_back(static_cast<uint8_t>(s.tickFrame));
    pkt.push_back(static_cast<uint8_t>(s.tickFrame >> 8));
    pkt.push_back(static_cast<uint8_t>(Op::IMPORTANT));
    const size_t headLen = pkt.size();
    int resentThisTick = 0;
    for (auto& p : s.importantQ) {
        if (p.sent) {
            if (p.wait > 0) { --p.wait; continue; }
            if (resentThisTick >= 1) continue;
        }
        if (pkt.size() + p.wire.size() > kImportantPacketBudget) break;   // 다음 틱에
        if (p.sent) { ++resentThisTick; ++s.importantResent; }
        pkt.insert(pkt.end(), p.wire.begin(), p.wire.end());
        p.sent = true;
        p.sentFrame = s.tickFrame;
        p.wait = kImportantResendTicks;
    }
    if (pkt.size() == headLen) return;
    Emit(s, /*reliable=*/false, /*stream=*/0, pkt.data(), pkt.size(), out);
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
// ⚠ 사람 종류 번호는 **두 군데**에 적혀 있다 — 여기(`kPlayerType`)와 뜯는 쪽
//   (`ThingFlags.cpp` 의 `ThingIsPlayer`). 어긋나면 짜는 길이와 뜯는 길이가 갈려
//   **사람 뒤 항목이 전부 밀린다.** 그래서 여기서 못 박는다.
static_assert(kPlayerType == 713, "ThingIsPlayer() 의 713 과 같이 고칠 것");

namespace {
// nox_xxx_xferIndexedDirection_509E20 — 방향(0~255) → 행(x+3y+4). blob_587000 정수 방향표 192088 · 문턱 6(230092).
const uint8_t kDirRow[256] = {
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
};
// 갱신 항목 방향 4비트 — 정답지 짜는 쪽 그대로(`GAME4_1.c:4302~4309` 소환수 · `:4471~4478` 사람):
//   xferIndexedDirection 행(x+3y+4, 가운데 4 는 없음) → 4 이하면 그대로 · 넘으면 −1 → 0~7 을 위 4비트에.
//   클라는 4 이상이면 +1 해서 9칸 그림표 자리로 되돌린다(GAME2_3.c:1677~1679).
inline uint8_t WireDirNibble(uint8_t facing) {
    const uint8_t row = kDirRow[facing];
    return static_cast<uint8_t>(row <= 3 ? row : row - 1);
}
// nox_server_testTwoPointsAndDirection_4E6E50 의 표 — blob_587000 202504, [행][구역]. 맨손은 &1 을 본다.
const uint8_t kDirSector[9][16] = {
    { 0, 9, 6, 0, 5, 1, 4, 0, 10, 8, 2, 0, 0, 0, 0, 0 },
    { 0, 1, 2, 0, 4, 5, 6, 0, 8, 9, 10, 0, 0, 0, 0, 0 },
    { 0, 5, 10, 0, 6, 4, 2, 0, 9, 1, 8, 0, 0, 0, 0, 0 },
    { 0, 8, 4, 0, 1, 9, 5, 0, 2, 10, 6, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 4, 8, 0, 2, 6, 10, 0, 1, 5, 9, 0, 0, 0, 0, 0 },
    { 0, 10, 5, 0, 9, 8, 1, 0, 6, 2, 4, 0, 0, 0, 0, 0 },
    { 0, 2, 1, 0, 8, 10, 9, 0, 4, 6, 5, 0, 0, 0, 0, 0 },
    { 0, 6, 9, 0, 10, 2, 8, 0, 5, 4, 1, 0, 0, 0, 0, 0 },
};
// sub_4E6CE0 — 두 점의 상대 구역(GAME3_3.c:2424)
int RegionCode(float fx, float fy, float tx, float ty) {
    const float dx = tx - fx, dy = ty - fy;
    const float a = dx * 0.41304299f - dy;
    const double b = static_cast<double>(dx) * 2.4210529 - dy;
    const float c = dx * -2.4210529f - dy;
    const float d = dx * -0.41304299f - dy;
    const int code = (a < 0.0f ? 0 : 8) | (b < 0.0 ? 0 : 4) | (c < 0.0f ? 0 : 2) | (d >= 0.0f ? 1 : 0);
    switch (code) {
        case 0: return 2;
        case 2: return 6;
        case 3: return 4;
        case 4: return 10;
        case 0xB: return 5;
        case 0xC: return 8;
        case 0xD: return 9;
        case 0xF: return 1;
        default: return 0;
    }
}
uint8_t TestTwoPointsAndDirection(float fx, float fy, uint8_t dir, float tx, float ty) {
    return kDirSector[kDirRow[dir]][RegionCode(fx, fy, tx, ty)];
}
}  // namespace

// ★ `playerAnim` — 사람 항목의 **자세 번호**(0 IDLE · 4 WALK). 사람이 아니면 안 쓴다.
//   ⭕ 번호표는 소스 확증(`draw_debug.go:294`). 걸을 때 4 를 보내야 다리가 움직인다.
static void FillEntry(AliasTable& tbl, uint16_t netCode, uint16_t type,
                      uint32_t frame, int x, int y, uint8_t facing, ObjEntry& e,
                      bool complex = true, uint8_t playerAnim = 0,
                      int forceDir = -1, int playerFrame = -1) {
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
            (WireDirNibble(facing) << 4) | kCreatureAnimIdle);
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
            // ★★★ **정정(2026-09-11) — 사람 항목은 별칭이든 아니든 늘 두 바이트다.**
            //
            //   ⭕ 근거 — 클라 디코더 `GAME2_3.c:1685` —
            //       if (*(uint8_t*)(v19 + 112) & 4) { v28 = *++v14; }  // 자세를 **별도 바이트**로
            //       else                            { v28 = v20 & 0xF; }
            //     `+112` 은 `ObjClass` 다(`client/drawable.go:572` — `ObjClass // 28, 112`).
            //     그리고 **사람은 `Class & 0x4`** 다(`draw_debug.go:87` — 그 가지에서만
            //     사람 자세표 `debugStates`(IDLE/DIE/DEAD/JUMP/WALK…)를 쓴다).
            //     ★ 즉 사람이면 **자세가 늘 별도 한 바이트로 온다.** 별칭 여부와 무관하다 —
            //       클래스는 오브젝트 속성이지 어떻게 줄여 보냈나와 상관이 없다.
            //
            //   ⚠ 위 실측 주석의 「별칭이면 덧붙이 1」과 부딪힌다. 1 로 보내면 클라는
            //     **다음 항목의 첫 바이트를 자세로 먹고** 그 뒤가 밀린다. 사람이 마지막
            //     항목이면 밀림이 화면에 안 드러나므로 실측 때 1 로 읽혔을 수 있다.
            //
            //   ★ **하위 4비트에 자세를 싣던 앞 시도는 화면을 못 바꿨다**(2026-09-11 실측):
            //     서 있는 채로 자세만 0↔2 로 바꿔 찍었더니 다른 픽셀 9,476·4,672 로
            //     **대조군(같은 자세 두 장) 8,200 범위 안**이었다. `else` 가지를 안 타는 것이다.
            // ★★★ **되돌림(2026-09-11 실측) — 자세는 하위 4비트가 맞다.**
            //
            //   위 추론(사람이 `Class & 4` 라 자세가 별도 바이트)은 **화면이 반박했다.**
            //   서 있는 채로 자세만 바꿔 **캐릭터 자리(95×135)** 만 재니 —
            //
            //     보낸 바이트                        화면
            //     `0x40`(1B) · `0x40 02` · `0x40 04`  회색   ← 셋이 **픽셀 단위로 같다**
            //     `0x42`(1B) · `0x44`(1B) · `0x40 FF` 파랑
            //
            //   ⑴ **1바이트 `0x42`·`0x44` 는 `0x40` 과 뚜렷이 다르다**(1,568 픽셀, 잡음 바닥 396)
            //      → 하위 4비트가 실제로 화면을 바꾼다
            //   ⑵ **2바이트로 보낸 `02`·`04` 는 아무 효과가 없다** → 클라가 그 바이트를
            //      자세로 안 읽는다. `0xFF` 만 달랐는데, `0xff` 는 **다음 항목의 「전체 번호」
            //      표식**이라 그 뒤를 통째로 다르게 읽은 것으로 보인다
            //
            //   ⚠ 즉 **사람의 `ObjClass` 에는 비트 2(0x4)가 안 켜져 있다.**
            //     `draw_debug.go:87` 의 `Class()&0x4` 가지는 그리기 디버그의 갈래일 뿐,
            //     와이어 길이를 정하는 `+112 & 4` 와 같은 비트라는 보장이 없었다.
            //     ★ 정답지 읽기도 **화면으로 확인하기 전까지는 가설**이다.
            // ★★★ **다시 2바이트로** (2026-09-11 두 번째 판정).
            //
            //   앞서 2바이트를 한 번 넣었다 뺐는데, 그때 실험이 **반쪽**이었다 —
            //   `--anim-pose` 를 스냅샷 경로에만 걸고 **주기 갱신은 자세 0 을 계속 덮어쓰던**
            //   상태로 쟀다. 두 경로 다 걸고 다시 재니 「하위 4비트가 먹는다」던 숫자가
            //   전부 **토글 잡음**이었음이 드러났다(자세4 vs 자세0 = 216, 대조군 488).
            //
            //   ⭕ 2바이트를 받쳐 주는 근거 —
            //     · 클라 `GAME2_3.c:1685` — `ObjClass & 4` 면 자세를 **별도 바이트**로 읽는다
            //     · `+112` 이 `ObjClass` (`opennox/src/client/drawable.go:572`)
            //     · **`verify_update`(정품 녹음 뜯고 다시 짜기)가 2바이트로도 통과한다**
            //   ⚠ 위 코드는 `ff` 일 때 둘째 바이트를 **0 으로 고정**하고 별칭일 때는 아예
            //     안 붙였다. 그러면 자세는 늘 IDLE 이고, 별칭 항목에서는 **다음 항목의 첫
            //     바이트를 자세로 먹는다.** 그것이 「걷기 모션 없음」의 모습과 맞는다.
            // ★★★ **바라보는 쪽을 싣는다** (2026-09-12 배선). 전에는 `0x40` 고정이었다.
            //
            //   ⚠ `0x40` 은 「값 없음」이 아니었다 — `(0x40>>4)&7 = 4` 이고 클라는 4 이상이면
            //     +1 하므로(`GAME2_3.c:1677-1679`) **방향 5 로 못 박혀 있었다.** 어느 쪽을
            //     보든 늘 같은 쪽을 보고 서 있었던 것이다.
            //
            //   ⭕ 근거 다섯 —
            //     ⑴ 자리 — 클라가 `(값>>4)&7` 로 읽는다 (`GAME2_3.c:1677-1679`)
            //     ⑵ 짜임 — **정품 서버 빌더**가 소환수(`COMPLEX_OBJ`)와 사람(`PLAYER_OBJ`)에
            //        **같은 자리**를 쓴다 (`opennox/src/network_lists.go:290-300` —
            //        `b1[off] = b2[9]` 뒤에 사람만 `b2[11]`(자세)이 하나 더 붙는다)
            //     ⑶ 왜 4 를 건너뛰나 — 클라의 방향 그림 배열이 **`Frames [9]`** 다
            //        (`opennox/src/client/drawable_player.go:14`). 9칸 중 4번을 비우고
            //        0,1,2,3,5,6,7,8 여덟 자리를 쓴다
            //     ⑷ 변환 — `(facing>>5)&7`. **우리 소환수 가지가 이미 쓰던 식**이다(위 339행)
            //        ❌ **정정(2026-09-13 묶음 3-a ④)** — 이 식은 틀렸다. 9칸 자리는 각도 팔분이 아니라
            //          **(x+3y+4) 격자**다(`GAME4_1.c:4471~4478` xferIndexedDirection → 0 = 왼쪽 위 · 5 = 동쪽).
            //          화면 증거: 서버 방향 10(동쪽)을 `(10>>5)&7 = 0` 으로 보내니 캐릭터가 **왼쪽 위로** 주먹을 뻗었다
            //          (`_evidence/b3a_punch_exp2` f_29·f_49). 09-12 실측은 「바뀐다」만 쟀지 「맞는 쪽」을 안 쟀다.
            //          → WireDirNibble(정답지 표 kDirRow) 로 바꿨다.
            //     ⑸ 값 — `facing` 은 살아 있다. 클라가 `PLAYER_INPUT` 코드 1 로 보내고
            //        (`Server.cpp:1592`) 걸음도 이 방향으로 민다(`:1597-1601`)
            //   ⭕ 실측(2026-09-12) — `--anim-dir 0` 두 장이 **196 픽셀**(잡음 바닥)인데
            //     `0` vs `6` 은 **1,992·1,996 픽셀**로 열 배다. 경계상자도 캐릭터 자리다
            //
            // ⚠ **하위 4비트는 0 으로 둔다.** 사람은 자세를 **둘째 바이트**로 받으므로
            //   (`ObjClass & 4` 가지) 하위 4비트를 안 읽는다. 소환수가 거기에
            //   `kCreatureAnimIdle` 을 넣는 것과 다르다 — 사람에 그걸 넣으면 안 된다.
            e.extraBytes[0] = static_cast<uint8_t>(WireDirNibble(facing) << 4);
            e.extraBytes[1] = static_cast<uint8_t>(playerAnim);  // 자세 — 별도 바이트
            e.extra = 2;
            // ★ 2026-09-13 묶음 3-a — **애니 칸 바이트.** 정답지 `nox_xxx_netPlayerObjSend_518C30`(GAME4_1.c:4447~4453)은
            //   공격(상태 1) 같은 상태에서 +236 칸 번호를 b2[10] 에 싣고, 짜는 쪽(`network_lists.go:290~299`)이
            //   그 값이 ff 가 아니면 방향 바이트에 0x80 을 켜고 **방향 · 칸 · 자세** 세 바이트로 보낸다.
            //   클라는 0x80 이면 칸을 읽어 `spriteSetFrameMB` 에 넣는다(GAME2_3.c:1680~1682).
            if (playerFrame >= 0) {
                e.extraBytes[2] = e.extraBytes[1];
                e.extraBytes[1] = static_cast<uint8_t>(playerFrame);
                e.extraBytes[0] |= 0x80;
                e.extra = 3;
            }
            // ★ 진단 — 방향 3비트를 못 박는다. **덧붙이가 클라에 닿는지**를 가르는 시험이다.
            if (forceDir >= 0) {
                e.extraBytes[0] = static_cast<uint8_t>(((forceDir & 0x07) << 4) | (e.extraBytes[0] & 0x80));
            }
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
        // 스테이지를 다 잡았다 — 다음 스테이지, 아니면 판끝.
        if (!roundOver_ && questKilled_ >= questTotal_) {
            if (questStage_ + 1 < kQuestStageCount) {
                ++questStage_;
                SpawnQuestStage(&out);   // 다음 웨이브가 밀려온다
            } else {
                roundOver_ = true;
                restartAtFrame_ = frame_ + kRoundRestartTicks;
                SendNotice("Quest complete", out);
            }
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

bool Server::OwnersEnemy(const std::string& a, bool aWild, const std::string& b, bool bWild) const {
    // `server/object.go:1667~1741` 가운데 지금 있는 것들 — 같은 주인 → 적 아님 · 같은 팀 → 적 아님 ·
    //   한쪽이라도 팀 있음 → 적 · 둘 다 몬스터 → 적 아님 · 그 밖 → 적
    if (aWild && bWild) return false;                      // 둘 다 맵 몬스터(팀 없음)
    if (aWild || bWild) return true;                       // 사람(또는 사람 소환수) 대 맵 몬스터
    if (a == b) return false;                              // 같은 주인
    return IsEnemyTo(a, b);                                // 사람 대 사람(퀘스트 규칙은 IsEnemyTo 주석)
}

// 피해 함수(+716) — 정답지 이식(묶음 3-e, 2026-09-13).
//   ⚠ 옮기지 않은 갈래(다른 조각): 방패·대검·지팡이 막기(3-e2 — 상태 15/16 · 18~21) · 되받아치기 버프 27 · 불/전기 보호 · 흡혈 ·
//     전기 방패 22 · 마법 방패 26(주문 4) · 입은 것 닳기·방어 마법 효과(아이템 5) · 스켈레톤 막기(몬스터 행동 21 · AI 6)
void Server::HurtTarget(const Target& t, int dmg, std::vector<OutPacket>& out,
                        const std::string& by, uint8_t dtype, const AttackSrc* src) {
    if (t.none() || dmg <= 0) return;
    // 진단 임프(`--debug-own-pet`)는 맞지 않는다 — 판정 사진을 찍기 전에 몬스터에게 죽었다(09-13, 갱신에 프레임 1920~2010 만 실림 — 약 3초).
    if (debugOwnPetNetCode_ != 0 && !debugPetWild_ && t.pet >= 0 &&
        pets_[static_cast<size_t>(t.pet)].netCode == debugOwnPetNetCode_) return;
    // ★ **닿은 피해를 여기서 센다.** `hits_` 는 「투사체가 명중한 횟수」만 세는데,
    //   판정 요약이 그것을 「맞음」이라 찍어 왔다. 그래서 근접으로 피해가 122건 오간
    //   실클라 런이 「맞음 0」으로 나와 **싸움이 안 붙은 것으로 오판**했다(09-10).
    //   갈라 세야 「안 맞았다」와 「투사체만 안 맞았다」가 구분된다.
    ++hurts_;
    const bool byWild = by.empty();                        // 주인 없는 것이 때림
    const bool quest = (questTotal_ > 0 || questStage_ > 0);   // gameFlags 4096

    if (t.pet < 0) {
        auto vi = sessions_.find(t.addr);
        if (vi == sessions_.end()) return;
        Session& s = vi->second;
        // ── nox_server_handler_PlayerDamage_4E17B0 ──
        if (!s.inGame || s.respawnAtFrame != 0) return;    // +16 & 2 · DEAD
        if (s.invulnUntilSet && frame_ < s.invulnUntilFrame) { ++invulnBlocked_; return; }   // 버프 23
        if (s.observer) return;                            // 3680 & 1
        // ── 막기(묶음 3-e2 · `GAME3_3.c:206~330`) — 때린 것이 있고 · 종류 15 아님 · 주먹/유성/독구름 아님 · **앞쪽**이면 ──
        //   ① 방패(ArmorEquip 0x3000000)를 **올린 상태(16)** 면 전기(9·17) 말고 다 막는다(M_LABEL_78) — 투사체는 되받아친다
        //   ② 아니면 대검(0x400): 투사체·종류 0·11 을 가만(13)·막기(18~20) 중에 막는다 → 18~20 중 하나로
        //   ③ 아니면 지팡이(0x7FF8000): 투사체 아닌 종류 0·11 을 같은 상태에서 막는다 → 21
        //   ⚠ 되받아치기(projectileReflect)·막기 소리·막을 때 아이템 닳기는 투사체 4 · 소리 10 · 아이템 5
        if (src && dtype != 15 &&
            (TestTwoPointsAndDirection(static_cast<float>(s.posX), static_cast<float>(s.posY), s.facingPrev, src->x, src->y) & 1)) {
            const bool shield = (ArmorEquipOf(s) & 0x3000000) != 0;
            const uint8_t st = s.moveState;
            const bool guardState = (st == 13 || st == 18 || st == 19 || st == 20);
            if (st == 16 && shield) {
                if (dtype != 9 && dtype != 17) {
                    std::printf("[막기] 프레임 %u %s 방패로 막음(종류 %u · 피해 %d)\n", frame_, s.addr.c_str(), dtype, dmg);
                    std::fflush(stdout);
                    return;
                }
            } else {
                const uint32_t weap = WeaponEquipOf(s);
                bool toArmor = false;                      // LABEL_120 으로
                if (weap & 0x400) {
                    if (src->missile || dtype == 0 || dtype == 11) {
                        if (!guardState) {
                            toArmor = true;
                        } else {
                            PlayerSetState(s, static_cast<uint8_t>(18 + NextRandom(3)), frame_);   // randomInt(18, 20)
                            std::printf("[막기] 프레임 %u %s 대검으로 막음 → 상태 %u\n", frame_, s.addr.c_str(), s.moveState);
                            std::fflush(stdout);
                            return;
                        }
                    }
                }
                if (!toArmor && (weap & 0x7FF8000) && (dtype == 0 || dtype == 11) && !src->missile && guardState) {
                    PlayerSetState(s, 21, frame_);
                    std::printf("[막기] 프레임 %u %s 지팡이로 막음\n", frame_, s.addr.c_str());
                    std::fflush(stdout);
                    return;
                }
            }
        }
        // 방어 — 종류마다 방어값(+228)을 먹는 정도가 다르고, 깎인 피해의 소수부는 +84 에 이월된다
        const float armor = ArmorValueFloat(s.charClass);
        auto carry = [&](float f) {
            const float a3 = static_cast<float>(static_cast<double>(f) + static_cast<double>(s.dmgRemainder));
            const int v = static_cast<int>(a3);            // nox_float2int = (int)
            s.dmgRemainder = static_cast<float>(static_cast<double>(a3) - static_cast<double>(v));
            return v;
        };
        int hp;
        switch (dtype) {
            case 0: case 3: case 7: case 8: case 10: case 11:
                hp = carry(static_cast<float>((1.0 - static_cast<double>(armor)) * static_cast<double>(dmg)));
                break;
            case 2:
                hp = carry(static_cast<float>((1.0 - static_cast<double>(armor) * 0.5) * static_cast<double>(dmg)));
                break;
            case 9: case 17:
                // sub_4E2220 — 1 + 금속 갑옷 몫. 기본 장비엔 금속이 없어 1(아이템 5 에서 채움)
                hp = carry(static_cast<float>(1.0 * static_cast<double>(dmg)));
                break;
            default:                                       // 1·12·4·5·6·14·15·16 · 그 밖 — 그대로
                hp = dmg;
                break;
        }
        if (dmg > 0 && hp == 0) hp = 1;                    // `v33 > 0 && !a1`
        if (quest) {                                       // × sub_4E40B0(blob 202032 = 1.0)
            const int v44 = hp;
            hp = static_cast<int>(static_cast<float>(1.0 * static_cast<double>(hp)));
            if (v44 > 0 && hp < 1) hp = 1;
        }
        // ── nox_xxx_damageDefaultProc_4E0B30 ──
        // 편 거르기 — 친선 피해 설정(게임플레이 깃발 1, 기본 꺼짐)이 꺼져 있으면 때린 쪽 주인이 적이 아닐 때 무시.
        //   단 자기가 자기를 때린 것은 퀘스트가 아니면 들어간다(`a1 != v12 || gameFlags 4096`).
        if (!OwnersEnemy(s.addr, false, by, byWild) && (by != s.addr || quest)) return;
        // 한 대에 20 넘게 맞으면 움찔(상태 30 · 공격 중 1 · 방패 올리는 중 15 는 아님)
        if (hp >= 20 && s.moveState != kPlayerStateAttack && s.moveState != 15) PlayerSetState(s, 30, frame_);
        Damage(t.addr, hp, out, by);                       // unitDamageClear_4EE5E0
        return;
    }

    const size_t i = static_cast<size_t>(t.pet);
    if (i >= pets_.size()) return;
    Creature& k = pets_[i];
    const auto tb = typeBodies_.find(k.type);
    const PhysBody* body = (tb != typeBodies_.end()) ? &tb->second : nullptr;
    int amount = dmg;
    // ── 몬스터 피해 함수(thing.bin DAMAGE) ──
    if (body) {
        switch (body->damageFn) {
            case kDmgMechGolem: if (dtype == 9 || dtype == 17) amount = 2 * dmg; break;   // sub_4E24E0
            case kDmgFlammable: if (dtype == 1 || dtype == 12 || dtype == 7) amount = 9999999; break;   // 4E2520
            case kDmgBlackPowder:                                                          // 4E2560
                if (dtype != 1 && dtype != 12 && dtype != 2 && dtype != 0) return;
                amount = 999999;
                break;
            default: break;                                // Default · Stone · Skeleton(막기는 AI 6) · Player
        }
    }
    // ── nox_xxx_damageDefaultProc_4E0B30 ──
    if (!OwnersEnemy(k.owner, k.owner.empty(), by, byWild)) return;   // 편 거르기(자기가 자기를 때리는 경우는 없음)
    int v15 = dtype;
    if (body) {
        const uint32_t sub = body->subclass;
        if ((sub & 0x200) && v15 == 5) return;             // 독 면역
        if (sub & 0x400) {                                 // 불 면역 — 1·12 무시 · 7 은 절반
            if (v15 == 1 || v15 == 12) return;
            if (v15 == 7) amount /= 2;
        } else if (sub & 0x800) {                          // 전기 면역
            if (v15 == 9 || v15 == 17) return;
        }
    }
    if (v15 == 1 || v15 == 12 || v15 == 7) {               // 불 보호(버프 — 주문 4) 몫 · 0 이면 1
        amount = static_cast<int>(static_cast<float>(1.0 * static_cast<double>(amount)));
        if (!amount) amount = 1;
    }
    if (v15 == 9 || v15 == 17) {                           // 전기 보호 몫 · 0 이면 1
        amount = static_cast<int>(static_cast<float>(1.0 * static_cast<double>(amount)));
        if (!amount) amount = 1;
    }
    // unitDamageClear_4EE5E0
    if (k.health > amount) {
        k.health = static_cast<uint16_t>(k.health - amount);
        // ★ R4 — 불려 나온 것의 체력이 바뀌었다. **주인에게만** 흘러간다
        //   (`FlushPending` 이 `IsEnemyTo` 로 거른다 — 남의 소환수는 적이라 안 간다).
        MarkDirty(k.netCode, PendingReport::What::UnitHealth);
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
    // ★ R4 — 체력이 바뀌었다. 「알려야 할 것」에 올린다(보낼지는 틱 끝에서 가른다).
    if (after != before) MarkDirty(s.netCode, PendingReport::What::UnitHealth);

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
        // ★ 체력·마나를 같이 찍는다 (2026-09-11). 안 찍어서 **마나가 얼마인지 아무도 몰랐고**,
        //   화면의 빈 마나칸을 보고 「클라에게 안 알린 탓」이라고 잘못 짚었다.
        //   시전은 `cost > s.mana` 면 **카운터도 안 올리고 조용히 안 일어난다** —
        //   그래서 「주문이 안 나간다」와 「마나가 없다」가 로그에서 구분이 안 됐다.
        std::snprintf(buf, sizeof(buf), " 사람 %u=%d,%d,%u,%u 체력%u/%u 마나%u/%u",
                      (unsigned)o.netCode, o.x, o.y, (unsigned)o.facing, (unsigned)o.team,
                      (unsigned)o.health, (unsigned)o.maxHealth,
                      (unsigned)o.mana, (unsigned)o.maxMana);
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

// 둘이 서로 적인가 — 선언부(`Server.h`)에 정품 규칙과 근거를 적어 두었다.
bool Server::IsEnemyTo(const Session& a, const Session& b) const {
    if (&a == &b) return false;                       // 자기 자신
    // ➖ 퀘스트는 협동이므로 **사람끼리는 아군**으로 본다(우리 값 — 정품 확인 못 함).
    if (questTotal_ > 0 || questStage_ > 0) return false;
    if (a.team != 0 && a.team == b.team) return false;   // 같은 팀
    if (a.team != 0 || b.team != 0) return true;         // 한쪽이라도 팀이 있으면 적
    return true;                                          // 팀 없는 사람끼리 — 아레나는 전원 적
}

bool Server::IsEnemyTo(const std::string& a, const std::string& b) const {
    const auto ia = sessions_.find(a);
    const auto ib = sessions_.find(b);
    if (ia == sessions_.end() || ib == sessions_.end()) return false;
    return IsEnemyTo(ia->second, ib->second);
}

// ★★★ **바뀌었으니 알려라** — 정품 더티 비트 틀(`GAME4_2.c:7136`)을 옮긴 것.
//
//   정품은 오브젝트에 **플레이어별 비트**를 세우는데, 우리는 **세션에 목록**을 쌓는다
//   (까닭은 `Session::pending` 주석). 「누구에게 보낼까」는 **보낼 때** 가른다 —
//   정품도 `netUpdateObjectSpecial` 안에서 그때 `unitIsEnemyTo` 를 묻는다.
//   그 사이 팀이 바뀔 수 있으므로 **미리 거르지 않는다.**
void Server::MarkDirty(uint16_t netCode, PendingReport::What what) {
    if (netCode == 0) return;
    const PendingReport pr{netCode, what};
    for (auto& kv : sessions_) {
        Session& s = kv.second;
        if (!s.inGame) continue;
        // 같은 것이 이미 있으면 안 넣는다 — 한 틱에 값이 여러 번 바뀌어도 한 번만 보낸다.
        // ⚠ 한 틱 목록은 짧으므로 선형 탐색으로 충분하다.
        if (std::find(s.pending.begin(), s.pending.end(), pr) != s.pending.end()) continue;
        s.pending.push_back(pr);
    }
}

// 쌓인 것을 보내고 목록을 비운다 — 정품의 「보낸 뒤 비트 끄기」에 해당한다.
void Server::FlushPending(std::vector<OutPacket>& out) {
    for (auto& kv : sessions_) {
        Session& s = kv.second;
        if (!s.inGame) { s.pending.clear(); continue; }
        for (const auto& pr : s.pending) {
            // ★★ 마법부여 — `0x5A REPORT_ENCHANT`
            //   ⭕ 짜임 `netReportEnchant_4D8F90`(`GAME3_2.c:4496-4503`):
            //       v3[0] = 90;                                   // 0x5A
            //       *(uint16_t*)&v3[1] = netGetUnitCodeServ(a2);  // 넷코드
            //       *(uint32_t*)&v3[3] = a2[85];                  // **비트 묶음 uint32**
            //       netSendPacket1(a1, v3, 7, 0, 1);              // 총 7 · **중요**
            //   ⭕ 비트 자리 — `(Buffs & (1 << v))`(`client/drawable.go:739`) ·
            //       `ENCHANT_INVULNERABLE = 23`(`server/spells_enchant.go:60`)
            //   ★ 우리는 부활 무적을 이 비트로 알린다. 정품 더티 비트 `0x800000` 자리다.
            if (pr.what == PendingReport::What::Enchant) {
                const Session* who = nullptr;
                for (const auto& kv2 : sessions_) {
                    if (kv2.second.inGame && kv2.second.netCode == pr.netCode) { who = &kv2.second; break; }
                }
                if (!who) continue;
                const uint32_t bits =
                    (who->invulnUntilSet && frame_ < who->invulnUntilFrame) ? kEnchantInvulnerable : 0u;
                uint8_t msg[7] = {
                    static_cast<uint8_t>(Op::REPORT_ENCHANTMENT),
                    static_cast<uint8_t>(pr.netCode),
                    static_cast<uint8_t>(pr.netCode >> 8),
                    static_cast<uint8_t>(bits),
                    static_cast<uint8_t>(bits >> 8),
                    static_cast<uint8_t>(bits >> 16),
                    static_cast<uint8_t>(bits >> 24),
                };
                EmitSeq(s, msg, sizeof(msg), out);
                ++enchantSent_;
                continue;
            }
            if (pr.what != PendingReport::What::UnitHealth) continue;   // 나머지는 아직
            // 그 번호의 임자를 찾는다 — **사람과 불려 나온 것 둘 다** 본다.
            uint16_t hp = 0;
            bool found = false;
            for (const auto& kv2 : sessions_) {
                if (kv2.second.inGame && kv2.second.netCode == pr.netCode) {
                    // ⭕ **적이 아닐 때만** 보낸다 — `GAME4_2.c:7152-7157`
                    if (IsEnemyTo(s, kv2.second)) break;
                    hp = kv2.second.health;
                    found = true;
                    break;
                }
            }
            if (!found) {
                for (const auto& k : pets_) {
                    if (k.netCode != pr.netCode) continue;
                    // ★★ **불려 나온 것은 「주인이 같을 때만」 아군이다.**
                    //   ⭕ 정품 `IsEnemyTo`(`opennox/src/server/object.go:1717-1721`) —
                    //     `own1 := obj.FindOwnerChainPlayer()` · `own2 := …` ·
                    //     `if own1 == own2 { return false }` — **주인을 따라 올라가** 견준다.
                    //   즉 **내 소환수 체력은 나에게 오고, 남의 것·맵 몬스터는 적이라 안 온다.**
                    //   ➖ 정품의 `SubClass & 0x20`(중립 몬스터) 갈래는 3모드에 안 쓴다.
                    if (k.owner != kv.first) break;      // 남의 것이거나 주인 없는 맵 몬스터
                    hp = k.health;
                    found = true;
                    break;
                }
            }
            if (!found) continue;
            // ⭕ 짜임 — `netReportUnitCurrentHP_4D8620`(`GAME3_2.c:4022-4038`):
            //     [0]=65(0x41) · [1..2]=넷코드 · **[3]=체력 >> 1** · 총 4 · `a6=1`(중요)
            //   체력을 **2로 나눠 한 바이트**에 담는다(최대 510 까지 표현된다).
            uint8_t msg[4] = {
                static_cast<uint8_t>(Op::REPORT_HEALTH),
                static_cast<uint8_t>(pr.netCode),
                static_cast<uint8_t>(pr.netCode >> 8),
                static_cast<uint8_t>(hp >> 1),
            };
            EmitSeq(s, msg, sizeof(msg), out);
            ++unitHealthSent_;
        }
        s.pending.clear();
    }
}

// 퀘스트를 연다 — **스테이지 1 부터.** `count` 는 이제 안 쓴다(스테이지 표가 정한다).
//
// ★★ 전에는 「몬스터 N 마리 세우고 다 잡으면 끝」이었다. 정품 NoxQuest 는 웨이브가
//   밀려오고 스테이지가 오른다(사용자 결정, 2026-09-11). 스테이지 표는 `GameBalance.h`.
void Server::StartQuest(int count, int centerX, int centerY, double spread) {
    (void)count;
    questCenterX_ = centerX;
    questCenterY_ = centerY;
    questSpread_ = spread;
    questStage_ = 0;
    questKilled_ = 0;
    roundOver_ = false;
    SpawnQuestStage(nullptr);   // 첫 스테이지 (알림은 판이 뜬 뒤 첫 틱에서)
}

// 지금 스테이지의 몬스터를 세운다. `out` 이 있으면 스테이지 알림도 보낸다.
void Server::SpawnQuestStage(std::vector<OutPacket>* out) {
    if (questStage_ >= kQuestStageCount) return;
    // ⚠ `QuestStage`(타입)는 멤버 함수 `QuestStage()` 와 이름이 겹쳐 클래스 안에서 가려진다.
    //   `auto` 로 받아 그 충돌을 피한다.
    const auto& st = kQuestStages[questStage_];
    // ⚠ 한 웨이브 상한(⭕ gamedata Normal=5). 스테이지 표가 그보다 크면 잘라 세운다.
    const int n = st.count < kWaveMaxActive ? st.count : kWaveMaxActive;
    questTotal_ = n;
    questKilled_ = 0;
    for (int i = 0; i < n; ++i) {
        const double ang = 2.0 * 3.14159265358979 * i / (n > 0 ? n : 1);
        Creature k;
        k.owner.clear();                 // 주인이 없다 — 아무 편도 아니다
        k.type = st.monster;
        k.netCode = nextShotNetCode_++;
        if (nextShotNetCode_ == 0) nextShotNetCode_ = 4096;
        k.x = questCenterX_ + questSpread_ * std::cos(ang);
        k.y = questCenterY_ + questSpread_ * std::sin(ang);
        k.health = static_cast<uint16_t>(TuningFor(k.type).health);
        pets_.push_back(k);
    }
    std::printf("[스테이지] %d/%d — %s (%d마리)\n",
                questStage_ + 1, kQuestStageCount, st.label, n);
    std::fflush(stdout);
    if (out) SendNotice(st.label, *out);
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

// 투사체가 끝난 자리의 이펙트 (P7).
//
// ⭕ 짜임은 클라 원본 확정(2026-09-11) — 점(총 5B) `op | x:i16 | y:i16` ·
//   선(총 9B) `op | x1 | y1 | x2 | y2`(u16 넷). 그림 이름까지 코드에 있다
//   (0x85 "FireBoom" · 0x8c "DynamicLightning" · 0x8d "DynamicEnergyBolt").
// ◑ **주문↔이펙트 대응은 이름 짝짓기다** — 파이어볼→FireBoom 은 자연스럽지만
//   정품 와이어로 짝을 확인한 표본은 없다(전투 FX 표본 0건).
// · 명중이든 소진이든 **끝나는 자리에서 한 번** 그린다.
void Server::SendProjectileFx(const Projectile& p, std::vector<OutPacket>& out) {
    const int16_t ex = static_cast<int16_t>(p.x);
    const int16_t ey = static_cast<int16_t>(p.y);
    switch (p.spell) {
        case 27: {   // 파이어볼 → 0x85 EXPLOSION — 끝난 점에서 터진다
            uint8_t m[5] = {
                static_cast<uint8_t>(Op::FX_EXPLOSION),
                static_cast<uint8_t>(ex), static_cast<uint8_t>(ex >> 8),
                static_cast<uint8_t>(ey), static_cast<uint8_t>(ey >> 8),
            };
            Broadcast(/*viaSeq=*/false, m, sizeof(m), out);
            ++fxSent_;
            break;
        }
        case 43:     // 연쇄번개 → 0x8c · 에너지볼트 → 0x8d — 쏜 자리에서 끝난 자리까지 선
        case 24: {
            const uint16_t sx = static_cast<uint16_t>(p.launchX);
            const uint16_t sy = static_cast<uint16_t>(p.launchY);
            uint8_t m[9] = {
                static_cast<uint8_t>(p.spell == 43 ? Op::FX_LIGHTNING : Op::FX_ENERGY_BOLT),
                static_cast<uint8_t>(sx), static_cast<uint8_t>(sx >> 8),
                static_cast<uint8_t>(sy), static_cast<uint8_t>(sy >> 8),
                static_cast<uint8_t>(ex), static_cast<uint8_t>(ex >> 8),
                static_cast<uint8_t>(ey), static_cast<uint8_t>(ey >> 8),
            };
            Broadcast(/*viaSeq=*/false, m, sizeof(m), out);
            ++fxSent_;
            break;
        }
        default: break;   // 대응을 모르는 주문은 안 그린다
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
            const uint8_t sop = static_cast<uint8_t>(probe.stopOp());
            ++stoppedByOp_[sop];
            // ★ 진단 — **멈춘 자리의 바이트를 그대로 남긴다**(오프코드마다 첫 한 번만).
            //   「어느 오프코드에서 멈췄나」만으로는 못 고친다. `0xc9` 를 하위 명령별 표까지
            //   넣었는데 여전히 버렸다 — **그 하위 명령이 표에 없는 값**일 수 있고,
            //   그건 바이트를 봐야 안다. (2026-09-11)
            if (stopSampleShown_.insert(sop).second) {
                const size_t rest = probe.stopRest();
                const size_t base = (bodyLen >= rest) ? bodyLen - rest : 0;
                std::printf("[멈춤표본] op=%02x 남은 %zu |", sop, rest);
                for (size_t q = 0; q < rest && q < 8; ++q)
                    std::printf(" %02x", body[base + q]);
                std::printf("\n");
                std::fflush(stdout);
            }
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

            // ★ 준비 — 이때부터 중요 메시지를 싣는다(정답지 `GAME3_2.c:5602` 상태 비트 0x10).
            //   클라는 이걸 되풀이해 보낸다(09-13 로그: 약 62틱마다 · 까닭 미상, 관측대기).
            case Op::CLIENT_READY:
                if (!s.importantReady) {
                    s.importantReady = true;
                    std::printf("[중요] 프레임 %u %s 준비 — 대기 %zu 개를 싣기 시작\n",
                                frame_, s.addr.c_str(), s.importantQ.size());
                }
                break;

            // ★ 중요 메시지 확인 — `ab <프레임4>`. 그 프레임 조각에 실었던 것을 지운다
            //   (정답지 `nox_net_importantACK_4E55A0` · `network_server.go:223`).
            //   하위 16비트로 맞춘다 — 우리가 싣는 시각이 `27`(16비트)이고 클라가 윗자리를 제 시계로 채운다.
            case Op::IMPORTANT_ACK: {
                if (it.bodyLen() < 4) break;
                const uint8_t* b = it.body();
                const uint32_t fr = static_cast<uint32_t>(b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24));
                const size_t before = s.importantQ.size();
                s.importantQ.erase(std::remove_if(s.importantQ.begin(), s.importantQ.end(),
                    [fr](const Session::PendingImportant& p) {
                        return p.sent && ((p.sentFrame ^ fr) & 0xFFFF) == 0;
                    }), s.importantQ.end());
                if (s.importantAcked == 0 && before != s.importantQ.size())
                    std::printf("[중요] 프레임 %u %s 첫 확인(시각 %u) — %zu 개 지움, 남은 %zu\n", frame_,
                                s.addr.c_str(), static_cast<unsigned>(fr & 0xFFFF),
                                before - s.importantQ.size(), s.importantQ.size());
                s.importantAcked += static_cast<uint32_t>(before - s.importantQ.size());
                ++handled_;
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
                    // ★ 조작이 하나라도 왔다 — 정답지 ImplB 는 목록이 비었으면 상태를 안 건드린다(`object_update.go:485`).
                    s.inputThisTick = true;
                    if (code == 1 && dataLen == 1) {
                        // 바라보는 쪽 — 정답지 CCOrientation → Direction2. ★ 2026-09-13 묶음 3-a 부터 틱의 ImplB 에서 갈아 끼운다
                        //   (정답지는 조작 목록을 ImplA 힘·공격 **뒤에** 읽는다 — 받자마자 바꾸면 한 틱 이르다).
                        s.facingInput = q[0];
                        s.hasFacingInput = true;
                    } else if (code >= 2 && code <= 5 && dataLen == 1) {
                        // 걸음 — ★ 2026-09-13 부터 **바로 옮기지 않는다.** 「이번 틱에 이동 입력이 왔다」만 적고
                        //   틱의 걷기 물리(UpdatePlayerMovement)가 상태를 걷기/달리기로 바꿔 힘을 준다.
                        //   ⭕ 앞·뒤·왼·오른(2~5)은 **힘의 방향을 안 바꾼다** — 정답지는 코드마다 깃발(+Field60)만
                        //     다르게 켜고 힘은 늘 바라보는 쪽으로 준다(`object_update.go:519~534` · `:192`).
                        //     전에 뒤·왼·오른을 128·192·64 로 돌려 민 것은 **우리가 지어낸 규칙**이었다.
                        s.moveInputThisTick = true;
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
                        // ⚠ 코드 6 은 **누르는 순간 한 번만** 온다(2026-09-09 실측 — 180초 눌러 조각 10,885 개 중 2건).
                        // ★★ 2026-09-13 묶음 3-a·3-b — **정답지로.** 클릭은 「이번 틱에 CCAction 이 왔다」만 적는다.
                        //   틱의 ImplB(PlayerInputAttack)가 기력(무기)·쉬는 시간(Field0)을 보고 상태 1 로 넣고,
                        //   ImplA(PlayerAttack)가 맨손은 애니 끝 · 무기는 애니 한가운데 틱에 판정한다.
                        //   전에는 여기서 바로 때리고 15틱 쉬었다(kMeleeCooldownTicks · 우리 값) — 걷어냈다.
                        else {
                            s.actionThisTick = true;
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
                c.equipBit = w.equipBit;
                c.isWeapon = w.isWeapon;
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

                    // ★★ **남들에게 겉모습을 알린다** (P7). 그동안 본인 표시(0x60/0x61)만
                    //   있어서 옆 사람 화면의 몸이 안 바뀌었다.
                    //   짜임은 클라 원본 확정(2026-09-11) —
                    //     입기: op | 번호2(**bit15=사람**) | 장비비트4 | (수식4, 빈 칸 ff)
                    //     벗기: op | 번호2(**bit15 안 봄** — 클라가 사람 전용으로 처리) | 장비비트4
                    //   장비비트가 0 이면(표에 없는 이름) **안 보낸다** — 짐작으로 안 채운다.
                    if (c.equipBit != 0) {
                        uint8_t am[11];
                        size_t an = 7;
                        if (wear) {
                            am[0] = static_cast<uint8_t>(c.modifiable
                                ? (c.isWeapon ? Op::REPORT_MODIFIABLE_WEAPON_EQUIP
                                              : Op::REPORT_MODIFIABLE_ARMOR_EQUIP)
                                : (c.isWeapon ? Op::REPORT_MUNDANE_WEAPON_EQUIP
                                              : Op::REPORT_MUNDANE_ARMOR_EQUIP));
                        } else {
                            am[0] = static_cast<uint8_t>(
                                c.isWeapon ? Op::REPORT_WEAPON_DEQUIP : Op::REPORT_ARMOR_DEQUIP);
                        }
                        const uint16_t who = static_cast<uint16_t>(
                            s.netCode | (wear ? 0x8000u : 0u));
                        am[1] = static_cast<uint8_t>(who);
                        am[2] = static_cast<uint8_t>(who >> 8);
                        PutU32(am + 3, c.equipBit);
                        if (wear && c.modifiable) {
                            am[7] = am[8] = am[9] = am[10] = 0xFF;   // 수식 없음 = -1
                            an = 11;
                        }
                        Broadcast(/*viaSeq=*/true, am, an, out);
                        ++appearanceSent_;
                    } else {
                        ++appearanceUnknown_;
                    }
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
                    back.equipBit = s.inventory[i].equipBit;
                    back.isWeapon = s.inventory[i].isWeapon;
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
            // 클라가 나간다 — 두 가지 길로 온다.
            //
            //   `0x0a CLIENT_CLOSE`     접속 자체를 닫는다
            //   `0xae OUTGOING_CLIENT`  **판에서 빠진다** (클라 원본 `network.go:77`)
            //
            // ★ `0xae` 는 `SendReliable` + `WaitServerResponse` 로 온다 — **답을 기다린다.**
            //   전에는 길이를 2 로 잘못 알아 조각째 버렸고(2026-09-11 63건), 답이 없으니
            //   클라가 깨끗하게 못 나갔다. 그것이 「재접속 안 됨」으로 보였다.
            case Op::OUTGOING_CLIENT:
            case Op::CLIENT_CLOSE: {
                s.closing = true;
                ++handled_;
                break;
            }

            case Op::TRY_SPELL: {
                // ★★ **진입 자체를 무조건 찍는다** (2026-09-11).
                //   논리로 후보를 다 배제했는데 현상이 남았다 — `0x79` 가 `seen_` 에는
                //   들어가는데(그 자리는 switch **직전**이다) 주문 카운터도 마나막힘도 0 이고
                //   문지기 진단조차 안 찍혔다. 어느 전제가 틀렸는지 **관측으로** 가른다.
                {
                    static int toldSpell = 0;
                    if (toldSpell < 6) {
                        ++toldSpell;
                        std::printf("[시전] 들어옴 — inGame=%d · 본문 %zu(%zu 여야 함) · 주문번호 %u · 비용 %u · 마나 %u\n",
                                    s.inGame ? 1 : 0, it.bodyLen(), kTrySpellBodyLen,
                                    it.bodyLen() >= 1 ? (unsigned)it.body()[0] : 999u,
                                    it.bodyLen() >= 1 ? (unsigned)SpellCost(it.body()[0]) : 999u,
                                    (unsigned)s.mana);
                        std::fflush(stdout);
                    }
                }
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

                // ① 시전을 알린다 — `70 <주문번호>` 2바이트.
                //
                // ★★★ **정정(2026-09-11) — 시전자에게만, 중요 메시지로.**
                //
                //   ⭕ 정품 송신 자리 `opennox/src/legacy/GAME4.c:2455-2463` —
                //       if (!*(uint8_t*)(v0 + 36)) {            // 몸짓 시전 중이 아닐 때
                //           v23[0] = 112;  v23[1] = <주문 번호>;
                //           addToMsgListCli(*(uint8_t*)(v5 + 2064), 1, v23, 2);
                //       }                  //         ↑ **시전자**의 플레이어 인덱스 · ↑ **1 = 중요**
                //     `v5` 는 시전자 → `+748` → `+276` 을 따라간 것이고 `+2064` 가 그 사람의
                //     플레이어 인덱스다. 즉 **그 한 사람에게만** 간다.
                //
                //   ⚠ 전에는 **판 안의 모두에게** `reliable=false` 로 보냈다. 둘 다 달랐다.
                //     받는 쪽(`opennox/src/network_client.go:274-279` → `Sub_49BB80(data[1])`)은
                //     **시전자 UI**(주문서·재사용 대기)로 보이는데, 남이 그것을 받으면
                //     **자기가 쓰지도 않은 주문의 상태**를 건드리게 된다.
                //     사용자가 겪은 「주문 쓰니까 튕김」의 가장 유력한 용의자다.
                //   ⚠ 앞서 `--no-spell-start` 로 가르려 한 실험은 **거짓 지문을 쓰던 때**라
                //     무효다(「조용해서 내보냄」이 접속 임시 포트도 가리켰다). 다시 재야 한다.
                if (!noSpellStart_)
                {
                    uint8_t msg[2];
                    msg[0] = static_cast<uint8_t>(Op::REPORT_SPELL_START);
                    msg[1] = spell;
                    EmitSeq(s, msg, sizeof(msg), out);   // 시전자에게만 · 중요
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
                        p.launchX = s.x;
                        p.launchY = s.y;
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
                if (it.bodyLen() > kInfoClassAt) {
                    const uint8_t c = it.body()[kInfoClassAt];
                    s.charClass = (c <= 2) ? static_cast<CharClass>(c) : CharClass::Warrior;
                    // ⚠ 레벨은 이 구조에 없다 — 온라인은 직업 최대 스탯을 그대로 준다.
                    s.charLevel = kMaxLevel;
                    // ★ **읽은 값을 찍는다** (2026-09-11). 화면에는 `Jack the Conjurer` 가
                    //   골라져 있는데 서버는 `마나0/0`(워리어 값)으로 봤다 — 어디서 어긋나는지
                    //   찍지 않으면 못 가른다. `원래 0x%02x` 가 51번 자리의 날바이트다.
                    std::printf("[직업] 원래 0x%02x → %s · 레벨 %u · 본문 %zu바이트\n",
                                c,
                                (c == 0 ? "워리어" : c == 1 ? "위저드" : c == 2 ? "컨저러" : "모름→워리어"),
                                (unsigned)s.charLevel, it.bodyLen());
                    std::fflush(stdout);
                } else {
                    // ⚠ 여기로 오면 직업을 **아예 못 읽는다** — 기본값(워리어)이 그대로 남는다.
                    std::printf("[직업] ✗ 본문이 %zu바이트뿐이라 직업을 못 읽었다 (%zu 넘어야 한다)\n",
                                it.bodyLen(), kInfoClassAt);
                    std::fflush(stdout);
                }
                if (s.maxHealth == 0) {
                    // ★ **온라인은 레벨을 안 본다** — 그 직업의 최대 스탯을 그대로 준다
                    //   (`GameBalance.h` HealthOnline 주석 · 정품이 접속자 레벨도 10 으로 민다).
                    s.maxHealth = static_cast<uint16_t>(HealthOnline(s.charClass));
                    s.maxMana   = static_cast<uint16_t>(ManaOnline(s.charClass));
                    s.strength  = StrengthOnline(s.charClass);
                    s.health = s.maxHealth;
                    s.mana   = s.maxMana;
                    // ★ 진단 — 들어올 때 체력을 낮춰 둔다(`--start-health N`).
                    //   ⚠ **최대 체력은 안 건드린다** — 그래야 부활 뒤 「최대로 돌아왔나」를 잰다.
                    //   왜 필요한가: 실클라 졸업시험이 90초 동안 안 죽어 「죽음·부활」을 못 쟀다
                    //   (거머리 2마리가 38 밖에 못 깎았다, 2026-09-11). 낮춰 두면 금방 죽는다.
                    if (startHealth_ > 0 && startHealth_ < s.maxHealth)
                        s.health = static_cast<uint16_t>(startHealth_);
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

// ════════════════════════════════════════════════════════════════════════════
// 벽 충돌 — 정답지 이식 (2026-09-13 묶음 2-b)
//
//   ★ 정답지 벽은 **칸이 아니라 칸 안의 대각선 선분**이다. 칸 원점 (23x, 23y) 에서
//     왼쪽 선분 = (0,0)→(23,23) 방향(\) · 오른쪽 선분 = (23,0)→(0,23) 방향(/).
//     방향 0~10 이 둘 중 무엇을, 어느 구간(0~23 · 0~11.5 · 11.5~23)만큼 쓰는지 표가 정한다.
//   전에는 벽 칸을 통째 네모로 막았다(CanStand) — 화면의 벽 선과 멈추는 자리가 달랐다.
// ════════════════════════════════════════════════════════════════════════════
namespace {
// `GAME5.c:53` table_313272 — [왼쪽 켬, 최소, 최대, 오른쪽 켬, 최소, 최대]
struct WallSeg { bool l; float lmin, lmax; bool r; float rmin, rmax; };
constexpr WallSeg kWallSegs[11] = {
    { false, 0, 0,     true,  0, 23 },
    { true,  0, 23,    false, 0, 0 },
    { true,  0, 23,    true,  0, 23 },
    { true,  0, 11.5f, true,  0, 23 },
    { true,  0, 23,    true,  11.5f, 23 },
    { true,  11.5f, 23, true, 0, 23 },
    { true,  0, 23,    true,  0, 11.5f },
    { true,  0, 11.5f, true,  11.5f, 23 },
    { true,  11.5f, 23, true, 11.5f, 23 },
    { true,  11.5f, 23, true, 0, 11.5f },
    { true,  0, 11.5f, true,  0, 11.5f },
};
// `0x587000+292496` 방향별 이웃 확인 비트 — 정답지 데이터 덩어리(`blob_587000.dat`)에서 읽은 값
constexpr uint8_t kWallNeighborBits[11] = { 9, 6, 15, 11, 7, 13, 14, 3, 5, 12, 10 };
constexpr float kGrid = 23.0f;
constexpr float kInvGrid = 0.043478262f;      // `GAME5.c:6610`
constexpr float kDiag = 0.70709997f;          // `GAME5.c:6735`
constexpr float kWallPush = 100.0f;           // `dword_587000_292492` = 100.0 (데이터 덩어리)

// sub_57CD30 · sub_57CD70 — 선분 p1→p2 와 칸 대각선의 교점
inline void CrossLeft(float x1, float y1, float x2, float y2, float cx, float cy, float& ox, float& oy) {
    const float dx = x2 - x1, dy = y2 - y1;
    const float v = ((cy - y1) * dx - (cx - x1) * dy) / (dy - dx);
    ox = cx + v; oy = cy + v;
}
inline void CrossRight(float x1, float y1, float x2, float y2, float cx, float cy, float& ox, float& oy) {
    const float dx = x2 - x1, dy = y2 - y1;
    const float v = ((cy - y1) * dx - (cx - x1) * dy) / (-dy - dx);
    ox = cx - v; oy = cy + v;
}
// sub_550280 — 왼쪽 대각선 위 가장 가까운 점(구간 끝은 a4·a5 가 허락할 때만 붙든다)
inline bool NearestOnLeft(float ox, float oy, float lo, float hi, bool a4, bool a5,
                          float px, float py, float& cx, float& cy) {
    float t = ((px - ox) + (py - oy)) * kDiag * kDiag;
    if (t < lo) { if (!a4) return false; t = lo; }
    if (t > hi) { if (!a5) return false; t = hi; }
    cx = t + ox; cy = t + oy;
    return true;
}
// sub_5502F0 — 오른쪽 대각선
inline bool NearestOnRight(float ox, float oy, float lo, float hi, bool a4, bool a5,
                           float px, float py, float& cx, float& cy) {
    float t = 23.0f - ((py - oy) * kDiag - (px - (ox + 23.0f)) * kDiag) * kDiag;
    if (t < lo) { if (!a4) return false; t = lo; }
    if (t > hi) { if (!a5) return false; t = hi; }
    cx = t + ox; cy = 23.0f - t + oy;
    return true;
}
// sub_550380 — 가까운 점까지가 반지름보다 가까우면 밀어낸다: 벽 쪽 속도 성분을 없애고
//   (반지름 − 거리) × 100 ÷ 질량 을 가속(+96 Pos24 = Sub548600)에 더한다.
//   ⚠ 공(GameBall) 예외·충돌 기록(collSysAddCollision)은 사람에게 안 쓰여 뺐다.
inline bool PushOut(double r, double mass, double px, double py, float cx, float cy,
                    double& vx, double& vy, double& ax, double& ay) {
    const float dx = static_cast<float>(px) - cx, dy = static_cast<float>(py) - cy;
    float len = std::sqrt(dx * dx + dy * dy);
    if (len == 0.0f) len = 0.0099999998f;
    if (len >= static_cast<float>(r)) return false;
    const float nx = dx / len, ny = dy / len;
    const double into = -ny * vy + -nx * vx;
    if (into > 0.0) { vx += into * nx; vy += into * ny; }
    const double push = (r - len) * kWallPush;
    ax += push * nx / mass;
    ay += push * ny / mass;
    return true;
}
}  // namespace

// sub_57B500 의 문·창·비밀 벽 없는 판 — 벽이 있으면 방향, 없으면 −1.
int Server::WallShapeAt(int gx, int gy) const {
    if (gx < 0 || gx >= 256 || gy < 0 || gy >= 256 || wallTiles_.empty()) return -1;
    const WallTile& w = wallTiles_[gy * 256 + gx];
    return w.present ? w.dir : -1;
}

// mapTraceRayImpl(`server/wall.go:1055`) — 이 칸의 벽 선분을 p1→p2 가 가로지르나.
bool Server::MapTraceRayTile(int gx, int gy, float x1, float y1, float x2, float y2, uint8_t flags) const {
    if (gx < 0 || gx >= 256 || gy < 0 || gy >= 256) return false;
    if (flags & 0x8) {   // Flag4 — 가까운 칸(60 안)은 Flag3 로
        const float v7 = gx * kGrid + 11.5f - x1, v8 = gy * kGrid + 11.5f - y1;
        if (v8 * v8 + v7 * v7 < 3600.0f) flags |= 0x4;
    }
    const int t = WallShapeAt(gx, gy);
    if (t < 0 || t > 10) return false;
    // GetWallAtGrid(Flag1 없음)은 칸 짝홀((x+y)&1)이 홀이면 벽이 없다고 본다(`server/wall.go:238`)
    if (!(flags & 0x1) && ((gx + gy) & 1)) return false;
    const WallTile& w = wallTiles_[gy * 256 + gx];
    const uint32_t f2 = (w.tile < wallDefFlags_.size()) ? wallDefFlags_[w.tile] : 0;
    if ((f2 & 0x2) || ((flags & 0x40) && !(f2 & 0x1))) return false;
    const float xl = std::min(x1, x2), xh = std::max(x1, x2);
    const float yl = std::min(y1, y2), yh = std::max(y1, y2);
    const float ox = gx * kGrid, oy = gy * kGrid;
    const WallSeg& s = kWallSegs[t];
    if (t != 0 && s.l) {
        float lx, ly;
        CrossLeft(x1, y1, x2, y2, ox, oy, lx, ly);
        if (ox + s.lmin <= lx && ox + s.lmax >= lx && lx >= xl && lx <= xh && ly >= yl && ly <= yh) return true;
    }
    if (t != 1 && s.r) {
        float rx, ry;
        CrossRight(x1, y1, x2, y2, ox + kGrid, oy, rx, ry);
        if (ox + s.rmin <= rx && ox + s.rmax >= rx && rx >= xl && rx <= xh && ry >= yl && ry <= yh) return true;
    }
    return false;
}

// MapTraceRayAt(`server/wall.go:898`) — p1→p2 가 벽에 안 막히면 참. 걸음·겨눔 모두 이 길.
//   ⚠ 정답지 그대로의 걸음(곧은 가로·세로 먼저, 그 밖은 기울기 따라 칸 줄 훑기).
bool Server::MapTraceRay(float x1, float y1, float x2, float y2, uint8_t flags) const {
    const int wx1 = static_cast<int>(x1 / kGrid), wy1 = static_cast<int>(y1 / kGrid);
    if (wx1 < 0 || wx1 >= 256 || wy1 < 0 || wy1 >= 256) return false;
    const int wx2 = static_cast<int>(x2 / kGrid), wy2 = static_cast<int>(y2 / kGrid);
    if (wx1 == wx2) {
        const int step = (y2 < y1) ? -1 : 1;
        if (wy1 == wy2 + step) return true;
        for (int yi = wy1; yi != wy2 + step; yi += step)
            if (MapTraceRayTile(wx1, yi, x1, y1, x2, y2, flags)) return false;
        return true;
    }
    if (wy1 == wy2) {
        const int step = (x2 < x1) ? -1 : 1;
        if (wx1 == wx2 + step) return true;
        for (int xi = wx1; xi != wx2 + step; xi += step)
            if (MapTraceRayTile(xi, wy1, x1, y1, x2, y2, flags)) return false;
        return true;
    }
    const float dx = x2 - x1, dy = y2 - y1;
    if (std::fabs(dy) >= std::fabs(dx)) {
        const float mulX = (x1 >= x2) ? -1.0f : 1.0f;
        const double stepY = (y1 >= y2) ? -1.0 : 1.0;
        const float slope = dy / dx;
        float xnext = 0;
        for (float x = x1; mulX * x <= mulX * x2; x = xnext + 0.1f * mulX) {
            const int xi = static_cast<int>(std::trunc(x / kGrid));
            float xmin = static_cast<float>(xi) * kGrid, xmax = static_cast<float>(xi + 1) * kGrid;
            if (mulX <= 0) std::swap(xmin, xmax);
            xnext = xmax;
            float ya = y1 + (xmin - x1) * slope;
            if ((stepY > 0 && ya < y1) || (stepY < 0 && ya > y1)) ya = y1;
            float yb = y1 + (xmax - x1) * slope;
            if ((stepY > 0 && yb > y2) || (stepY < 0 && yb < y2)) yb = y2;
            const int yi1 = static_cast<int>(std::trunc(static_cast<double>(ya / kGrid)) - stepY);
            const int yi2 = static_cast<int>(std::trunc(static_cast<double>(yb / kGrid)) + stepY);
            const int dd = (yi1 >= yi2) ? -1 : 1;
            for (int yi = yi1; yi != yi2 + dd; yi += dd)
                if (MapTraceRayTile(xi, yi, x1, y1, x2, y2, flags)) return false;
        }
        return true;
    }
    const float mulY = (y1 >= y2) ? -1.0f : 1.0f;
    const double stepX = (x1 >= x2) ? -1.0 : 1.0;
    const float slope = dx / dy;
    float ynext = 0;
    for (float y = y1; mulY * y <= mulY * y2; y = ynext + 0.1f * mulY) {
        const int yi = static_cast<int>(std::trunc(y / kGrid));
        float ymin = static_cast<float>(yi) * kGrid, ymax = static_cast<float>(yi + 1) * kGrid;
        if (mulY <= 0) std::swap(ymin, ymax);
        ynext = ymax;
        float xa = x1 + (ymin - y1) * slope;
        if ((stepX > 0 && xa < x1) || (stepX < 0 && xa > x1)) xa = x1;
        float xb = x1 + (ymax - y1) * slope;
        if ((stepX > 0 && xb > x2) || (stepX < 0 && xb < x2)) xb = x2;
        const int xi1 = static_cast<int>(std::trunc(static_cast<double>(xa / kGrid)) - stepX);
        const int xi2 = static_cast<int>(std::trunc(static_cast<double>(xb / kGrid)) + stepX);
        const int dd = (xi1 >= xi2) ? -1 : 1;
        for (int xi = xi1; xi != xi2 + dd; xi += dd)
            if (MapTraceRayTile(xi, yi, x1, y1, x2, y2, flags)) return false;
    }
    return true;
}

// sub_54FEF0 + sub_54FFC0 — 원 대 벽. 사각형(프레임 시작 자리 ± 반지름)이 덮는 칸마다
//   왼쪽·오른쪽 대각선에서 가장 가까운 점을 찾아 밀어낸다.
//   ⚠ a4·a5(선분 끝을 붙들까)는 **두 대각선 사이에서 이어진다** — 정답지가 두 번째 블록에서 되돌리지 않는다.
//   ⚠ 벽 소리·비밀 벽 열기(sub_548100)와 「닿은 벽」 기록(+296)은 뺐다 — 묶음 9·10.
bool Server::CircleVsWalls(double bx, double by, double r, double mass,
                           double nx, double ny, double& vx, double& vy, double& ax, double& ay) const {
    if (!wallsOn_ || wallTiles_.empty()) return false;
    const int x0 = static_cast<int>(static_cast<float>(bx - r) * kInvGrid);
    const int y0 = static_cast<int>(static_cast<float>(by - r) * kInvGrid);
    const int x1 = static_cast<int>(static_cast<float>(bx + r) * kInvGrid);
    const int y1 = static_cast<int>(static_cast<float>(by + r) * kInvGrid);
    const float px = static_cast<float>(nx), py = static_cast<float>(ny);
    bool any = false;
    for (int gy = y0; gy <= y1; ++gy) {
        for (int gx = x0; gx <= x1; ++gx) {
            const int d = WallShapeAt(gx, gy);
            if (d < 0 || d > 10) continue;
            const WallSeg& s = kWallSegs[d];
            const uint8_t nb = kWallNeighborBits[d];
            const float ox = gx * kGrid, oy = gy * kGrid;
            bool a4 = false, a5 = false;
            float cx = 0, cy = 0;
            if (s.l) {
                if ((nb & 2) && WallShapeAt(gx - 1, gy - 1) == -1) a4 = true;
                if ((nb & 4) && WallShapeAt(gx + 1, gy + 1) == -1) a5 = true;
                bool got;
                if (d == 8 || d == 9) {
                    a4 = true;
                    got = NearestOnLeft(ox, oy, s.lmin, s.lmax, true, a5, px, py, cx, cy);
                } else {
                    if (d == 7 || d == 10) a5 = true;
                    got = NearestOnLeft(ox, oy, s.lmin, s.lmax, a4, a5, px, py, cx, cy);
                }
                if (got && PushOut(r, mass, nx, ny, cx, cy, vx, vy, ax, ay)) any = true;
            }
            if (s.r) {
                if ((nb & 8) && WallShapeAt(gx - 1, gy + 1) == -1) a4 = true;
                if ((nb & 1) && WallShapeAt(gx + 1, gy - 1) == -1) a5 = true;
                bool got;
                if (d == 9 || d == 10) {
                    got = NearestOnRight(ox, oy, s.rmin, s.rmax, a4, true, px, py, cx, cy);
                } else {
                    if (d == 7 || d == 8) a4 = true;
                    got = NearestOnRight(ox, oy, s.rmin, s.rmax, a4, a5, px, py, cx, cy);
                }
                if (got && PushOut(r, mass, nx, ny, cx, cy, vx, vy, ax, ay)) any = true;
            }
        }
    }
    return any;
}

// ════════════════════════════════════════════════════════════════════════════
// 오브젝트끼리 충돌 — 정답지 이식 (2026-09-13 묶음 2-c)
// ════════════════════════════════════════════════════════════════════════════
namespace {
constexpr uint32_t kClsMonster = 0x2, kClsPlayer = 0x4, kClsDoor = 0x80, kClsElevator = 0x4000, kClsShaft = 0x8000;
constexpr uint32_t kFlBelow = 0x1, kFlAllowOverlap = 0x8, kFlShort = 0x10, kFlDestroyed = 0x20, kFlNoCollide = 0x40,
                   kFlNoPushChars = 0x2000, kFlAirborne = 0x4000, kFlFalling = 0x20000;
constexpr float kBodyPush = 40.0f;   // `dword_587000_292488` = 40.0 (데이터 덩어리)
// 딱 겹쳤을 때 고르는 방향 넷 — `0x587000+292784` · `+289928` (둘 다 ±0.1 축 벡터)
constexpr float kNudge[4][2] = { { 0.1f, 0.0f }, { -0.1f, 0.0f }, { 0.0f, 0.1f }, { 0.0f, -0.1f } };

// 몸의 충돌 사각형(+232~+244, `GAME3_3.c:2508` objectUnkUpdateCoords) — 프레임 시작 자리 기준
inline void BodyRect(const Server::PhysBody& b, double x, double y,
                     double& x0, double& y0, double& x1, double& y1) {
    if (b.shape == 3) {
        x0 = x + b.boxLB; y0 = y + b.boxLB; x1 = x + b.boxRT; y1 = y + b.boxRT;
    } else {
        x0 = x - b.r; y0 = y - b.r; x1 = x + b.r; y1 = y + b.r;
    }
}

// sub_54A990 — 원(중심 p, 반지름 r)과 상자(자리 bx,by)의 겹침. 반환 ≥ 0 이면 닿음, n 은 상자→원 방향.
//   상자 꼭짓점 넷: A (LT,LB) · B (LB2,LT2) · C (RT,RB) · D (RB2,RT2) — LT2=LT, LB2=LB, RT2=RT, RB2=RB.
double BoxOverlap(float px, float py, float r, const Server::PhysBody& b, float bx, float by, float& nx, float& ny) {
    const float ax = b.boxLT + bx, ay = b.boxLB + by;
    const float cbx = b.boxLB + bx, cby = b.boxLT + by;
    const float ccx = b.boxRT + bx, ccy = b.boxRB + by;
    const float cdx = b.boxRB + bx, cdy = b.boxRT + by;
    const float v31 = (ay - ax + px - py) * kDiag;
    const float v29 = (cby - cbx + px - py) * kDiag;
    const float v17 = (ccy + ccx - px - py) * kDiag;
    const float v18 = (ay + ax - px - py) * kDiag;
    int k = 0;
    if (v31 <= 0.0f) { if (v29 < 0.0f) k = 2; } else { k = 1; }
    if (v17 >= 0.0f) { if (v18 > 0.0f) k |= 4; } else { k |= 8; }
    auto corner = [&](float qx, float qy) -> double {
        const float dx = px - qx, dy = py - qy;
        float len = std::sqrt(dy * dy + dx * dx);
        if (len == 0.0f) len = 0.1f;
        const double res = r - len;
        if (res >= 0.0) { nx = dx / len; ny = dy / len; }
        return res;
    };
    switch (k) {
        case 0: {
            float dx = px - bx, dy = py - by;
            if (dx == 0.0f && dy == 0.0f) { const int i = std::rand() % 4; dx = kNudge[i][0]; dy = kNudge[i][1]; }
            double len = std::sqrt(dy * dy + dx * dx);
            if (len == 0.0) len = 0.1;
            nx = static_cast<float>(dx / len); ny = static_cast<float>(dy / len);
            return len;
        }
        case 1: nx = kDiag;  ny = -kDiag; return r - v31;
        case 2: nx = -kDiag; ny = kDiag;  return r + v29;
        case 4: nx = -kDiag; ny = -kDiag; return r - v18;
        case 5: return corner(ax, ay);
        case 6: return corner(cbx, cby);
        case 8: nx = kDiag;  ny = kDiag;  return r + v17;
        case 9: return corner(ccx, ccy);
        case 10: return corner(cdx, cdy);
        default: return -1.0;
    }
}
}  // namespace

// sub_548220(거르기 sub_548360 + 모양별 나누기) → 원 대 원(550D00) · 원 대 상자(54AD50, a3=0).
//   ⚠ 뺀 것: 오각형·염력손·검은가루 예외 · Trigger 종류 예외 · NO_COLLIDE_OWNER 같은 편 · 문(5488B0/551250)·
//     승강기(551AE0/551C40)·상자 대 상자(550F80) · 충돌 기록(collSysAddCollision → 콜백 548740) — 계기가 오는 묶음에서.
void Server::BodyVsBody(const PhysBody& a1, double nx, double ny, double bx, double by, double vx, double vy,
                        const PhysBody& a2, double ox, double oy, double obx, double oby, double ovx, double ovy,
                        double& ax, double& ay) const {
    // 거르기 (sub_548360 에서 옮긴 것만)
    if ((a1.flags & (kFlDestroyed | kFlNoCollide)) || (a2.flags & (kFlDestroyed | kFlNoCollide))) return;
    if ((a1.cls & kClsDoor) && (a2.flags & (kFlBelow | kFlAllowOverlap))) return;
    if ((a2.cls & kClsDoor) && (a1.flags & (kFlBelow | kFlAllowOverlap))) return;
    if ((a1.flags & kFlAllowOverlap) && (a2.flags & kFlAllowOverlap)) return;
    if (((a1.flags & (kFlBelow | kFlShort)) && (a2.flags & (kFlAirborne | kFlFalling))) ||
        ((a2.flags & (kFlBelow | kFlShort)) && (a1.flags & (kFlAirborne | kFlFalling)))) return;
    if ((a1.cls & kClsDoor) && (a2.cls & kClsDoor)) return;
    if ((a1.cls & kClsElevator) && (a2.cls & kClsElevator)) return;
    if ((a1.cls & kClsShaft) && (a2.cls & kClsShaft)) return;
    if ((a1.cls | a2.cls) & (kClsDoor | kClsElevator | kClsShaft)) return;   // ⚠ 문·승강기 충돌은 묶음 9
    if (a1.shape != 2) return;   // 사람·소환수는 원이다

    const bool losPair = (a1.cls & 0x2204) && (a2.cls & 0x2204);
    if (a2.shape == 2) {
        // ── 원 대 원 (`GAME5.c:7183`) ──
        float dx = static_cast<float>(ox - nx), dy = static_cast<float>(oy - ny);
        if (dx == 0.0f && dy == 0.0f) { const int i = std::rand() % 4; dx = kNudge[i][0]; dy = kNudge[i][1]; }
        float len = std::sqrt(dy * dy + dx * dx);
        if (len == 0.0f) len = 0.0099999998f;
        const float overlap = a1.r + a2.r - len;
        if (overlap <= 0.0f) return;
        if (losPair && !MapTraceRay(static_cast<float>(bx), static_cast<float>(by),
                                    static_cast<float>(obx), static_cast<float>(oby), 0)) return;
        bool push = !((a1.flags & kFlAllowOverlap) || (a2.flags & kFlAllowOverlap));
        if ((!(a1.cls & 6) || (a2.flags & kFlNoPushChars) != kFlNoPushChars) && push) {
            const double unx = dx / len, uny = dy / len;
            const double dvx = vx - ovx, dvy = vy - ovy;
            const double mmin = std::min(a1.mass, a2.mass);
            const double k = kBodyPush * overlap;
            double px = -(k * unx), py = -(k * uny);
            if (!(a1.cls & 6) || !(a2.cls & 6)) {
                const double t = -uny * dvx + dvy * unx;
                px -= t * -uny * mmin * 0.69999999;
                py -= t * mmin * unx * 0.69999999;
            }
            ax += px / a1.mass;
            ay += py / a1.mass;
        }
        return;
    }
    if (a2.shape == 3) {
        // ── 원 대 상자 (`GAME5.c:3465`, a3 = 0 — 원 쪽을 민다) ──
        float unx = 0, uny = 0;
        const double over = BoxOverlap(static_cast<float>(nx), static_cast<float>(ny), a1.r, a2,
                                       static_cast<float>(ox), static_cast<float>(oy), unx, uny);
        if (over < 0.0) return;
        if (losPair && !MapTraceRay(static_cast<float>(bx), static_cast<float>(by),
                                    static_cast<float>(obx), static_cast<float>(oby), 0)) return;
        const bool push = !((a1.flags & kFlAllowOverlap) || (a2.flags & kFlAllowOverlap));
        if ((!(a1.cls & 6) || !(a2.flags & kFlNoPushChars)) && push) {
            const double k = kBodyPush * over;
            const double dvx = vx - ovx, dvy = vy - ovy;
            const double tx = -uny, ty = unx;
            const double t = tx * dvx + dvy * unx;
            const double mmin = std::min(a1.mass, a2.mass);
            const double v16 = mmin * t;
            const double px = unx * k - v16 * tx * 0.69999999;
            const double py = uny * k - v16 * ty * 0.69999999;
            ax += px / a1.mass;
            ay += py / a1.mass;
        }
    }
}

// 진단 — (x,y) 에서 가장 가까운 벽 선분까지 거리(±3칸 안). 벽 없으면 −1.
double Server::DebugWallDistance(double x, double y, int* outGx, int* outGy, int* outDir) const {
    double best = -1.0;
    const int cx = static_cast<int>(x / kGrid), cy = static_cast<int>(y / kGrid);
    for (int gy = cy - 3; gy <= cy + 3; ++gy) {
        for (int gx = cx - 3; gx <= cx + 3; ++gx) {
            const int d = WallShapeAt(gx, gy);
            if (d < 0 || d > 10) continue;
            const WallSeg& s = kWallSegs[d];
            const float ox = gx * kGrid, oy = gy * kGrid;
            float px, py;
            auto take = [&](float qx, float qy) {
                const double dd = std::sqrt((x - qx) * (x - qx) + (y - qy) * (y - qy));
                if (best < 0 || dd < best) {
                    best = dd;
                    if (outGx) *outGx = gx;
                    if (outGy) *outGy = gy;
                    if (outDir) *outDir = d;
                }
            };
            if (s.l && NearestOnLeft(ox, oy, s.lmin, s.lmax, true, true, static_cast<float>(x), static_cast<float>(y), px, py)) take(px, py);
            if (s.r && NearestOnRight(ox, oy, s.rmin, s.rmax, true, true, static_cast<float>(x), static_cast<float>(y), px, py)) take(px, py);
        }
    }
    return best;
}

// 사람 자세 번호 — 정답지는 **상태**에서 뽑는다(`GAME4.c:1709` nox_common_mapPlrActionToStateId_4FA2B0:
//   0 걷기 → 4 · 5 달리기 → 6 · 13 가만 → 0 · 1 공격 → 애니 번호(맨손이면 +8 Field8)).
//   전에는 지난 틱과 자리를 견줘 움직였으면 걷기(4)로 봤다(`animMoving`) — 달리기(6)가 없었고
//   멈춘 뒤 미끄러지는 동안에도 걷는 자세였다.
//   ⚠ 무기 든 공격(0x47F0000 → 29/31 · 그 밖 sub_4FA280)·가만(13)의 대검 0x26·죽음 등은 그 조각(3-b·3-c)에서 채운다.
uint8_t Server::PlayerAnimByte(const Session& s) {
    const uint32_t equip = WeaponEquipOf(s);
    switch (s.moveState) {
        case kPlayerStateWalk: return kAnimWalk;
        case kPlayerStateRun: return kAnimRun;
        case kPlayerStateAttack: case 14: case 22: {
            // `:1720~1737` — 마법 지팡이(0x47F0000)는 29/31 · 무기(0·1 아님)거나 Field8 이 0 이면 sub_4FA280 표 · 맨손은 Field8
            if (equip & 0x47F0000) return 31;   // ⚠ +96&2(충전 휘두르기) 면 29 — 마법 지팡이는 묶음 4
            if ((equip && equip != 1) || s.attackAnim == 0) {
                // sub_4FA280 — 비트 2 부터 가장 낮은 켜진 비트 → blob_587000 215824 표
                static const uint8_t kWeaponAnim[27] = { 0, 0, 33, 34, 0, 0, 44, 44, 27, 28, 37, 26, 35, 39, 39,
                                                         29, 31, 31, 31, 31, 31, 31, 31, 32, 32, 32, 31 };
                for (int b = 2; b < 27; ++b) if (equip & (1u << b)) return kWeaponAnim[b];
                return 0;
            }
            return s.attackAnim;
        }
        case kPlayerStateIdle: return (equip & 0x400) ? 0x26 : kAnimIdle;   // 대검이면 GREAT_SWORD_IDLE(`:1756~1758`)
        case 30: return 52;                                                  // 움찔 RECOIL(`:1793`)
        case 15: case 16: case 17: return 40;                                // RAISE_SHIELD(`:1759~1763`)
        case 18: return 48;                                                  // GREAT_SWORD_BLOCK_LEFT
        case 19: return 49;                                                  // GREAT_SWORD_BLOCK_RIGHT
        case 20: return 47;                                                  // GREAT_SWORD_BLOCK_DOWN
        case 21: return 30;                                                  // STAFF_BLOCK
        default: return kAnimIdle;
    }
}
// 칸 번호(+236)를 싣는 상태 — `GAME4_1.c:4447~4453` 목록(1·2·10·14~22·24~30·32). 지금 들어오는 것은 1 뿐이다.
bool Server::PlayerAnimFrameSent(const Session& s) {
    switch (s.moveState) {
        case 1: case 2: case 10: case 14: case 15: case 16: case 17: case 18: case 19: case 20:
        case 21: case 22: case 24: case 25: case 26: case 27: case 28: case 29: case 30: case 32:
            return true;
        default:
            return false;
    }
}

// 사람 걷기 한 틱 — 정답지 순서 그대로 (묶음 2-a, 2026-09-13).
//
//   ① 힘(ImplA · `object_update.go:133~192`)  상태가 걷기(0)·달리기(5)이고 움직일 수 있으면
//      달리기 = 이미 달리는 중이거나 커서가 몸에서 100 밖 → 속도 ×2 · 힘 = 바라보는 쪽 × 속도
//   ② 상태(ImplB · `:480~534`)  이번 틱 조작이 있으면 — 걷던 중이면 일단 가만(13)으로,
//      이동 코드가 있었으면 ①의 달리기 여부로 걷기/달리기. 조작이 하나도 없으면 상태를 그대로 둔다.
//      ⚠ 입력은 **다음 틱 힘**으로 나타난다(정답지도 ImplA 가 ImplB 보다 먼저 돈다).
//   ③ 적분(`server.go:146` · `GAME4_1.c:3218~3238`)  0.2 씩 5번 — 속도 += (힘 − 속도×0.5)×0.2, 자리 += 속도×0.2
//
//   ★ 벽 — 묶음 2-b(2026-09-13)에서 정답지로 바꿨다: 걸음마다 원 대 벽 밀어내기 + 선 추적(아래 ③).
//     반지름 kBodyRadius(thing.bin NewPlayer EXTENT 10) · 질량 kPlayerMass(MASS 1).
//   ⚠ 죽었거나(정답지 상태 3·4 는 ImplB 를 건너뜀) 관전(Field3680&3)이면 힘도 상태 변경도 없다 — 속도만 잦아든다.
//   ★ 2026-09-13 묶음 3-a — ① 에 상태 1(공격: PlayerAttack), ② 에 CCAction(PlayerInputAttack)이 더해졌다.
//     바라보는 쪽(CCOrientation)도 ② 에서 갈아 끼운다 — 전에는 받자마자 바꿔 ① 힘이 한 틱 일찍 새 방향을 썼다.
void Server::UpdatePlayerMovement(uint32_t frame, std::vector<OutPacket>& out) {
    // ★ 2026-09-13 묶음 2-c — 두 단계로 나눴다. 정답지는 한 걸음(0.2)마다 **움직이는 모두의 충돌을 먼저** 돌리고
    //   (`GAME4_1.c:3195` sub_5481C0 루프) **그다음 모두를 적분**한다(`:3200` 루프). 사람 하나씩 5걸음을 다 돌리면
    //   뒤에 도는 사람이 앞사람의 **끝난 자리**와 부딪혀 순서가 결과를 바꾼다.
    struct Mover { Session* s; double fx, fy, bx, by, ax, ay; bool collide; };
    std::vector<Mover> movers;
    // 밖에서 x·y 를 바꿨으면(접속·부활·진단 옮기기) 실수 자리를 거기에 맞추고 멈춘다.
    //   ★ 3-a — **모두 먼저** 맞춘다. 앞사람의 공격 판정이 뒷사람의 옛 실수 자리를 보면 안 된다.
    for (auto& kv : sessions_) {
        Session& s = kv.second;
        if (!s.inGame) continue;
        if (std::lround(s.posX) != s.x || std::lround(s.posY) != s.y) {
            s.posX = s.x; s.posY = s.y;
            s.velX = s.velY = 0.0;
        }
    }
    for (auto& kv : sessions_) {
        Session& s = kv.second;
        if (!s.inGame) continue;
        const bool dead = (s.respawnAtFrame != 0);
        const bool canMove = !dead && !s.observer;

        // ⓪ 기력 회복(`object_update.go:108` — 죽지 않았으면 100 아래일 때 100/30 씩, ImplA 보다 먼저)
        if (!dead && s.stamina < 100) s.stamina = static_cast<uint8_t>(s.stamina + 100 / kServerTickRate);
        // ① 힘 · 공격(ImplA)
        double fx = 0.0, fy = 0.0;
        bool run = false;
        if (canMove && (s.moveState == kPlayerStateWalk || s.moveState == kPlayerStateRun)) {
            const double cdx = s.aimX - s.posX, cdy = s.aimY - s.posY;
            run = !(s.moveState != kPlayerStateRun && cdx * cdx + cdy * cdy <= kRunCursorDist * kRunCursorDist);
            const double speed = kPlayerWalkPerFrame * (run ? 2.0 : 1.0);
            const double rad = s.facing * 2.0 * 3.14159265358979 / 256.0;
            fx += std::cos(rad) * speed;
            fy += std::sin(rad) * speed;
        } else if (canMove && s.moveState == kPlayerStateAttack) {
            // `object_update.go:224~234` — 0 이 오면 활(WeaponEquip&4)이면 14 · Field34 = 프레임, 아니면 13 · Field8 지움
            if (!PlayerAttack(s, frame, out)) {
                if (WeaponEquipOf(s) & 0x4) {
                    PlayerSetState(s, 14, frame);
                    s.stateFrame = frame;
                } else {
                    PlayerSetState(s, kPlayerStateIdle, frame);
                    s.attackAnim = 0;
                }
            }
        } else if (canMove && s.moveState == 30) {
            // 움찔(3-e) — `object_update.go:454~461` RECOIL(52) 칸이 끝나면 13
            const int frames = playerAnimFrames_[52][0], hold = playerAnimFrames_[52][1];
            s.animFrame = static_cast<uint8_t>((frame - s.stateFrame) / static_cast<uint32_t>(hold + 1));
            if (s.animFrame >= frames) PlayerSetState(s, kPlayerStateIdle, frame);
        } else if (canMove && s.moveState == kPlayerStateIdle) {
            // 가만(13) — `object_update.go:327~338` 방패를 들었으면 들어오는 투사체가 있거나 가만히 30/4 틱을 넘기면 올리기(15)
            //   ⚠ 웨이포인트(sub_4F9A80)는 묶음 6 · 채팅 모드는 없음
            if ((ArmorEquipOf(s) & 0x3000000) &&
                !(!IncomingMissile(s) && (frame - s.stateFrame) <= kServerTickRate / 4)) {
                PlayerSetState(s, 15, frame);
                s.animFrame = 0;
            }
        } else if (canMove && s.moveState == 15) {
            // 방패 올리는 중 — RAISE_SHIELD(40) 칸이 끝나면 16(`:346~353`)
            const int frames = playerAnimFrames_[40][0], hold = playerAnimFrames_[40][1];
            s.animFrame = static_cast<uint8_t>((frame - s.stateFrame) / static_cast<uint32_t>(hold + 1));
            if (s.animFrame >= frames) {
                PlayerSetState(s, 16, frame);
                s.animFrame = static_cast<uint8_t>(frames - 1);
            }
        } else if (canMove && s.moveState == 16) {
            s.animFrame = static_cast<uint8_t>(playerAnimFrames_[40][0] - 1);   // 올린 채(`:354~357`)
        } else if (canMove && s.moveState == 17) {
            // 방패 내리는 중 — 칸을 거꾸로, 0 이면 13(`:358~370`)
            const int frames = playerAnimFrames_[40][0], hold = playerAnimFrames_[40][1];
            int v11 = frames - static_cast<int>((frame - s.stateFrame) / static_cast<uint32_t>(hold + 1));
            if (v11 >= frames) {
                s.animFrame = static_cast<uint8_t>(frames - 1);
            } else {
                if (v11 <= 0) {
                    v11 = 0;
                    PlayerSetState(s, kPlayerStateIdle, frame);
                }
                s.animFrame = static_cast<uint8_t>(v11);
            }
        } else if (canMove && (s.moveState >= 18 && s.moveState <= 21)) {
            // 대검 막기 18·19·20(GREAT_SWORD_BLOCK_LEFT/RIGHT/DOWN 48·49·47) · 지팡이 막기 21(STAFF_BLOCK 30) — 칸 끝나면 13(`:371~398`)
            static const int kBlockAnim[4] = { 48, 49, 47, 30 };
            const int anim = kBlockAnim[s.moveState - 18];
            const int frames = playerAnimFrames_[anim][0], hold = playerAnimFrames_[anim][1];
            s.animFrame = static_cast<uint8_t>((frame - s.stateFrame) / static_cast<uint32_t>(hold + 1));
            if (s.animFrame >= frames) PlayerSetState(s, kPlayerStateIdle, frame);
        }
        // ② 조작(ImplB) — 목록이 비었으면 아무것도 안 한다(`object_update.go:485`)
        if (canMove && s.inputThisTick) {
            if (s.moveState == kPlayerStateWalk || s.moveState == kPlayerStateRun) {
                PlayerSetState(s, kPlayerStateIdle, frame);
                s.stateFrame = frame;
            }
            if (s.hasFacingInput) s.facing = s.facingInput;   // CCOrientation(`:497~502`, 얼림·광전사 없음)
            // 이동 — 맨손 공격 중(상태 1)에는 안 먹는다(`:506~507`) · 방패 올린 채(16)면 먼저 내린다(17, `:513~514`)
            if (s.moveInputThisTick && s.moveState != kPlayerStateAttack) {
                if (s.moveState == 16) {
                    PlayerSetState(s, 17, frame);
                } else {
                    PlayerSetState(s, run ? kPlayerStateRun : kPlayerStateWalk, frame);
                    s.stateFrame = frame;
                }
            }
            // CCAction(`:536~544`) — playerCanAttack(버프 25 없음 · 상태 23 아님) → 공 차기 아니면 playerInputAttack
            if (s.actionThisTick && s.moveState != 23) PlayerInputAttack(s, frame, out);
        }
        if (dead) s.moveState = kPlayerStateIdle;
        s.inputThisTick = s.moveInputThisTick = s.actionThisTick = s.hasFacingInput = false;

        // ⚠ 죽으면 정답지는 ALLOW_OVERLAP 을 켜고(상태 3→4, `object_update.go:247`) 관전은 NO_COLLIDE 를 켠다
        //   (`player.go:156`) — 둘 다 벽·오브젝트 충돌을 안 받는다(sub_5481C0 의 0x60 · 0x8 검사).
        movers.push_back({ &s, fx, fy, s.posX, s.posY, 0.0, 0.0, !dead && !s.observer });
    }
    // 남이 이번 틱에 준 힘(훅 20 — objectApplyForce_52DF80 → ForceVec)은 **모두의 갱신이 끝난 뒤** 적분에 들어간다
    //   (`server.go:135` updateUnits: BBB 갱신 → updateCollide 적분 → CCC 에서 ForceVec 지움). 순서가 결과를 안 바꾸게 여기서 더한다.
    for (auto& m : movers) {
        m.fx += m.s->forceX; m.fy += m.s->forceY;
        m.s->forceX = m.s->forceY = 0.0;
    }

    // ③ 적분 — 걸음(0.2)마다 [모두 충돌] → [모두 적분]. 사각형 칸 범위는 **프레임 시작 자리**(정답지 +232 는 +56 PosVec).
    PhysBody playerBody;
    playerBody.shape = 2; playerBody.r = static_cast<float>(kBodyRadius);
    playerBody.cls = kClsPlayer; playerBody.flags = 0; playerBody.mass = static_cast<float>(kPlayerMass);   // NewPlayer FLAGS NULL
    for (int i = 0; i < kMoveSubSteps; ++i) {
        for (auto& m : movers) {
            Session& s = *m.s;
            m.ax = m.ay = 0.0;
            if (!m.collide) continue;
            double qx0, qy0, qx1, qy1;
            BodyRect(playerBody, m.bx, m.by, qx0, qy0, qx1, qy1);
            // 오브젝트 먼저(getUnitsInRect → sub_548220) — 상대 사각형과 겹칠 때만(`server/map.go:302`)
            for (const auto& o : obstacles_) {
                double ox0, oy0, ox1, oy1;
                BodyRect(o, o.x, o.y, ox0, oy0, ox1, oy1);
                if (!(ox0 < qx1 && ox1 > qx0 && oy0 < qy1 && oy1 > qy0)) continue;
                BodyVsBody(playerBody, s.posX, s.posY, m.bx, m.by, s.velX, s.velY,
                           o, o.x, o.y, o.x, o.y, 0.0, 0.0, m.ax, m.ay);
            }
            for (const auto& m2 : movers) {
                if (m2.s == m.s || !m2.collide) continue;
                double ox0, oy0, ox1, oy1;
                BodyRect(playerBody, m2.bx, m2.by, ox0, oy0, ox1, oy1);
                if (!(ox0 < qx1 && ox1 > qx0 && oy0 < qy1 && oy1 > qy0)) continue;
                BodyVsBody(playerBody, s.posX, s.posY, m.bx, m.by, s.velX, s.velY,
                           playerBody, m2.s->posX, m2.s->posY, m2.bx, m2.by, m2.s->velX, m2.s->velY, m.ax, m.ay);
            }
            for (const auto& k : pets_) {
                auto tb = typeBodies_.find(k.type);
                if (tb == typeBodies_.end()) continue;   // 몸을 모르는 종류는 안 부딪힌다(지어내지 않는다)
                if (!tb->second.hasCollide || (tb->second.shape != 2 && tb->second.shape != 3)) continue;   // COLLIDE 없음 · 몸 없음
                double ox0, oy0, ox1, oy1;
                BodyRect(tb->second, k.x, k.y, ox0, oy0, ox1, oy1);
                if (!(ox0 < qx1 && ox1 > qx0 && oy0 < qy1 && oy1 > qy0)) continue;
                BodyVsBody(playerBody, s.posX, s.posY, m.bx, m.by, s.velX, s.velY,
                           tb->second, k.x, k.y, k.x, k.y, 0.0, 0.0, m.ax, m.ay);
            }
            // 벽 — 오브젝트 다음(sub_5481C0 순서)
            CircleVsWalls(m.bx, m.by, kBodyRadius, kPlayerMass, s.posX, s.posY, s.velX, s.velY, m.ax, m.ay);
        }
        for (auto& m : movers) {
            Session& s = *m.s;
            s.velX += ((m.ax + m.fx) - s.velX * kPlayerDamping) * kMoveSubStep;
            s.velY += ((m.ay + m.fy) - s.velY * kPlayerDamping) * kMoveSubStep;
            double nx = s.posX + s.velX * kMoveSubStep;
            double ny = s.posY + s.velY * kMoveSubStep;
            if (nx < 0) nx = 0;
            if (ny < 0) ny = 0;
            if (nx > kMaxCoord) nx = kMaxCoord;
            if (ny > kMaxCoord) ny = kMaxCoord;
            if (MapTraceRay(static_cast<float>(s.posX), static_cast<float>(s.posY),
                            static_cast<float>(nx), static_cast<float>(ny), 0x1)) {
                s.posX = nx; s.posY = ny;
            }
        }
    }
    for (auto& m : movers) {
        m.s->x = static_cast<int>(std::lround(m.s->posX));
        m.s->y = static_cast<int>(std::lround(m.s->posY));
        m.s->facingPrev = m.s->facing;   // CCC: Direction1 = Direction2(`server.go:167`)
        m.s->prevX = m.bx; m.s->prevY = m.by;   // CCC: PrevPos(+72) = 이번 틱 시작 자리(movers 는 게임 안 모두 — 관전·죽음 포함)
    }
}

// ════════════════════════════════════════════════════════════════════════
// 공격 (묶음 3-a, 2026-09-13) — 정답지 이식
//
//   흐름  클릭(CCAction) → ImplB playerInputAttack(`GAME4.c:1588`) → 상태 1
//         → 틱마다 ImplA playerAttack(`GAME4_3.c:3855`) — 애니 칸이 끝나는 틱에 playerTraceAttack(`:3624`)
//         → 0 을 돌려주면 상태 13(`object_update.go:224~234`)
//   판정  sub_518040 사각형 후보(`GAME4_1.c:4091` · 거리 조건 sub_5180B0) → sub_5386A0 앞쪽 가장 가까운 것(몬스터 우선)
//         → sub_538510 방향 표 · 깃발 · 선 추적(벽 0x5) → 피해 함수(+716) · 훅이면 힘 20
//   ⚠ 빠진 것(다른 조각)  무기 갈래 전부(3-b) · 피해 함수 정답지(3-e: 지금은 HurtTarget) · 벽 부수기 mapDamageToWalls(9)
//     · 빗나감 소리 879(10) · 소환수 밀기(몬스터 물리 6) · 버프(4) · 공 차기 checkWinkFlags(깃발공 모드 없음)
//     · 커서 아래 물건(아이템 몸 표 없음)
// ════════════════════════════════════════════════════════════════════════
namespace {


// `server/math.go:49` PointOnTheLine — p 를 선분에 내린 발, 선분 상자 안이면 참
bool PointOnTheLine(float x1, float y1, float x2, float y2, float px, float py, float& ox, float& oy) {
    const double dpx = x2 - x1, dpy = y2 - y1;
    const double d2 = dpy * dpy + dpx * dpx;
    const double dd = static_cast<double>(py - y1) * dpy + static_cast<double>(px - x1) * dpx;
    ox = static_cast<float>(dpx * dd / d2 + x1);
    oy = static_cast<float>(dpy * dd / d2 + y1);
    return std::min(x1, x2) <= ox && ox <= std::max(x1, x2) && std::min(y1, y2) <= oy && oy <= std::max(y1, y2);
}
// `server/map.go:606` LineTraceXxx — 두 선분(각자 사각형 두 점)이 걸치나
bool LineTraceXxx(float ax0, float ay0, float ax1, float ay1, float bx0, float by0, float bx1, float by1) {
    const float r1x0 = std::min(ax0, ax1), r1x1 = std::max(ax0, ax1), r1y0 = std::min(ay0, ay1), r1y1 = std::max(ay0, ay1);
    const float r2x0 = std::min(bx0, bx1), r2x1 = std::max(bx0, bx1), r2y0 = std::min(by0, by1), r2y1 = std::max(by0, by1);
    if (r1x1 < r2x0 || r1x0 > r2x1 || r1y1 < r2y0 || r1y0 > r2y1) return false;
    if (r1y0 == r1y1 && r2y0 == r2y1) return true;
    const float a1w = r1x1 - r1x0, a1h = r1y1 - r1y0, a2w = r2x1 - r2x0, a2h = r2y1 - r2y0;
    const float dx = r2x0 - r1x0, dy = r2y0 - r1y0;
    const float dd1 = dy * a1w - dx * a1h;
    const float dd2 = a2w * a1h - a2h * a1w;
    const float dd3 = dy * a2w - dx * a2h;
    if (dd1 == 0.0f || dd2 == 0.0f || (dd1 < 0.0f && dd2 > 0.0f)) return false;
    if ((dd1 > 0.0f && dd2 < 0.0f) || std::fabs(dd2) < std::fabs(dd1) ||
        (dd3 < 0.0f && dd2 > 0.0f) || (dd3 > 0.0f && dd2 < 0.0f) || std::fabs(dd2) < std::fabs(dd3)) return false;
    return true;
}
}  // namespace

// `GAME4_3.c:2586` monsterTestBlockShield_533E70 + sub_533EB0 — 100 안의 투사체 중 **앞쪽에서 나를 향해** 오는 것이 있나.
//   남(같은 편·자기 것 아님 — sub_54E6F0) · 방향 표 &1(지난 자리) · 속도가 바라보는 쪽과 반대 · 옆으로 20 안 ·
//   나를 겨누는 각(내적 > 0.7) · 시야선. ⚠ 투사체 SUBCLASS·충돌 함수 검사(sub_54E730 나머지)는 투사체 묶음(4)
bool Server::IncomingMissile(const Session& s) const {
    const float px = static_cast<float>(s.posX), py = static_cast<float>(s.posY);
    const float dirX = std::cos(s.facingPrev * 2.0f * 3.14159265f / 256.0f);
    const float dirY = std::sin(s.facingPrev * 2.0f * 3.14159265f / 256.0f);
    for (const auto& p : shots_) {
        const float mx = static_cast<float>(p.x), my = static_cast<float>(p.y);
        if ((mx - px) * (mx - px) + (my - py) * (my - py) > 100.0f * 100.0f) continue;   // getMissilesInCircle 100
        if (p.owner == s.addr) continue;                                                   // 같은 사슬
        auto ow = sessions_.find(p.owner);
        if (ow != sessions_.end() && ow->second.team != 0 && ow->second.team == s.team) continue;   // 같은 팀
        const float prevX = static_cast<float>(p.x - p.vx), prevY = static_cast<float>(p.y - p.vy);   // +72
        if (!(TestTwoPointsAndDirection(px, py, s.facingPrev, prevX, prevY) & 1)) continue;
        const double v5 = p.vx * dirX, v6 = p.vy * dirY;
        if (!(v5 + v6 < 0.0)) continue;
        const float v10 = mx - px, v11 = my - py;
        double v8 = static_cast<double>(dirX) * v11 + -static_cast<double>(dirY) * v10;
        if (v8 < 0.0) v8 = -v8;
        if (!(v8 < 20.0)) continue;
        const double v9 = std::sqrt(p.vx * p.vx + p.vy * p.vy);
        const double v14 = std::sqrt(static_cast<double>(v11) * v11 + static_cast<double>(v10) * v10);
        if (v9 == 0.0 || v14 == 0.0) continue;
        if (!(p.vy / v9 * -(v11 / v14) + p.vx / v9 * -(v10 / v14) > 0.69999999)) continue;
        if (!MapTraceVision(px, py, mx, my)) continue;                                     // CanInteract(나, 투사체, 0)
        return true;
    }
    return false;
}

// `server/map.go:522` MapTraceVision — 그늘 깃발(0x10000) 장애물이 선을 가리나, 그다음 벽(깃발 9).
//   ⚠ 문(클래스 0x80)은 묶음 9. 사람·소환수 몸에는 그늘 깃발이 없다(NewPlayer FLAGS NULL).
//   skipObstacle = 판정 대상 자신(자기를 가린다고 보지 않는다 — `it == obj2`).
bool Server::MapTraceVision(float x1, float y1, float x2, float y2) const {
    return MapTraceVisionSkip(x1, y1, x2, y2, -1);
}
bool Server::MapTraceVisionSkip(float x1, float y1, float x2, float y2, int skipObstacle) const {
    const float qx0 = std::min(x1, x2), qx1 = std::max(x1, x2), qy0 = std::min(y1, y2), qy1 = std::max(y1, y2);
    for (size_t i = 0; i < obstacles_.size(); ++i) {
        if (static_cast<int>(i) == skipObstacle) continue;
        const PhysBody& o = obstacles_[i];
        if (!(o.flags & 0x10000)) continue;
        double ox0, oy0, ox1, oy1;
        BodyRect(o, o.x, o.y, ox0, oy0, ox1, oy1);
        if (ox1 < qx0 || ox0 > qx1 || oy1 < qy0 || oy0 > qy1) continue;   // EachObjInRect
        if (o.shape == 2) {
            float fx, fy;
            if (PointOnTheLine(x1, y1, x2, y2, o.x, o.y, fx, fy)) {
                const double dx = fx - o.x, dy = fy - o.y;
                if (dy * dy + dx * dx <= static_cast<double>(o.r * o.r)) return false;
            }
        } else if (o.shape == 3) {
            // 네 변 — LT·LB / LB2·LT2 / RT·RB / RB2·RT2 (LT2=LT … 는 CalcBox 가 같게 둔다)
            const float px = o.x, py = o.y;
            if (LineTraceXxx(x1, y1, x2, y2, px + o.boxLT, py + o.boxLB, px + o.boxLB, py + o.boxLT)) return false;
            if (LineTraceXxx(x1, y1, x2, y2, px + o.boxLT, py + o.boxLB, px + o.boxRT, py + o.boxRB)) return false;
            if (LineTraceXxx(x1, y1, x2, y2, px + o.boxRB, py + o.boxRT, px + o.boxRT, py + o.boxRB)) return false;
            if (LineTraceXxx(x1, y1, x2, y2, px + o.boxRB, py + o.boxRT, px + o.boxLB, py + o.boxLT)) return false;
        }
    }
    return MapTraceRay(x1, y1, x2, y2, 0x9);
}

// `object.go:1394` playerSetState 중 지금 들어오는 갈래 — 죽음·공중·도발·광전사·함성은 그 조각에서.
bool Server::PlayerSetState(Session& s, uint8_t st, uint32_t frame) {
    bool res = true;
    if (st != kPlayerStateAttack) {
        s.attackAnim = 0;                                   // Field8 = 0(`:1426`)
    } else {
        // LABEL_26(`:1446~1464`) — 쉬는 시간이 끝났으면 애니를 새로 뽑는다. 아니면 상태를 안 바꾼다.
        if (s.attackReadyFrame <= frame) {
            s.attackReadyFrame = 0;
            if (WeaponEquipOf(s) != 0) {
                s.attackAnim = 0;                           // 무기 — Field8 = 0(자세는 무기 비트에서 · PlayerAnimByte)
            } else {
                // 맨손 — 23·24 반반, 워리어면 IntClamp(0,100) > 75 로 25
                s.attackAnim = static_cast<uint8_t>(kAnimPunchLeft + NextRandom(2));
                if (s.charClass == CharClass::Warrior && NextRandom(101) > 75) s.attackAnim = kAnimPunchHook;
                // 투명 해제 · 무적 해제 · 타원 방패 취소 — 버프 중 지금 있는 것은 부활 무적뿐
                if (s.invulnUntilSet && frame < s.invulnUntilFrame) s.invulnUntilFrame = frame;
            }
        } else {
            res = false;
            st = s.moveState;
        }
    }
    if (s.moveState != st) {                                // LABEL_42
        s.moveState = st;
        s.stateFrame = frame;
        s.animFrame = 0;
    }
    return res;
}

// 플레이어 +4 WeaponEquip — 든 무기의 클래스 비트. ⚠ 지금은 직업 기본 장비에서 온다(주워 들기·바꿔 들기는 묶음 5).
uint32_t Server::WeaponEquipOf(const Session& s) {
    const WeaponStats* w = WeaponStatsOf(s);
    return w ? w->classBits : 0u;
}
// 든 무기의 modifier 값 — 시험용 바꿔 들기(DebugSetWeapon)가 있으면 그 이름, 없으면 직업 기본 장비
const WeaponStats* Server::WeaponStatsOf(const Session& s) {
    if (s.debugWeapon >= 0) {
        if (s.debugWeapon == 0) return nullptr;             // 맨손
        for (const auto& w : kWeapons)
            if (static_cast<uint32_t>(s.debugWeapon) == w.classBits) return &w;
        return nullptr;
    }
    return WeaponOf(s.charClass);
}

// `GAME4.c:1395` weaponGetStaminaByType_4F7E80 — 무기 비트 → 깎는 기력
static int WeaponStaminaCost(uint32_t equip) {
    if (equip & 0x200) return 70;
    if (equip & 0x4000) return 100;
    if (equip & 0x800) return 50;
    if (equip & 0x100) return 45;
    if (equip & 0x1000) return 75;
    if (equip & 0x2000) return 100;
    if (equip & 0x7FF8000) return 45;
    return (equip & 0x400) ? 75 : 10;
}

// `GAME3_2.c:4124` netReportStamina_4D8800 — `47 <기력>` 2바이트 · 중요(netSendPacket1 → 4E5030 · cc)
void Server::ReportStamina(Session& s, std::vector<OutPacket>& out) {
    const uint8_t msg[2] = { static_cast<uint8_t>(Op::REPORT_STAMINA), s.stamina };
    EmitSeq(s, msg, sizeof(msg), out);
}

// `server/network.go:629~645` earthquakeSend — 자리에서 300 안의 사람마다 `97 <흔들림>`(비신뢰 · 목록 Kind1)
void Server::EarthquakeSend(float x, float y, int jiggle, std::vector<OutPacket>& out) {
    for (auto& kv : sessions_) {
        Session& o = kv.second;
        if (!o.inGame) continue;
        const double dx = static_cast<double>(x - static_cast<float>(o.x));
        const double dy = static_cast<double>(y - static_cast<float>(o.y));
        const double dd2 = dy * dy + dx * dx;
        if (dd2 >= 90000.0) continue;
        const uint8_t msg[2] = { static_cast<uint8_t>(Op::FX_JIGGLE),
                                 static_cast<uint8_t>(static_cast<int8_t>(static_cast<long long>((1.0 - dd2 * 1.1111111e-05) * jiggle))) };
        Emit(o, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);
    }
}

// `GAME4.c:1588` playerInputAttack — 겨눈 것이 적(또는 빈 곳)일 때만.
void Server::PlayerInputAttack(Session& s, uint32_t frame, std::vector<OutPacket>& out) {
    if (CursorOnFriend(s)) {                                // playerAimsAtEnemy_4F9DC0 가 0
        std::printf("[공격] 프레임 %u %s 커서 아래가 편 — 안 때림\n", frame, s.addr.c_str());
        return;
    }
    const uint32_t equip = WeaponEquipOf(s);
    if (equip) {
        // ⚠ 마법 지팡이(0x47F0000 · 충전 쓰기 useByNetCode)는 주문 묶음(4)에서. 기본 장비에 없다.
        if (s.moveState != kPlayerStateAttack) {
            const int cost = WeaponStaminaCost(equip);
            if (s.stamina >= cost) {
                s.stamina = static_cast<uint8_t>(s.stamina - cost);
                ReportStamina(s, out);
                s.stateFrame = frame;                       // a1[34] = 프레임 — setState 보다 먼저
                s.animFrame = 0;
                const bool entered = PlayerSetState(s, kPlayerStateAttack, frame);
                if (!entered) s.stamina = static_cast<uint8_t>(s.stamina + cost);   // sub_4F7DB0 되돌림
                std::printf("[공격] 프레임 %u %s 무기 0x%X %s · 기력 %d 깎음 → %u\n", frame, s.addr.c_str(), equip,
                            entered ? "상태 1 들어감" : "쉬는 시간이라 안 들어감(기력 되돌림)", cost, s.stamina);
            } else {
                std::printf("[공격] 프레임 %u %s 무기 0x%X 기력 모자람 %u < %d\n", frame, s.addr.c_str(), equip, s.stamina, cost);
            }
            std::fflush(stdout);
        }
        // 투명(0)·무적(23) 끄기 · 타원 방패(67) 취소 — 무기 갈래는 **클릭마다** 끈다(`:1629~1631`)
        if (s.invulnUntilSet && frame < s.invulnUntilFrame) s.invulnUntilFrame = frame;
        return;
    }
    if (s.moveState != kPlayerStateAttack) {
        const bool entered = PlayerSetState(s, kPlayerStateAttack, frame);
        std::printf("[공격] 프레임 %u %s %s 애니 %u\n", frame, s.addr.c_str(),
                    entered ? "상태 1 들어감" : "쉬는 시간이라 안 들어감", s.attackAnim);
        std::fflush(stdout);
    }
}

// `GAME4.c:1637` playerAimsAtEnemy 의 거꾸로 — 커서 아래 것(findObjectAtCursor_54AF40)이 **적이 아니면** 참.
//   퀘스트(게임 깃발 4096)면 늘 적으로 본다. 커서 오브젝트 고르기(`GAME5.c:3547~3640`) —
//   커서 100 안 · 자기 아님 · 죽음/지워짐 아님 · 클래스 0x80000206 · 시야선 · 관전자 아님 ·
//   원 몸이면 (x, y−z) 중심 반지름 안(int 로 자른 r²) · 여럿이면 y+z 가 가장 큰 것(화면에서 앞)
bool Server::CursorOnFriend(const Session& s) const {
    if (questTotal_ > 0 || questStage_ > 0) return false;   // gameFlags 4096
    if (!s.hasAim) return false;
    const float cx = static_cast<float>(s.aimX), cy = static_cast<float>(s.aimY);
    float best = 0.0f;
    enum { None, Player, Pet } kind = None;
    const Session* bestP = nullptr;
    const Creature* bestK = nullptr;
    auto consider = [&](float ox, float oy, float r) -> bool {
        const float dx0 = ox - cx, dy0 = oy - cy;
        if (dx0 * dx0 + dy0 * dy0 > 100.0f * 100.0f) return false;         // EachObjInCircle 100
        if (!MapTraceVision(ox, oy, static_cast<float>(s.posX), static_cast<float>(s.posY))) return false;   // mapCheck(후보, 사람)
        const int r2 = static_cast<int>(r * r);
        const float ddx = cx - ox, ddy = cy - oy;                            // z = 0
        if (static_cast<float>(r2) <= ddy * ddy + ddx * ddx && cy != oy) return false;
        if (cy == oy && !(cx > ox - r && cx < ox + r)) return false;
        const float score = oy;                                              // +104 z + +60 y
        if (score <= best) return false;
        best = score;
        return true;
    };
    for (const auto& kv : sessions_) {
        const Session& o = kv.second;
        if (&o == &s || !o.inGame || o.respawnAtFrame != 0 || o.observer) continue;   // 0x8020 · 관전(3680&1)
        if (consider(static_cast<float>(o.posX), static_cast<float>(o.posY), static_cast<float>(kBodyRadius))) {
            kind = Player; bestP = &o;
        }
    }
    for (const auto& k : pets_) {
        auto tb = typeBodies_.find(k.type);
        if (tb == typeBodies_.end() || tb->second.shape != 2) continue;   // 상자 몸 몬스터는 map_57B850(아직 없음)
        if (consider(static_cast<float>(k.x), static_cast<float>(k.y), tb->second.r)) { kind = Pet; bestK = &k; }
    }
    if (kind == Player) return !IsEnemyTo(s, *bestP);
    if (kind == Pet) {
        if (bestK->owner.empty()) return false;                // 맵 몬스터 — 사람에게 늘 적
        if (bestK->owner == s.addr) return true;               // 내 소환수
        return !IsEnemyTo(s.addr, bestK->owner);
    }
    return false;
}

// `GAME4_3.c:3855` playerAttack — 맨손(`:4020~4066`) · 무기(`:4067~4419`) · 끝(LABEL_159 `:4420~4439`). 참 = 아직 공격 중.
//   무기 갈래는 **애니 한가운데 칸(frames/2)** 에 한 번 판정한다(지난 틱 칸보다 클 때만 — 같은 칸 두 번 방지).
//   ⚠ 옮기지 않은 갈래(다른 묶음): 활·석궁(0x4·0x8 화살 오브젝트 5) · 차크람·수리검(0x40·0x80 날아가는 오브젝트 5)
//     · 마법 지팡이 0x47F0000(주문 4) · 무기 마법 효과 itemApplyAttackEffect(5) · 광전사 돌진·함성(능력 4)
bool Server::PlayerAttack(Session& s, uint32_t frame, std::vector<OutPacket>& out) {
    const uint32_t equip = WeaponEquipOf(s);
    const WeaponStats* w = WeaponStatsOf(s);
    const uint8_t prevFrame = s.animFrame;                  // v76 아래 바이트(+236 지난 틱 값)
    int frames = 0, hold = 0, idx = 0;
    auto range = [&](int anim) {
        frames = (anim >= 0 && anim < kPlayerAnimSlots) ? playerAnimFrames_[anim][0] : 0;
        hold = (anim >= 0 && anim < kPlayerAnimSlots) ? playerAnimFrames_[anim][1] : 0;
    };
    auto calcIdx = [&]() { idx = static_cast<int>((frame - s.stateFrame) / static_cast<uint32_t>(hold + 1)); };
    auto logJudge = [&](const AttackInfo& ai, bool hit, int anim) {
        ++melees_;
        std::printf("[공격] 프레임 %u %s 판정(시작 %u · 칸 %d/%d) 애니 %d 피해 %d 종류 %u 사거리 %.0f → %s\n", frame,
                    s.addr.c_str(), s.stateFrame, idx, frames, anim, ai.dmg, ai.type, ai.range, hit ? "닿음" : "빗나감");
        std::fflush(stdout);
    };
    // calcBoltDamage_4EF1E0 — (힘 − 요구힘) × 계수 + 최소 · float 로 담고 sub_538510 이 +0.5 버림
    auto weaponDmg = [&]() {
        const float f = static_cast<float>(static_cast<double>(s.strength - w->reqStrength) *
                                           static_cast<double>(static_cast<float>(w->coeff)) + w->damageMin);
        return static_cast<int>(static_cast<long long>(static_cast<double>(f) + 0.5));
    };
    auto meleeInfo = [&](uint8_t type, bool overlap) {
        AttackInfo ai;
        ai.dmg = weaponDmg();
        ai.type = type;
        ai.range = static_cast<float>(kBodyRadius) + static_cast<float>(w->range);
        ai.px = static_cast<float>(s.posX); ai.py = static_cast<float>(s.posY);
        ai.hitsOverlap = overlap;
        ai.weapon = true;
        return ai;
    };
    auto setReady = [&]() {
        if (s.attackReadyFrame == 0) s.attackReadyFrame = frame + static_cast<uint32_t>(frames * (hold + 1));
    };
    auto midHit = [&]() { return idx == frames / 2 && idx > prevFrame; };

    if (equip == 0) {
        if (s.attackAnim == 0) {
            // `:3903~3916` — 애니가 비었으면 여기서 뽑는다(`!class && rand(0,100) >= 75` → 25)
            s.attackAnim = static_cast<uint8_t>(kAnimPunchLeft + NextRandom(2));
            if (s.charClass == CharClass::Warrior && NextRandom(101) >= 75) s.attackAnim = kAnimPunchHook;
        }
        const int anim = s.attackAnim;
        range(anim);
        setReady();
        calcIdx();
        if (idx >= frames) {
            AttackInfo ai;
            const int add = (anim == kAnimPunchLeft || anim == kAnimPunchRight) ? kPunchDamageMin
                          : (anim == kAnimPunchHook) ? kKickDamageMin : 0;
            const float coeff = (anim >= kAnimPunchLeft && anim <= kAnimPunchHook) ? kUnarmedCoeffF : 0.0f;
            const float dmg = static_cast<float>(static_cast<double>(s.strength) * coeff + add);
            ai.dmg = static_cast<int>(static_cast<long long>(static_cast<double>(dmg) + 0.5));
            ai.type = 10;
            ai.range = static_cast<float>(kBodyRadius) + kUnarmedReachExtra;
            ai.px = static_cast<float>(s.posX); ai.py = static_cast<float>(s.posY);
            ai.anim = static_cast<uint8_t>(anim);
            logJudge(ai, PlayerTraceAttack(s, ai, out), anim);
        }
    } else if (equip & 0x47F8000) {
        // 지팡이 — 막대(0x8000) 면 29 휘두르기. 마법 지팡이는 +96&2 가 아니면 31(주문 4)
        const int anim = 29;
        range(anim);
        setReady();                                         // 29 면 길이만큼 · 31 이면 지금(`:4075~4079`)
        calcIdx();
        if (idx < frames && midHit()) {
            AttackInfo ai = meleeInfo(0, false);
            logJudge(ai, PlayerTraceAttack(s, ai, out), anim);
        }
    } else if (equip & 0x7800000) {
        const int anim = (equip & 0x3800000) ? 32 : 31;    // 32 찌르기만 판정
        range(anim);
        setReady();
        calcIdx();
        if (anim == 32 && midHit()) {
            AttackInfo ai = meleeInfo(0, false);
            logJudge(ai, PlayerTraceAttack(s, ai, out), anim);
        }
    } else if (!(equip & 0xC0)) {
        struct Branch { uint32_t bits; int anim; uint8_t type; bool overlap; };
        // `:4149~4288` 순서 그대로 — 롱소드 · 칼 · 대검 · 망치 · 철퇴 · 도끼
        static const Branch kBranches[] = {
            { 0x200, 28, 0, false }, { 0x100, 27, 0, false }, { 0x400, 37, 0, false },
            { 0x4000, 39, 2, true }, { 0x800, 26, 2, true }, { 0x3000, 35, 0, true },
        };
        const Branch* br = nullptr;
        for (const auto& b : kBranches) if (equip & b.bits) { br = &b; break; }
        if (br) {
            range(br->anim);
            setReady();
            calcIdx();
            if (midHit()) {
                AttackInfo ai = meleeInfo(br->type, br->overlap);
                if (br->bits == 0x4000) {
                    // 망치 — 앞 35 를 중심으로 반지름 = 무기 사거리(몸 반지름 안 더함) · 둘레 모두 · 땅울림(`:4220~4240`)
                    const float rad = s.facingPrev * 2.0f * 3.14159265f / 256.0f;
                    ai.px = std::cos(rad) * 35.0f + static_cast<float>(s.posX);
                    ai.py = std::sin(rad) * 35.0f + static_cast<float>(s.posY);
                    ai.range = static_cast<float>(w->range);
                    logJudge(ai, PlayerTraceAttack(s, ai, out), br->anim);
                    EarthquakeSend(static_cast<float>(s.posX), static_cast<float>(s.posY),
                                   static_cast<int>(std::lround(static_cast<double>(s.strength) * 0.1)), out);   // nox_float2int
                } else {
                    logJudge(ai, PlayerTraceAttack(s, ai, out), br->anim);
                }
            }
        }
        // 활(0x4)·석궁(0x8) — 화살 오브젝트가 없어 묶음 5 로(여기 오면 아무 일도 안 하고 LABEL_159)
    }
    // 차크람(0x40)·수리검(0x80) — 날아가는 오브젝트라 묶음 5 로
    s.animFrame = static_cast<uint8_t>(idx >= frames ? frames - 1 : idx);   // LABEL_159
    return idx < frames;
}

// `GAME4_3.c:3624` playerTraceAttack — 참 = 무엇에든 닿음(빗나감 소리 879~884 를 가른다, 소리는 묶음 10).
//   망치(무기 SUBCLASS 0x4000)는 판정 중심 둘레 **모두**에 sub_538510 을 부르고, 그 밖은 하나를 고른다(sub_5386A0).
bool Server::PlayerTraceAttack(Session& s, const AttackInfo& ai, std::vector<OutPacket>& out) {
    const float ax = static_cast<float>(s.posX), ay = static_cast<float>(s.posY);
    const float dirX = std::cos(s.facingPrev * 2.0f * 3.14159265f / 256.0f);
    const float dirY = std::sin(s.facingPrev * 2.0f * 3.14159265f / 256.0f);
    const bool hammer = ai.weapon && (WeaponEquipOf(s) & 0x4000);
    const float gx = ai.px, gy = ai.py;                     // sub_518040 중심(a2+16 이거나 사람 자리)
    bool hitFlag = false;                                   // dword_5d4594_2488656

    enum Kind { None, Player, Pet, Obstacle };
    // sub_5180B0 거리 조건 — 사거리 − (중심에서 가장자리까지) > 0
    auto gate = [&](const PhysBody& b, float ox, float oy) {
        double g;
        if (b.shape == 2) {
            g = ai.range - (std::sqrt((gx - ox) * (gx - ox) + (gy - oy) * (gy - oy)) - b.r);
        } else if (b.shape == 3) {
            float nx, ny;
            g = BoxOverlap(gx, gy, ai.range, b, ox, oy, nx, ny);
        } else {
            g = ai.range - std::sqrt((gx - ox) * (gx - ox) + (gy - oy) * (gy - oy));
        }
        return g > 0.0;
    };
    // sub_538510 — 방향 표 &1 · 깃발 · 선 추적(벽 0x5) → 피해(+716) · 맨손 훅이면 힘 20
    auto strike = [&](Kind kind, const std::string& addr, int idx) {
        float tx = 0, ty = 0;
        uint32_t tflags = 0;
        if (kind == Player) {
            const Session& o = sessions_.at(addr);
            tx = static_cast<float>(o.posX); ty = static_cast<float>(o.posY);
            if (o.respawnAtFrame != 0) tflags |= 0x8000 | 0x8;
            if (o.observer) tflags |= 0x40;
        } else if (kind == Pet) {
            tx = static_cast<float>(pets_[idx].x); ty = static_cast<float>(pets_[idx].y);
            tflags = typeBodies_.at(pets_[idx].type).flags;
        } else {
            tx = obstacles_[idx].x; ty = obstacles_[idx].y;
            tflags = obstacles_[idx].flags;
        }
        if (!(TestTwoPointsAndDirection(ax, ay, s.facingPrev, tx, ty) & 1)) return;
        if (tflags & 0x8040) return;
        if (!ai.hitsOverlap && (tflags & 0x8)) return;
        hitFlag = true;                                     // 재질 NONE(0x4000) 이 아니면 — 재질은 아직 안 읽는다
        if (!MapTraceRay(ax, ay, tx, ty, 0x5)) return;
        AttackSrc src;                                      // 막기 판정(3-e2) — 때린 사람의 지난 자리(+72)
        src.x = static_cast<float>(s.prevX);
        src.y = static_cast<float>(s.prevY);
        if (kind == Player) {
            HurtTarget(Target{ addr, -1 }, ai.dmg, out, s.addr, ai.type, &src);
            auto it = sessions_.find(addr);
            if (!ai.weapon && ai.anim == kAnimPunchHook && it != sessions_.end()) {
                // 훅 — 맞은 쪽 ForceVec += (맞은 자리 − 때린 자리) × (10 × 20 ÷ 질량) ÷ (거리 + 0.1)
                Session& o = it->second;
                const float dpx = static_cast<float>(o.posX) - ax, dpy = static_cast<float>(o.posY) - ay;
                const float r = static_cast<float>(std::sqrt(static_cast<double>(dpx * dpx + dpy * dpy)) + 0.1);
                const float f = 10.0f * kHookForce / static_cast<float>(kPlayerMass);
                o.forceX += static_cast<float>(static_cast<double>(dpx) * f / r);
                o.forceY += static_cast<float>(static_cast<double>(dpy) * f / r);
            }
        } else if (kind == Pet) {
            HurtTarget(Target{ std::string(), idx }, ai.dmg, out, s.addr, ai.type, &src);
            // ⚠ 훅 힘 — 소환수는 아직 물리가 없다(묶음 6)
        }
        // 장애물 — 피해 함수가 없는 것(나무·바위). 부서지는 것은 묶음 5·9.
    };

    PhysBody playerBody;
    playerBody.shape = 2; playerBody.r = static_cast<float>(kBodyRadius); playerBody.cls = kClsPlayer;

    if (hammer) {
        // 둘레 모두 — 목록이 바뀔 수 있어(소환수가 죽으면 지워짐) 먼저 모은 뒤 친다
        std::vector<std::pair<Kind, std::pair<std::string, uint16_t>>> hits;
        for (const auto& kv : sessions_) {
            const Session& o = kv.second;
            if (&o == &s || !o.inGame) continue;
            if (gate(playerBody, static_cast<float>(o.posX), static_cast<float>(o.posY))) hits.push_back({ Player, { kv.first, uint16_t{0} } });
        }
        for (const auto& k : pets_) {
            auto tb = typeBodies_.find(k.type);
            if (tb == typeBodies_.end()) continue;
            if (gate(tb->second, static_cast<float>(k.x), static_cast<float>(k.y))) hits.push_back({ Pet, { std::string(), k.netCode } });
        }
        for (size_t i = 0; i < obstacles_.size(); ++i) {
            if (gate(obstacles_[i], obstacles_[i].x, obstacles_[i].y)) hits.push_back({ Obstacle, { std::string(), static_cast<uint16_t>(i) } });
        }
        for (const auto& h : hits) {
            if (h.first == Pet) {
                int idx = -1;
                for (size_t i = 0; i < pets_.size(); ++i) if (pets_[i].netCode == h.second.second) { idx = static_cast<int>(i); break; }
                if (idx >= 0) strike(Pet, std::string(), idx);
            } else if (h.first == Player) {
                if (sessions_.count(h.second.first)) strike(Player, h.second.first, -1);
            } else {
                strike(Obstacle, std::string(), h.second.second);
            }
        }
        return hitFlag;
    }

    // 하나 고르기 — sub_5386A0 앞쪽 가장 가까운 것(몬스터 우선)
    float best = ai.range;
    Kind kind = None;
    std::string bestAddr;
    int bestIdx = -1;
    bool bestMonster = false;
    auto pick = [&](const PhysBody& b, float ox, float oy, bool isEnemy, int obstacleIdx) -> bool {
        if (!gate(b, ox, oy)) return false;
        if (b.flags & 0x8049) return false;
        if (!((b.cls & 6) || !(b.flags & 0x10) || (b.flags & 0x80))) return false;
        if (!(isEnemy || !(b.cls & 6))) return false;
        if (!MapTraceVisionSkip(ax, ay, ox, oy, obstacleIdx)) return false;   // unitCanInteractWith(…, 1)
        if (!(best > 0.0f)) return false;
        const float vx = ox - ax;
        const double vy = oy - ay;
        double len = std::sqrt(vy * vy + vx * vx);
        if (len == 0.0) return false;
        if (!(vy / len * dirY + vx / len * dirX > 0.5)) return false;
        if (b.shape == 2) {
            len = len - b.r;
        } else if (b.shape == 3) {
            float nx, ny;
            const double v7 = BoxOverlap(ax, ay, best, b, ox, oy, nx, ny);
            if (v7 < 0.0) return false;
            len = best - v7;
        }
        if (len < 0.0) len = 0.0;
        const bool isMonster = (b.cls & kClsMonster) != 0;
        if ((len < best || (kind != None && !bestMonster && isMonster)) && (kind == None || !bestMonster)) {
            best = static_cast<float>(len);
            bestMonster = isMonster;
            return true;
        }
        return false;
    };
    for (const auto& kv : sessions_) {
        const Session& o = kv.second;
        if (&o == &s || !o.inGame) continue;
        PhysBody b = playerBody;
        if (o.respawnAtFrame != 0) b.flags |= 0x8000 | 0x8;    // 죽음 DEAD · ALLOW_OVERLAP
        if (o.observer) b.flags |= 0x40;                       // 관전 NO_COLLIDE
        if (pick(b, static_cast<float>(o.posX), static_cast<float>(o.posY), IsEnemyTo(s, o), -1)) {
            kind = Player; bestAddr = kv.first;
        }
    }
    for (size_t i = 0; i < pets_.size(); ++i) {
        const Creature& k = pets_[i];
        auto tb = typeBodies_.find(k.type);
        if (tb == typeBodies_.end()) continue;                 // 몸을 모르는 종류(지어내지 않는다)
        const bool enemy = k.owner.empty() || (k.owner != s.addr && IsEnemyTo(s.addr, k.owner));
        if (pick(tb->second, static_cast<float>(k.x), static_cast<float>(k.y), enemy, -1)) {
            kind = Pet; bestIdx = static_cast<int>(i);
        }
    }
    for (size_t i = 0; i < obstacles_.size(); ++i) {
        const PhysBody& o = obstacles_[i];
        if (pick(o, o.x, o.y, false, static_cast<int>(i))) { kind = Obstacle; bestIdx = static_cast<int>(i); }
    }
    if (kind == None) return false;
    strike(kind, bestAddr, bestIdx);
    // ⚠ 무기 닳기 playerDamageWeapon_4E1560(ItemDamagePercentage) — 아이템 체력(묶음 5) · 벽 부수기 mapDamageToWalls(9)
    return hitFlag;
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
        // 퀘스트는 스테이지 1 부터 다시. 새 웨이브가 다음 틱에 밀려온다.
        if (questTotal_ > 0) {
            questStage_ = 0;
            questAnnounced_ = false;
            SpawnQuestStage(nullptr);
        }
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
                    SendProjectileFx(p, out);   // 못 맞혀도 끝나는 자리에서 그린다
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
                const uint8_t spellId = p.spell;
                AttackSrc src;                   // 막기 판정(3-e2) — 투사체의 지난 자리
                src.x = static_cast<float>(p.x - p.vx);
                src.y = static_cast<float>(p.y - p.vy);
                src.missile = true;
                SendProjectileFx(p, out);       // 맞은 자리에서 그린다
                shots_.erase(shots_.begin() + static_cast<long>(i));
                ++hits_;
                // 주문 피해 종류 — 파이어볼 직격이 1(불), 에너지볼트·연쇄번개가 9(전기).
                //   ⚠ 주문 효과 정답지는 묶음 4. 여기선 피해 함수(3-e)에 넘길 종류 번호만 맞춘다.
                HurtTarget(hit, dmg, out, owner, static_cast<uint8_t>(spellId == 27 ? 1 : 9), &src);
                gone = true;
            }
            if (!gone) ++i;
        }
    }

    // ★ 진단 — 첫 사람이 들어오고 N 틱 뒤, 크리처를 사람 좌우 ±300 으로 **한 번** 옮긴다
    //   (`--creature-away-after N`, 2026-09-13).
    //   왜 — 「사라짐(OBJECT_OUT_OF_SIGHT)」을 화면으로 보려면 크리처가 시야 사각형 밖으로 나가야 하는데,
    //   그러려면 사람이 걸어야 한다. **재는 대상은 사라짐이지 걷기가 아니니 걷기를 빼 버린다**
    //   (`--item-at-spawn` 과 같은 논리).
    //   ±300 은 화면 안(±640)이지만 판정용 사각형(--creature-aoi-half 120 → 나감 ±144) 밖이다 —
    //   대조군(시야 판정 끔)은 계속 보이고, 실험군만 사라져야 한다.
    // 진단 둘이 함께 쓰는 「첫 사람이 들어온 프레임」.
    {
        for (const auto& kv : sessions_) if (kv.second.inGame) { if (firstInGameFrame_ == 0) firstInGameFrame_ = frame_; break; }
    }
    // ★ 진단(묶음 3-e) — N 틱마다 관전 아닌 첫 사람에게 주인 없는 것의 칼 피해 25. 움찔(20 이상)·체력 막대를 화면으로 본다.
    if (debugHurtEvery_ > 0 && firstInGameFrame_ != 0 && frame_ > firstInGameFrame_ &&
        (frame_ - firstInGameFrame_) % debugHurtEvery_ < static_cast<uint32_t>(ticks)) {
        for (auto& kv : sessions_) {
            Session& s = kv.second;
            if (!s.inGame || s.observer || s.respawnAtFrame != 0) continue;
            const uint16_t before = s.health;
            AttackSrc front;                                   // --debug-hurt-front — 바라보는 쪽 30 앞에서 때린 것으로(막기 판정용)
            const float rad = s.facingPrev * 2.0f * 3.14159265f / 256.0f;
            front.x = static_cast<float>(s.posX) + std::cos(rad) * 30.0f;
            front.y = static_cast<float>(s.posY) + std::sin(rad) * 30.0f;
            HurtTarget(Target{ kv.first, -1 }, 25, out, std::string(), 0, debugHurtFront_ ? &front : nullptr);
            std::printf("[진단] 프레임 %u 에 %s 피해 25(종류 0) → 체력 %u→%u · 이월 %.2f · 상태 %u\n", frame_, kv.first.c_str(),
                        before, s.health, s.dmgRemainder, s.moveState);
            std::fflush(stdout);
            break;
        }
    }
    // ★ 진단 — 첫 사람이 들어오고 N 틱 뒤 **그 사람 소유 임프**를 사람 자리에 만들어 고정한다
    //   (`--debug-own-pet N`, 2026-09-13).
    //   왜 — 친구 알림의 화면 효과는 **커서 색**이다(친구면 파랑 · 적이면 빨강, `video_cgo.go:616`).
    //   퀘스트엔 소환수가 없고, 움직이는 소환수에 커서를 맞추기 어렵다. 그래서 사람 자리에 붙여 두고
    //   커서를 화면 한가운데(=사람 자리)에 둔다. 소환 조작(A키)을 빼 버린다(`--item-at-spawn` 과 같은 논리).
    if (debugOwnPetAfter_ > 0 && debugOwnPetNetCode_ == 0 && firstInGameFrame_ != 0 &&
        frame_ >= firstInGameFrame_ + debugOwnPetAfter_) {
        for (const auto& kv : sessions_) {
            if (!kv.second.inGame) continue;
            Creature k;
            k.owner = debugPetWild_ ? std::string() : kv.second.addr;   // --debug-dummy 면 주인 없음(적)
            k.type = SummonType(87);   // SUMMON_IMP → Imp
            k.netCode = nextShotNetCode_++;
            if (nextShotNetCode_ == 0) nextShotNetCode_ = 4096;
            // ⚠ 사람 자리에 겹치면 화면에서 구분이 안 되고 커서도 안 걸렸다(09-13) — 오른쪽 +80 에 떨어뜨린다.
            k.x = kv.second.x + (debugPetWild_ ? 25 : 80);   // 허수아비는 주먹 사거리(30) 안
            k.y = kv.second.y;
            k.health = static_cast<uint16_t>(TuningFor(k.type).health);
            pets_.push_back(k);
            debugOwnPetNetCode_ = k.netCode;
            std::printf("[진단] 프레임 %u 에 %s %s 임프(번호 %u · 체력 %u)를 사람(%d,%d) 오른쪽 +%d 에 고정\n",
                        frame_, kv.second.addr.c_str(), debugPetWild_ ? "옆에 주인 없는" : "소유",
                        static_cast<unsigned>(k.netCode), static_cast<unsigned>(k.health),
                        kv.second.x, kv.second.y, debugPetWild_ ? 25 : 80);
            std::fflush(stdout);
            break;
        }
    }
    if (debugAwayAfter_ > 0 && !debugAwayDone_) {
        const Session* who = nullptr;
        for (const auto& kv : sessions_) if (kv.second.inGame) { who = &kv.second; break; }
        if (who && frame_ >= firstInGameFrame_ + debugAwayAfter_) {
            int side = 1;
            for (auto& k : pets_) { k.x = who->x + 300.0 * side; k.y = who->y; side = -side; }
            debugAwayDone_ = true;
            std::printf("[진단] 프레임 %u 에 크리처 %zu 마리를 사람(%d,%d) 좌우 ±300 으로 옮김\n",
                        frame_, pets_.size(), who->x, who->y);
            std::fflush(stdout);
        }
    }
    // ★ 진단 — 첫 사람 N틱 뒤 **그 사람 팀을 한 번 바꾼다**(`--debug-team-after N`, 2026-09-13).
    //   왜 — 팀이 바뀌면 정답지는 그 사람에게 `36 RESET_FRIENDS` 를 보내고 친구를 다시 알린다.
    //   친구 임프를 둔 채 팀을 바꿔 **다시 알리기가 빠지면 임프가 적(빨강)으로 바뀌는지**를 화면으로 가른다.
    if (debugTeamAfter_ > 0 && !debugTeamDone_ && firstInGameFrame_ != 0 &&
        frame_ >= firstInGameFrame_ + debugTeamAfter_) {
        for (auto& kv : sessions_) {
            if (!kv.second.inGame) continue;
            const uint8_t before = kv.second.team;
            kv.second.team = (before == 1) ? 2 : 1;
            std::printf("[진단] 프레임 %u 에 %s 팀 %u → %u\n", frame_, kv.second.addr.c_str(),
                        static_cast<unsigned>(before), static_cast<unsigned>(kv.second.team));
            std::fflush(stdout);
            break;
        }
        debugTeamDone_ = true;
    }

    // ── 사람을 걷게 하고 때리게 한다 (묶음 2-a · 3-a) — 걸음마다 제 프레임으로 ──
    for (int step = 0; step < ticks; ++step)
        UpdatePlayerMovement(frame_ - static_cast<uint32_t>(ticks - 1 - step), out);

    // ── 불려 나온 것들을 움직인다 ──
    // 부른 사람 편이다 — **부른 사람은 안 때린다.** 그게 없으면 부르자마자 자기가 맞는다.
    for (int step = 0; step < ticks && !pets_.empty(); ++step) {
        for (auto& k : pets_) {
            if (debugOwnPetNetCode_ != 0 && k.netCode == debugOwnPetNetCode_) continue;   // 진단 임프 — 고정
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
                AttackSrc src;                              // 막기 판정(3-e2) — 소환수 자리(소환수는 지난 자리를 아직 안 둔다)
                src.x = static_cast<float>(k.x);
                src.y = static_cast<float>(k.y);
                HurtTarget(t, tune.damage, out, k.owner, 0, &src);   // 부른 사람의 점수다
                break;   // 목록이 바뀔 수 있다 — 이번 걸음은 여기서 접는다
            }
        }
    }

    // ★★ **걷고 있나를 정한다** — 갱신 항목의 애니 번호(IDLE/WALK)를 가르는 데 썼다.
    //   ⚠ 2026-09-13 부터 자세는 상태에서 뽑는다(PlayerAnimByte). 이 값은 더 안 쓰인다 — 진단용으로만 남는다.
    //   지난 틱 자리와 견줘 움직였으면 걷는 것으로 본다.
    //   ⚠ 문턱 1.0 은 ➖ 우리 값이다. 정품 덤프의 「정지」 표본이 틱당 0.3~1.3 유닛이라
    //     그보다 위를 걷기로 잡았다. 너무 낮으면 서 있어도 걷는 자세가 된다.
    for (auto& kv : sessions_) {
        Session& s = kv.second;
        if (!s.inGame) continue;
        const double dx = s.x - s.animPrevX;
        const double dy = s.y - s.animPrevY;
        s.animMoving = (dx * dx + dy * dy) > 1.0;
        s.animPrevX = s.x;
        s.animPrevY = s.y;

    }

    // ★★★ **서버는 걷는 그림 칸을 안 굴린다** (2026-09-11 화면으로 확정).
    //
    //   한동안 `0x6B REPORT_ANIMATION_FRAME` 으로 칸을 6틱마다 한 칸씩 밀어 보냈다.
    //   **아무 효과가 없었다.** 서 있는 채로 칸만 0↔2 로 바꿔 찍어 견주니 —
    //     대조군(같은 칸 두 장) 8,200 픽셀 · 실험(칸 0 vs 2) 7,484 픽셀 — **실험이 더 작다.**
    //
    //   까닭은 클라 코드에 있었다. `0x6B` 은 `spriteSetFrameMB` 로 **스프라이트 칸만** 세울 뿐
    //   (`cdecode.c:1482-1498`) **자세 시퀀스를 안 고른다.** 그리고 걷기 같은 되풀이 애니는
    //   `AnimLoop` 라 클라가 **`Frame()` 으로 스스로 굴린다**(`drawable_vector.go:119-124`) —
    //   `AnimStart` 조차 안 쓴다. 즉 **서버가 굴릴 자리가 아니다.**
    //
    //   ⭕ 반대 증거도 잡았다 — 자세만 4(WALK)로 못 박고 **안 걸은 채** 두 장을 찍으니
    //     788 픽셀이 달랐다(잡음 바닥 396). 그 런의 계수는 `걸음 0 · 걷그림 2`,
    //     즉 **서버가 칸을 한 번도 안 굴렸는데 화면은 돌았다.**
    //
    //   ⚠ `0x6B` 자체는 정품에도 있다(`netReportAnimFrame_4D81F0`). 다만 **「칸이 바뀐
    //     오브젝트」 더티 비트 `0x10000`** 이 켜졌을 때만 나간다(`GAME4_2.c:7148`).
    //     우리가 그 비트를 세울 일이 생기면 그때 되살린다. 지금은 세울 일이 없다.

    // ★★ **들어온 사람에게 마나를 한 번 알린다** (2026-09-11 화면으로 확인).
    //   서버는 `ManaOnline` 으로 마나를 제대로 채워 놓고 **클라에게 말을 안 했다.**
    //   `0x45` 가 나가는 데가 주문·부활·오벨리스크 셋뿐인데 **갓 들어온 사람은 그 셋 다
    //   안 겪는다.** 그래서 `g1_A_join.png` 에서 체력 막대는 가득한데 마나 막대는
    //   **테두리만** 있었다 — 사용자가 「마나칸 0」이라 한 그것이다.
    //
    // ⚠ 접속 묶음 **바로 뒤**에 보내면 안 된다 — 정품 덤프와 바이트가 밀려
    //   `replay_connect` 가 깨진다(그렇게 넣었다가 실패로 확인했다). 첫 틱이 안전한 자리다.
    for (auto& kv : sessions_) {
        Session& s = kv.second;
        if (!s.inGame || s.netCode == 0) continue;
        // ★ 처음 얼마 동안 **여러 번** 보낸다 — 한 번만 보내면 클라가 아직 채비 전이라 놓친다.
        //   30틱(≈1초) 간격으로 `kHudTellTimes` 번. 그 뒤로는 값이 바뀔 때만 나간다.
        if (s.hudToldCount >= kHudTellTimes) continue;
        if (s.hudNextFrame != 0 && frame_ < s.hudNextFrame) continue;
        ++s.hudToldCount;
        s.hudNextFrame = frame_ + 30;
        uint8_t mp[5] = {
            static_cast<uint8_t>(Op::REPORT_MANA),
            static_cast<uint8_t>(s.netCode),
            static_cast<uint8_t>(s.netCode >> 8),
            static_cast<uint8_t>(s.mana),
            static_cast<uint8_t>(s.mana >> 8),
        };
        EmitSeq(s, mp, sizeof(mp), out);   // 홀로 — 제 마나다

        // ★★ **체력도 같이 알린다** (2026-09-11, 사용자 플레이로 드러남).
        //   `0x43` 은 **맞을 때만** 나갔다 — 마나와 **똑같은 빠뜨림**이다.
        //   그래서 갓 들어온 사람은 체력바가 비어 보였다.
        //   ⚠ 나는 캡처에서 빨간 막대가 얇은 것을 보고 **「컨저러라 체력이 적어서」**로
        //     넘겼다. 마나만 보고 **바로 옆의 체력은 안 봤다.** HUD 는 하나씩 대조해야 한다.
        //   ⚠ 번호가 안 실리는 메시지라 **본인에게만** 보낸다(`Damage` 쪽 주석과 같은 까닭).
        uint8_t hp[3] = {
            static_cast<uint8_t>(Op::REPORT_PLAYER_HEALTH),
            static_cast<uint8_t>(s.health),
            static_cast<uint8_t>(s.health >> 8),
        };
        EmitSeq(s, hp, sizeof(hp), out);

        // ★★★ **막대가 그려지려면 「최대치」가 있어야 한다** (2026-09-11, 클라 원본 확정).
        //   `0x43` 은 **현재값만** 세운다 — `sub_470CB0` → `windows[0].field_1`.
        //   **최대값은 `field_2` 이고 그것을 세우는 것은 `0xdd` 다**
        //   (`cdecode.c:2723` → `cliSetTotalHealth_470C80(현재, 최대)`).
        //   **최대가 0 이면 막대가 안 그려진다.** 우리는 `0xdd` 를 **부활 때만** 보냈다.
        //
        //   ⚠ 나는 캡처에서 빨간 막대가 얇은 것을 보고 **「컨저러라 체력이 적어서」**라고
        //     설명을 지어 붙였다. 관측은 「얇다」까지였는데 거기에 까닭을 달아 `⭕` 로 적었고,
        //     그 한 줄이 이 결함을 오래 가렸다. **모르면 모른다고 적을 것.**
        //
        //   짜임 ⭕ — `op | 번호2 | 현재2 | 최대2`(총 7바이트). 마나는 `0xde` 로 같은 꼴이다.
        {
            uint8_t th[7] = {
                static_cast<uint8_t>(Op::REPORT_TOTAL_HEALTH),
                static_cast<uint8_t>(s.netCode), static_cast<uint8_t>(s.netCode >> 8),
                static_cast<uint8_t>(s.health),  static_cast<uint8_t>(s.health >> 8),
                static_cast<uint8_t>(s.maxHealth), static_cast<uint8_t>(s.maxHealth >> 8),
            };
            EmitSeq(s, th, sizeof(th), out);
            uint8_t tm[7] = {
                static_cast<uint8_t>(Op::REPORT_TOTAL_MANA),
                static_cast<uint8_t>(s.netCode), static_cast<uint8_t>(s.netCode >> 8),
                static_cast<uint8_t>(s.mana),    static_cast<uint8_t>(s.mana >> 8),
                static_cast<uint8_t>(s.maxMana), static_cast<uint8_t>(s.maxMana >> 8),
            };
            EmitSeq(s, tm, sizeof(tm), out);
        }
    }

    UpdateFlags(out);
    UpdateProps(out);   // 문·오벨리스크 (P5)

    // ★★ 무적이 **켜지거나 꺼지는 순간**을 잡는다 — 프레임 기반이라 「끝나는 때」는
    //   아무도 안 알려 준다. 매 틱 지금 값과 **마지막으로 알린 값**을 견줘 다를 때만 올린다.
    //   ⭕ 정품도 더티 비트가 켜졌을 때만 보낸다(`GAME4_2.c:7170`).
    for (auto& kv : sessions_) {
        Session& s = kv.second;
        if (!s.inGame || s.netCode == 0) continue;
        const uint32_t now =
            (s.invulnUntilSet && frame_ < s.invulnUntilFrame) ? kEnchantInvulnerable : 0u;
        if (now == s.enchantSent) continue;
        s.enchantSent = now;
        MarkDirty(s.netCode, PendingReport::What::Enchant);
    }

    // ★ R4 — 이 틱에 바뀐 것들을 각자에게 보내고 목록을 비운다.
    //   정품의 「비트가 켜진 것만 보내고 곧바로 끈다」와 같은 자리다.
    FlushPending(out);
    // 퀘스트 첫 스테이지 알림 — StartQuest 때는 out 이 없어 못 보냈다. 첫 틱에 한 번.
    if (questTotal_ > 0 && !questAnnounced_) {
        questAnnounced_ = true;
        if (questStage_ < kQuestStageCount) SendNotice(kQuestStages[questStage_].label, out);
    }

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
        // ★ 2026-09-13 — **준비(CLIENT_READY) 전이면 0 에서 멈춰 기다린다.** 스냅샷 속 cc 는 대기열을 안 거치고
        //   한 번만 나가므로, 준비 전에 닿아 클라가 버리면 순번 구멍이 생겨 뒤 cc 가 전부 막힌다(Session::importantQ 주석).
        if (s.snapshotIn >= 0 && (s.snapshotIn > 0 || s.importantReady) && --s.snapshotIn < 0 &&
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
                // ★ 자세 — 상태에서 뽑는다(PlayerAnimByte: 걷기 4 · 달리기 6 · 가만 0 · 공격 = 애니 번호). 묶음 2-a·3-a
                FillEntry(s.aliases, s.netCode, kPlayerType, s.tickFrame, s.x, s.y,
                          s.facing, me, /*complex=*/true,
                          /*playerAnim=*/ forceAnimPose_ >= 0
                              ? static_cast<uint8_t>(forceAnimPose_)
                              : PlayerAnimByte(s),
                          forceAnimDir_,
                          PlayerAnimFrameSent(s) ? s.animFrame : -1);
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
        // ①' 중요 메시지 — 준비 뒤에만, 확인 올 때까지 다시 싣는다(Session::importantQ 주석).
        FlushImportant(s, out);

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
        // ★★★ **머리는 「카메라」다 — 사람이 아니다.** (2026-09-11 짜는 쪽 소스로 확정)
        //
        //   갱신 스트림 머리는 `MSG_PLAYER_OBJ` 에서 만들어지는데(`network_lists.go:230`),
        //   그 메시지를 만드는 함수 이름이 답이다 —
        //   `opennox/src/network.go:573` **`nox_xxx_netPlayerObjSendCamera_519330`** :
        //
        //       buf[0] = MSG_PLAYER_OBJ
        //       PutUint16(buf[1:], 0)      // 번호 **0**
        //       PutUint16(buf[3:], 0)      // 종류 **0**
        //       PutUint16(buf[5:], pos.X)  // 카메라 자리
        //       PutUint16(buf[7:], pos.Y)
        //       buf[9]  = 0                // 방향 **0**
        //       buf[10] = 0xff             // 그림 칸 **없음**
        //       buf[11] = 0                // 자세 **0**
        //
        //   ⭕ 우리 실측(2026-09-08)의 「머리 `ff` · 번호 0 · 종류 0」과 **정확히 맞는다.**
        //   ★ 잠깐 「머리가 곧 보는 사람이니 자세를 여기 실어야 한다」고 고쳤다가 되돌렸다 —
        //     `sub_4DF810` 만 보면 그렇게 읽히지만, **그 입력을 만드는 함수**를 열어 보니
        //     전부 0 인 카메라였다. **정답지도 한 겹만 읽으면 틀린 결론이 난다.**
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
            // ★ 남도 걷는 자세가 보여야 한다. 사람이 아니면 이 값은 안 쓰인다.
            // ★★ `--anim-pose` 진단은 **여기에도** 걸어야 한다(2026-09-11 자가검증에서 잡음).
            //   앞서 스냅샷 쪽에만 걸어 놓고 「자세가 먹는다」고 판정했는데, **실제로 걸을 때
            //   쓰이는 길은 이 주기 갱신**이다. 한쪽만 재고 다른 쪽을 결론지을 뻔했다.
            FillEntry(s.aliases, o.netCode, useType, s.tickFrame, o.x, o.y, o.facing, e,
                      /*complex=*/true,
                      /*playerAnim=*/ forceAnimPose_ >= 0
                          ? static_cast<uint8_t>(forceAnimPose_)
                          : PlayerAnimByte(o),
                      forceAnimDir_,
                      PlayerAnimFrameSent(o) ? o.animFrame : -1);
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
                    // ★★★ **정정(2026-09-11) — 남 항목도 별칭 여부와 무관하게 두 바이트다.**
                    //   자기 항목과 같은 결함이었다(아래 `else` 가지 주석 참조).
                    //   `extra = 1` 이면 **자세 바이트가 안 나가** 클라가 다음 항목의
                    //   첫 바이트를 자세로 먹고 그 뒤가 통째로 밀린다.
                    if (e.extra < 2) e.extra = 2;   // 3 = 애니 칸이 붙은 것(묶음 3-a) — 줄이지 않는다
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
                    // ⭕ 2026-09-13 묶음 3-a 정정 — `0x30` 은 **그때 상대가 바라보던 쪽**이었다.
                    //   정답지 짜는 쪽(`GAME4_1.c:4471~4478`)이 이 바이트 위 4비트에 방향만 싣는다(0x80 = 애니 칸 붙음).
                    //   고정값으로 덮으면 남의 방향이 늘 3 이 되고 0x80 도 지워진다. 진단 스위치를 줄 때만 덮는다.
                    if (peerPose_ >= 0) e.extraBytes[0] = static_cast<uint8_t>(peerPose_);
                    // ★ 둘째 바이트(자세)는 `FillEntry` 가 넣은 것을 그대로 둔다.
                    //   전에는 여기서 `0x00` 으로 지웠다 — 남이 걸어도 늘 선 자세였다.
                } else {
                    // ★★★ **별칭을 `ff` 로 올렸으면 덧붙이도 첫 소개 규약(2바이트)으로
                    //   같이 올려야 한다.** `FillEntry` 는 「`ff` 면 `40 00`, 별칭이면 `40`」
                    //   으로 정해 놓는데, 여기서 별칭만 `ff` 로 덮고 덧붙이는 1 로 둔 채
                    //   내보내고 있었다. 클라는 `40` 다음 바이트를 **다음 항목의 시작**으로
                    //   읽어 스트림이 통째로 어긋난다.
                    //   지문 = 둘째가 들어온 지 100ms 남짓에 **먼저 있던 클라가 그냥 사라진다**
                    //   (`0x0a CLIENT_CLOSE` 도 `0xae` 도 없이 프로세스가 죽는다).
                    //   2026-09-09 실측 — `--no-update` 로 갱신만 끄면 둘 다 끝까지 살았다.
                    //   ★★★ **정정(2026-09-11 와이어 실측) — 별칭일 때도 두 바이트다.**
                    //
                    //     여기서 `FillEntry` 가 정해 놓은 값을 **하드코딩으로 덮어쓰고 있었다.**
                    //     `extraBytes[1] = 0x00` 이 **자세를 지웠고**, `extra = 1` 이
                    //     **자세 바이트 자체를 안 나가게** 했다. 오늘 하루 자세 실험이
                    //     전부 0 픽셀이던 진짜 원인이다 — `FillEntry` 를 아무리 고쳐도
                    //     이 세 줄이 그 뒤에서 되돌리고 있었다.
                    //
                    //   ⭕ 근거 둘 —
                    //     ⑴ **짜는 쪽 정품 코드** `opennox/src/network_lists.go:297-299`:
                    //        `if b2[0] == MSG_PLAYER_OBJ { off++; b1[off] = b2[11] }`
                    //        — 사람이면 별칭 여부와 무관하게 **자세 한 바이트를 더** 붙인다
                    //     ⑵ **프록시 와이어 실측**(`CAP_c9fix.txt`) — 별칭 `0xc9` 항목이
                    //        2바이트로 읽힌다. ⚠ 다만 그 둘째 바이트 `00` 은 우리가 1 만
                    //        보내서 **다음 항목의 첫 바이트가 끌려 들어온 것**이다.
                    //        클라도 같은 규칙으로 읽으니 그 뒤가 통째로 밀린다
                    //
                    //   ⚠ 「별칭이면 한 바이트」라던 2026-09-08 실측은 **판독 오독**이었다
                    //     (그 덤프엔 별칭 사람 항목이 0건이었다 — `nox_verify` 로 확인).
                    if (e.extra < 2) e.extra = 2;   // 3 = 애니 칸이 붙은 것(묶음 3-a)
                    // `extraBytes` 는 `FillEntry` 가 이미 채웠다 — **덮어쓰지 않는다.**
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
        //
        // ★ **시야 판정** (2026-09-13 · 기본 꺼짐 — `--creature-aoi`).
        //   정답지 `legacy/GAME4_1.c:4488` `nox_xxx_netSendObjects2Plr_519410` 는 플레이어마다
        //   오브젝트의 「보이는 중」 비트(+148)를 들고, 보이다가 안 보이면 사라짐을 보낸다.
        //   순회 범위는 플레이어 위치 ± (화면절반 + 100) 사각형이다(`server.go:533` · `player.go:325`).
        //   ⚠ 시야선(벽 뒤)은 아직 없다 — 거리만 본다(묶음 1 ⓓ3 에서 넣는다).
        //   ⚠ 들어옴 ±half · 나감 ±half×1.2 — 경계를 드나드는 것이 매 틱 재소개되면 클라가
        //     멎는다(Session::staticIntroAt 주석의 사고). 그 사이는 이전 상태를 유지한다.
        //   ⚠ **별칭은 건드리지 않는다** — Alias.h 의 「잊지 말 것」 주석.
        std::vector<uint16_t> creatureExits;
        for (const auto& k : pets_) {
            if (creatureAoiHalfX_ > 0.0) {
                const double dx = std::fabs(k.x - s.x);
                const double dy = std::fabs(k.y - s.y);
                const bool inside  = dx <= creatureAoiHalfX_ && dy <= creatureAoiHalfY_;
                const bool outside = dx > creatureAoiHalfX_ * 1.2 || dy > creatureAoiHalfY_ * 1.2;
                const bool was = s.visibleCreatures.count(k.netCode) != 0;
                if (inside && !was) s.visibleCreatures.insert(k.netCode);
                if (outside && was) {
                    s.visibleCreatures.erase(k.netCode);
                    creatureExits.push_back(k.netCode);
                }
                if (s.visibleCreatures.count(k.netCode) == 0) continue;   // 안 보이는 것은 안 싣는다
            }
            ObjEntry e;
            FillEntry(s.aliases, k.netCode, k.type, s.tickFrame,
                      static_cast<int>(k.x), static_cast<int>(k.y), k.facing, e);
            Relativize(e, k.netCode, static_cast<int>(k.x), static_cast<int>(k.y),
                       baseX, baseY);
            m.objects.push_back(e);
        }
        // 죽어서 목록에서 빠진 것은 「보이는 중」에서도 지운다 — 번호가 다시 쓰일 때 섞이지 않게.
        for (auto it = s.visibleCreatures.begin(); it != s.visibleCreatures.end();) {
            bool alive = false;
            for (const auto& k : pets_) if (k.netCode == *it) { alive = true; break; }
            it = alive ? std::next(it) : s.visibleCreatures.erase(it);
        }

        // ★ **친구 알림** (2026-09-13 · 기본 꺼짐 — `--friends`).
        //   정답지 분배자(`GAME4_1.c:4488`)는 오브젝트의 +140(대기)·+144(친구) 비트를 보고
        //   `OBJECT_FRIEND_ADD(52)` / `REMOVE(53)` 를 **Packet1** 로 보낸다(`GAME3_2.c:4831` →
        //   `GAME3_3.c:1742` → `netImportant_4E5770` 이 [0xCC][순번][길이][본문] 로 싣는다 = 내 EmitSeq).
        //   친구 = **적이 아닌 것**(`server/object.go:1575` — 함수 이름 「HostileMimic」은 동작과 반대다).
        //   클라는 이 목록으로 **커서·선택 원 색**을 가른다(`video_cgo.go:616` 친구·물건 파랑 / 그 외 빨강).
        //   ⚠⚠ **부분 구현이다** — 적대 판정(`server/object.go:1667` IsEnemyTo, 규칙 20개) 중
        //     ⑬ 주인 같음 · ⑭ 같은 팀 · ⑱ 팀 있으면 적 · ⑳ 그 외 적 만 넣었다.
        //     매혹(⑪)·관전(⑫)·몬스터 종류와 SubClass(⑥~⑩)·생성기(④⑤⑮⑯)·도깨비불(⑰)은 묶음 4·6·8 에서 채운다.
        //
        // ★ **팀이 바뀌면 친구 목록을 비우라고 알린다** — `36 RESET_FRIENDS`(본문 없음, Packet1).
        //   정답지: 팀에 넣을 때(`common__system__team.c:154`)·뺄 때(`GAME1_1.c:377`) 그 사람에게
        //   `sub_4D97E0`(GAME3_2.c:4838) → 곧이어 친구 비트를 다시 세운다(`sub_4E8110`·monsterMarkUpdate).
        //   클라는 목록을 통째로 비운다(`client__network__cdecode.c:595` → `sub_4959B0`).
        //   그래서 여기서도 **보냈다고 적어 둔 친구를 잊어** 아래 블록이 다시 알리게 한다.
        //   ⚠ 친구 알림이 꺼져 있어도 보낸다 — 정답지는 기능 스위치 없이 보낸다(클라 목록이 비어 있으면 해가 없다).
        if (s.team != s.friendResetTeam) {
            s.friendResetTeam = s.team;
            const uint8_t reset[1] = { static_cast<uint8_t>(Op::RESET_FRIENDS) };
            EmitSeq(s, reset, sizeof(reset), out);
            s.friendsSent.clear();
        }
        if (friendsOn_) {
            auto sendFriend = [&](uint16_t nc, bool friendly) {
                const bool sent = s.friendsSent.count(nc) != 0;
                if (friendly == sent) return;   // 바뀔 때만 보낸다
                const uint8_t msg[3] = {
                    static_cast<uint8_t>(friendly ? Op::OBJECT_FRIEND_ADD : Op::OBJECT_FRIEND_REMOVE),
                    static_cast<uint8_t>(nc), static_cast<uint8_t>(nc >> 8) };
                EmitSeq(s, msg, sizeof(msg), out);
                std::printf("[친구] 프레임 %u %s 에게 번호 %u %s\n", frame_, s.addr.c_str(),
                            static_cast<unsigned>(nc), friendly ? "ADD" : "REMOVE");
                if (friendly) s.friendsSent.insert(nc); else s.friendsSent.erase(nc);
            };
            for (const auto& k : pets_) {
                if (creatureAoiHalfX_ > 0.0 && s.visibleCreatures.count(k.netCode) == 0) continue;
                bool friendly = false;
                if (!k.owner.empty() && k.owner == s.addr) {
                    friendly = true;                                        // ⑬ 자기 소환수
                } else if (!k.owner.empty() && s.team != 0) {
                    auto oi = sessions_.find(k.owner);
                    if (oi != sessions_.end() && oi->second.team == s.team) friendly = true;   // ⑭ 같은 팀
                }
                sendFriend(k.netCode, friendly);                            // 그 외 ⑱⑳ 적
            }
            for (const auto& kv2 : sessions_) {
                const Session& o = kv2.second;
                if (!o.inGame || o.addr == s.addr) continue;
                sendFriend(o.netCode, s.team != 0 && o.team == s.team);    // ⑭ 같은 팀 · 그 외 ⑱⑳ 적
            }
            // 사라진 것(죽은 크리처·나간 사람)은 목록에서 지운다 — 번호가 다시 쓰일 때 섞이지 않게.
            for (auto it = s.friendsSent.begin(); it != s.friendsSent.end();) {
                bool alive = false;
                for (const auto& k : pets_) if (k.netCode == *it) { alive = true; break; }
                if (!alive) for (const auto& kv2 : sessions_) if (kv2.second.inGame && kv2.second.netCode == *it) { alive = true; break; }
                it = alive ? std::next(it) : s.friendsSent.erase(it);
            }
        }

        // ── 맵 정적 오브젝트 — **시야 반경 안만, 가까운 순 상한** (독립 서버 P4-b) ──
        //
        // 담는 규칙과 상한의 이유는 AppendAoiStatics 주석에. 드나듦은 별칭 임대(2초)가
        // 알아서 정리하고, 같은 자리에 서 있으면 부분집합이 안 바뀌어 별칭도 안정된다.
        // ⚠ 기본 꺼짐(aoiOn_) — 실클라 검증(P4-c) 전에는 켜지 않는다.
        // ★ 크리처 사라짐(위 루프의 creatureExits)을 먼저 담는다 — 보내는 길은 정적 오브젝트와 같다.
        // ⚠ **효과는 증명되지 않았다** (2026-09-13 돌연변이 시험). 이 줄을 빼 OUT_OF_SIGHT 를 안 보내도
        //   크리처가 화면에서 **똑같이 사라졌다** — 클라가 갱신에서 빠진 동적 오브젝트를 스스로 지운다.
        //   (실험군·돌연변이 옮김 순간 사람 옆 자리 픽셀 차 457 대 445, 두 사진 모두 원뿔 없음)
        //   정답지(`GAME4_1.c:4488`)가 보내므로 남긴다. 효과가 드러나는 조건(정적 오브젝트? 즉시 대 지연?)은 관측대기.
        std::vector<uint16_t> outOfSight(creatureExits);
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
        // ★ **시야에서 빠진 것** — `32 <번호2>`. **중요 대기열에 맨 메시지로** 싣는다(2026-09-13).
        //   정답지 `nox_xxx_netObjectOutOfSight_528A60`(GAME4_2.c:7651) = `netSendPacket0_4E5420(pl, 32 번호2, 3, 0, 1)`
        //   — Packet0 은 cc 봉투 없이 본문 그대로, 확인이 올 때까지 다시 싣는다.
        //   전에는 갱신 조각 끝에 한 번만 붙였다(갱신이 안 나간 틱엔 아예 빠졌다).
        //   ⚠ 싣는 것은 **다음 틱** FlushImportant — 정답지는 같은 프레임 갱신 뒤에 싣는다(network_lists.go:217). 1틱 늦다.
        for (uint16_t nc : outOfSight) {
            const uint8_t msg[3] = { static_cast<uint8_t>(Op::OBJECT_OUT_OF_SIGHT),
                                     static_cast<uint8_t>(nc), static_cast<uint8_t>(nc >> 8) };
            EmitImportantRaw(s, msg, sizeof(msg), out);
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
