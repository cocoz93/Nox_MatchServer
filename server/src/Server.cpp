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
#include "nox/SpellNames.h"
#include "nox/DirVectors.h"

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

// ── 묶음 4-b1 — damageDefaultProc 의 버프 갈래 도우미 ─────────────────────────
// a2(때린 이) — 소환수가 때렸으면 그 소환수, 아니면 사람(by). 둘 다 없으면 없음(주인 없는 불·독 등)
Target Server::DamageAttacker(const std::string& by, const AttackSrc* src) const {
    if (src && src->byPet >= 0 && static_cast<size_t>(src->byPet) < pets_.size()) return Target{ std::string(), src->byPet };
    if (!by.empty() && sessions_.count(by)) return Target{ by, -1 };
    return Target{};
}

// 감전(22) 되갚기(`GAME3_2.c:7101~7116`) — 무기(a3)로 근접(sub_4E1400)해 때린 유닛에게 ShockDamage[4] 전기(9) · 사람이면 상태 23 · 버프 끔(소리 135 → 10)
//   sub_4E1400(a2, a3): 무기 클래스이고 원거리 SUBCLASS(0x47F0000) 아님 → 1 · 투사체면 0
void Server::DamageShockBack(const Target& victim, const Buffs& vb, const Target& attacker, const AttackSrc* src, std::vector<OutPacket>& out) {
    if (attacker.none() || !vb.Has(22) || !src || !src->obj || src->missile) return;
    if (!attacker.addr.empty()) {
        const Session& a = sessions_.at(attacker.addr);
        if (WeaponEquipOf(a) & 0x47F0000) return;          // 지팡이·원거리 무기
    }
    BuffOff(victim, 22);
    const int back = static_cast<int>(static_cast<float>(BalanceFloatInd("ShockDamage", 4)));
    const std::string victimOwner = !victim.addr.empty() ? victim.addr : pets_[static_cast<size_t>(victim.pet)].owner;
    HurtTarget(attacker, back, out, victimOwner, 9, nullptr);   // (a2+716)(a2, a1, 0, 피해, 9)
    if (!attacker.addr.empty()) {
        auto it = sessions_.find(attacker.addr);
        if (it != sessions_.end()) PlayerSetState(it->second, 23, frame_);
    }
}

// 불·전기 보호(`checkFireProtect_4DFE40` · `checkElectrProtect_4DFF40`) — 아이템 몫(최대 0.5, 아이템 5) + 버프 세기 표 · 합 최대 0.6 · 곱한 뒤 0 이면 1
int Server::DamageProtect(const Buffs& b, uint8_t dtype, int amount) const {
    auto protect = [&](int buff, const char* table) {
        const float items = 0.0f;                           // ⚠ 입은 것 마법부여 합(sub_4DFD10 · 4DFD80) — 아이템 5
        float r = items;
        if (b.Has(buff)) r = static_cast<float>(BalanceFloatInd(table, static_cast<int>(b.power[buff]) - 1) + items);
        if (r > 0.60000002f) r = 0.60000002f;
        return r;
    };
    if (dtype == 1 || dtype == 12 || dtype == 7) {
        const float v = protect(17, "FireSpellProtection");
        amount = static_cast<int>(static_cast<float>((1.0 - v) * static_cast<double>(amount)));
        if (!amount) amount = 1;
    }
    if (dtype == 9 || dtype == 17) {
        const float v = protect(20, "ElectricitySpellProtection");
        amount = static_cast<int>(static_cast<float>((1.0 - v) * static_cast<double>(amount)));
        if (!amount) amount = 1;
        // ⚠ 사람 +160 = 2 · NPC 몬스터 +2094 = 2(쓰는 곳 미확인 — 관측대기)
    }
    return amount;
}

// 흡혈(13, `GAME3_2.c:7268~7287`) — 때린 이가 흡혈이면 VampirismCoeff[세기−1] × 피해(최소 1)를 채우고 `a2 <때린 이 x,y> <맞은 이 x,y> <채운 양>` 을 맞은 자리 화면 안에
void Server::DamageVampirism(const Target& attacker, const Target& victim, int amount, std::vector<OutPacket>& out) {
    const Buffs* ab = BuffsOf(attacker);
    if (!ab || !ab->Has(13)) return;
    const float heal = static_cast<float>(BalanceFloatInd("VampirismCoeff", static_cast<int>(ab->power[13]) - 1) * static_cast<double>(amount));
    uint16_t v32 = static_cast<uint16_t>(static_cast<int>(heal));
    if (v32 < 1u) v32 = 1;
    UnitAdjustHP(attacker, v32, out);
    float ax = 0, ay = 0, vx = 0, vy = 0;
    auto posOf = [&](const Target& t, float& x, float& y) {
        if (!t.addr.empty()) { x = static_cast<float>(sessions_.at(t.addr).posX); y = static_cast<float>(sessions_.at(t.addr).posY); }
        else { x = static_cast<float>(pets_[static_cast<size_t>(t.pet)].x); y = static_cast<float>(pets_[static_cast<size_t>(t.pet)].y); }
    };
    posOf(attacker, ax, ay);
    posOf(victim, vx, vy);
    const uint16_t p[4] = { static_cast<uint16_t>(static_cast<int>(ax)), static_cast<uint16_t>(static_cast<int>(ay)),
                            static_cast<uint16_t>(static_cast<int>(vx)), static_cast<uint16_t>(static_cast<int>(vy)) };
    uint8_t msg[11] = { static_cast<uint8_t>(Op::FX_VAMPIRISM) };
    for (int k = 0; k < 4; ++k) { msg[1 + 2 * k] = static_cast<uint8_t>(p[k]); msg[2 + 2 * k] = static_cast<uint8_t>(p[k] >> 8); }
    msg[9] = static_cast<uint8_t>(v32);
    msg[10] = static_cast<uint8_t>(v32 >> 8);
    SendFxAllCli(vx, vy, msg, sizeof(msg), out);           // netSendVampFx_523270 — 자리 = 맞은 이
}

// nox_xxx_unitAdjustHP_4EE460(`GAME3_3.c:7097`) — 최대 아래일 때만 채움(최대로 자름) · **알림은 없다**(unitSetHP 는 동기 비트만).
//   사람 `43` 은 다음 틱 PlayerReportAnything(+10 ≠) 이, `42` 는 갱신이 음수만 보낸다 — 회복은 `42` 로 안 나간다 · 소환수는 주인 체력 알림 41
void Server::UnitAdjustHP(const Target& t, int dv, std::vector<OutPacket>& out) {
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        if (it == sessions_.end() || !it->second.inGame) return;
        Session& s = it->second;
        if (s.health >= s.maxHealth) return;
        int after = s.health + dv;
        if (after > s.maxHealth) after = s.maxHealth;
        s.health = static_cast<uint16_t>(after);
        MarkDirty(s.netCode, PendingReport::What::UnitHealth);
        (void)out;
        return;
    }
    if (t.pet < 0 || static_cast<size_t>(t.pet) >= pets_.size()) return;
    Creature& k = pets_[static_cast<size_t>(t.pet)];
    const int maxHp = TuningFor(k.type).health;
    if (k.health >= maxHp) return;
    int after = k.health + dv;
    if (after > maxHp) after = maxHp;
    k.health = static_cast<uint16_t>(after);
    MarkDirty(k.netCode, PendingReport::What::UnitHealth);   // mobInformOwnerHP_4EE4C0
}

// collSysAddCollision_548630 — 같은 짝(순서 무관)은 한 번만
void Server::AddContact(const ContactRef& a, const ContactRef& b) const {
    auto same = [](const ContactRef& x, const ContactRef& y) { return x.kind == y.kind && x.addr == y.addr && x.idx == y.idx; };
    for (const auto& pr : contacts_)
        if ((same(pr.first, a) && same(pr.second, b)) || (same(pr.first, b) && same(pr.second, a))) return;
    contacts_.push_back({ a, b });
}

// nox_xxx_collide_4FDF90(`GAME4.c:2966`) — a1 이 a2 에 닿았다
//   ① a1 감전(22) · a2 가 DEAD|ALLOW_OVERLAP 아닌 유닛 · 적 → 버프 끄고(소리 135 → 10) ShockDamage[세기−1] 전기(9)를 a2 에(때린 이 = a1)
//   ② a2 가 유닛·몬스터 생성기(0x20006)이고 DEAD|DESTROYED 아니고 같은 편 아니면 a1 투명(0) 끔
//   ③ a1 사람 · a2 몬스터 생성기(0x20000) → a1 투명 끔(생성기는 8 — 지금 없음)
void Server::CollideBuffs(const ContactRef& a1, const ContactRef& a2, std::vector<OutPacket>& out) {
    if (a1.kind == 3) return;                              // 장애물은 버프가 없다
    const Target t1 = (a1.kind == 1) ? Target{ a1.addr, -1 } : Target{ std::string(), a1.idx };
    const Target t2 = (a2.kind == 1) ? Target{ a2.addr, -1 } : Target{ std::string(), a2.idx };
    const Buffs* b1 = BuffsOf(t1);
    if (!b1) return;
    const bool unit2 = (a2.kind == 1 || a2.kind == 2) && BuffsOf(t2) != nullptr;
    bool dead2 = false;
    if (a2.kind == 1 && unit2) dead2 = sessions_.at(a2.addr).respawnAtFrame != 0;   // DEAD(ALLOW_OVERLAP 은 죽음과 함께)
    auto owner = [&](const Target& t) -> std::string { return !t.addr.empty() ? t.addr : pets_[static_cast<size_t>(t.pet)].owner; };
    const std::string own1 = owner(t1);
    if (b1->Has(22) && unit2 && !dead2) {
        const std::string own2 = owner(t2);
        if (OwnersEnemy(own1, own1.empty(), own2, own2.empty())) {
            const int power = b1->power[22];
            BuffOff(t1, 22);
            const int dmg = static_cast<int>(static_cast<float>(BalanceFloatInd("ShockDamage", power - 1)));
            AttackSrc src;
            if (a1.kind == 1) { src.x = static_cast<float>(sessions_.at(a1.addr).posX); src.y = static_cast<float>(sessions_.at(a1.addr).posY); }
            else { src.x = static_cast<float>(pets_[static_cast<size_t>(a1.idx)].x); src.y = static_cast<float>(pets_[static_cast<size_t>(a1.idx)].y); src.byPet = a1.idx; }
            HurtTarget(t2, dmg, out, own1, 9, &src);           // (a2+716)(a2, a1, a1, 피해, 9)
            if (!BuffsOf(t1)) return;                          // 되갚다 사라진 소환수
        }
    }
    if (unit2 && !dead2) {
        const std::string own2 = owner(t2);
        const bool sameTeam = (!own1.empty() && own1 == own2) ||
                              (a1.kind == 1 && a2.kind == 1 && sessions_.at(a1.addr).team != 0 &&
                               sessions_.at(a1.addr).team == sessions_.at(a2.addr).team);
        if (!sameTeam) BuffOff(t1, 0);
    }
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
        if (s.buffs.Has(23)) { ++invulnBlocked_; return; }   // 버프 23
        if (s.observer) return;                            // 3680 & 1
        // ── 죽음 알림 기록(+304 갈래 · +300 종류, 묶음 3-d · `GAME3_3.c:164~246`) — 부를 때마다 0 에서 다시 ──
        //   때린 이(a2)가 있으면: 무기·투사체(a3)가 있고 때린 이와 다르면 갈래 1·그 종류 /
        //   없고 피해 종류가 10(맨손)·2(망치)면 갈래 1·때린 이의 종류(사람 713 · 몬스터 그 종류). 막혀도 이 기록은 남는다
        s.killKind = 0;
        // ── 되받아치기 방패(27, `GAME3_3.c:173~204`) — 때린 것(a3 없으면 a2)이 앞쪽이면 ──
        //   투사체면 되받아치고(projectileReflect · 주인 바꿈) 막음 · 아니면 종류 16·17(맨손 마법·?)만 막음 · 종류 16 이면 점 효과 132
        //   ⚠ 옛 자리표시 투사체(shots_)는 오브젝트가 없어 되받아치기 없이 막기만 — 진짜 투사체는 4-c
        if (s.buffs.Has(27)) {
            // v9 = a3 ? a3 : a2 — 무기·투사체 자리(src), 없으면 때린 사람 자리
            float vx9 = 0, vy9 = 0;
            bool have9 = false, missile9 = false;
            if (src) { vx9 = src->x; vy9 = src->y; have9 = true; missile9 = src->missile; }
            else if (!by.empty() && sessions_.count(by)) { vx9 = static_cast<float>(sessions_.at(by).posX); vy9 = static_cast<float>(sessions_.at(by).posY); have9 = true; }
            if (have9 && (missile9 || dtype == 16 || dtype == 17) &&
                (TestTwoPointsAndDirection(static_cast<float>(s.posX), static_cast<float>(s.posY), s.facingPrev, vx9, vy9) & 1)) {
                if (dtype == 16) {
                    const uint16_t fx = static_cast<uint16_t>(static_cast<int>(s.posX)), fy = static_cast<uint16_t>(static_cast<int>(s.posY));
                    const uint8_t msg[5] = { static_cast<uint8_t>(Op::FX_VIOLET_SPARKS), static_cast<uint8_t>(fx), static_cast<uint8_t>(fx >> 8),
                                             static_cast<uint8_t>(fy), static_cast<uint8_t>(fy >> 8) };
                    SendFxAllCli(static_cast<float>(s.posX), static_cast<float>(s.posY), msg, sizeof(msg), out);
                }
                return;                                    // 소리 122 → 10
            }
        }
        if (src) {
            if (src->obj) {
                s.killKind = 1;
                s.killType = src->objType;
            } else if (dtype == 10 || dtype == 2) {
                s.killKind = 1;
                s.killType = src->byMonsterType ? src->byMonsterType : kPlayerType;
            }
        }
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
        // 기록이 비었으면 갈래 2·피해 종류 — 정답지 switch 가 이름 붙인 종류(0~12·14~17)만 적는다(13·18 넘는 것은 default 라 안 적음)
        if (s.killKind == 0 && (dtype <= 12 || (dtype >= 14 && dtype <= 17))) {
            s.killKind = 2;
            s.killType = dtype;
        }
        if (dmg > 0 && hp == 0) hp = 1;                    // `v33 > 0 && !a1`
        if (quest) {                                       // × sub_4E40B0(blob 202032 = 1.0)
            const int v44 = hp;
            hp = static_cast<int>(static_cast<float>(1.0 * static_cast<double>(hp)));
            if (v44 > 0 && hp < 1) hp = 1;
        }
        // ── nox_xxx_damageDefaultProc_4E0B30(`GAME3_2.c:7017`) ──
        if (s.buffs.Has(23)) { ++invulnBlocked_; return; }   // 무적(소리 71 → 10)
        // 편 거르기 — 친선 피해 설정(게임플레이 깃발 1, 기본 꺼짐)이 꺼져 있으면 때린 쪽 주인이 적이 아닐 때 무시.
        //   단 자기가 자기를 때린 것은 퀘스트가 아니면 들어간다(`a1 != v12 || gameFlags 4096`).
        if (!OwnersEnemy(s.addr, false, by, byWild) && (by != s.addr || quest)) return;
        const Target attacker = DamageAttacker(by, src);
        DamageShockBack(Target{ s.addr, -1 }, s.buffs, attacker, src, out);   // 감전(22) 되갚기
        hp = DamageProtect(s.buffs, dtype, hp);            // 불·전기 보호(버프 17·20 + 아이템 5)
        // 맞으면 투명(0)이 풀린다 — 때린 이가 없으면 종류 12 일 때만(+528/+532 때린 자리는 몬스터 AI 7)
        if (!attacker.none() || dtype == 12) BuffOff(Target{ s.addr, -1 }, 0);
        // ⚠ 입은 것 방어 효과 itemApplyDefendEffect2 · 무기 앞 피해 효과 itemApplyPreDamageEffect(아이템 5) · 맞는 소리(10)
        s.lastDamageType = dtype;                          // +524(`GAME3_2.c:7234`)
        DamageVampirism(attacker, Target{ s.addr, -1 }, hp, out);   // 흡혈(13)
        // ⚠ 공 들고 맞음(gameballOnPlayerDamage 8) · 퀘스트/코옵 sub_4FB050(8) · 몬스터 원한 sub_532880(7)
        // 한 대에 20 넘게 맞으면 움찔(상태 30 · 공격 중 1 · 방패 올리는 중 15 는 아님)
        if (hp >= 20 && s.moveState != kPlayerStateAttack && s.moveState != 15) PlayerSetState(s, 30, frame_);
        // ★ 마법 방패(26) — 정답지 `damageDefaultProc` LABEL_137 자리.
        //   독(5)은 안 막고, 죽음(15)을 자기가 자기에게 건 것도 안 막는다.
        if (s.buffs.Has(26) && dtype != 5 && !(dtype == 15 && by == s.addr)) {
            ShieldReduceDamage(Target{ s.addr, -1 }, hp, dtype, src, out);
            if (hp == 0) return;                           // 막았다 — 아예 안 맞은 것이 된다
        }
        // +520 마지막으로 때린 것(`GAME3_2.c:7229~7233`) — a3 가 있으면 그것(주인 = a2), 없으면 a2. 사슬의 사람 = `by`(소환수면 주인)
        Session::LastHit hit;
        if (src) {
            const bool a2Monster = src->byMonsterType != 0;
            const bool a2Player = !a2Monster && !byWild;
            if (src->obj) {
                hit.cls = 3;
                hit.ownerCls = a2Monster ? 2 : (a2Player ? 1 : 0);
            } else {
                hit.cls = a2Monster ? 2 : (a2Player ? 1 : 0);
            }
            hit.monsterType = src->byMonsterType;
            hit.player = by;
        }
        // ★ 2026-09-15 7-c — `src` 가 없으면 **빈 hit 를 넘기지 않는다.**
        //   전에는 빈 것을 넘겨 `Damage` 가 `if (hit)` 가지를 타면서
        //   **때린 사람(`by`)이 통째로 사라졌다** — 죽음 알림의 죽인이도 늘 0 이었다.
        //   정답지 `GAME3_2.c:7229~7233` 은 **a3 가 없으면 a2**(때린 이)를 넣는다 —
        //   그 자리가 `Damage` 의 else 가지(`cls = 1 · player = by`)다
        Damage(t.addr, hp, out, by, src ? &hit : nullptr);   // unitDamageClear_4EE5E0
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
    if (k.buffs.Has(23)) return;                           // 무적(소리 71 → 10)
    // ⚠ 몬스터 +2188·불 종류 +1440 깃발 · 죽은 좀비 갈래는 몬스터 AI(7)
    if (!OwnersEnemy(k.owner, k.owner.empty(), by, byWild)) return;   // 편 거르기(자기가 자기를 때리는 경우는 없음)
    const Target attacker = DamageAttacker(by, src);
    DamageShockBack(t, k.buffs, attacker, src, out);       // 감전(22) 되갚기 — 되갚는 이는 사람(무기)이라 소환수 목록은 안 바뀐다
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
    amount = DamageProtect(pets_[i].buffs, static_cast<uint8_t>(v15), amount);   // 불·전기 보호(버프 17·20 + 아이템 5)
    if (!attacker.none() || v15 == 12) BuffOff(t, 0);      // 맞으면 투명 풀림
    DamageVampirism(attacker, t, amount, out);             // 흡혈(13)
    // ⚠ 마법 방패(26) 줄이기(4-d) · 맞는 소리(10) · 몬스터 원한(7)
    k.lastDamageFrame = frame_;                            // +536(`GAME3_2.c:7236` — 몬스터도 같은 자리)
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
                    const std::string& by, const Session::LastHit* hit, bool ignoreInvuln) {
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
    if (!ignoreInvuln && s.buffs.Has(23)) {                // 버프 23(4-a4) · unitDamageClear(DEATH 버프)는 안 본다
        ++invulnBlocked_;
        return false;
    }
    // +520 마지막으로 때린 것(3-d) — 피해 함수가 넘긴 것, 없으면 `by`(사람 · 무기 없음)로
    if (hit) {
        s.lastHit = *hit;
    } else {
        s.lastHit = Session::LastHit{};
        if (!by.empty()) { s.lastHit.cls = 1; s.lastHit.player = by; }
    }

    const int before = s.health;
    int after = before - amount;
    if (after < 0) after = 0;
    s.health = static_cast<uint16_t>(after);
    // ★ R4 — 체력이 바뀌었다. 「알려야 할 것」에 올린다(보낼지는 틱 끝에서 가른다).
    if (after != before) MarkDirty(s.netCode, PendingReport::What::UnitHealth);

    // ①② 09-14 갱신 알림 정답지화 — `42` 증감·`43` 절대값을 여기서 곧바로 보내던 것을 걷어냈다.
    //    `42` 는 갱신 스트림이 **보는 사람마다** 마지막 피해(+536) 뒤 2틱 넘어 **음수만** 보낸다(ReportHealthDeltaTo · Packet0)
    //    `43` 은 다음 틱 PlayerReportAnything 이 +10 과 다를 때 보낸다(netSendPlrHealthToTeam_4D86E0 · Packet1 · 본인만)
    s.lastDamageFrame = frame_;                            // +536 = gameFrame()(`GAME3_2.c:7236`)

    if (s.health > 0) return false;

    // ★ 죽음 알림 `a9 0e` — diePlayer_54D2B0 가 소리·DEAD 깃발보다 먼저 보낸다(`GAME5.c:5051`, 묶음 3-d)
    SendKillInform(s, out);

    // ★ 7-c — 알림 바로 뒤가 점수 셈 자리다(`GAME5.c:5051` 알림 → `:5078` 셈).
    //   이 서버는 늘 아레나(깃발 256)라 `playerUpdateScore_54D980` 갈래만 탄다
    PlayerUpdateScore(s, out);

    // ③ 죽음 — 껍데기 안. 실측 `e8 d8 02`.
    //    ★ **모두에게.** 남이 쓰러지는 것을 봐야 판이 판이 된다.
    {
        uint8_t msg[3];
        msg[0] = static_cast<uint8_t>(Op::PLAYER_DIED);
        msg[1] = static_cast<uint8_t>(s.netCode);
        msg[2] = static_cast<uint8_t>(s.netCode >> 8);
        Broadcast(/*viaSeq=*/true, msg, sizeof(msg), out);
    }
    // ★ 2026-09-13 묶음 3-c — 죽음은 **상태**다(diePlayer_54D2B0 · `GAME5.c:5066~5068`: DEAD 깃발 → 상태 3).
    //   전에는 45틱 뒤 저절로 되살렸다(자리표시). 정답지는 상태 3 → 30틱 → 4 → 15틱 → **클릭해야** 부활(object_update.go:242~275).
    //   respawnAtFrame 은 이제 「죽은 프레임」(DEAD 깃발 겸) — 0 이면 산 것.
    s.respawnAtFrame = frame_ ? frame_ : 1;
    PlayerSetState(s, 3, frame_);
    DropFlag(addr, out);   // 들고 있었으면 그 자리에 떨군다
    // `GAME5.c:5098~5104` — setUnitBuffFlags(0) · 능력 끄기(7-d 에서 옮김) · +3600 = 0 · playerCancelSpells · unitClearBuffs
    SetBuffFlags(Target{ addr, -1 }, 0);
    s.assistSet = false;
    AbilityCancelAll(s, out);                              // 7-d — `GAME5.c:5100` 능력 끄기(`playerCancelAbils_4FC180`)
    PlayerCancelSpells(addr, out);                         // playerCancelSpells_4FEAE0(`GAME5.c:5103`, 4-a3)
    ClearBuffs(Target{ addr, -1 });
    // ⚠ 옮기지 않은 것 — 죽는 소리(10) · 가진 것 전부 떨구기(퀘스트 아니면 · 아이템 5)
    //   · 모드별 점수 정답지(8 — 지금은 아래 우리 점수) · 능력 끄기(7)

    // ④ 점수 — 때린 쪽이 있고 자기 자신이 아니면 그쪽이 하나 얻는다.
    //    자기가 자기를 죽인 것(자기에게 건 주문)은 점수가 아니다.
    if (!by.empty() && by != addr) {
        auto k = sessions_.find(by);
        if (k != sessions_.end()) {
            // ★ 7-c — 점수 올리기는 `PlayerUpdateScore` 가 위에서 이미 했다(정답지 `playerUpdateScore_54D980`).
            //   여기 남은 것은 **판 끝 판정**뿐이다
            if (!roundOver_ && k->second.score >= static_cast<int32_t>(kArenaKillLimit)) {
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
                const Buffs* who = nullptr;                   // 4-a4 — 사람·불려 나온 것의 버프 비트(+340)
                for (const auto& kv2 : sessions_) {
                    if (kv2.second.inGame && kv2.second.netCode == pr.netCode) { who = &kv2.second.buffs; break; }
                }
                if (!who) for (const auto& k : pets_) if (k.netCode == pr.netCode) { who = &k.buffs; break; }
                if (!who) continue;
                const uint32_t bits = who->bits;
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
            PlayerManaAdd(*nearest, kObeliskManaPerTick);   // playerManaAdd_4EEB80 — `45` 는 다음 틱 PlayerReportAnything(+4 ≠ +6)
            ++obeliskMana_;
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
                GameServerReadyMB(s, out);                 // network_server.go:99 → nox_xxx_gameServerReadyMB_4DD180
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
            // ── 충돌 요청(묶음 3-d) — `7b <번호2>` · Server::TryCollide 주석 ──
            case Op::TRY_COLLIDE: {
                if (!s.inGame || it.bodyLen() != 2) break;
                TryCollide(s, static_cast<uint16_t>(it.body()[0] | (it.body()[1] << 8)), out);
                ++handled_;
                break;
            }

            case Op::TRY_ABILITY: {
                // ── 묶음 7-d 능력 쓰기 — `network_server.go:91`(`0x7A`)
                //   **몸 1바이트** — `<능력 번호>`. 채팅 모드가 아니고 `+3680 & 3`(관전)이 아니어야 한다
                if (!s.inGame || it.bodyLen() != 1) break;
                if (!s.observer) AbilityDo(s, it.body()[0], out);
                ++handled_;
                break;
            }

            case Op::TRY_CREATURE_COMMAND: {
                // ── 묶음 6-i 소환수 명령 — `sdecode.c:193`(`0x78`)
                //   **몸 3바이트** — `<번호 u16><명령 u8>`. 번호가 **0 이면 내 것 전부**에게 준다
                //   ⚠ 정답지는 사람 자료 `+3680 & 1`(명령 금지)일 때 통째로 무시한다 — 그 깃발은 아직 없다
                if (!s.inGame || it.bodyLen() != 3) break;
                const uint16_t nc = static_cast<uint16_t>(it.body()[0] | (it.body()[1] << 8));
                OrderUnit(s, nc, it.body()[2]);
                ++handled_;
                break;
            }

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

                // ── 묶음 5-h — **금은 가방에 안 들어간다**(`pickupGold_4F3A60`) ──
                //
                // 사람이면 **금액을 더하고 물건을 없앤다**. 사람이 아니면(몬스터) 보통 줍기로 간다.
                // ⚠ 안 옮긴 것 — 줄글(`GoldPickup` 문자열 + 금액)과 소리 307 은 묶음 10.
                if (w.goldPickup) {
                    PlayerAddGold(s, w.goldAmount, out);
                    {   // 모두에게 — 바닥에서 사라진다
                        uint8_t msg[3];
                        msg[0] = static_cast<uint8_t>(Op::DESTROY_OBJECT);
                        msg[1] = static_cast<uint8_t>(w.netCode);
                        msg[2] = static_cast<uint8_t>(w.netCode >> 8);
                        Broadcast(/*viaSeq=*/false, msg, sizeof(msg), out);
                    }
                    TakeStatic(w.netCode);
                    ++goldPicked_;
                    ++handled_;
                    break;
                }

                // ── 묶음 5-c — 물약은 **줍는 순간이 다르다**(`nox_xxx_pickupPotion_4F37D0`) ──
                //
                // `PICKUP = PotionPickup` 인 것만 이 길로 온다(thing.bin 실측 38종).
                //   ① **직업이 못 쓰는 것이면 아예 못 줍는다** — 글 `pickup.c:ObjectEquipClassFail`
                //   ② 마실 수 있으면 **그 자리에서 마시고 없어진다**(가방에 안 들어간다)
                //   ③ 못 마시면 그냥 가방으로 — 아래 5-a 규칙을 그대로 탄다
                if (w.potionPickup) {
                    const int r = PickupPotion(s, w, out);
                    if (r < 0) { ++pickClassFail_; ++handled_; break; }   // 못 줍는다
                    if (r > 0) {                                          // 그 자리에서 마셨다
                        {   // 모두에게 — 바닥에서 사라진다
                            uint8_t msg[3];
                            msg[0] = static_cast<uint8_t>(Op::DESTROY_OBJECT);
                            msg[1] = static_cast<uint8_t>(w.netCode);
                            msg[2] = static_cast<uint8_t>(w.netCode >> 8);
                            Broadcast(/*viaSeq=*/false, msg, sizeof(msg), out);
                        }
                        TakeStatic(w.netCode);
                        ++drankOnPickup_;
                        ++handled_;
                        break;
                    }
                }

                // ── 묶음 5-a — 줍기 규칙(`nox_xxx_pickupDefault_4F31E0`) ──────────
                //
                // ① **들 밑천이 0 이면 아예 못 든다**(`object_pickup.go:23·68`).
                //    ⚠⚠ 밑천은 thing.bin 의 `CARRYCAPACITY = 100` 이 **아니다**. 그건 오브젝트를
                //    만들 때의 기본값이고(`server/object.go:330`), 사람은 접속 때 `plrReadVals_4EEDC0` 가
                //    **힘에서 다시 계산해 `+490` 에 덮어쓴다**(`GAME3_3.c:7704`) — 위저드 1650.
                //    `Session::carryCap` 주석에 근거를 적어 두었다.
                if (s.carryCap == 0) { ++pickNoCap_; ++handled_; break; }

                // ② **무게 한도** — `무게 > 밑천 × 2 − 이미 든 무게` 면 거절하고 글을 낸다
                //    (`object_pickup.go:70~79`). 한도가 밑천의 **두 배**인 것이 정답지 식이다.
                {
                    int carried = 0;
                    for (const auto& c2 : s.inventory) carried += static_cast<int>(c2.weight);
                    if (static_cast<int>(w.weight) > static_cast<int>(s.carryCap) * 2 - carried) {
                        SendPriMsg(s, "pickup.c:CarryingTooMuch", out);
                        ++pickTooHeavy_;
                        ++handled_;
                        break;
                    }
                }

                // ③ **먹는 것은 같은 종류를 셋까지**(퀘스트·협동은 아홉 — `object_pickup.go:81~92`).
                if (w.isFood) {
                    int same = 0;
                    for (const auto& c2 : s.inventory) if (c2.type == w.type) ++same;
                    const bool quest = (questTotal_ > 0 || questStage_ > 0);   // gameFlags 4096|8192
                    if (same >= (quest ? 9 : 3)) {
                        SendPriMsg(s, "pickup.c:MaxSameItem", out);
                        ++pickTooMany_;
                        ++handled_;
                        break;
                    }
                }
                // ⚠ 안 옮긴 갈래 — 물건에 **편**이 붙어 있으면 남의 편은 못 줍고 알림글 16 이 간다
                //   (`object_pickup.go:54~64`). 우리 `WorldStatic` 에는 편 칸이 없다(맵 물건은 편이 없다).
                //   깃발·편 물건이 생기는 묶음 8 에서 붙인다.

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
                c.useAmount = w.useAmount;
                c.equipBit = w.equipBit;
                c.isWeapon = w.isWeapon;
                c.weight = w.weight;
                c.isFood = w.isFood;
                c.foodBits = w.foodBits;
                c.useFn = w.useFn;
                c.charges = w.charges; c.chargesMax = w.chargesMax;
                c.wandSpell = w.wandSpell; c.wandCooldown = w.wandCooldown;
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
                // ★ 묶음 5-d — **무엇을 부를지는 `USE` 의 앞말이 정한다**(`UseFn`).
                //   전에는 무엇이든 물약 길로 보냈다 — 사과·버섯·사이다가 조용히 아무 일도 안 했다.
                bool spent = false;
                switch (c.useFn) {
                    case UseFn::Potion:         spent = UsePotion(s, c, out); break;
                    case UseFn::Consume:        spent = UseConsume(s, c, out); break;
                    case UseFn::Mushroom:       spent = UseMushroom(s, out); break;
                    case UseFn::ConsumeConfuse: spent = UseCiderConfuse(s, c, out); break;
                    case UseFn::WandCast:       spent = UseWandCast(s, s.inventory[at], out); break;   // 5-e — 충전만 깎고 안 없어짐
                    default: break;             // 아직 안 옮긴 갈래(책·완드·활) — 아무 일도 안 난다
                }
                if (spent) {
                    // 쓴 것은 없어진다. 세계에 되돌려 놓지 않는다.
                    s.inventory.erase(s.inventory.begin() + static_cast<long>(at));
                    ++used_;
                } else {
                    ++useUnknown_;
                }
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
                    back.useAmount = s.inventory[i].useAmount;
                    back.equipBit = s.inventory[i].equipBit;
                    back.isWeapon = s.inventory[i].isWeapon;
                    back.weight = s.inventory[i].weight;
                    back.isFood = s.inventory[i].isFood;
                    back.foodBits = s.inventory[i].foodBits;
                    back.useFn = s.inventory[i].useFn;
                    back.charges = s.inventory[i].charges; back.chargesMax = s.inventory[i].chargesMax;
                    back.wandSpell = s.inventory[i].wandSpell; back.wandCooldown = s.inventory[i].wandCooldown;
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
                // ★★ 2026-09-13 묶음 4-a — 정답지 받기 그대로(sdecode:211 MSG_TRY_SPELL · 본문 21).
                //   u32 × 5 주문(글리프면 여럿) · u8 대상 깃발(0 이 아니면 자기에게). 여기서는 **넣기만** 한다 —
                //   음소를 3틱마다 하나씩 따라가다 끝나야 검사·마나·효과가 난다(SpellCastByBook · PlayerSpell).
                //   ⚠ 전에는 받자마자 마나를 깎고 `70` 을 중요로 보내고 효과를 냈다(자리표시). `70` 은 Kind1(틱 목록)이다.
                if (!s.inGame || it.bodyLen() != kTrySpellBodyLen) break;
                uint32_t sp[5] = {};
                for (int k = 0; k < 5; ++k) {
                    const auto* b = it.body() + 4 * k;
                    sp[k] = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
                            (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
                }
                const bool targetSelf = it.body()[20] != 0;
                bool ok = true;
                if (s.observer) {
                    SendPriMsg(s, "GeneralPrint:NoSpellWarningGeneral", out);   // 3680 & 1
                    ok = false;
                }
                // 3680 & 2(몬스터 지켜보기 — "ConjureNoSpellWarning1") 는 관전 기능(8) · +16 & 0x4000 은 아직 켜는 곳 없음
                if (ok) {                                  // 게임 깃발 128(Chat 모드) 아님
                    int count = 0;
                    for (int k = 0; k < 5; ++k) if (sp[k]) ++count;
                    const bool quest = (questTotal_ > 0 || questStage_ > 0);
                    bool friendGate = false;               // 「주문 하나 · 공격 주문 · 커서 오브젝트가 친구」면 넣지도 않는다
                    if (count == 1 && sp[0] < 137 && (SpellFlags(static_cast<uint8_t>(sp[0])) & 0x20) && !quest) {
                        const Target cur = FindObjectAtCursor(s);
                        if (!cur.none() && !TargetIsEnemy(s, cur)) friendGate = true;
                    }
                    if (!friendGate && !SpellByBookInsert(s, sp, count, 3, targetSelf, out) && count == 1) {
                        for (int k = 0; k < 5; ++k) if (sp[k]) ReportSpellStat(s, sp[k], 0, out);
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
                    // 온라인 = EngineAdmin → serverSetAllSpells(p, true, 0): 주문 1~136 전부 세기 3(spells.go:117) · 직업 거르기는 시전 때
                    for (int k = 1; k < 137; ++k) s.spellLvl[k] = 3;
                    s.strength  = StrengthOnline(s.charClass);
                    s.carryCap  = static_cast<uint16_t>(CarryCapacityOnline(s.charClass));   // +490(묶음 5)
                    s.health = s.maxHealth;
                    s.mana   = s.maxMana;
                    s.manaPrev = s.mana;                   // sub_4EFF10 +4 = +6 = 최대(`GAME3_3.c:8103`)
                    s.statsDirty = true;                   // plrReadVals_4EEDC0 끝 +2184 = 1(`GAME3_3.c:7723`) — 스냅샷 꼬리가 싣고 끈다
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
                    std::vector<uint8_t> jb = BuildJoinBundle(s.netCode, s.frame, info97, gameSettings_);
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
bool Server::MapTraceRayTile(int gx, int gy, float x1, float y1, float x2, float y2, uint8_t flags, float* hx, float* hy) const {
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
    // 교차점 — 왼쪽·오른쪽 대각선(둘 다면 p1 에 가까운 것이 첫째 · `server/wall.go:1110~1133`)
    bool hitL = false, hitR = false;
    float lx = 0, ly = 0, rx = 0, ry = 0;
    if (t != 0 && s.l) {
        CrossLeft(x1, y1, x2, y2, ox, oy, lx, ly);
        hitL = (ox + s.lmin <= lx && ox + s.lmax >= lx && lx >= xl && lx <= xh && ly >= yl && ly <= yh);
    }
    if (t != 1 && s.r) {
        CrossRight(x1, y1, x2, y2, ox + kGrid, oy, rx, ry);
        hitR = (ox + s.rmin <= rx && ox + s.rmax >= rx && rx >= xl && rx <= xh && ry >= yl && ry <= yh);
    }
    if (!hitL && !hitR) return false;
    if (hx && hy) {
        bool useRight = !hitL;
        if (hitL && hitR) {
            const float dxl = x1 - lx, dxr = x1 - rx, dyl = y1 - ly, dyr = y1 - ry;
            useRight = (dyr * dyr + dxr * dxr < dyl * dyl + dxl * dxl);
        }
        *hx = useRight ? rx : lx;
        *hy = useRight ? ry : ly;
    }
    return true;
}

// MapTraceRayAt(`server/wall.go:898`) — p1→p2 가 벽에 안 막히면 참. 걸음·겨눔 모두 이 길.
//   ⚠ 정답지 그대로의 걸음(곧은 가로·세로 먼저, 그 밖은 기울기 따라 칸 줄 훑기).
bool Server::MapTraceRay(float x1, float y1, float x2, float y2, uint8_t flags, TraceHit* hit) const {
    const int wx1 = static_cast<int>(x1 / kGrid), wy1 = static_cast<int>(y1 / kGrid);
    if (wx1 < 0 || wx1 >= 256 || wy1 < 0 || wy1 >= 256) return false;
    const int wx2 = static_cast<int>(x2 / kGrid), wy2 = static_cast<int>(y2 / kGrid);
    if (wx1 == wx2) {
        const int step = (y2 < y1) ? -1 : 1;
        if (wy1 == wy2 + step) return true;
        for (int yi = wy1; yi != wy2 + step; yi += step)
            if (MapTraceRayTile(wx1, yi, x1, y1, x2, y2, flags, hit ? &hit->x : nullptr, hit ? &hit->y : nullptr)) { if (hit) { hit->gx = wx1; hit->gy = yi; } return false; }
        return true;
    }
    if (wy1 == wy2) {
        const int step = (x2 < x1) ? -1 : 1;
        if (wx1 == wx2 + step) return true;
        for (int xi = wx1; xi != wx2 + step; xi += step)
            if (MapTraceRayTile(xi, wy1, x1, y1, x2, y2, flags, hit ? &hit->x : nullptr, hit ? &hit->y : nullptr)) { if (hit) { hit->gx = xi; hit->gy = wy1; } return false; }
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
                if (MapTraceRayTile(xi, yi, x1, y1, x2, y2, flags, hit ? &hit->x : nullptr, hit ? &hit->y : nullptr)) { if (hit) { hit->gx = xi; hit->gy = yi; } return false; }
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
            if (MapTraceRayTile(xi, yi, x1, y1, x2, y2, flags, hit ? &hit->x : nullptr, hit ? &hit->y : nullptr)) { if (hit) { hit->gx = xi; hit->gy = yi; } return false; }
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
//     승강기(551AE0/551C40)·상자 대 상자(550F80) · 충돌 기록은 4-b1 부터(콜백은 4FDF90 버프 몫만 — 사람·몬스터 충돌 함수는 7)
void Server::BodyVsBody(const PhysBody& a1, double nx, double ny, double bx, double by, double vx, double vy,
                        const PhysBody& a2, double ox, double oy, double obx, double oby, double ovx, double ovy,
                        double& ax, double& ay, const ContactRef* ra, const ContactRef* rb) const {
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
        if (ra && rb) AddContact(*rb, *ra);                // collSysAddCollision_548630(a2, a1) — 4-b1
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
        if (ra && rb) AddContact(*ra, *rb);                // collSysAddCollision_548630(a1, a2) — 4-b1
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
        case 2: case 10: return 21;                                          // 시전 CAST_SPELL(`:1740~1743`, 4-a)
        case kPlayerStateIdle: return (equip & 0x400) ? 0x26 : kAnimIdle;   // 대검이면 GREAT_SWORD_IDLE(`:1756~1758`)
        case 30: return 52;                                                  // 움찔 RECOIL(`:1793`)
        case 3: case 4:                                                      // 죽음 DIE 1 · 죽어 있음 DEAD 2
            return (s.lastDamageType == 16) ? 51 : (s.moveState == 3 ? 1 : 2);   // 종류 16 이면 DUST(`GAME4_1.c:4457~4461`)
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
        // ⓪' 죽음(묶음 3-c) — `object_update.go:242~275` 상태 3: 30틱 넘으면 4(속도·힘 0) · 상태 4: 15틱 넘으면
        //   관전 아니고 CCAction(클릭) 이 왔거나 자동 부활 설정이면 부활. 그 틱의 조작 목록은 비운다(`cb.Reset()`).
        //   ⚠ 제거전 한도·관전 넘기기는 모드 규칙(8)
        bool respawnedNow = false;                         // 이 틱의 ImplA 는 상태 4 갈래였다 — 13 갈래를 또 돌리지 않는다
        if (s.respawnAtFrame != 0) {
            if (s.moveState == 3 && (frame - s.stateFrame) > kServerTickRate) {
                PlayerSetState(s, 4, frame);
                s.stateFrame = frame;
                s.velX = s.velY = 0.0;
                s.forceX = s.forceY = 0.0;
            } else if (s.moveState == 4 && (frame - s.stateFrame) > kServerTickRate / 2 && !s.observer &&
                       (s.actionThisTick || autoRespawn_)) {
                s.inputThisTick = s.moveInputThisTick = s.actionThisTick = s.hasFacingInput = false;
                PlayerRespawn(s, frame, out);
                respawnedNow = true;
            }
        }
        const bool dead = (s.respawnAtFrame != 0);
        const bool canMove = !dead && !s.observer && !respawnedNow;

        // ⓪ 기력 회복(`object_update.go:108` — 죽지 않았으면 100 아래일 때 100/30 씩, ImplA 보다 먼저)
        if (!dead && s.stamina < 100) s.stamina = static_cast<uint8_t>(s.stamina + 100 / kServerTickRate);
        // ① 힘 · 공격(ImplA)
        double fx = 0.0, fy = 0.0;
        bool run = false;
        if (canMove && (s.moveState == kPlayerStateWalk || s.moveState == kPlayerStateRun) && !s.buffs.Has(25) && !s.buffs.Has(5)) {
            // `object_update.go:132~191` — playerCanMove(얼림 25·붙잡힘 5) · 달리기면 SpeedCur × 2(그 틱만) ·
            //   혼란(3)이면 Direction2 를 흔든다(playerConfusedGetDirection_4F7A40) · 힘 = Direction2 방향 표 × SpeedCur
            //   ⚠ 무게 넘침(+3656 → inform 13 3)·살금살금 능력·발소리는 아이템 5·능력 7·소리 10 · 작살 끌림 sub_4F9AB0 은 7
            const double cdx = s.aimX - s.posX, cdy = s.aimY - s.posY;
            run = !(s.moveState != kPlayerStateRun && cdx * cdx + cdy * cdy <= kRunCursorDist * kRunCursorDist);
            const double speed = static_cast<double>(s.speedCur) * (run ? 2.0 : 1.0);
            if (s.buffs.Has(3)) s.facing = ConfusedDirection(s, frame);
            fx += DirVecX(s.facing) * speed;
            fy += DirVecY(s.facing) * speed;
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
        } else if (canMove && s.moveState == 2) {
            // 시전(4-a) — `object_update.go:235~241` CAST_SPELL(21) 칸 = (프레임 − Field34) ÷ (붙듦+1) 을 u8 로 · 칸 수 넘으면 마지막 칸
            const int frames = playerAnimFrames_[21][0], hold = playerAnimFrames_[21][1];
            s.animFrame = static_cast<uint8_t>((frame - s.stateFrame) / static_cast<uint32_t>(hold + 1));
            if (static_cast<int>(s.animFrame) >= frames) s.animFrame = static_cast<uint8_t>(frames - 1);
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
            if (s.hasFacingInput && !s.buffs.Has(25)) s.facing = s.facingInput;   // CCOrientation(`:497~502`) — 얼림(25)이면 안 돔 · ⚠ 퀘스트 거래·광전사는 5·7
            // 자연의 힘(31) 지속 중이면 방향 입력만 받는다(`:496` orientationOnly = Sub4FEE50 · 4-a3)
            const bool orientationOnly = DurActive(31, s.addr);
            // 이동 — playerCanMove 면 붙들고 있는 주문 취소(cancelAllSpells_4FEE90, `:509~510`) ·
            //   맨손 공격 중(상태 1)에는 안 먹는다(`:506~507`) · 방패 올린 채(16)면 먼저 내린다(17, `:513~514`)
            //   playerCanMove_4F9BC0 — 얼림(25)·붙잡힘(5) 버프면 못 움직임 ⚠ 퀘스트 거래 · 상태 1 + 특정 무기 갈래는 아이템(5)
            const bool playerCanMove = !s.buffs.Has(25) && !s.buffs.Has(5);
            if (s.moveInputThisTick && !orientationOnly && playerCanMove) {
                CancelChannelSpells(s.addr, out);
                if (s.moveState != kPlayerStateAttack) {
                    if (s.moveState == 16) {
                        PlayerSetState(s, 17, frame);
                    } else {
                        PlayerSetState(s, run ? kPlayerStateRun : kPlayerStateWalk, frame);
                        s.stateFrame = frame;
                    }
                }
            }
            // CCAction(`:536~544`) — playerCanAttack(버프 25 없음 · 상태 23 아님) → 공 차기 아니면 playerInputAttack
            if (s.actionThisTick && !orientationOnly && !s.buffs.Has(25) && s.moveState != 23) PlayerInputAttack(s, frame, out);
        }
        // 달리기(8) 버프 — 공격 중(1)이 아니면 상태 5(달림)로 묶는다(`object_update.go:120~122`)
        if (canMove && s.buffs.Has(8) && s.moveState != kPlayerStateAttack) PlayerSetState(s, kPlayerStateRun, frame);
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

    // ②' 날아가는 것들(4-a2) — 정답지 순서 BBB(갱신) → CallUpdate(투사체) → updateCollide(적분). 맞힘은 사람의 지난 틱 끝 자리 기준
    UpdateMissiles(frame, out);

    // ③ 적분 — 걸음(0.2)마다 [모두 충돌] → [모두 적분]. 사각형 칸 범위는 **프레임 시작 자리**(정답지 +232 는 +56 PosVec).
    PhysBody playerBody;
    playerBody.shape = 2; playerBody.r = static_cast<float>(kBodyRadius);
    playerBody.cls = kClsPlayer; playerBody.flags = 0; playerBody.mass = static_cast<float>(kPlayerMass);   // NewPlayer FLAGS NULL
    contacts_.clear();                                     // nox_xxx_allocHitArray_5486D0
    for (int i = 0; i < kMoveSubSteps; ++i) {
        for (auto& m : movers) {
            Session& s = *m.s;
            m.ax = m.ay = 0.0;
            if (!m.collide) continue;
            const ContactRef me{ 1, s.addr, -1 };
            double qx0, qy0, qx1, qy1;
            BodyRect(playerBody, m.bx, m.by, qx0, qy0, qx1, qy1);
            // 오브젝트 먼저(getUnitsInRect → sub_548220) — 상대 사각형과 겹칠 때만(`server/map.go:302`)
            for (const auto& o : obstacles_) {
                double ox0, oy0, ox1, oy1;
                BodyRect(o, o.x, o.y, ox0, oy0, ox1, oy1);
                if (!(ox0 < qx1 && ox1 > qx0 && oy0 < qy1 && oy1 > qy0)) continue;
                const ContactRef other{ 3, std::string(), static_cast<int>(&o - obstacles_.data()) };
                BodyVsBody(playerBody, s.posX, s.posY, m.bx, m.by, s.velX, s.velY,
                           o, o.x, o.y, o.x, o.y, 0.0, 0.0, m.ax, m.ay, &me, &other);
            }
            for (const auto& m2 : movers) {
                if (m2.s == m.s || !m2.collide) continue;
                double ox0, oy0, ox1, oy1;
                BodyRect(playerBody, m2.bx, m2.by, ox0, oy0, ox1, oy1);
                if (!(ox0 < qx1 && ox1 > qx0 && oy0 < qy1 && oy1 > qy0)) continue;
                const ContactRef other{ 1, m2.s->addr, -1 };
                BodyVsBody(playerBody, s.posX, s.posY, m.bx, m.by, s.velX, s.velY,
                           playerBody, m2.s->posX, m2.s->posY, m2.bx, m2.by, m2.s->velX, m2.s->velY, m.ax, m.ay, &me, &other);
            }
            for (const auto& k : pets_) {
                auto tb = typeBodies_.find(k.type);
                if (tb == typeBodies_.end()) continue;   // 몸을 모르는 종류는 안 부딪힌다(지어내지 않는다)
                if (!tb->second.hasCollide || (tb->second.shape != 2 && tb->second.shape != 3)) continue;   // COLLIDE 없음 · 몸 없음
                double ox0, oy0, ox1, oy1;
                BodyRect(tb->second, k.x, k.y, ox0, oy0, ox1, oy1);
                if (!(ox0 < qx1 && ox1 > qx0 && oy0 < qy1 && oy1 > qy0)) continue;
                const ContactRef other{ 2, std::string(), static_cast<int>(&k - pets_.data()) };
                BodyVsBody(playerBody, s.posX, s.posY, m.bx, m.by, s.velX, s.velY,
                           tb->second, k.x, k.y, k.x, k.y, 0.0, 0.0, m.ax, m.ay, &me, &other);
            }
            // 벽 — 오브젝트 다음(sub_5481C0 순서)
            CircleVsWalls(m.bx, m.by, kBodyRadius, kPlayerMass, s.posX, s.posY, s.velX, s.velY, m.ax, m.ay);
        }
        for (auto& m : movers) {
            Session& s = *m.s;
            // updateObjectsVelocity_5118A0(`GAME4_1.c:3205~3213`) — 붙잡힘(5)·얼림(25)·매혹(28)이면 ForceVec(+88: 걸음·밀림)은 빼고 부딪힘 힘(+96)만
            const bool pinned = s.buffs.Has(5) || s.buffs.Has(25) || s.buffs.Has(28);
            const double pfx = pinned ? 0.0 : m.fx, pfy = pinned ? 0.0 : m.fy;
            s.velX += ((m.ax + pfx) - s.velX * kPlayerDamping) * kMoveSubStep;
            s.velY += ((m.ay + pfy) - s.velY * kPlayerDamping) * kMoveSubStep;
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
    // nox_xxx_collide_548740 — 부딪힌 짝마다 a.collide(b)·4FDF90(a, b) → b.collide(a)·4FDF90(b, a)
    //   ⚠ 충돌 함수(+696 — PlayerCollide 광전사 · MonsterCollide 등)는 7 · 여기서는 버프 몫 4FDF90 만(4-b1)
    {
        const auto pairs = contacts_;
        const size_t petCount = pets_.size();
        for (const auto& pr : pairs) {
            // 되갚기로 소환수가 죽으면 자리 번호가 밀린다 — 그 뒤 소환수 짝은 건너뛴다(정답지는 지우기가 틱 끝이라 안 밀림)
            if (pets_.size() != petCount && (pr.first.kind == 2 || pr.second.kind == 2)) continue;
            CollideBuffs(pr.first, pr.second, out);
            CollideBuffs(pr.second, pr.first, out);
        }
    }
    for (auto& m : movers) {
        m.s->x = static_cast<int>(std::lround(m.s->posX));
        m.s->y = static_cast<int>(std::lround(m.s->posY));
        m.s->facingPrev = m.s->facing;   // CCC: Direction1 = Direction2(`server.go:167`)
        m.s->prevX = m.bx; m.s->prevY = m.by;   // CCC: PrevPos(+72) = 이번 틱 시작 자리(movers 는 게임 안 모두 — 관전·죽음 포함)
    }
    // CCC: SpeedCur = (SpeedBonus + SpeedBase) × (1 − 0.2 × +541) · 느림(4)이면 × 0.5(`server.go:171~174`) — 빠르게(9) × 1.25 는 updateUnitBuffs 끝
    //   ⚠ SpeedBonus(아이템 무게·마법부여 5) · +541 느려짐 겹수(독·5) 는 0
    for (auto& m : movers) {
        Session& s = *m.s;
        s.speedCur = static_cast<float>((0.0 + kPlayerWalkPerFrame) * (1.0 - 0.2 * 0));
        if (s.buffs.Has(4)) s.speedCur *= 0.5f;
    }
    // CCC: updateUnitBuffs_4FF620(`server.go:189`) — 버프 남은 틱 −1 · 0 이면 끔(4-a4)
    std::vector<std::string> buffUnits;
    for (auto& m : movers) buffUnits.push_back(m.s->addr);
    for (const auto& a : buffUnits) UpdateUnitBuffs(Target{ a, -1 }, out);
    for (size_t i = pets_.size(); i-- > 0;) UpdateUnitBuffs(Target{ std::string(), static_cast<int>(i) }, out);   // 몬스터도 CCC 목록에(죽으면 지워지니 뒤에서부터)
    // 묶음 7-d — `serverAbilities::Update` · 쉬는 시간 −1 · 지속이 다한 것 끄기
    for (auto& kv : sessions_) if (kv.second.inGame) AbilityUpdate(kv.second, out);
    // 묶음 7-g — 작살 줄(`updateHarpoon_54F380` + `UpdatePlayer`)
    for (auto& kv : sessions_) if (kv.second.inGame) HarpoonUpdate(kv.second, out);
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
//   ★ 4-a2 — getMissilesInCircle 은 투사체 목록 전부를 돈다 → 날리는 Magic(missiles_)도 넣는다 · 바라보는 쪽은 방향 표(194136)
bool Server::IncomingMissile(const Session& s) const {
    const float px = static_cast<float>(s.posX), py = static_cast<float>(s.posY);
    const float dirX = DirVecX(s.facingPrev);
    const float dirY = DirVecY(s.facingPrev);
    auto test = [&](float mx, float my, float prevX, float prevY, double vx, double vy, const std::string& owner, const Buffs* mb) {
        if ((mx - px) * (mx - px) + (my - py) * (my - py) > 100.0f * 100.0f) return false;   // getMissilesInCircle 100
        if (owner == s.addr) return false;                                                   // 같은 사슬
        auto ow = sessions_.find(owner);
        if (ow != sessions_.end() && ow->second.team != 0 && ow->second.team == s.team) return false;   // 같은 팀
        if (!(TestTwoPointsAndDirection(px, py, s.facingPrev, prevX, prevY) & 1)) return false;   // +72
        const double v5 = vx * dirX, v6 = vy * dirY;
        if (!(v5 + v6 < 0.0)) return false;
        const float v10 = mx - px, v11 = my - py;
        double v8 = static_cast<double>(dirX) * v11 + -static_cast<double>(dirY) * v10;
        if (v8 < 0.0) v8 = -v8;
        if (!(v8 < 20.0)) return false;
        const double v9 = std::sqrt(vx * vx + vy * vy);
        const double v14 = std::sqrt(static_cast<double>(v11) * v11 + static_cast<double>(v10) * v10);
        if (v9 == 0.0 || v14 == 0.0) return false;
        if (!(vy / v9 * -(v11 / v14) + vx / v9 * -(v10 / v14) > 0.69999999)) return false;
        if (!CanSeeBuffs(&s.buffs, mb, vx, vy, 0) || !MapTraceVision(px, py, mx, my)) return false;   // CanInteract(나, 투사체, 0)
        return v14 < 100.0;                                                                  // 가장 가까운 것 < 100
    };
    for (const auto& m : missiles_) {
        if (test(m.posX, m.posY, m.prevX, m.prevY, m.velX, m.velY, m.owner, &m.buffs)) return true;
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
bool Server::PlayerSetState(Session& s, uint8_t st, uint32_t frame, std::vector<OutPacket>* out) {
    bool res = true;
    if (s.respawnAtFrame != 0 && st != 3 && st != 4) return false;   // DEAD 면 3·4 만(`:1398`)
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
                BuffOff(Target{ s.addr, -1 }, 0);
                BuffOff(Target{ s.addr, -1 }, 23);
                if (out) DurCancelFor(67, s.addr, *out);
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

// `GAME4.c:1211` mapFindPlayerStart_4F7AB0 — PlayerStart 중 **적에게서 가장 가까운 거리가 가장 먼** 곳.
//   적(나 말고 · IsEnemyTo)이 하나도 없거나 고른 곳이 없으면 무작위(randomInt(0, 수−1)). PlayerStart 가 없으면 (2000,2000).
//   ⚠ PlayerStart 의 ENABLED 깃발·팀(sub_4F7CE0)은 맵 xfer 를 아직 안 읽어 모두 켜짐·팀 없음으로 본다(맵 묶음 9).
bool Server::FindPlayerStart(const Session& s, float& x, float& y) {
    if (playerStarts_.empty()) { x = 2000.0f; y = 2000.0f; return false; }
    bool noEnemy = true;                                   // v8
    int best = -1;                                         // j
    float bestD = 0.0f;                                    // v17
    for (size_t i = 0; i < playerStarts_.size(); ++i) {
        float nearest = 10000000.0f;                        // v15
        for (const auto& kv : sessions_) {
            const Session& o = kv.second;
            if (&o == &s || !o.inGame || !IsEnemyTo(s, o)) continue;
            const double dx = playerStarts_[i].first - o.posX, dy = playerStarts_[i].second - o.posY;
            const double d2 = dy * dy + dx * dx;
            if (d2 < nearest) nearest = static_cast<float>(d2);
            noEnemy = false;
        }
        if (nearest > bestD) { best = static_cast<int>(i); bestD = nearest; }
    }
    if (noEnemy || best < 0) best = static_cast<int>(NextRandom(static_cast<uint32_t>(playerStarts_.size())));
    x = playerStarts_[best].first;
    y = playerStarts_[best].second;
    return true;
}

// `object.go:722~731` unitMove 의 플레이어 몫 — 온라인이면 `ea <프레임4>` 를 그 사람에게(오래된 정적 그림을 지우라는 뜻,
//   클라 sub_45A670) · Sub4DE4D0: 정적 오브젝트의 「보이는 중」 비트를 꺼서 다시 소개되게.
void Server::ForgetDrawables(Session& s, std::vector<OutPacket>& out) {
    const uint32_t f = s.tickFrame + 1;                    // 이 틱 끝에 실리는 시각(27) 과 같은 값
    const uint8_t msg[5] = { static_cast<uint8_t>(Op::FORGET_DRAWABLES), static_cast<uint8_t>(f),
                             static_cast<uint8_t>(f >> 8), static_cast<uint8_t>(f >> 16), static_cast<uint8_t>(f >> 24) };
    EmitSeq(s, msg, sizeof(msg), out);
    s.staticIntroAt.clear();
}

// `GAME4.c:1420` playerRespawn_4F7EF0 + `GAME3_3.c:7901` playerMakeDefItems(a1, 1, 0) — 퀘스트 아닌 갈래.
//   ⚠ 옮기지 않은 것 — 기본 장비 다시 만들기(playerRespawnItem · 아이템 5 — 클라는 E9 의 무기 깃발로 겉모습을 되살린다) ·
//     부활 소리 148(10) · 부활 이펙트 respawnPlayerImpl_53FBC0(4) · 능력·주문 끄기(7·4) · 퀘스트 목숨·되살림 자리(8)
void Server::PlayerRespawn(Session& s, uint32_t frame, std::vector<OutPacket>& out) {
    s.health = s.maxHealth;                                // unitHPsetOnMax
    for (auto& kv : sessions_) if (kv.second.inGame) s.hpReportedTo[kv.first] = s.health;   // +12+2×i 서른둘 = 지금 체력(`GAME3_3.c:7937`) — 그 뒤 피해만 `42`
    PlayerManaRefresh(s);                                  // playerManaRefresh_4EECF0 — +6 = 이전 · +4 = 최대 → 다음 틱 `45`
    s.dmgRemainder = 0.0f;                                 // +84
    s.respawnAtFrame = 0;                                  // 깃발 &= 0xFFEB3FE7 — DEAD 끔
    PlayerSetState(s, kPlayerStateIdle, frame);
    ClearBuffs(Target{ s.addr, -1 });                      // unitClearBuffs_4FF580(아래에서 23 을 다시 켠다)
    PlayerCancelSpells(s.addr, out);                       // playerCancelSpells_4FEAE0(`GAME3_3.c:8119`, 4-a3)
    s.lastHit = Session::LastHit{};                        // +520 = 0(`GAME3_3.c:7958`, 3-d)
    MarkDirty(s.netCode, PendingReport::What::UnitHealth);
    // 나에게 — 최대·지금 체력(`dd`) · 마나는 워리어가 아니면(`de`)
    {
        const uint8_t dd[7] = { static_cast<uint8_t>(Op::REPORT_TOTAL_HEALTH), static_cast<uint8_t>(s.netCode),
                                static_cast<uint8_t>(s.netCode >> 8), static_cast<uint8_t>(s.health),
                                static_cast<uint8_t>(s.health >> 8), static_cast<uint8_t>(s.maxHealth),
                                static_cast<uint8_t>(s.maxHealth >> 8) };
        EmitSeq(s, dd, sizeof(dd), out);
        if (s.charClass != CharClass::Warrior) {
            const uint8_t de[7] = { static_cast<uint8_t>(Op::REPORT_TOTAL_MANA), static_cast<uint8_t>(s.netCode),
                                    static_cast<uint8_t>(s.netCode >> 8), static_cast<uint8_t>(s.mana),
                                    static_cast<uint8_t>(s.mana >> 8), static_cast<uint8_t>(s.maxMana),
                                    static_cast<uint8_t>(s.maxMana >> 8) };
            EmitImportantRawSupersede(s, de, sizeof(de), out);   // netReportTotalMana_4D88C0 → sub_4E5450(Packet0)
        }
    }
    // 모두에게 — `e9 <번호2> <프레임4> <무기 깃발> 01`(netSendPlayerRespawn_4EFC30 · 끝 1 = 클라가 기본 장비 겉모습을 다시 세움)
    //   무기 깃발 = getRespawnWeaponFlags_4EF580 — 서버에 쓸 수 있는 기본 장비 종류 비트. 모두 허용 = 0xff(녹음 값과 같음)
    //   ⚠ 프레임은 받는 사람마다 그 사람의 틱 번호로 싣는다(우리 서버는 접속마다 시계가 따로다 — Session::tickFrame)
    for (auto& kv : sessions_) {
        Session& o = kv.second;
        if (!o.inGame) continue;
        const uint32_t f = o.tickFrame + 1;               // 이 틱 끝에 실리는 시각(27) 과 같은 값 — 대기열이 시각 뒤에 나간다
        const uint8_t e9[9] = { static_cast<uint8_t>(Op::PLAYER_RESPAWN), static_cast<uint8_t>(s.netCode),
                                static_cast<uint8_t>(s.netCode >> 8), static_cast<uint8_t>(f), static_cast<uint8_t>(f >> 8),
                                static_cast<uint8_t>(f >> 16), static_cast<uint8_t>(f >> 24), 0xff, 0x01 };
        EmitSeq(o, e9, sizeof(e9), out);
    }
    // 자리 — 시작 지점 고르기 → 옮기기(FORGET_DRAWABLES)
    float sx = 0.0f, sy = 0.0f;
    FindPlayerStart(s, sx, sy);
    s.x = static_cast<int>(std::lround(sx));
    s.y = static_cast<int>(std::lround(sy));
    s.posX = sx; s.posY = sy;
    s.prevX = sx; s.prevY = sy;
    s.velX = s.velY = 0.0;
    ForgetDrawables(s, out);
    // 온라인이면 무적 버프 23 을 5초(5 × 30 틱)
    ApplyEnchant(Target{ s.addr, -1 }, 23, 5 * static_cast<int>(kServerTickRate), 5);   // buffApplyTo(23, 5×FPS, 5) · GAME4.c:2201
    std::printf("[부활] 프레임 %u %s → (%.0f,%.0f) · 시작 지점 %zu곳 · 무적 %u틱\n", frame, s.addr.c_str(), sx, sy,
                playerStarts_.size(), 5 * kServerTickRate);
    std::fflush(stdout);
}

// ── 알림 (묶음 3-d) ───────────────────────────────────────────────
//
// diePlayer_54D2B0(`GAME5.c:4971~5057`) — 온라인(게임 깃발 0x2000 · 이 서버는 늘 온라인)이면 모두에게 `a9 0e` 11바이트.
//   <죽인 사람 번호2><도운 사람 번호2><죽은 사람 번호2><종류2><갈래1> · Kind1(틱 목록, `GAME3_2.c:5190~5198`) = 비신뢰
//   클라(sub_495210 · sub_495500)는 화면 위 가운데에 「죽인이 +도운이 [그림] 죽은이」를 90틱 띄운다 —
//   갈래 1 이면 그 종류의 그림(pretty_image) · 2 면 피해 종류별 주문 그림 · 그 밖은 기본 그림(주문 15)
void Server::SendKillInform(Session& s, std::vector<OutPacket>& out) {
    // 죽인 사람 v24 = findParentChainPlayer(+520) — 사람이면 그 번호
    uint16_t killer = 0;
    if (!s.lastHit.player.empty()) {
        const auto k = sessions_.find(s.lastHit.player);
        if (k != sessions_.end() && k->second.inGame) killer = k->second.netCode;
    }
    // 도운 사람 v2 — +3600 이 켜져 있고 10초(10×FPS) 안이면 +3604 사람(살아 있는 유닛) · 죽인 이·죽은 이와 같으면 0(sub_4E7540 · 4-a4)
    uint16_t assist = 0;
    if (s.assistSet && frame_ - s.assistFrame < 10u * kServerTickRate) {
        const auto h = sessions_.find(s.assistBy);
        if (h != sessions_.end() && h->second.inGame && h->second.addr != s.lastHit.player && h->second.addr != s.addr)
            assist = h->second.netCode;
    }
    // 갈래·종류 — +520 이 몬스터면(또는 주인이 몬스터인 무기·투사체면) 갈래 1·그 몬스터 종류, 그 밖은 기록(+304/+300)
    uint8_t kind = s.killKind;
    uint16_t type = s.killType;
    if (s.lastHit.cls == 2 || (s.lastHit.cls == 3 && s.lastHit.ownerCls == 2)) {
        kind = 1;
        type = s.lastHit.monsterType;
    }
    const uint8_t msg[11] = {
        static_cast<uint8_t>(Op::INFORM), static_cast<uint8_t>(InformCode::Kill),
        static_cast<uint8_t>(killer), static_cast<uint8_t>(killer >> 8),
        static_cast<uint8_t>(assist), static_cast<uint8_t>(assist >> 8),
        static_cast<uint8_t>(s.netCode), static_cast<uint8_t>(s.netCode >> 8),
        static_cast<uint8_t>(type), static_cast<uint8_t>(type >> 8), kind,
    };
    for (auto& kv : sessions_) {
        if (kv.second.inGame) Emit(kv.second, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);
    }
    std::printf("[죽음 알림] 프레임 %u 죽은 %u · 죽인 %u · 종류 %u · 갈래 %u\n", frame_, s.netCode, killer, type, kind);
    std::fflush(stdout);
    s.killKind = 0;                                        // +304 = 0
    // ⚠ 갈래 2·종류 2 면 죽인 사람의 능력 1(광전사 돌진) 쉬는 시간을 지운다(sub_4FC0B0 · `GAME5.c:5052~5053`) → 묶음 7
}

// playerReportAnything_4D9900(`GAME3_2.c:4855`) — 자기 몸 갱신(`GAME4_1.c:4428`)마다 「알린 값과 다른 것」만 본인에게 보낸다.
//   옮긴 갈래 — 방어값: +228(지금) 과 +232(알린 값)를 float 로 견줘 다르면 `49 <float 4>` 를 Packet0(중요 · 봉투 없음) · 알린 값을 맞춘다
//   ⚠ 나머지 갈래 — 금 `4a`(+2164, 아이템 5) · 마법부여 표시 `5b`(+440, 아이템 5) · 퀘스트 목숨·은/금 열쇠 `f0 04~06`(8) ·
//     총량 다시 알림 깃발 2184(dd·de·스탯) · 팀 체력(netSendPlrHealthToTeam) · 마나 +4≠+6(netReportMana — 지금은 쓰는 자리마다 우리 식)
// playerReportAnything_4D9900(`GAME3_2.c:4855~4970`) — 자기 갱신(a1 == a2 · netUpdate_518EE0 → netPlayerObjSend) 때 매 틱 「바뀐 것만」
void Server::PlayerReportAnything(Session& s, std::vector<OutPacket>& out) {
    // ① 방어 +228 ≠ +232 → `49 <f32>`(Packet0)
    const float armor = ArmorValueFloat(s.charClass);
    if (s.armorReported != armor) {
        uint32_t bits = 0;
        std::memcpy(&bits, &armor, sizeof(bits));
        const uint8_t msg[5] = { static_cast<uint8_t>(Op::REPORT_ARMOR_VALUE), static_cast<uint8_t>(bits),
                                 static_cast<uint8_t>(bits >> 8), static_cast<uint8_t>(bits >> 16),
                                 static_cast<uint8_t>(bits >> 24) };
        EmitImportantRaw(s, msg, sizeof(msg), out);        // netReportArmorVal_4D8A30 — netSendPacket0
        s.armorReported = armor;
    }
    // ② 금 +2164 ≠ +2168 → `4a` · ③ 마법부여 표시 +440 ≠ +2172 → `5b` → 아이템(5) · 퀘스트(4096) 목숨·열쇠 → 모드(8)
    // ④ +2184(plrReadVals) → `dd` 총 체력(Packet1) · `de` 총 마나(sub_4E5450 · +2251 직업 0 = 워리어면 안 보냄) · `48` 능력치 14B(Packet0)
    if (s.statsDirty) {
        const uint8_t dd[7] = { static_cast<uint8_t>(Op::REPORT_TOTAL_HEALTH), static_cast<uint8_t>(s.netCode),
                                static_cast<uint8_t>(s.netCode >> 8), static_cast<uint8_t>(s.health),
                                static_cast<uint8_t>(s.health >> 8), static_cast<uint8_t>(s.maxHealth),
                                static_cast<uint8_t>(s.maxHealth >> 8) };
        EmitSeq(s, dd, sizeof(dd), out);                   // netReportTotalHealth_4D85C0
        if (s.charClass != CharClass::Warrior) {
            const uint8_t de[7] = { static_cast<uint8_t>(Op::REPORT_TOTAL_MANA), static_cast<uint8_t>(s.netCode),
                                    static_cast<uint8_t>(s.netCode >> 8), static_cast<uint8_t>(s.mana),
                                    static_cast<uint8_t>(s.mana >> 8), static_cast<uint8_t>(s.maxMana),
                                    static_cast<uint8_t>(s.maxMana >> 8) };
            EmitImportantRawSupersede(s, de, sizeof(de), out);   // netReportTotalMana_4D88C0
        }
        // servSendStats_4D8990(`GAME3_2.c:4201`) — 번호 · 최대 체력(+556+4) · 최대 마나(+8) · 짐 한계(+490) · 속도(+2235) · 힘(+2239) · 레벨(+3684)
        const uint16_t carry = static_cast<uint16_t>(CarryCapacityOnline(s.charClass));
        const uint16_t spd = static_cast<uint16_t>(SpeedOnline(s.charClass));
        const uint16_t str = static_cast<uint16_t>(s.strength);
        const uint8_t st[14] = { static_cast<uint8_t>(Op::REPORT_STATS), static_cast<uint8_t>(s.netCode), static_cast<uint8_t>(s.netCode >> 8),
                                 static_cast<uint8_t>(s.maxHealth), static_cast<uint8_t>(s.maxHealth >> 8),
                                 static_cast<uint8_t>(s.maxMana), static_cast<uint8_t>(s.maxMana >> 8),
                                 static_cast<uint8_t>(carry), static_cast<uint8_t>(carry >> 8),
                                 static_cast<uint8_t>(spd), static_cast<uint8_t>(spd >> 8),
                                 static_cast<uint8_t>(str), static_cast<uint8_t>(str >> 8), s.charLevel };
        EmitImportantRaw(s, st, sizeof(st), out);
        s.statsDirty = false;
    }
    // ⑤ 체력 +10 ≠ 지금 → `43 <체력2>`(Packet1 · 본인만 — 번호가 안 실린다) · 퀘스트면 팀에 `c4 0c`(8) · +10 = 지금
    if (s.health != s.healthReported) {
        const uint8_t hp[3] = { static_cast<uint8_t>(Op::REPORT_PLAYER_HEALTH), static_cast<uint8_t>(s.health), static_cast<uint8_t>(s.health >> 8) };
        EmitSeq(s, hp, sizeof(hp), out);                   // netSendPlrHealthToTeam_4D86E0
        s.healthReported = s.health;
    }
    // ⑥ 마나 +4 ≠ +6 → `45 <번호2> <마나2>`(sub_4E5450 · 워리어는 안 보냄) · +6 = +4
    if (s.mana != s.manaPrev) {
        if (s.charClass != CharClass::Warrior) {
            const uint8_t mp[5] = { static_cast<uint8_t>(Op::REPORT_MANA), static_cast<uint8_t>(s.netCode), static_cast<uint8_t>(s.netCode >> 8),
                                    static_cast<uint8_t>(s.mana), static_cast<uint8_t>(s.mana >> 8) };
            EmitImportantRawSupersede(s, mp, sizeof(mp), out);   // netReportMana_4D8930
        }
        s.manaPrev = s.mana;
    }
}

// sdecode `0x7B` MSG_TRY_COLLIDE(`server__network__sdecode.c:256`) — 클라는 「쓰기」 커서(IMMOBILE 상자·미믹, `input_c.go:372~385`)에서
//   누르면 `7b <번호2>` 를 보낸다(`GAME1_2.c:3162`). 서버는 관전(3680 & 3)·가게(+280)·대화(+284) 중이 아니면
//   그 오브젝트의 충돌 함수(+696)를 (그것, 사람, 0) 으로 부른다. 윗비트(0x8000)가 선 번호는 extent 다(packetDynamicUnitCode_578B40)
//   ⚠ 충돌 함수 자체(ChestCollide 등)는 그 오브젝트 묶음(상자·보물 5 · 맵 오브젝트 9)에서 옮긴다. 지금은 찾고 · 세고 · 적는다
void Server::TryCollide(Session& s, uint16_t code, std::vector<OutPacket>& out) {
    (void)out;
    // 3680 & 1 관전 · & 2 몬스터 지켜보기(관전 기능 — 8) · 가게·대화 모임은 아직 없다(5·10)
    if (s.observer) return;
    uint16_t netCode = code;
    if (code & 0x8000) {                                   // extent → 그 오브젝트의 번호(없으면 0)
        netCode = 0;
        for (const auto& m : mapObjects_) {
            if (m.extent == static_cast<uint32_t>(code & 0x7FFF)) { netCode = m.netCode; break; }
        }
    }
    if (netCode == 0) return;
    // nox_server_getObjectFromNetCode_4ECCB0 — 사람 · 소환수·몬스터 · 깃발 · 맵 오브젝트
    bool found = false;
    uint16_t type = 0;
    for (const auto& kv : sessions_) {
        if (kv.second.inGame && kv.second.netCode == netCode) { type = kPlayerType; found = true; break; }
    }
    for (size_t i = 0; !found && i < pets_.size(); ++i) {
        if (pets_[i].netCode == netCode) { type = pets_[i].type; found = true; }
    }
    for (size_t i = 0; !found && i < flags_.size(); ++i) {
        if (flags_[i].netCode == netCode) { type = flags_[i].type; found = true; }
    }
    for (size_t i = 0; !found && i < mapObjects_.size(); ++i) {
        if (mapObjects_[i].netCode == netCode) { type = mapObjects_[i].type; found = true; }
    }
    if (!found) return;
    ++tryCollides_;
    const auto fn = collideFn_.find(type);
    if (fn == collideFn_.end() || fn->second.empty()) return;   // +696 이 없으면 아무 일 없음
    ++tryCollideDeferred_;
    std::printf("[충돌 요청] 프레임 %u %s → 번호 %u 종류 %u 충돌 함수 %s — 아직 안 옮김\n",
                frame_, s.addr.c_str(), netCode, type, fn->second.c_str());
    std::fflush(stdout);
}

// ── 시전 틀 (묶음 4-a) ───────────────────────────────────────────────
//
// 정답지 흐름 — 받기 sdecode:211 → spellByBookInsert_4FE340(목록에 넣고 상태 2) → 매 틱 spellCastByBook_4FCB80
//   (음소를 3틱마다 하나씩 나무를 따라 걷고 · 첫 틱에 시전자에게 `70 <주문>`) → 끝에 PlayerSpell(player.go:462 — 검사·마나·inform)
//   → castSpellByUser·castSpell(spells.go:422 — 공격 주문이면 투명·무적·타원 방패 끔 · 겨눈 주문은 날림)
//   → spellAccept(spells.go:193 — 효과 함수 갈래)

// server/spells.go createSpellFrom — 정의를 넣고 음소 나무를 자라게 한다(끝 자리 Ind = 주문 번호 · 음소 끝에 End 4)
void Server::SetSpellDef(uint8_t spell, uint8_t mana, uint32_t flags, const std::vector<uint8_t>& phonemes) {
    if (spell == 0 || spell >= 137) return;
    SpellDef& d = spellDefs_[spell];
    d.valid = true;
    d.enabled = true;
    d.mana = mana;
    d.flags = flags;
    d.phonemes.clear();
    spellCost_[spell] = mana;
    if (phonemes.empty()) return;
    int leaf = 0;
    for (uint8_t ph : phonemes) {
        if (ph >= 9) return;
        int next = phonemeTree_[static_cast<size_t>(leaf)].next[ph];
        if (next < 0) {
            phonemeTree_.push_back(PhonemeNode{});
            next = static_cast<int>(phonemeTree_.size() - 1);
            phonemeTree_[static_cast<size_t>(leaf)].next[ph] = next;
        }
        leaf = next;
    }
    phonemeTree_[static_cast<size_t>(leaf)].ind = spell;
    d.phonemes = phonemes;
    d.phonemes.push_back(4);                               // spell.PhonEnd
}

// spellGetPower_4FE7B0(`GAME4.c:3321`) — 멀티 모드(게임 깃발 1392 = KOTR|CTF|깃발볼|아레나|제거전)면 3 · 사람은 SpellLvl
//   ⚠ 이 서버의 모드(아레나·CTF·퀘스트)는 퀘스트만 1392 밖 — 퀘스트는 SpellLvl(온라인이면 전부 3)
int Server::SpellPower(const Session& s, uint8_t spell) const {
    const bool quest = (questTotal_ > 0 || questStage_ > 0);
    if (!quest) return 3;
    return spell < 137 ? s.spellLvl[spell] : 0;
}

// sub_4FD0E0(`GAME4.c:2686`) — 꺼진 주문 10 · 사람이면 playerCheckSpellClass_57AEA0(`GAME5_2.c:1435`):
//   위저드 0x2000000 · 컨저러 0x4000000 · 누구나 0x1000000 가 없으면 9 (워리어는 늘 9)
int Server::SpellCheckClass(const Session& s, uint8_t spell) const {
    if (spell >= 137 || !spellDefs_[spell].valid || !spellDefs_[spell].enabled) return 10;
    const uint32_t fl = spellDefs_[spell].flags;
    uint32_t cls;
    if (s.charClass == CharClass::Wizard) cls = 0x2000000;
    else if (s.charClass == CharClass::Conjurer) cls = 0x4000000;
    else return 9;
    return ((fl & 0x1000000) || (fl & cls)) ? 0 : 9;
}

// checkPlrCantCastSpell_4FD150(`GAME4.c:2706`)
//   ⚠ 옮기지 않은 갈래 — KOTR 왕관·깃발볼 공(17·16)은 모드 8 ·
//     주먹(29)·자연의 힘(31)·유성(52) 이미 있음 3 · 매직미사일(50)·픽시(58) 개수 표는 그 효과(4-c) 와 함께
int Server::CheckPlrCantCastSpell(const Session& s, uint8_t spell, bool glyph) const {
    if (!glyph && flags_.size() > 0 && (SpellFlags(spell) & 0x80000)) {
        // CTF(게임 깃발 32) — 가진 것 가운데 FLAG 클래스(0x10000000)가 있으면 13
        for (const auto& f : flags_) if (f.carrier == s.addr) return 13;
    }
    if (s.buffs.Has(29)) return 14;                        // LABEL_9 — 반마법(ANTI_MAGIC 29) 버프면 14(4-a4)
    const bool quest = (questTotal_ > 0 || questStage_ > 0);
    if (!quest && (spell == 111 || spell == 112 || spell == 113 || spell == 114)) return 10;   // sub_4D7100 — 퀘스트 전용 소환
    return 0;
}

// sub_4FCF90(`GAME4.c:2638`) — 소환(75~114)은 blob_587000 217668 표 · 그 밖 ManaCost(,a3) · 모자라면 −1
//   ⚠ 모자랄 때 +160 = ManaCost(,1) · +164 = FPS 는 HUD 깜빡임 자리(아직 읽는 곳을 못 찾음 — 옮기지 않음)
int Server::SpellManaTake(Session& s, uint8_t spell, int costKind, std::vector<OutPacket>& out) {
    if (spell == 0 || spell >= 137) return -1;
    int cost;
    if (spell >= 75 && spell <= 114) {
        static const uint8_t kSummonCost[40] = {   // blob_587000 217668 + 4×(75~114)
            15, 60, 60, 60, 30, 30, 30, 15, 60, 60, 15, 30, 30, 30, 85, 85, 30, 60, 85, 60,
            30, 30, 60, 30, 15, 30, 85, 30, 30, 15, 60, 30, 30, 30, 30, 60, 60, 60, 60, 60 };
        cost = kSummonCost[spell - 75];
    } else {
        cost = spellDefs_[spell].mana;
        if (costKind == 2) {
            // ManaCost(id, 2) — 함정에 넣을 때 에너지볼트·연쇄번개·마나폭탄은 gamedata 값(4-e 글리프에서)
        }
    }
    (void)costKind;
    if (s.mana < cost) return -1;
    PlayerManaSub(s, cost);                                // playerManaSub_4EEBF0(`GAME4.c:2663`) — `45` 는 다음 틱 PlayerReportAnything(+4 ≠ +6)
    (void)out;
    return cost;
}

void Server::SpellManaGive(Session& s, int mana, std::vector<OutPacket>& out) {
    if (mana <= 0) return;
    PlayerManaAdd(s, mana);                                // sub_4FD030 → playerManaAdd_4EEB80
    (void)out;
}

// ── 갱신 알림 정답지화(09-14) — 마나는 「이전 값(+6)」을 남기고, 알림은 PlayerReportAnything 이 한다 ──
// playerManaAdd_4EEB80(`GAME3_3.c:7466`) — +6 = 지금 · +4 += v · 최대로 자름(보호 CRC 는 안 옮김)
void Server::PlayerManaAdd(Session& s, int v) {
    s.manaPrev = s.mana;
    int after = static_cast<int>(s.mana) + v;
    if (after > s.maxMana) after = s.maxMana;
    if (after < 0) after = 0;
    s.mana = static_cast<uint16_t>(after);
}
// playerManaSub_4EEBF0(`GAME3_3.c:7502`) — +6 = 지금 · 모자라면 0(GODMODE 는 안 옮김)
void Server::PlayerManaSub(Session& s, int v) {
    s.manaPrev = s.mana;
    s.mana = static_cast<uint16_t>(s.mana > v ? s.mana - v : 0);
}
// playerManaRefresh_4EECF0(`GAME3_3.c:7573`) — +6 = 지금 · +4 = 최대
void Server::PlayerManaRefresh(Session& s) {
    s.manaPrev = s.mana;
    s.mana = s.maxMana;
}

// sub_4E5450(`GAME3_3.c:1767`) — 이 사람 대기열에서 **첫 바이트(옵)가 같은 것**을 지우고 Packet0 으로 싣는다.
//   정답지 저장본은 맨 메시지라 Packet1 것도 지워지지만, 우리가 이 길로 보내는 것(`45`·`de`)은 다 Packet0 이라 맨 항목만 본다.
void Server::EmitImportantRawSupersede(Session& s, const uint8_t* msg, size_t n, std::vector<OutPacket>& out) {
    if (n == 0) return;
    const uint8_t op = msg[0];
    s.importantQ.erase(std::remove_if(s.importantQ.begin(), s.importantQ.end(),
                                      [op](const Session::PendingImportant& q) { return !q.wire.empty() && q.wire[0] == op; }),
                       s.importantQ.end());
    EmitImportantRaw(s, msg, n, out);
}

// netPlayerObjSend_518C30(`GAME4_1.c:4405~4425`) · netSendComplexObject_518960(`:4270~4280`) 의 a4/a3 갈래 —
//   오브젝트가 **보는 사람마다** 「마지막으로 알린 체력」을 든다(사람 +12+2×i · 몬스터 +412+2×i). 마지막 피해(+536) 뒤 2틱 넘으면 견줘
//   다르면 netReportHealthDelta_4D8760(보는 이, 번호, 지금 − 알린 것) → **음수만** `42 <번호2> <증감2>` Packet0. 그러고 알린 것 = 지금.
//   ⚠ 처음 보는 오브젝트는 정답지도 0 과 견준다(양수 → 안 보냄 · 기록) — 우리는 「없으면 지금 값을 적고 안 보냄」으로 같다
void Server::ReportHealthDeltaTo(Session& viewer, uint16_t netCode, uint16_t hp, uint32_t lastDamageFrame,
                                 std::map<std::string, uint16_t>& seen, std::vector<OutPacket>& out) {
    if (frame_ - lastDamageFrame <= 2) return;
    auto it = seen.find(viewer.addr);
    if (it == seen.end()) { seen.emplace(viewer.addr, hp); return; }
    if (it->second == hp) return;
    const int delta = static_cast<int>(hp) - static_cast<int>(it->second);
    if (delta < 0) {
        const uint16_t raw = static_cast<uint16_t>(static_cast<int16_t>(delta));
        const uint8_t msg[5] = { static_cast<uint8_t>(Op::REPORT_HEALTH_DELTA), static_cast<uint8_t>(netCode), static_cast<uint8_t>(netCode >> 8),
                                 static_cast<uint8_t>(raw), static_cast<uint8_t>(raw >> 8) };
        EmitImportantRaw(viewer, msg, sizeof(msg), out);
    }
    it->second = hp;
}

// netInformTextMsg(server/network.go:206) — `a9 <갈래> <u32>` Kind1(틱 목록) · 갈래 0 = 주문 실패 코드 · 1 = 시전 성공 주문 번호
void Server::InformSpell(Session& s, uint8_t code, uint32_t val, std::vector<OutPacket>& out) {
    const uint8_t msg[6] = { static_cast<uint8_t>(Op::INFORM), code, static_cast<uint8_t>(val),
                             static_cast<uint8_t>(val >> 8), static_cast<uint8_t>(val >> 16), static_cast<uint8_t>(val >> 24) };
    Emit(s, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);
}

// netReportSpellStat(server/network.go:362) — `df <u32 주문> <u8>` Packet0(중요 · 봉투 없음)
void Server::ReportSpellStat(Session& s, uint32_t spell, uint8_t stat, std::vector<OutPacket>& out) {
    const uint8_t msg[6] = { static_cast<uint8_t>(Op::REPORT_SPELL_STAT), static_cast<uint8_t>(spell),
                             static_cast<uint8_t>(spell >> 8), static_cast<uint8_t>(spell >> 16),
                             static_cast<uint8_t>(spell >> 24), stat };
    EmitImportantRaw(s, msg, sizeof(msg), out);
}

// netPriMsgToPlayer_4DA2C0(`GAME3_2.c:5209`) — `a9 0f <a3> <글 열쇠>\0` Kind1 · 글은 클라가 CSF 에서 찾는다 · 48 자 넘으면 안 보냄
void Server::SendPriMsg(Session& s, const char* key, std::vector<OutPacket>& out) {
    const size_t n = std::strlen(key);
    if (n == 0 || n > 0x30) return;
    std::vector<uint8_t> msg;
    msg.push_back(static_cast<uint8_t>(Op::INFORM));
    msg.push_back(15);
    msg.push_back(0);
    msg.insert(msg.end(), key, key + n + 1);
    Emit(s, /*reliable=*/false, /*stream=*/0, msg.data(), msg.size(), out);
}

// spellByBookInsert_4FE340(`GAME4.c:3090~3265`)
bool Server::SpellByBookInsert(Session& s, const uint32_t spells[5], int count, uint32_t gap, bool targetSelf,
                               std::vector<OutPacket>& out) {
    if (s.respawnAtFrame != 0) return false;               // +16 & 0x8022 — 죽음(0x8000)
    for (int i = 0; i < 5; ++i) if (spells[i] >= 137) return false;
    for (int i = 0; i < 5; ++i) if (spells[i] && s.spellLvl[spells[i]] == 0) return false;   // 안 배움
    if (s.castFrame != 0) return false;                    // 시전 중
    bool glyph = false;
    for (int i = 0; i < count; ++i) if (spells[i] == 34) glyph = true;
    if (glyph) {
        // ⚠ 글리프 갈래(spellCheckSmth 마나 합 · 함정 수 한도 MaxTrapCount · 컨저러 폭탄병 한도)는 4-e 에서
        return false;
    }
    int err = SpellCheckClass(s, static_cast<uint8_t>(spells[0]));
    if (!err) err = CheckPlrCantCastSpell(s, static_cast<uint8_t>(spells[0]), false);
    if (err) {
        InformSpell(s, 0, static_cast<uint32_t>(err), out);   // + 소리 231(10)
        return false;
    }
    // 받은 조작은 다음 틱(frame_ + 1)의 것이다 — 정답지는 같은 틱 안에서 받기 → 유닛 → 시전 순으로 돈다
    const uint32_t now = frame_ + 1;
    PlayerSetState(s, 2, now);
    s.castFlag = 1;
    s.castFrame = now;
    CastEntry e;
    e.caster = s.addr;
    for (int i = 0; i < 5; ++i) e.spells[i] = (i < count) ? spells[i] : 0;
    e.glyph = glyph;
    e.nextFrame = now;
    e.gap = gap;
    e.targetSelf = targetSelf;
    castList_.insert(castList_.begin(), e);                // 목록 머리
    return true;
}

// spellCastByBook_4FCB80(`GAME4.c:2401~2573`) — 매 틱. 머리부터 돈다
void Server::SpellCastByBook(uint32_t frame, std::vector<OutPacket>& out) {
    for (size_t i = 0; i < castList_.size();) {
        CastEntry& e = castList_[i];
        auto it = sessions_.find(e.caster);
        if (it == sessions_.end() || !it->second.inGame || it->second.respawnAtFrame != 0) {   // +16 & 0x8020
            castList_.erase(castList_.begin() + static_cast<long>(i));
            continue;
        }
        Session& s = it->second;
        if (frame < e.nextFrame) { ++i; continue; }
        const uint32_t spell = e.spells[e.slot];
        if (e.phoneme == 0 && !noSpellStart_) {
            const uint8_t msg[2] = { static_cast<uint8_t>(Op::REPORT_SPELL_START), static_cast<uint8_t>(spell) };
            Emit(s, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);   // Kind1 — 시전자에게만
        }
        if (static_cast<uint32_t>(phonemeTree_[static_cast<size_t>(e.leaf)].ind) != spell) {
            // 음소 하나 — 남에게 음소 소리(sub_4FC960 → 10) · 나무를 한 칸 · 사람이면 +184
            uint8_t ph = 0;
            if (spell < 137 && e.phoneme < spellDefs_[spell].phonemes.size()) ph = spellDefs_[spell].phonemes[e.phoneme];
            int next = -1;
            if (e.leaf >= 0 && ph < 9) next = phonemeTree_[static_cast<size_t>(e.leaf)].next[ph];
            // PhonemeLeaf.Next 가 nil 이면 정답지는 nil 자리를 가리킨다 — 다음 비교에서 v7(nil 의 첫 u32) 을 읽는다.
            //   정의가 있는 주문은 음소를 끝까지 따라가면 반드시 그 주문 자리에 닿는다. 없으면 뿌리로 두어 「모름」으로 끝낸다.
            e.leaf = (next >= 0) ? next : 0;
            s.castLeaf = e.leaf;
            ++e.phoneme;
            e.nextFrame = frame + e.gap;
            if (next < 0) {
                // 나무에서 떨어졌다 — 다음 틱에 끝 갈래로(음소 수를 넘기면 정답지도 PhonEnd 뒤 nil)
                e.phoneme = 0xFF;
            }
            if (e.phoneme != 0xFF) { ++i; continue; }
        }
        // ⚠ 글리프 갈래(+29 — 마나 선불·+192 목록·다음 칸)는 4-e
        s.castCursorX = static_cast<float>(s.aimX);        // +220/+224 = playerinfo 571/572
        s.castCursorY = static_cast<float>(s.aimY);
        const bool targetSelf = e.targetSelf;
        castList_.erase(castList_.begin() + static_cast<long>(i));
        // playerinfo[910] = [12] ? 시전자 : +288(커서 오브젝트) — PlayerSpell 이 쓴다
        Target tgt;
        if (targetSelf) tgt.addr = s.addr;
        else tgt = FindObjectAtCursor(s);
        spellTarget_ = tgt;
        PlayerSpell(s, out);
        s.castFrame = 0;
        s.castFlag = 0;
        s.glyphCount = 0;
    }
}

// PlayerSpell(player.go:462~533)
void Server::PlayerSpell(Session& s, std::vector<OutPacket>& out) {
    bool unknown = true;                                   // ok2
    int err = 1;                                           // a1
    const int leaf = s.castLeaf;
    const uint8_t spell = (leaf > 0) ? static_cast<uint8_t>(phonemeTree_[static_cast<size_t>(leaf)].ind) : 0;
    const bool quest = (questTotal_ > 0 || questStage_ > 0);
    if (leaf == 0) {
        unknown = true;
    } else if (spell != 0) {
        if (!quest && (SpellFlags(spell) & 0x20)) {
            const Target cur = FindObjectAtCursor(s);      // ud.CursorObj(+288)
            if (!cur.none() && !TargetIsEnemy(s, cur)) {
                return;                                    // ⚠ 정답지 그대로 — 상태 2·STAT 를 안 건드리고 나간다
            }
        }
        if (s.spellLvl[spell] != 0 || spell == 34) {
            unknown = false;
            err = SpellCheckClass(s, spell);
            if (err == 0) err = CheckPlrCantCastSpell(s, spell, false);
            if (err != 0) {
                InformSpell(s, 0, static_cast<uint32_t>(err), out);   // + 소리 PermanentFizzle(10)
            } else {
                const int mana = SpellManaTake(s, spell, 1, out);
                if (mana < 0) {
                    err = 11;
                    ++spellsNoMana_;
                    InformSpell(s, 0, 11, out);            // + 소리 ManaEmpty(10)
                } else {
                    SpellArg arg;
                    arg.obj = spellTarget_;                // Obj3640
                    if (quest && (SpellFlags(spell) & 0x20) && !arg.obj.none() && !TargetIsEnemy(s, arg.obj))
                        arg.obj = Target{};
                    arg.x = s.castCursorX;                 // CursorPos
                    arg.y = s.castCursorY;
                    if (CastSpellByUser(spell, -1, s, arg, out)) {
                        InformSpell(s, 1, spell, out);
                        ++spells_[spell];
                    } else {
                        SpellManaGive(s, mana, out);
                        err = 8;
                    }
                }
            }
        }
    }
    if (s.moveState == 2) PlayerSetState(s, kPlayerStateIdle, frame_);
    if (unknown) {
        // 줄 글 "plyrspel.c:SpellUnknown"(netSendLineMessage_4D9EB0 · `a8` 글 — 서버가 문자열표를 찾아 싣는다) → 10
    } else if (err != 0) {
        ReportSpellStat(s, spell, 0, out);
    } else if (!(SpellFlags(spell) & 0x100000)) {
        ReportSpellStat(s, spell, 15, out);
    }
}

// castSpellByUser4FDD20 · castSpell(spells.go:422~447)
bool Server::CastSpellByUser(uint8_t spell, int lvl, Session& s, const SpellArg& arg, std::vector<OutPacket>& out) {
    if (lvl < 0) lvl = SpellPower(s, spell);
    if (SpellFlags(spell) & 0x20) {
        // 공격 주문 — 투명(0) 끄기 · 무적(23) 끄기 · 타원 방패(67) 지속 주문 취소
        BuffOff(Target{ s.addr, -1 }, 0);
        BuffOff(Target{ s.addr, -1 }, 23);
        DurCancelFor(67, s.addr, out);
    }
    if (!(SpellFlags(spell) & 0x4) || arg.obj.addr == s.addr) return SpellAccept(spell, s, s.addr, arg, lvl, out);
    // 겨눈 주문 — createSpellFly_4FDDA0 로 "Magic" 오브젝트를 날린다(4-a2) · 닿으면 SpellProjectileCollide 가 받아들인다
    (void)lvl;
    return CreateSpellFly(s, arg.obj, spell, out);
}

// spellAccept(spells.go:193~420)
bool Server::SpellAccept(uint8_t spell, Session& s, const std::string& obj3, const SpellArg& arg, int lvl, std::vector<OutPacket>& out) {
    if (spell == 0 || spell >= 137 || obj3.empty()) return false;   // spellID 0 · obj3 == nil(날아가는 것의 주인이 사라짐)
    if (!spellDefs_[spell].valid) return false;
    const uint32_t fl = spellDefs_[spell].flags;
    // Unk8(0x80) — 대상이 있는데 유닛이 아니면 거절 · 지금 대상은 사람·소환수(유닛)뿐
    // gameCaptureMagic_4FDC10 — CTF 에서 깃발 든 사람에게 CANT_HOLD_CROWN 주문 → 0 · 소리 PermanentFizzle(10)
    if (!arg.obj.none() && !arg.obj.addr.empty() && (fl & 0x80000)) {
        for (const auto& f : flags_) if (f.carrier == arg.obj.addr) return false;
    }
    lastAcceptTarget_ = arg.obj;                             // 시험용 기록
    ++acceptCount_;
    lastAcceptCaster_ = s.addr;
    // ── 효과 갈래(spells.go:219~412 · 효과 = 주문 번호) ── 함수(fnc) 갈래는 lvl > 5 를 5 로 자르고 0 이면 PermanentFizzle(10) · false
    int fnc = -1;                                            // -1 = 이 갈래가 아직 없음
    {
        const int fl5 = lvl > 5 ? 5 : lvl;
        const Target targ = arg.obj;
        BuffConf c;
        switch (spell) {
            case 1:   c.durOpt = "AnchorEnchantDuration"; c.orig = obj3; c.offensive = true; fnc = CastBuffSpell(spell, 14, fl5, targ, c, out); break;   // castAnchor
            case 3:   c.dur = 4; c.durInSec = true; c.orig = obj3; c.offensive = true; fnc = CastBuffSpell(spell, 2, fl5, targ, c, out); break;         // castBlind
            case 15:  c.dur = 450; c.once = true; c.orig = obj3; c.offensive = true; fnc = CastBuffSpell(spell, 16, fl5, targ, c, out); break;      // castDeath
            case 17:  c.dur = 60; c.durInSec = true; fnc = CastBuffSpell(spell, 6, fl5, targ, c, out); break;                                    // castDetectMagic
            case 26:  c.durOpt = "FearEnchantDuration"; fnc = CastBuffSpell(spell, 11, fl5, targ, c, out); break;                                // castFear
            case 32:  c.dur = 4; c.durInSec = true; c.orig = obj3; c.offensive = true; fnc = CastBuffSpell(spell, 25, fl5, targ, c, out); break;    // castFreeze
            case 36:  c.durOpt = "HasteEnchantDuration"; fnc = CastBuffSpell(spell, 9, fl5, targ, c, out); break;                               // castHaste
            case 37:  c.durOpt = "InfravisionEnchantDuration"; fnc = CastBuffSpell(spell, 21, fl5, targ, c, out); break;                        // castInfravision
            case 39:  c.durOpt = "InvisibilityEnchantDuration"; c.defensive = true; c.pointFx = static_cast<uint8_t>(Op::FX_SMOKE_BLAST);
                      fnc = CastBuffSpell(spell, 0, fl5, targ, c, out); break;                                                                  // castInvisibility
            case 40:  c.durOpt = "InvulnerabilityEnchantDuration"; c.defensive = true; fnc = CastBuffSpell(spell, 23, fl5, targ, c, out); break;  // castInvulnerability
            case 42:  c.durOpt = "LightEnchantDuration"; c.orig = obj3; c.offensive = true; fnc = CastBuffSpell(spell, 15, fl5, targ, c, out); break;   // castLight
            case 55:  c.dur = static_cast<int>(BalanceFloat("WarCryDuration")); c.orig = obj3; c.offensive = true;                              // castNullify — abilities.defs[Warcry].duration
                      fnc = CastBuffSpell(spell, 29, fl5, targ, c, out); break;
            case 61:  c.durOpt = "ProtectElectricityEnchantDuration"; fnc = CastBuffSpell(spell, 20, fl5, targ, c, out); break;                 // castProtectElectricity
            case 62:  c.durOpt = "ProtectFireEnchantDuration"; fnc = CastBuffSpell(spell, 17, fl5, targ, c, out); break;                        // castProtectFire
            case 64:  c.durOpt = "ProtectPoisonEnchantDuration"; fnc = CastBuffSpell(spell, 18, fl5, targ, c, out); break;                      // castProtectPoison
            case 70:  c.durOpt = "RunEnchantDuration"; c.orig = obj3; c.offensive = true; fnc = CastBuffSpell(spell, 8, fl5, targ, c, out); break;      // castRun
            case 72:  c.durOpt = "SlowEnchantDuration"; c.orig = obj3; c.offensive = true; fnc = CastBuffSpell(spell, 4, fl5, targ, c, out); break;     // castSlow
            case 130: c.durOpt = "VampirismEnchantDuration"; c.durOptMulQuest = "QuestVampDurMultiplier";
                      fnc = CastBuffSpell(spell, 13, fl5, targ, c, out); break;                                                                 // castVampirism
            case 131: c.dur = 12; c.durInSec = true; c.durLevelMul = true; c.orig = obj3; c.offensive = true;
                      fnc = CastBuffSpell(spell, 10, fl5, targ, c, out); break;                                                                 // castVillain
            // useShock_52C5A0(`GAME4_2.c:9653`) — 대상이 없으면 0.
            //   ⚠ **글리프에 걸면** 피해 갈래다(`ShockTrapDamage[세기-1]` 종류 9 전기) — 글리프가 아직 없다(묶음 4-e).
            //   그 밖에는 **충격 마법부여(22)** 를 `ShockEnchantDuration` 만큼. 도운 사람 기록은 **안 한다**.
            case 20:  c.durOpt = "ShockEnchantDuration"; fnc = CastBuffSpell(spell, 22, fl5, targ, c, out); break;
            case 12: fnc = CastConfuse(obj3, targ, fl5); break;                                   // nox_xxx_castConfuse_52C1E0
            case 74: fnc = CastStun(obj3, targ, fl5); break;                                      // nox_xxx_castStun_52C2C0
            case 33: fnc = CastFumble(targ); break;                                               // nox_xxx_castFumble_52C060
            case 38: fnc = CastInversion(s, obj3); break;                                         // sub_52BEB0
            case 14: fnc = CastCurePoison(spell, s, targ, fl5, out); break;                       // nox_xxx_castCurePoison_52CDB0
            case 8: {                                                                          // ChannelLife — 지속(빔 없음)
                // ⚠ 지속 갈래는 **레벨을 안 자른다**(정답지는 New 를 lvl>5 자르기 **앞**에서 돌려준다) — fl5 가 아니라 lvl
                DurFns f;
                f.update = &Server::DurChannelLife;
                fnc = DurNew(spell, obj3, obj3, false, arg, lvl, f, 0, out) ? 1 : 0;
                break;
            }
            case 35: {                                                                         // GreaterHeal — 지속(치료 광선)
                DurFns f;
                f.create = &Server::DurGreaterHealCreate;
                f.update = &Server::DurGreaterHealTick;
                fnc = DurNew(spell, obj3, obj3, false, arg, lvl, f, 0, out) ? 1 : 0;
                break;
            }
            case 22: {                                                                         // DrainMana — 지속(마나 빨기 광선)
                DurFns f;
                f.update = &Server::DurDrainMana;
                fnc = DurNew(spell, obj3, obj3, false, arg, lvl, f, 0, out) ? 1 : 0;
                break;
            }
            case 43: {                                                                         // ChainLightning — 지속(사슬 번개) · dt 30
                DurFns f;
                f.create = &Server::DurChainLightningCreate;
                f.update = &Server::DurChainLightningTick;
                fnc = DurNew(spell, obj3, obj3, false, arg, lvl, f, 30, out) ? 1 : 0;
                break;
            }
            case 46: case 47: case 48: case 49:                                                // Mark 1~4 — 표식 걸기
                fnc = CastMark(spell, s, arg, out);
                break;
            case 121: {                                                                        // TeleportPop — 무작위 표식으로
                DurFns f;
                f.create = &Server::DurTelePopCreate;
                f.update = &Server::DurTelePopTick;
                fnc = DurNew(spell, obj3, obj3, false, arg, lvl, f, 0, out) ? 1 : 0;
                break;
            }
            case 122: case 123: case 124: case 125: {                                          // TeleportToMark 1~4
                DurFns f;
                f.create = &Server::DurTeleMarkCreate;
                f.update = &Server::DurTeleMarkTick;
                fnc = DurNew(spell, obj3, obj3, false, arg, lvl, f, 0, out) ? 1 : 0;
                break;
            }
            case 75: case 76: case 77: case 78: case 79: case 80: case 81: case 82: case 83:
            case 84: case 85: case 86: case 87: {                                              // Summon — 지속(크기별 시전 시간)
                DurFns f;
                f.create = &Server::DurSummonCreate;
                f.update = &Server::DurSummonTick;
                fnc = DurNew(spell, obj3, obj3, false, arg, lvl, f, 0, out) ? 1 : 0;
                break;
            }
            case 4: {                                                                          // Blink — 지속(한 박자 뒤 자리 옮김)
                DurFns f;
                f.create = &Server::DurBlinkCreate;
                f.update = &Server::DurBlinkTick;
                fnc = DurNew(spell, obj3, obj3, false, arg, lvl, f, 0, out) ? 1 : 0;
                break;
            }
            case 51: {                                                                         // Shield — 지속(마법 방패 26)
                DurFns f;
                f.create = &Server::DurShieldCreate;
                f.update = &Server::DurShieldTick;
                f.destroy = &Server::DurShieldDestroy;
                fnc = DurNew(spell, obj3, obj3, false, arg, lvl, f, 0, out) ? 1 : 0;
                break;
            }
            case 24: {                                                                         // EnergyBolt — 지속(전기 광선) · dt 30
                DurFns f;
                f.create = &Server::DurEnergyBoltCreate;
                f.update = &Server::DurEnergyBoltTick;
                fnc = DurNew(spell, obj3, obj3, false, arg, lvl, f, 30, out) ? 1 : 0;
                break;
            }
            case 59: {                                                                        // Plasma — 지속(완드가 있어야 돈다 · 5-f)
                DurFns f;
                f.create = &Server::DurPlasmaCreate;
                f.update = &Server::DurPlasmaTick;
                f.destroy = &Server::DurPlasmaDestroy;
                fnc = DurNew(spell, obj3, obj3, false, arg, lvl, f,
                             static_cast<uint32_t>(BalanceFloat("PlasmaSearchTime")), out) ? 1 : 0;
                break;
            }
            case 23: fnc = CastEarthquake(s, fl5, out); break;                                 // castEquake_52DE40
            case 60: fnc = CastPoison(obj3, targ, fl5, out); break;                            // castPoison_52C720
            case 128: fnc = CastToxicCloud(s, arg, out); break;                                // castToxicCloud_52DB60
            case 5: fnc = CastBurn(s, arg, out); break;                                       // castBurn_52C3E0
            case 27: fnc = CastFireball(s, lvl, out); break;                                  // castFireball_52C790
            case 52: fnc = CastMeteor(s, arg, lvl, out); break;                               // castMeteor_52D9D0
            case 53: fnc = CastMeteorShower(s, arg, lvl, out); break;                         // castMeteorShower_52D8A0
            case 29: fnc = CastFist(s, arg, lvl, out); break;                                 // castFist_52D3C0
            case 16: fnc = CastDeathRay(s, arg, out); break;                                  // castDeathRay(15 는 SPELL_DEATH 다)
            case 10: case 11: fnc = CastCleansingFlame(spell, s, lvl, out); break;            // spellCastCleansingFlame_52D5C0
            case 2: fnc = CastArachnaphobia(s, arg, out); break;                              // spellArachna_52DC80
            case 58: fnc = CastPixieSwarm(spell, s, lvl, out); break;                        // castPixies_540440
            case 65: fnc = CastPushPull(s, fl5, /*pull=*/true); break;                          // castPull_52BFA0
            case 66: fnc = CastPushPull(s, fl5, /*pull=*/false); break;                         // castPush_52C000
            case 41: fnc = CastLesserHeal(spell, s, targ, out); break;                            // sub_52DD50 LesserHeal
            case 68: case 133: fnc = CastRestoreHealth(targ, out); break;                         // castSpellWinkORrestoreHealth_52BF20 RestoreHealth·Wink
            case 69: fnc = CastRestoreMana(targ); break;                                          // sub_52BF50 RestoreMana
            default: break;
        }
    }
    if (fnc >= 0) return fnc != 0;                           // 0 이면 소리 PermanentFizzle(obj4) → 10
    if (SpellEffectPlaceholder(spell, s, arg, out)) return true;
    // 효과 함수를 아직 안 옮긴 주문 — 받아들인 것으로 친다(정답지 default: return true 와 같은 쪽) · 세고 적는다
    ++spellEffectDeferred_;
    std::printf("[시전] 프레임 %u %s 주문 %u(%s) — 효과 함수 아직 안 옮김(묶음 4-b~4-f)\n", frame_, s.addr.c_str(),
                spell, SpellName(spell) ? SpellName(spell) : "?");
    std::fflush(stdout);
    return true;
}

// nox_xxx_gameServerReadyMB_4DD180(`GAME3_2.c:5592`) — CLIENT_READY 마다(정품 클라는 두 번 보낸다)
//   ① +3680 |= 16(준비 — importantReady) ② 온라인·채팅 모드 아니면 무적 23 끄고 다시 5×FPS·세기 5
//   ③ ⚠ 깃발 오브젝트(클래스 0x10000000) 미니맵 표시(미니맵) ④ ⚠ 관전이면 +908/+909 = 몸 자리 · 협동 팀(512)이면 관전 풀기(8)
//   ⑤ ⚠ nox_xxx_wall_4DF1E0 — 부서진 벽·비밀 벽·마법 벽·퀘스트 문 알림(9·4-d) ⑥ ⚠ 퀘스트 건틀릿(8)
//   ⑦ netStatsMultiplier_4D9C20 — `ef <f32 체력> <f32 마나> <f32 힘> <f32 속도>` Packet0 · 직업 배수(아레나는 초기값 1 · 퀘스트 DefaultStatMult 는 8)
void Server::GameServerReadyMB(Session& s, std::vector<OutPacket>& out) {
    if (!s.inGame) return;
    BuffOff(Target{ s.addr, -1 }, 23);
    ApplyEnchant(Target{ s.addr, -1 }, 23, 5 * static_cast<int>(kServerTickRate), 5);
    const float mult[4] = { 1.0f, 1.0f, 1.0f, 1.0f };     // Health · Mana · Strength · Speed(server/player.go:167 init)
    uint8_t msg[17] = { static_cast<uint8_t>(Op::STAT_MULTIPLIERS) };
    for (int i = 0; i < 4; ++i) std::memcpy(msg + 1 + 4 * i, &mult[i], 4);
    EmitImportantRaw(s, msg, sizeof(msg), out);
}

// ── 묶음 4-b4 디버프 C 함수 ─────────────────────────────────────────────
// nox_xxx_castConfuse_52C1E0(`GAME4_2.c:9421`) — 대상 없으면 0 · 혼란(3) ConfuseEnchantDuration · 세기 · 도운 사람(sub_4E7540 a3)
int Server::CastConfuse(const std::string& obj3, const Target& targ, int lvl) {
    if (!BuffsOf(targ)) return 0;
    ApplyEnchant(targ, 3, static_cast<int>(static_cast<float>(BalanceFloat("ConfuseEnchantDuration"))), lvl);
    MarkAssist(obj3, targ);
    return 1;
}

// nox_xxx_castStun_52C2C0(`GAME4_2.c:9436`) — StunEnchantDuration · 사람: 주문 쓰는 직업(+2251)이면 붙잡힘(5) · 워리어면 느림(4)
//   · 몬스터: 질량(+120) > 15 면 느림(4) 아니면 붙잡힘(5) · 도운 사람
int Server::CastStun(const std::string& obj3, const Target& targ, int lvl) {
    if (!BuffsOf(targ)) return 0;
    const int dur = static_cast<int>(static_cast<float>(BalanceFloat("StunEnchantDuration")));
    int buff = 5;
    if (!targ.addr.empty()) {
        buff = (sessions_.at(targ.addr).charClass != CharClass::Warrior) ? 5 : 4;
    } else {
        const auto tb = typeBodies_.find(pets_[static_cast<size_t>(targ.pet)].type);
        const float mass = (tb != typeBodies_.end()) ? tb->second.mass : 1.0f;
        buff = (mass > 15.0f) ? 4 : 5;
    }
    ApplyEnchant(targ, buff, dur, lvl);
    MarkAssist(obj3, targ);
    return 1;
}

// nox_xxx_castFumble_52C060(`GAME4_2.c:9359`) — 대상 없으면 0 · 소리(주문 On → 10) · 1
//   ⚠ 사람·NPC: 든 무기·지팡이·방패(EQUIPPED 0x100 · 클래스 0x1001000 / 0x2000000+SUBCLASS 2) 떨구기는 아이템(5) · 공 들고 있으면 밀어내기(8)
//   ⚠ 그 밖 몬스터(SUBCLASS 0x2000 아님): 가진 것 전부 떨구기(5) + 시전자 자리에서 힘 50(소환수 물리 6)
int Server::CastFumble(const Target& targ) {
    if (!BuffsOf(targ)) return 0;
    return 1;
}

// sub_52BEB0(`GAME4_2.c:9295`) — 시전자(a4) 둘레 InversionRange 안 날아가는 것마다 changeOwner(그것, a3) · 소리(10) · 1
int Server::CastInversion(const Session& a4, const std::string& obj3) {
    const float r = static_cast<float>(BalanceFloat("InversionRange"));
    const float r2 = r * r;
    for (auto& m : missiles_) {                             // getMissilesInCircle — 거리² ≤ r²
        const float dx = static_cast<float>(a4.posX) - m.posX, dy = static_cast<float>(a4.posY) - m.posY;
        if (dx * dx + dy * dy <= r2) MissileChangeOwner(m, obj3, frame_);
    }
    // ⚠ 옛 자리표시 투사체(shots_)는 Magic 이 아니라 거른다(4-c)
    return 1;
}

// nox_xxx_castCurePoison_52CDB0(`GAME4_2.c:9813`) — 대상 없으면 0 · 독(+540) 있으면 풀기(4-c 독과 함께) ·
//   독 없고 대상이 시전자가 아니면 소리 · 1 / 독 없고 자기에게면 마나를 ManaCost(,1) 만큼 되돌려 줌 · 1
int Server::CastCurePoison(uint8_t spell, Session& a2, const Target& targ, int lvl, std::vector<OutPacket>& out) {
    (void)lvl;
    if (!BuffsOf(targ)) return 0;
    const uint8_t* pp = PoisonOf(targ);                     // 4-c2a — 이제 진짜 독을 본다
    if (pp && *pp) {                                        // 독이 있으면 풀고 끝(글 PoisonCure/PoisonClean 은 10)
        RemovePoison(targ, out);
        return 1;
    }
    if (targ.addr != a2.addr) return 1;                     // 소리 → 10
    SpellManaGive(a2, spellDefs_[spell].mana, out);          // sub_4FD030(대상, ManaCost(주문, 1))
    return 1;
}

// ── 묶음 4-b5a 즉발 회복 (spells.go:288~337 · fnc(spellID, obj12, obj3, obj4, &sa, lvl)) ──
//
// sub_52DD50(`GAME4_2.c:10369`) LesserHeal — 대상 없으면 0 · 대상 최대HP && 자기(obj12==대상)면 마나 되돌림(공짜) ·
//   아니면 LesserHealAmount(gamedata) × 시전자 직업 최대체력 만큼 회복 · 소리 aud44(spell,1)
//   ⚠ 직접 시전은 obj12==obj3==시전자라 「자기」 검사 = 대상 == 시전자. 남에게 거는 것은 겨눔(0x4)이라 날아가는 것이 되어 맞을 때 SpellAccept(4-a2)
//   ⚠ nox_float2int 을 **버림**((int))으로 둔다 — 이 리포 관례(Server.cpp 피해 계산). 정품 값과 1 차이면 반올림으로 바꿀 것(화면 ④ 때 대조)
int Server::CastLesserHeal(uint8_t spell, Session& caster, const Target& targ, std::vector<OutPacket>& out) {
    if (!BuffsOf(targ)) return 0;                          // *a5 == 0 — 대상 없음
    bool atMax = false;
    if (!targ.addr.empty()) {
        auto it = sessions_.find(targ.addr);
        atMax = (it != sessions_.end() && it->second.health >= it->second.maxHealth);
    } else if (targ.pet >= 0 && static_cast<size_t>(targ.pet) < pets_.size()) {
        const Creature& k = pets_[static_cast<size_t>(targ.pet)];
        atMax = static_cast<int>(k.health) >= TuningFor(k.type).health;
    }
    if (atMax && targ.addr == caster.addr) {               // 최대 체력 && obj12(자기) == 대상 → 마나 되돌림
        SpellManaGive(caster, spellDefs_[spell].mana, out);   // sub_4FD030(a3, ManaCost(spell, 1))
        return 1;
    }
    double amount = BalanceFloat("LesserHealAmount");       // ARENA LesserHealAmount = 15(절대 HP)
    // ⚠⚠ 여기 곱하는 것은 **직업 배수**(온라인 1.0)다 — 최대 체력이 아니다.
    //   정답지 전역 이름이 `warriorMaxHealth_587000_*` 라 최대 체력으로 읽었다가 회복량을 100배 부풀렸다(ClassHealthMult 주석).
    amount *= static_cast<double>(ClassHealthMult(caster.charClass));
    UnitAdjustHP(targ, static_cast<int>(amount), out);     // unitAdjustHP(최대 아래일 때만·최대에서 자름) · 알림은 갱신 틱(PlayerReportAnything)
    return 1;
}

// castSpellWinkORrestoreHealth_52BF20 RestoreHealth·Wink — 대상 없으면 0 · 있으면 최대 체력으로(unitHPsetOnMax) · 소리 754
//   ⚠ Wink 는 정답지에서 이 함수(회복)뿐 — 순간이동 안 딸림
int Server::CastRestoreHealth(const Target& targ, std::vector<OutPacket>& out) {
    if (!BuffsOf(targ)) return 0;
    UnitAdjustHP(targ, 999999, out);                       // 최대에서 잘려 가득 채움 · 이미 최대면 아무 일 없음(알림 없음)
    return 1;
}

// sub_52BF50 RestoreMana — 대상 없으면 0 · 대상이 사람이면 최대 마나만큼 더함(playerManaAdd·최대에서 자름) · 소리 755
int Server::CastRestoreMana(const Target& targ) {
    if (!BuffsOf(targ)) return 0;
    if (!targ.addr.empty()) {                              // 소환수는 마나 없음 — 사람만
        auto it = sessions_.find(targ.addr);
        if (it != sessions_.end() && it->second.inGame) PlayerManaAdd(it->second, it->second.maxMana);
    }
    return 1;                                              // 대상이 있으면(사람 아니어도) 받아들인 것
}

// ── 묶음 4-b5b 지속 회복·흡수 — 대상 값 읽기 ──
// unitGetOldMana_4EEC80(`GAME3_3.c:7532`) — 이름과 달리 **지금** 마나(+748+4)다. 사람 아니면: 주인 있는 소환수 1000 · 그 밖 0
int Server::TargetManaCur(const Target& t) const {
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        return (it != sessions_.end() && it->second.inGame) ? static_cast<int>(it->second.mana) : 0;
    }
    return (t.pet >= 0 && static_cast<size_t>(t.pet) < pets_.size() && !pets_[static_cast<size_t>(t.pet)].owner.empty()) ? 1000 : 0;
}
// playerGetMaxMana_4EECB0(`:7550`) — 사람만, 아니면 0
int Server::TargetManaMax(const Target& t) const {
    if (t.addr.empty()) return 0;
    auto it = sessions_.find(t.addr);
    return (it != sessions_.end() && it->second.inGame) ? static_cast<int>(it->second.maxMana) : 0;
}
// unitGetHP_4EE780
int Server::TargetHealth(const Target& t) const {
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        return (it != sessions_.end() && it->second.inGame) ? static_cast<int>(it->second.health) : 0;
    }
    return (t.pet >= 0 && static_cast<size_t>(t.pet) < pets_.size()) ? static_cast<int>(pets_[static_cast<size_t>(t.pet)].health) : 0;
}
// sub_4FEA70(`GAME4.c:3352`) — x·y 어느 한쪽이라도 5 이상 떨어졌나
bool Server::TargetMovedFrom(const Target& t, float x, float y) const {
    float tx = 0, ty = 0;
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        if (it == sessions_.end()) return true;
        tx = static_cast<float>(it->second.posX); ty = static_cast<float>(it->second.posY);
    } else if (t.pet >= 0 && static_cast<size_t>(t.pet) < pets_.size()) {
        tx = static_cast<float>(pets_[static_cast<size_t>(t.pet)].x); ty = static_cast<float>(pets_[static_cast<size_t>(t.pet)].y);
    } else {
        return true;
    }
    return std::fabs(x - tx) >= 5.0f || std::fabs(y - ty) >= 5.0f;
}

// sub_52F460(`GAME4_2.c:11444`) ChannelLife — **시전자의 생명을 대상의 마나로 옮긴다**(빔 없음 — 이 주문은 netStartDurationRaySpell 갈래에 없다)
//   대상(+48) 없음·죽음/지워짐 → 취소 · 글리프(+20)면 대상 마나 +20 · 대상 피해 20 하고 한 방에 끝
//   시전자 달리기(8) → 취소 · 대상이 소환수고 건 자리에서 5 넘게 움직임 → 취소 · **대상 마나가 최대면 취소**
//   시전자 체력 > 1 이면: ChannelLifeCoeff[세기−1] 을 소수 이월(+72)로 쌓아 대상 마나에 붓고 **시전자는 1 깎인다** · 아니면 취소
int Server::DurChannelLife(DurSpell& d, std::vector<OutPacket>& out) {
    if (!BuffsOf(d.target)) return 1;                       // +48 없음
    if (!d.target.addr.empty()) {
        auto it = sessions_.find(d.target.addr);
        if (it == sessions_.end() || !it->second.inGame || it->second.respawnAtFrame != 0) return 1;   // DEAD|DESTROYED(0x8020)
    }
    if (d.flag20) {                                         // 글리프가 건 것 — 한 방(대상이 제 생명을 제 마나로)
        if (!d.target.addr.empty()) {
            auto it = sessions_.find(d.target.addr);
            if (it != sessions_.end()) PlayerManaAdd(it->second, 20);
        }
        Damage(d.target.addr, 20, out, std::string(), nullptr, /*ignoreInvuln=*/true);   // unitDamageClear
        return 1;
    }
    const Buffs* cb = BuffsOf(Target{ d.caster, -1 });
    if (cb && cb->Has(8)) return 1;                         // 시전자 달리기 — 채널 끊김
    if (d.target.addr.empty() && TargetMovedFrom(d.target, d.posX, d.posY)) return 1;   // 소환수 대상이 자리를 떴다
    if (TargetManaMax(d.target) == TargetManaCur(d.target)) return 1;                   // 대상 마나가 가득 — 더 줄 것이 없다
    if (TargetHealth(Target{ d.caster, -1 }) <= 1) return 1;                            // 시전자가 더 못 버틴다
    const float coeff = static_cast<float>(BalanceFloatInd("ChannelLifeCoeff", d.level - 1));
    const float v = d.field72 + coeff;
    const int give = static_cast<int>(v);                   // nox_float2int
    d.field72 = v - static_cast<float>(give);               // 소수 이월
    if (!d.target.addr.empty()) {
        auto it = sessions_.find(d.target.addr);
        if (it != sessions_.end()) PlayerManaAdd(it->second, give);
    }
    Damage(d.caster, 1, out, std::string(), nullptr, /*ignoreInvuln=*/true);            // unitDamageClear(시전자, 1)
    return 0;                                               // 이어 간다
}

// unitGetMaxHP_4EE7A0
int Server::TargetMaxHealth(const Target& t) const {
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        return (it != sessions_.end() && it->second.inGame) ? static_cast<int>(it->second.maxHealth) : 0;
    }
    return (t.pet >= 0 && static_cast<size_t>(t.pet) < pets_.size()) ? TuningFor(pets_[static_cast<size_t>(t.pet)].type).health : 0;
}

// unitCanInteractWith_5370E0(`legacy/game.go:296` → `server/object.go:105`) = **CanSee + 벽 시야추적**
bool Server::CanInteract(const Session& obj, const Target& t) const {
    const Buffs* tb = BuffsOf(t);
    if (!tb) return false;
    float tx = 0, ty = 0;
    double tvx = 0, tvy = 0;
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        if (it == sessions_.end() || !it->second.inGame) return false;
        tx = static_cast<float>(it->second.posX); ty = static_cast<float>(it->second.posY);
        tvx = it->second.velX; tvy = it->second.velY;
    } else if (t.pet >= 0 && static_cast<size_t>(t.pet) < pets_.size()) {
        tx = static_cast<float>(pets_[static_cast<size_t>(t.pet)].x); ty = static_cast<float>(pets_[static_cast<size_t>(t.pet)].y);
    } else {
        return false;
    }
    if (!CanSeeBuffs(&obj.buffs, tb, tvx, tvy, 0)) return false;
    return MapTraceVision(tx, ty, static_cast<float>(obj.posX), static_cast<float>(obj.posY));
}

// sub_4E6BD0(`GAME3_3.c:2372`) — 체력이 있고 **지난 1틱 안에** 맞았나(+536). 치료 광선은 맞는 순간 끊긴다
bool Server::TargetJustDamaged(const Target& t) const {
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        return it != sessions_.end() && it->second.inGame && (frame_ - it->second.lastDamageFrame) <= 1;
    }
    if (t.pet < 0 || static_cast<size_t>(t.pet) >= pets_.size()) return false;
    return (frame_ - pets_[static_cast<size_t>(t.pet)].lastDamageFrame) <= 1;
}

// netStartDurationRaySpell_4FF130(`GAME4.c:3429`) — `9e <갈래> <값> <번호2> <번호2>` 7바이트 Packet1 **모두에게**
//   갈래는 멈춤(NetStopRaySpell)과 **다른 표**다: 7→3 · 9→2 · 22→5 · 24→4 · 35→6 · 59→1(값 자리에 시전자 방향) · 43→하위 조각마다
//   그 밖(8 ChannelLife 등)은 **빔이 없다** — switch 의 default 가 아무것도 안 보낸다
//   ⚠ 미니맵 표시(netMarkMinimapForAll 2)는 미니맵 묶음에서
void Server::NetStartRaySpell(const DurSpell& d, std::vector<OutPacket>& out) {
    auto c = sessions_.find(d.caster);
    if (d.caster.empty() || c == sessions_.end() || !c->second.inGame) return;
    uint8_t typ = 0, val = static_cast<uint8_t>(d.level);
    // 하위 조각(연쇄 번개)은 **시작 쪽이 사람이 아닐 수도** 있다 — fromUnit 이 있으면 그쪽 번호를 쓴다
    uint16_t n1 = d.fromUnit.none() ? c->second.netCode : UnitNetCode(d.fromUnit);
    uint16_t n2 = UnitNetCode(d.target);
    switch (d.spell) {
        case 43:
            for (const auto& sub : d.sub108) NetStartRaySpell(sub, out);
            return;
        case 7:  typ = 3; break;
        case 9:  typ = 2; break;
        case 22: typ = 5; break;
        case 24: typ = 4; break;
        case 59: typ = 1; val = c->second.facingPrev; break;
        case 35:                                             // 큰 치유 — 받는 쪽이 앞 · 자기에게면 안 보냄
            if (!d.target.addr.empty() && d.target.addr == d.caster) return;
            typ = 6;
            std::swap(n1, n2);
            break;
        default: return;                                     // 빔 없는 주문
    }
    if (d.target.none() || n2 == 0) return;                  // 정답지도 `result = +48` 이 있어야 보낸다
    const uint8_t msg[7] = { static_cast<uint8_t>(Op::FX_DURATION_SPELL), typ, val, static_cast<uint8_t>(n1), static_cast<uint8_t>(n1 >> 8),
                             static_cast<uint8_t>(n2), static_cast<uint8_t>(n2 >> 8) };
    for (auto& kv : sessions_) if (kv.second.inGame) EmitSeq(kv.second, msg, sizeof(msg), out);
}

// sub_52F220(`GAME4_2.c:11329`) GreaterHeal 만들기 — 시전자 둘레 400 에서 받을 이를 찾는다
//   글리프(+20)거나 시전자가 없으면: 찾아서 20 회복하고 **한 방으로 끝**(1) · 아니면 대상을 잡아 두고
//   **적이 아니면** 빔을 켜고 이어 간다(0) · 적이면 취소 · 못 찾으면 글 "GreaterHealNoTarget" 뒤 취소
int Server::DurGreaterHealCreate(DurSpell& d, std::vector<OutPacket>& out) {
    auto c = sessions_.find(d.caster);
    const bool noCaster = d.caster.empty() || c == sessions_.end() || !c->second.inGame;
    if (noCaster || d.flag20) {
        if (noCaster && !d.flag20) return 1;
        const Target found = SpellFlySearchTarget(true, d.pos2X, d.pos2Y, d.posX, d.posY, std::string(), std::string(),
                                                  SpellFlags(d.spell), 400.0f, nullptr, nullptr);
        if (!found.none()) UnitAdjustHP(found, 20, out);
        return 1;                                            // 글리프는 한 방
    }
    Session& caster = c->second;
    const Target found = SpellFlySearchTarget(true, d.pos2X, d.pos2Y, static_cast<float>(caster.posX), static_cast<float>(caster.posY),
                                              d.caster, d.caster, SpellFlags(d.spell), 400.0f, &caster, &caster.buffs);
    d.target = found;
    if (found.none()) {
        SendPriMsg(caster, "ExecDur.c:GreaterHealNoTarget", out);
        return 1;
    }
    if (TargetIsEnemy(caster, found)) return 1;              // 적은 못 고친다
    NetStartRaySpell(d, out);                                // 치료 광선 켜기
    return 0;
}

// sub_52F2E0(`GAME4_2.c:11376`) GreaterHeal 틱 — 대상 체력을 GreaterHealRate(레벨) × 직업배수 만큼(소수 이월) 채우고 **시전자 마나 1**
//   끊김: 대상 없음·죽음 · 시전자 달리기(8) · **못 닿음**(CanInteract) · 시전자 마나 0 · 시전자가 **지난 1틱 안에 맞음** · 대상 체력 가득
int Server::DurGreaterHealTick(DurSpell& d, std::vector<OutPacket>& out) {
    if (!BuffsOf(d.target)) return 1;
    if (!d.target.addr.empty()) {
        auto it = sessions_.find(d.target.addr);
        if (it == sessions_.end() || !it->second.inGame || it->second.respawnAtFrame != 0) return 1;   // DEAD|DESTROYED
    }
    auto c = sessions_.find(d.caster);
    if (d.caster.empty() || c == sessions_.end() || !c->second.inGame) return 1;
    Session& caster = c->second;
    if (caster.buffs.Has(8)) return 1;                       // 달리면 채널이 끊긴다
    if (!CanInteract(caster, d.target)) return 1;            // 안 보이거나 벽 뒤
    if (caster.mana == 0) return 1;                          // 시전자 마나 바닥
    if (TargetJustDamaged(Target{ d.caster, -1 })) return 1; // 시전자가 방금 맞았다
    if (TargetMaxHealth(d.target) == TargetHealth(d.target)) return 1;   // 다 나았다
    const float v = d.field72 + GreaterHealRate(d.level) * ClassHealthMult(caster.charClass);
    const int heal = static_cast<int>(v);
    d.field72 = v - static_cast<float>(heal);                // 소수 이월
    UnitAdjustHP(d.target, heal, out);
    PlayerManaSub(caster, 1);
    return 0;
}

// sub_52E610 + sub_52E660(`GAME4_2.c:10776`·10789) — 시전자 둘레 ManaDrainRange 에서 **빨 수 있는 것**을 고른다
//   점수 = 무게 × 거리² 이고 **작은 쪽이 이긴다**. 사람은 무게 0.5 라 같은 거리면 마나 기둥(1.0)보다 **먼저 뽑힌다**
//   거를 것: 자기 자신 · 죽음/지워짐 · 마나 0 · **적이 아님** · 같은 팀 · 관전 중 · 벽 뒤(선 추적 5)
//   ⚠ 아직 안 옮긴 갈래 — 마나 기둥(클래스 0x400000·SUBCLASS 0x18, 무게 1.0)은 그 마나 웅덩이를 우리가 안 들고 있다(맵 오브젝트 묶음) ·
//     소환수(+1440 & 0x20 마나 가진 몬스터)도 우리 소환수엔 마나가 없다(크리처 묶음)
Target Server::DrainSearch(const Session& caster) const {
    const float range = static_cast<float>(BalanceFloat("ManaDrainRange"));
    const float range2 = range * range;
    float best = 1e+30f;
    Target found;
    for (const auto& kv : sessions_) {
        const Session& o = kv.second;
        if (!o.inGame || &o == &caster) continue;
        if (o.respawnAtFrame != 0) continue;                 // DEAD|DESTROYED(0x8020)
        if (o.observer) continue;                            // +3680 & 1
        if (o.mana == 0) continue;                           // sub_52E7C0 — 빨 마나가 있어야
        if (!IsEnemyTo(caster, o)) continue;                 // 적만 · 같은 팀 제외
        const float dx = static_cast<float>(caster.posX - o.posX);
        const float dy = static_cast<float>(caster.posY - o.posY);
        const float d2 = dx * dx + dy * dy;
        if (d2 > range2) continue;
        const float score = 0.5f * d2;                       // 사람 무게 0.5
        if (score >= best) continue;
        if (!MapTraceRay(static_cast<float>(caster.posX), static_cast<float>(caster.posY),
                         static_cast<float>(o.posX), static_cast<float>(o.posY), 0x5)) continue;   // 벽 뒤는 못 빤다
        best = score;
        found = Target{ o.addr, -1 };
    }
    return found;
}

// sub_52E450(`GAME4_2.c:10676`) — 실제로 빨아 옮긴다. 빨았으면 1
//   시전자가 사람이고 **마나가 이미 가득이면 0** · 사람 대상은 min(대상 마나, 양) 만큼 · 시전자에게 그만큼 더한다
//   ⚠ 퀘스트(4096)면 직업 마나 배수를 곱해 더한다 — 아레나는 그대로
int Server::DrainApply(Session& caster, const Target& t, int amount) {
    if (caster.mana >= caster.maxMana) return 0;
    if (t.addr.empty()) return 0;                            // 마나 기둥·소환수 갈래는 아직(위 주석)
    auto it = sessions_.find(t.addr);
    if (it == sessions_.end() || !it->second.inGame) return 0;
    Session& tgt = it->second;
    const int took = (static_cast<int>(tgt.mana) <= amount) ? static_cast<int>(tgt.mana) : amount;
    if (took <= 0) return 0;
    PlayerManaSub(tgt, took);
    PlayerManaAdd(caster, took);
    return 1;
}

// nox_xxx_spellDrainMana_52E210(`GAME4_2.c:10568`) — **틱마다 대상을 새로 찾아** 마나를 빨고 빔을 옮겨 단다
//   끊김: 시전자 달리기(8) · 시전자 마나 가득 · 시전자가 건 자리에서 5 넘게 움직임 · 시전자가 지난 1틱 안에 맞음 · 빨 대상이 없음
//   글리프(+20)면 찾은 이의 마나 50 을 깎고 한 방으로 끝
int Server::DurDrainMana(DurSpell& d, std::vector<OutPacket>& out) {
    auto c = sessions_.find(d.caster);
    const bool haveCaster = !d.caster.empty() && c != sessions_.end() && c->second.inGame;
    if (haveCaster && c->second.buffs.Has(8)) return 1;      // 달리면 끊긴다
    if (!haveCaster && !d.flag20) return 1;
    if (!haveCaster) return 1;                               // 글리프 갈래는 시전자 없이도 도는데, 우리 DurNew 는 시전자를 요구한다(4-e 에서)
    Session& caster = c->second;
    if (d.flag20) {                                          // 글리프 — 한 방
        const Target g = DrainSearch(caster);
        if (!g.addr.empty()) {
            auto gi = sessions_.find(g.addr);
            if (gi != sessions_.end()) PlayerManaSub(gi->second, 50);
        }
        return 1;
    }
    if (caster.mana >= caster.maxMana) return 1;             // 더 담을 데가 없다
    if (TargetMovedFrom(Target{ d.caster, -1 }, d.posX, d.posY)) return 1;   // 시전자가 자리를 떴다
    if (TargetJustDamaged(Target{ d.caster, -1 })) return 1; // 맞으면 끊긴다
    const Target found = DrainSearch(caster);
    d.target = found;
    if (found.none()) {                                      // 빨 것이 없다 — 켜 둔 빔이 있으면 끈다
        if (!d.field36.none()) NetStopRaySpell(d, d.field36, out);
        return 1;
    }
    const float v = d.field72 + static_cast<float>(BalanceFloatInd("ManaDrainCoeff", d.level - 1));
    const int take = static_cast<int>(v);
    d.field72 = v - static_cast<float>(take);                // 소수 이월
    if (!(d.field36.addr == found.addr && d.field36.pet == found.pet)) {     // 대상이 바뀌었다 — 빔을 옮겨 단다
        if (!d.field36.none()) NetStopRaySpell(d, d.field36, out);
        NetStartRaySpell(d, out);
    }
    DrainApply(caster, found, take);                         // ⚠ 빨린 소리 230/229 는 소리 묶음(반 초마다)
    d.field36 = found;
    return 0;
}

// ── 묶음 4-d 지속 공격 — 에너지볼트(24) ────────────────────────────────
//
// spellEnergyBoltSetTarget_52EC60(`GAME4_2.c:10981`) — 둘레 안에서 **가장 가까운** 하나를 고른다.
//   거른다: 죽음·부서짐(0x8020) · 자기 · **적이 아닌 것** · **앞쪽이 아닌 것**(TestTwoPointsAndDirection) ·
//   상호작용 못 하는 것(CanInteract = 볼 수 있고 벽에 안 가림).
Target Server::EnergyBoltSearch(const Session& caster, float range) const {
    const float range2 = range * range;
    float best = range2;
    Target found;
    for (const auto& kv : sessions_) {
        const Session& o = kv.second;
        if (!o.inGame || &o == &caster) continue;
        if (o.respawnAtFrame != 0) continue;                 // DEAD|DESTROYED
        if (!IsEnemyTo(caster, o)) continue;
        const float ox = static_cast<float>(o.posX), oy = static_cast<float>(o.posY);
        if (!(TestTwoPointsAndDirection(static_cast<float>(caster.posX), static_cast<float>(caster.posY),
                                        caster.facingPrev, ox, oy) & 1)) continue;   // 앞쪽만
        if (!CanInteract(caster, Target{ o.addr, -1 })) continue;
        const float dx = ox - static_cast<float>(caster.posX);
        const float dy = oy - static_cast<float>(caster.posY);
        const float d2 = dx * dx + dy * dy;
        if (d2 >= best) continue;
        best = d2;
        found = Target{ o.addr, -1 };
    }
    for (size_t k = 0; k < pets_.size(); ++k) {
        const Creature& c = pets_[k];
        if (c.health == 0) continue;
        if (!OwnersEnemy(caster.addr, false, c.owner, c.owner.empty())) continue;
        const float ox = static_cast<float>(c.x), oy = static_cast<float>(c.y);
        if (!(TestTwoPointsAndDirection(static_cast<float>(caster.posX), static_cast<float>(caster.posY),
                                        caster.facingPrev, ox, oy) & 1)) continue;
        const Target t{ std::string(), static_cast<int>(k) };
        if (!CanInteract(caster, t)) continue;
        const float dx = ox - static_cast<float>(caster.posX);
        const float dy = oy - static_cast<float>(caster.posY);
        const float d2 = dx * dx + dy * dy;
        if (d2 >= best) continue;
        best = d2;
        found = t;
    }
    return found;
}

// spellEnergyBoltStop_52E820(`GAME4_2.c:10630`) — 켤 때 **연쇄 번개(43)를 끄고** 점 효과 130 을 낸다. 0 = 살려 둔다
int Server::DurEnergyBoltCreate(DurSpell& d, std::vector<OutPacket>& out) {
    if (!d.caster.empty()) DurCancelFor(43, d.caster, out);
    return 0;
}

// spellEnergyBoltTick_52E850(`GAME4_2.c:10639`) — 매 틱
//   끊기는 자리: 시전자가 **달리기(8)** · 시전자가 없음 · 만든 뒤 2틱 넘겨 **맞음** · **마나 0**
//   고르기: 지금 대상이 아직 앞쪽·사거리 안·볼 수 있으면 그대로, 아니면 둘레에서 다시 고른다
//   피해: `EnergyBoltDamage[세기-1]` + 소수 이월 · 종류 **17(공중 전기)** · 대상이 바뀌면 옛 빔을 끄고 새 빔을 켠다
//   시전자는 자세 10 · **틱마다 마나 1**
int Server::DurEnergyBoltTick(DurSpell& d, std::vector<OutPacket>& out) {
    auto c = sessions_.find(d.caster);
    if (d.caster.empty() || c == sessions_.end() || !c->second.inGame) return 1;
    Session& caster = c->second;
    if (caster.buffs.Has(8)) return 1;                       // 달리면 끊긴다
    const float range = static_cast<float>(BalanceFloat("LightningRange"));
    if (frame_ - d.frame60 > 2 && TargetJustDamaged(Target{ d.caster, -1 })) return 1;
    // 지금 대상을 지킬 수 있나
    bool keep = false;
    if (!d.target.none()) {
        float tx = 0, ty = 0;
        if (TargetXY(d.target, tx, ty)) {
            const float dx = tx - static_cast<float>(caster.posX), dy = ty - static_cast<float>(caster.posY);
            keep = (TargetHealth(d.target) > 0)
                && ((TestTwoPointsAndDirection(static_cast<float>(caster.posX), static_cast<float>(caster.posY),
                                               caster.facingPrev, tx, ty) & 1) != 0)
                && (std::sqrt(dx * dx + dy * dy) <= range)
                && CanInteract(caster, d.target);
        }
    }
    if (!keep) d.target = EnergyBoltSearch(caster, range);
    if (d.target.none()) {                                   // 맞힐 것이 없다 — 빔만 끄고 살려 둔다
        if (!d.field36.none()) { NetStopRaySpell(d, d.field36, out); d.field36 = Target{}; }
        return 0;
    }
    const float v = d.field72 + static_cast<float>(BalanceFloatInd("EnergyBoltDamage", d.level - 1));
    const int dmg = static_cast<int>(v);
    d.field72 = v - static_cast<float>(dmg);                 // 소수 이월
    if (!(d.field36.addr == d.target.addr && d.field36.pet == d.target.pet)) {
        if (!d.field36.none()) NetStopRaySpell(d, d.field36, out);
        NetStartRaySpell(d, out);
    }
    HurtTarget(d.target, dmg, out, d.caster, 17, nullptr);   // 종류 17 = 공중 전기(방어를 안 탄다)
    d.field36 = d.target;
    PlayerSetState(caster, 10, frame_);                      // 시전 자세
    PlayerManaSub(caster, 1);
    // ★ 정답지는 깎은 **뒤에 「이전 마나」**(`unitGetOldMana_4EEC80` = +6)를 본다 —
    //   즉 **이미 0 이던 틱**에 끊긴다(그 틱의 피해는 이미 들어간 뒤다).
    if (caster.manaPrev == 0) return 1;
    return 0;
}

// ── 묶음 4-d(2) 연쇄 번개(43) ──────────────────────────────────────────
//
// lightningCanAttackCheck_52FF10(`GAME4_3.c:389`) — `from` 둘레에서 **가장 가까운** 하나.
//   거른다: 주인의 적이 아닌 것 · 죽음·부서짐 · `from` 자신 · 주인 자신 · **이미 고른 것** ·
//   상호작용 못 하는 것(CanInteract). ⚠ 에너지볼트와 달리 **앞쪽 검사가 없다**.
Target Server::LightningSearch(const Session& owner, const Target& from, float range,
                               const std::vector<Target>& taken) const {
    float fx = 0, fy = 0;
    if (!TargetXY(from, fx, fy)) return Target{};
    const float range2 = range * range;
    float best = range2;
    Target found;
    auto already = [&](const Target& t) {
        for (const auto& k : taken) if (k.addr == t.addr && k.pet == t.pet) return true;
        return false;
    };
    auto consider = [&](const Target& t, float tx, float ty) {
        if (t.addr == from.addr && t.pet == from.pet) return;
        if (!t.addr.empty() && t.addr == owner.addr) return;
        if (already(t)) return;
        if (!CanInteract(owner, t)) return;
        const float dx = tx - fx, dy = ty - fy;
        const float d2 = dx * dx + dy * dy;
        if (d2 >= best) return;
        best = d2;
        found = t;
    };
    for (const auto& kv : sessions_) {
        const Session& o = kv.second;
        if (!o.inGame || o.respawnAtFrame != 0) continue;
        if (!IsEnemyTo(owner, o)) continue;
        consider(Target{ o.addr, -1 }, static_cast<float>(o.posX), static_cast<float>(o.posY));
    }
    for (size_t k = 0; k < pets_.size(); ++k) {
        const Creature& cr = pets_[k];
        if (cr.health == 0) continue;
        if (!OwnersEnemy(owner.addr, false, cr.owner, cr.owner.empty())) continue;
        consider(Target{ std::string(), static_cast<int>(k) }, static_cast<float>(cr.x), static_cast<float>(cr.y));
    }
    return found;
}

// onStartLightning_52F820(`GAME4_2.c:11660`) — 켤 때 **에너지볼트(24)를 끄고** 소수 이월을 0 으로. 0 = 살려 둔다
//   ★ 묶음 5-g — **든 물건이 충전 지팡이면 그것을 적어 둔다.** 틱마다 마나 대신 그 충전을 쓴다.
//     · **글리프가 건 것(`+20`)이면 안 본다** — 글리프에는 손이 없다
//     · 정답지는 오브젝트 클래스 비트 `0x40000` 을 본다(플라스마의 `0x4000000` 과 **다른 갈래**다).
//       우리는 그 비트를 아직 안 담아 **쓸 때 부르는 함수**(`WandCastUse`)로 갈음한다
//     · ⚠ 플라스마와 달리 **완드가 없어도 취소되지 않는다** — 그냥 마나를 쓴다
int Server::DurChainLightningCreate(DurSpell& d, std::vector<OutPacket>& out) {
    d.field72 = 0.0f;
    d.field80 = 0;
    if (!d.caster.empty()) DurCancelFor(24, d.caster, out);
    if (!d.flag20) {
        auto c = sessions_.find(d.caster);
        if (c != sessions_.end() && c->second.inGame) {
            const int wi = HeldWandIndex(c->second);
            if (wi >= 0) d.field80 = static_cast<uint32_t>(wi + 1);   // 0 은 「없음」이라 +1 해 담는다
        }
    }
    return 0;
}

// onFrameLightning_52F8A0(`GAME4_3.c:71`) — 매 틱
//   끊기는 자리: **달리기(8)** · **이전 마나 0** · 만든 뒤 2틱 넘겨 **맞음**
//   사슬: 세기가 곧 **가지 수**(표 `0x587000+260380` 이 1~5 를 그대로 돌려준다).
//     ① 첫 대상 = 시전자 둘레 `LightningRange`(240) 안 가장 가까운 적
//     ② 세기>1 : 첫 대상 둘레 **×0.95** 에서 하나 더 · ③ 세기>2 : 첫 대상 둘레 **×0.90** 에서 하나 더
//     ④ 세기>3 : **둘째**(②) 둘레 ×0.85 · ⑤ 세기>4 : **셋째**(③) 둘레 ×0.80
//   빔 조각: (시전자→①) · (①→②) · (①→③) · (②나 ③→④) · (③이나 ②→⑤). 조각마다 `9e` 갈래 3
//   피해: `LightningDamage`(아레나 1.5) + 소수 이월 · 종류 **17** · 조각마다 **같은 값**
int Server::DurChainLightningTick(DurSpell& d, std::vector<OutPacket>& out) {
    auto c = sessions_.find(d.caster);
    if (d.caster.empty() || c == sessions_.end() || !c->second.inGame) return 1;
    Session& caster = c->second;
    if (caster.buffs.Has(8)) return 1;                        // 달리면 끊긴다
    if (caster.manaPrev == 0) return 1;                       // unitGetOldMana — 이미 0 이면 시작도 못 한다
    if (frame_ - d.frame60 > 2 && TargetJustDamaged(Target{ d.caster, -1 })) return 1;
    const float range = static_cast<float>(BalanceFloat("LightningRange"));

    // 지난 틱의 조각을 옆으로 밀어 둔다(+104) — 새 조각(+108)과 대조해 빔을 갈아 단다
    std::vector<DurSpell> prev;
    prev.swap(d.sub108);

    const int lvl = (d.level < 1) ? 1 : (d.level > 5 ? 5 : d.level);
    std::vector<Target> t;                                    // 고른 차례대로
    t.push_back(LightningSearch(caster, Target{ d.caster, -1 }, range, t));
    if (t[0].none()) {                                        // 맞힐 것이 없다 — 옛 빔만 끄고 살려 둔다
        for (const auto& sub : prev) if (!sub.target.none()) NetStopRaySpell(sub, sub.target, out);
        return 0;
    }
    Target t1, t2, t3, t4;
    if (lvl > 1) { t1 = LightningSearch(caster, t[0], range * 0.94999999f, t); if (!t1.none()) t.push_back(t1); }
    if (lvl > 2) { t2 = LightningSearch(caster, t[0], range * 0.89999998f, t); if (!t2.none()) t.push_back(t2); }
    if (lvl > 3 && !t1.none()) { t3 = LightningSearch(caster, t1, range * 0.85000002f, t); if (!t3.none()) t.push_back(t3); }
    if (lvl > 4 && !t2.none()) { t4 = LightningSearch(caster, t2, range * 0.80000001f, t); if (!t4.none()) t.push_back(t4); }

    // 새 조각 목록 — 정답지 `NewLightningSub(원본, 시작, 끝)` 순서 그대로
    auto addLink = [&](const Target& from, const Target& to) {
        DurSpell sub;
        sub.spell = 7;                                        // SPELL_CHAIN_LIGHTNING_BOLT — 빔 갈래 3
        sub.level = d.level;
        sub.caster = d.caster;                                // 알림을 보낼 때 쓸 사람
        sub.fromUnit = from;
        sub.target = to;
        d.sub108.push_back(sub);
    };
    addLink(Target{ d.caster, -1 }, t[0]);
    if (lvl > 1 && !t1.none()) addLink(t[0], t1);
    if (lvl > 2 && !t2.none()) addLink(t[0], t2);
    if (lvl > 3 && !t3.none()) addLink(t1.none() ? t2 : t1, t3);
    if (lvl > 4 && !t4.none()) addLink(t2.none() ? t1 : t2, t4);

    const float v = d.field72 + static_cast<float>(BalanceFloat("LightningDamage"));
    const int dmg = static_cast<int>(v);
    d.field72 = v - static_cast<float>(dmg);                  // 소수 이월

    // 자리마다 옛 조각과 견줘 **끝이나 시작이 달라졌으면** 옛 빔을 끄고 새 빔을 켠다
    for (size_t i = 0; i < d.sub108.size(); ++i) {
        DurSpell& sub = d.sub108[i];
        if (i < prev.size()) {
            const DurSpell& old = prev[i];
            const bool same = (old.target.addr == sub.target.addr && old.target.pet == sub.target.pet)
                           && (old.fromUnit.addr == sub.fromUnit.addr && old.fromUnit.pet == sub.fromUnit.pet);
            if (!same) {
                if (!old.target.none()) NetStopRaySpell(old, old.target, out);
                NetStartRaySpell(sub, out);
            }
        } else {
            NetStartRaySpell(sub, out);
        }
        if (dmg > 0) HurtTarget(sub.target, dmg, out, d.caster, 17, nullptr);
    }
    for (size_t i = d.sub108.size(); i < prev.size(); ++i)     // 남은 옛 빔은 끈다
        if (!prev[i].target.none()) NetStopRaySpell(prev[i], prev[i].target, out);

    // ★ 묶음 5-g — **완드를 들었으면 마나 대신 충전을 쓴다**(`GAME4_3.c:346~372`).
    //   자세도 다르다 — 완드면 **22(집중)**, 마나면 **10(시전)**.
    if (d.field80 != 0) {
        const size_t wi = static_cast<size_t>(d.field80 - 1);
        if (wi >= caster.inventory.size()) return 1;
        Session::Carried& w = caster.inventory[wi];
        if (w.chargesMax != 0) {
            if (w.charges == 0) return 1;
            --w.charges;
            PlayerSetState(caster, 22, frame_);
            SendCharges(caster, w, out);
            if (w.charges == 0) return 1;
        } else {
            PlayerSetState(caster, 22, frame_);
        }
        return 0;                                             // 마나는 안 쓴다
    }
    PlayerSetState(caster, 10, frame_);
    PlayerManaSub(caster, 1);
    if (caster.manaPrev == 0) return 1;
    return 0;
}

// castDeathRay(`spell_death_ray.go:19`) — 커서 자리에 **한 방**으로 터진다(지속 주문이 아니다).
//   · 시전자→커서 **선 추적(9)** 이 막히면 글(0·2)을 내고 0
//   · 둘레 피해 : 바깥 `DeathRayOutRadius`(15) · 안쪽 `DeathRayInRadius`(10) ·
//     피해 `DeathRayDamage`(아레나 100) · 종류 **16(죽음광선)**
//   · 그림 : `94 <시전자 x u16> <y u16> <커서 x u16> <y u16>` **9바이트 선**(netSendRayFx_5232F0)
//   ⚠ 아직 안 옮긴 것 — 그을음(sMakeScorch) · 소리
int Server::CastDeathRay(Session& caster, const SpellArg& arg, std::vector<OutPacket>& out) {
    const float cx = static_cast<float>(caster.posX), cy = static_cast<float>(caster.posY);
    if (!MapTraceRay(cx, cy, arg.x, arg.y, 9)) {
        InformSpell(caster, 0, 2, out);
        return 0;
    }
    const int dmg = static_cast<int>(BalanceFloat("DeathRayDamage"));
    const float rout = static_cast<float>(BalanceFloat("DeathRayOutRadius"));
    const float rin = static_cast<float>(BalanceFloat("DeathRayInRadius"));
    DamageUnitsAround(arg.x, arg.y, rout, rin, dmg, 16, caster.addr, Target{}, out);
    const uint16_t x1 = static_cast<uint16_t>(static_cast<int>(cx)), y1 = static_cast<uint16_t>(static_cast<int>(cy));
    const uint16_t x2 = static_cast<uint16_t>(static_cast<int>(arg.x)), y2 = static_cast<uint16_t>(static_cast<int>(arg.y));
    const uint8_t fx[9] = { static_cast<uint8_t>(Op::FX_DEATH_RAY),
                            static_cast<uint8_t>(x1), static_cast<uint8_t>(x1 >> 8),
                            static_cast<uint8_t>(y1), static_cast<uint8_t>(y1 >> 8),
                            static_cast<uint8_t>(x2), static_cast<uint8_t>(x2 >> 8),
                            static_cast<uint8_t>(y2), static_cast<uint8_t>(y2 >> 8) };
    SendFxAllCli(arg.x, arg.y, fx, sizeof(fx), out);
    ++fxSent_;
    return 1;
}

// ── 묶음 4-d(4) 방패(Shield 51) ────────────────────────────────────────
//
// castShield1_52F5A0(`GAME4_2.c:11510`) — 대상에게 **마법 방패(버프 26)** 를 걸고 **막아 낼 양**을 담아 둔다.
//   · 대상이 없거나 죽음·부서짐이면, 또는 사람도 몬스터도 아니면 **취소**
//   · 길이 = `ShieldDuration[세기-1]` — ⚠ 실측이 **0, 0, 0, 0, 0** 이다(주석 처리된 옛 줄만 600~1800).
//     즉 **시간으로는 안 끝나고**, 막아 낼 양(`ShieldHealth` 20·35·50·75·100)을 다 쓰면 끝난다
int Server::DurShieldCreate(DurSpell& d, std::vector<OutPacket>& out) {
    (void)out;
    if (d.target.none()) return 1;
    if (TargetHealth(d.target) <= 0) return 1;               // 죽음·부서짐
    if (!BuffsOf(d.target)) return 1;                        // 사람도 몬스터도 아니다
    const int dur = static_cast<int>(BalanceFloatInd("ShieldDuration", d.level - 1));
    ApplyEnchant(d.target, 26, dur, d.level);
    d.pool = static_cast<int>(BalanceFloatInd("ShieldHealth", d.level - 1));
    return 0;
}

// sub_52F650(`:11540`) — 대상이 없거나 죽었으면 끝낸다. 그 밖에는 이어 간다
int Server::DurShieldTick(DurSpell& d, std::vector<OutPacket>& out) {
    (void)out;
    if (d.target.none()) return 1;
    return (TargetHealth(d.target) <= 0) ? 1 : 0;
}

// sub_52F670(`:11553`) — 끝날 때 버프 26 을 끈다
void Server::DurShieldDestroy(DurSpell& d, std::vector<OutPacket>& out) {
    (void)out;
    if (!d.target.none()) BuffOff(d.target, 26);
}

// unitShield_52F690(`:11557`) — 그 대상에게 걸린 **방패 주문(51)** 을 찾아 주머니에서 뺀다.
//   찾는 주문이 없으면 버프만 끄고, 다 쓰면(또는 대상이 죽었으면) 주문을 취소한다.
void Server::ShieldDrain(const Target& t, int amount, std::vector<OutPacket>& out) {
    DurSpell* found = nullptr;
    for (auto& d : durSpells_) {
        if (d.spell != 51) continue;
        if (d.target.addr != t.addr || d.target.pet != t.pet) continue;
        found = &d;
        break;
    }
    if (!found) { BuffOff(t, 26); return; }
    if (TargetHealth(t) <= 0) { DurCancel(*found, out); return; }
    if (found->pool - amount > 0) found->pool -= amount;
    else DurCancel(*found, out);
}

// unitShieldReduceDamage_52F710(`:11588`) — 맞는 순간 끼어든다.
//   · 남은 체력이 **피해보다 크면** : 피해를 **반**으로(0 이면 1) 줄이고 그만큼 주머니에서 뺀다
//   · 죽을 만큼이면 : 체력을 **2** 로 두고 피해를 **0** 으로(= 아예 안 맞음) · 주머니를 통째로 비운다
//     ⚠ 단, **워리어가 방패를 올린 채 앞에서** 맞으면 피해를 **1** 로 남긴다(체력은 그대로 깎인다)
//   ⚠ 아직 안 옮긴 것 — 소리 131 · 방패 그림(netSendShieldFx_523670)
void Server::ShieldReduceDamage(const Target& t, int& dmg, uint8_t dtype, const AttackSrc* src, std::vector<OutPacket>& out) {
    (void)dtype;
    const int hp = TargetHealth(t);
    if (hp > dmg) {
        int half = dmg / 2;
        if (half == 0) half = 1;
        dmg = half;
        ShieldDrain(t, dmg, out);
        return;
    }
    bool guarded = false;
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        if (it != sessions_.end() && it->second.charClass == CharClass::Warrior && it->second.moveState == 16 && src) {
            guarded = (TestTwoPointsAndDirection(static_cast<float>(it->second.posX), static_cast<float>(it->second.posY),
                                                 it->second.facingPrev, src->x, src->y) & 1) != 0;
        }
    }
    if (guarded) {
        dmg = 1;
    } else {
        if (!t.addr.empty()) {
            auto it = sessions_.find(t.addr);
            if (it != sessions_.end()) it->second.health = 2;         // unitSetHP(2)
        } else if (t.pet >= 0 && static_cast<size_t>(t.pet) < pets_.size()) {
            pets_[static_cast<size_t>(t.pet)].health = 2;
        }
        dmg = 0;
    }
    ShieldDrain(t, 999999, out);
}

// ── 묶음 4-e(1) 점멸(Blink 4) ──────────────────────────────────────────
//
// teleportToMB_4E7190 — 그 자리로 옮긴다. 우리 서버는 물리 자리(posX)와 옛 자리칸(x)을 **둘 다** 맞춰야 한다.
void Server::TeleportTarget(const Target& t, float x, float y) {
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        if (it == sessions_.end()) return;
        Session& o = it->second;
        o.posX = x; o.posY = y;
        o.x = static_cast<int>(x); o.y = static_cast<int>(y);
        o.velX = o.velY = 0.0;                               // 옮기면 속도는 죽는다
        return;
    }
    if (t.pet < 0 || static_cast<size_t>(t.pet) >= pets_.size()) return;
    pets_[static_cast<size_t>(t.pet)].x = x;
    pets_[static_cast<size_t>(t.pet)].y = y;
}

// spellBlink2_530310(`GAME4_3.c:600`) — 켤 때 **언제 터질지**만 정한다.
//   협동(2048)이면 `TeleportDelay[세기-1]`(25·20·15·10·5) 틱 뒤, 아니면 **다음 틱**.
//   ⚠ 팀 모드(4096)에서 **글리프가 건 것**이면 아예 안 걸린다.
int Server::DurBlinkCreate(DurSpell& d, std::vector<OutPacket>& out) {
    (void)out;
    const bool quest = (questTotal_ > 0 || questStage_ > 0);
    if (!quest && d.flag20) return 1;                        // 팀 모드 + 글리프 → 취소
    if (quest) d.frame68 = frame_ + static_cast<uint32_t>(BalanceFloatInd("TeleportDelay", d.level - 1));
    else       d.frame68 = frame_ + 1;
    return 0;
}

// spellBlink1_530380(`GAME4_3.c:620`) — 정해 둔 프레임이 되면 **한 번** 옮기고 끝난다.
//   · 대상이 없거나 죽었으면 취소 · 아직이면 그냥 이어 간다
//   · **닻(버프 14)** 이 걸려 있으면 소리 231 을 내고 못 옮긴 채 끝난다
//   · 자리 = 맵 **길목** 가운데 무작위 하나(`waypoint_579F00`) · 없으면 **시작 지점**
//   · 옮기기 전후로 점 효과 137
int Server::DurBlinkTick(DurSpell& d, std::vector<OutPacket>& out) {
    if (d.target.none()) return 1;
    if (TargetHealth(d.target) <= 0) return 1;
    if (d.frame68 - 1 != frame_) return 0;                   // 아직 때가 아니다
    if (const Buffs* b = BuffsOf(d.target)) {
        if (b->Has(14)) return 1;                            // 닻 — 소리 231(10)
    }
    float tx = 0, ty = 0;
    if (!waypoints_.empty()) {
        const auto& w = waypoints_[NextRandom(static_cast<uint32_t>(waypoints_.size()))];
        tx = w.first; ty = w.second;
    } else if (!playerStarts_.empty()) {                     // mapFindPlayerStart_4F7AB0
        const auto& ps = playerStarts_[NextRandom(static_cast<uint32_t>(playerStarts_.size()))];
        tx = ps.first; ty = ps.second;
    } else {
        return 1;                                            // 갈 데가 없다
    }
    float ox = 0, oy = 0;
    TargetXY(d.target, ox, oy);
    auto pointFx = [&](float px, float py) {              // netSendPointFx_522FF0
        const uint16_t ix = static_cast<uint16_t>(static_cast<int>(px)), iy = static_cast<uint16_t>(static_cast<int>(py));
        const uint8_t msg[5] = { static_cast<uint8_t>(Op::FX_TELEPORT),
                                 static_cast<uint8_t>(ix), static_cast<uint8_t>(ix >> 8),
                                 static_cast<uint8_t>(iy), static_cast<uint8_t>(iy >> 8) };
        SendFxAllCli(px, py, msg, sizeof(msg), out);
    };
    pointFx(ox, oy);
    TeleportTarget(d.target, tx, ty);
    pointFx(tx, ty);
    return 1;                                                // 한 번 옮기면 끝
}

// ── 묶음 4-e(2) 표식·순간이동 ─────────────────────────────────────────
//
// 옮길 때 점 효과 137 을 **떠난 자리와 닿은 자리** 두 번 보낸다(투명이면 안 보낸다 — 그건 묶음 4-a4 쪽).
void Server::SendTeleportFx(float ox, float oy, float nx, float ny, std::vector<OutPacket>& out) {
    auto one = [&](float px, float py) {
        const uint16_t ix = static_cast<uint16_t>(static_cast<int>(px)), iy = static_cast<uint16_t>(static_cast<int>(py));
        const uint8_t msg[5] = { static_cast<uint8_t>(Op::FX_TELEPORT),
                                 static_cast<uint8_t>(ix), static_cast<uint8_t>(ix >> 8),
                                 static_cast<uint8_t>(iy), static_cast<uint8_t>(iy >> 8) };
        SendFxAllCli(px, py, msg, sizeof(msg), out);
    };
    one(ox, oy);
    one(nx, ny);
}

// sub_52CBD0(`GAME4_2.c:9800`) — 표식 걸기(46~49). 칸 = **주문 − 46**.
//   자리 = 글리프에 건 것이면 그 글리프 자리, 아니면 **시전자 자리**.
//   ⚠ 정답지는 `TeleportGlyph1~4` 오브젝트를 세워 두고 이미 있으면 **옮긴다** — 우리는 자리만 담는다(그림은 글리프 조각).
int Server::CastMark(uint8_t spell, Session& caster, const SpellArg& arg, std::vector<OutPacket>& out) {
    (void)arg;
    (void)out;
    if (spell < 46 || spell > 49) return 0;
    const int slot = spell - 46;
    caster.markSet[slot] = true;
    caster.markX[slot] = static_cast<float>(caster.posX);
    caster.markY[slot] = static_cast<float>(caster.posY);
    return 1;
}

// sub_5305D0(`GAME4_3.c:...`) — 표식으로 가기(122~125) 켤 때.
//   그 칸에 표식이 **없으면 아예 안 걸린다**. 있으면 언제 터질지만 잡는다(협동은 TeleportDelay, 아니면 다음 틱).
int Server::DurTeleMarkCreate(DurSpell& d, std::vector<OutPacket>& out) {
    (void)out;
    auto c = sessions_.find(d.caster);
    if (c == sessions_.end()) return 1;
    const int slot = static_cast<int>(d.spell) - 122;
    if (slot < 0 || slot > 3) return 1;
    if (!c->second.markSet[slot]) return 1;                  // 표식이 없다
    const bool quest = (questTotal_ > 0 || questStage_ > 0);
    d.frame68 = quest ? (frame_ + static_cast<uint32_t>(BalanceFloatInd("TeleportDelay", d.level - 1)))
                      : (frame_ + 1);
    return 0;
}

// sub_530650 — 그 프레임이 되면 **한 번** 옮기고 끝난다. 닻(14)이면 못 옮긴 채 끝난다
int Server::DurTeleMarkTick(DurSpell& d, std::vector<OutPacket>& out) {
    auto c = sessions_.find(d.caster);
    if (c == sessions_.end() || !c->second.inGame) return 1;
    if (d.target.none() || TargetHealth(d.target) <= 0) return 1;
    if (d.frame68 - 1 != frame_) return 0;
    if (const Buffs* b = BuffsOf(d.target)) { if (b->Has(14)) return 1; }   // 닻
    const int slot = static_cast<int>(d.spell) - 122;
    if (slot < 0 || slot > 3 || !c->second.markSet[slot]) return 1;
    float ox = 0, oy = 0;
    TargetXY(d.target, ox, oy);
    TeleportTarget(d.target, c->second.markX[slot], c->second.markY[slot]);
    SendTeleportFx(ox, oy, c->second.markX[slot], c->second.markY[slot], out);
    return 1;
}

// castTele_530820 — 무작위 표식으로 가기(121) 켤 때. 때만 잡는다(표식 검사는 터질 때 한다)
int Server::DurTelePopCreate(DurSpell& d, std::vector<OutPacket>& out) {
    (void)out;
    const bool quest = (questTotal_ > 0 || questStage_ > 0);
    d.frame68 = quest ? (frame_ + static_cast<uint32_t>(BalanceFloatInd("TeleportDelay", d.level - 1)))
                      : (frame_ + 1);
    return 0;
}

// sub_530880 — 그 프레임이 되면 시전자의 표식 **네 칸 가운데 걸린 것 하나를 무작위로** 골라 옮긴다.
//   하나도 없으면 그냥 끝난다. 닻(14)이면 못 옮긴 채 끝난다
int Server::DurTelePopTick(DurSpell& d, std::vector<OutPacket>& out) {
    auto c = sessions_.find(d.caster);
    if (c == sessions_.end() || !c->second.inGame) return 1;
    if (d.target.none() || TargetHealth(d.target) <= 0) return 1;
    if (d.frame68 - 1 != frame_) return 0;
    if (const Buffs* b = BuffsOf(d.target)) { if (b->Has(14)) return 1; }
    int have = 0;
    for (int i = 0; i < 4; ++i) if (c->second.markSet[i]) ++have;
    if (have == 0) return 1;                                 // 걸린 표식이 없다
    int slot = 0;
    do { slot = static_cast<int>(NextRandom(4)); } while (!c->second.markSet[slot]);
    float ox = 0, oy = 0;
    TargetXY(d.target, ox, oy);
    TeleportTarget(d.target, c->second.markX[slot], c->second.markY[slot]);
    SendTeleportFx(ox, oy, c->second.markX[slot], c->second.markY[slot], out);
    return 1;
}

// ── 묶음 5-f 플라스마(59) ─────────────────────────────────────────────
//
// 지속 주문 셋 — `plasmaSmth_531580`(켤 때) · `plasmaShot_531600`(틱) · `sub_5319E0`(끌 때).
// dt = `PlasmaSearchTime`(기획자료).
//
// ★ **이 주문이 「아이템 묶음 대기」였던 까닭** — 켤 때 **완드를 들고 있나**를 본다.
//   사람이 완드 갈래 물건을 안 들고 있으면 **곧바로 취소**된다(`return 1`).
//   그리고 틱마다 그 완드의 **충전을 하나씩 먹는다** — 완드가 250 충전인 까닭이 이것이다.
//
// ⚠ 안 옮긴 갈래 — 헤큐바(`PlasmaDamageHecubah`)는 그 몬스터가 없어 못 옮겼다 ·
//   빔 그림(`netStartDurationRaySpell`)은 4-a3 틀의 것을 쓴다 · 소리 98 은 묶음 10.

// plasmaSmth_531580 — 켤 때
int Server::DurPlasmaCreate(DurSpell& d, std::vector<OutPacket>& out) {
    d.field80 = 0;                                           // +72 든 완드(우리는 아래 wandIdx 로 쓴다)
    d.field84 = 0;                                           // +76 뒤로 돌았을 때의 3초 유예
    d.target = Target{};                                     // +48
    {   // netSendPointFx_522FF0(131) — 화면 안 사람에게 점 효과
        const uint16_t ix = static_cast<uint16_t>(static_cast<int>(d.posX)), iy = static_cast<uint16_t>(static_cast<int>(d.posY));
        const uint8_t fx[5] = { 131, static_cast<uint8_t>(ix), static_cast<uint8_t>(ix >> 8),
                                static_cast<uint8_t>(iy), static_cast<uint8_t>(iy >> 8) };
        SendFxAllCli(d.posX, d.posY, fx, sizeof(fx), out);
    }
    auto c = sessions_.find(d.caster);
    if (c == sessions_.end() || !c->second.inGame) return 0;  // 사람이 아니면 그냥 둔다(정답지도 0)
    // 든 것이 **완드 갈래**인가 — 아니면 취소한다. 이게 「완드가 있어야 도는 주문」의 정체다.
    const int wi = HeldWandIndex(c->second);
    if (wi < 0) return 1;
    d.field80 = static_cast<uint32_t>(wi + 1);               // 0 은 「없음」이라 +1 해 담는다
    return 0;
}

// sub_5319E0 — 끌 때. 정답지는 완드 자료의 「지금 쓰는 중」 비트를 내린다. 우리는 그 비트를 안 두어 할 일이 없다.
void Server::DurPlasmaDestroy(DurSpell& d, std::vector<OutPacket>& out) {
    (void)d; (void)out;
}

// plasmaShot_531600 — 매 틱
//   ① 대상이 아직 쓸 만한가 — 죽었거나 **400 밖**이거나 안 보이면 버린다
//      · 대상이 **뒤로 돌아가면** 곧바로 버리지 않고 **3초를 센다**(그 안에 다시 앞에 오면 유지)
//   ② 없으면 둘레 **400** 안에서 **앞쪽에 있는 가장 가까운 적**을 찾는다
//   ③ 있으면 **종류 14(플라스마)** 로 `PlasmaDamage` 만큼 때리고, 빔을 켜고, 자세 22(집중)
//      · 대상이 있는 동안 **끝 프레임을 계속 미룬다**(`+68 = 지금 + PlasmaSearchTime`)
//      · **완드 충전을 하나 먹는다.** 0 이 되면 끝난다
int Server::DurPlasmaTick(DurSpell& d, std::vector<OutPacket>& out) {
    auto c = sessions_.find(d.caster);
    if (c == sessions_.end() || !c->second.inGame) return 1;
    Session& s = c->second;
    if (s.respawnAtFrame != 0) return 1;

    const float px = static_cast<float>(s.posX), py = static_cast<float>(s.posY);
    const float kRange = 400.0f;

    // ① 들고 있던 대상을 다시 본다
    bool keep = false;
    if (!d.target.none()) {
        float tx = 0, ty = 0;
        if (TargetXY(d.target, tx, ty) && TargetHealth(d.target) > 0) {
            // 뒤로 돌았나 — `& 2` 면 뒤다. 그러면 3초를 센다(앞이면 셈을 지운다)
            const uint8_t dir = TestTwoPointsAndDirection(px, py, s.facingPrev, tx, ty);
            if (dir & 2) {
                if (d.field84 == 0) d.field84 = 3 * kServerTickRate;
            } else {
                d.field84 = 0;
            }
            const float dx = tx - px, dy = ty - py;
            if (dx * dx + dy * dy <= kRange * kRange && CanInteract(s, d.target)) keep = true;
        }
        if (!keep) d.target = Target{};
    }
    // ② 없으면 찾는다 — 앞쪽 · 400 안 · 가장 가까운 적
    if (!keep) {
        d.target = PlasmaSearch(s, kRange);
        d.field84 = 0;
    }
    // 유예 셈 — 0 이 되면 대상을 버린다
    if (d.field84 != 0) {
        --d.field84;
        if (d.field84 == 0) d.target = Target{};
    }

    // ③ 대상이 있으면 쏜다
    if (d.target.none()) {
        if (!d.field36.none()) {
            NetStopRaySpell(d, d.field36, out);
            d.field36 = Target{};
        }
        return 0;
    }
    if (!(d.field36.addr == d.target.addr && d.field36.pet == d.target.pet)) {
        if (!d.field36.none()) NetStopRaySpell(d, d.field36, out);
        NetStartRaySpell(d, out);
    }
    const int dmg = static_cast<int>(BalanceFloat("PlasmaDamage"));
    HurtTarget(d.target, dmg, out, d.caster, /*dtype=*/14);   // 14 = PLASMA
    if (TargetHealth(d.target) <= 0) {
        float tx = 0, ty = 0;
        if (TargetXY(d.target, tx, ty)) {
            const uint16_t ix = static_cast<uint16_t>(static_cast<int>(tx)), iy = static_cast<uint16_t>(static_cast<int>(ty));
            const uint8_t fx[5] = { 131, static_cast<uint8_t>(ix), static_cast<uint8_t>(ix >> 8),
                                    static_cast<uint8_t>(iy), static_cast<uint8_t>(iy >> 8) };
            SendFxAllCli(tx, ty, fx, sizeof(fx), out);
        }
    }
    d.field36 = d.target;
    PlayerSetState(s, 22, frame_);                           // CONCENTRATE
    // 대상이 있는 동안 끝을 미룬다 — 유예를 세는 중이면 안 미룬다(정답지 그대로)
    if (d.field84 == 0)
        d.frame68 = frame_ + static_cast<uint32_t>(BalanceFloat("PlasmaSearchTime"));

    // ④ 완드 충전 — 틱마다 하나
    if (d.field80 != 0) {
        const size_t wi = static_cast<size_t>(d.field80 - 1);
        if (wi >= s.inventory.size()) return 1;
        Session::Carried& w = s.inventory[wi];
        if (w.chargesMax != 0) {
            if (w.charges == 0) return 1;
            --w.charges;
            SendCharges(s, w, out);
            if (w.charges == 0) return 1;
        }
    }
    return 0;
}

// 든 것 가운데 **완드 갈래**의 자리(없으면 −1). 완드 = `USE = WandCastUse` 로 읽힌 것.
//   ⚠ 정답지는 오브젝트 클래스 비트 `0x4000000`(WAND)을 본다. 우리는 아직 그 비트를 안 담아
//     **쓸 때 부르는 함수**로 갈음한다 — 같은 집합이다(완드만 `WandCastUse` 를 쓴다).
int Server::HeldWandIndex(const Session& s) {
    for (size_t i = 0; i < s.inventory.size(); ++i)
        if (s.inventory[i].useFn == UseFn::WandCast) return static_cast<int>(i);
    return -1;
}

// sub_531920 — 둘레에서 고르는 잣대: **앞쪽**(`& 1`) · 볼 수 있고 · 가장 가까운 **적**.
Target Server::PlasmaSearch(const Session& s, float range) const {
    const float px = static_cast<float>(s.posX), py = static_cast<float>(s.posY);
    float best = range * range;
    Target found;
    auto consider = [&](const Target& t, float tx, float ty) {
        if (!(TestTwoPointsAndDirection(px, py, s.facingPrev, tx, ty) & 1)) return;   // 앞쪽만
        if (!CanInteract(s, t)) return;
        const float dx = tx - px, dy = ty - py;
        const float d2 = dx * dx + dy * dy;
        if (d2 >= best) return;
        best = d2;
        found = t;
    };
    for (const auto& kv : sessions_) {
        const Session& o = kv.second;
        if (!o.inGame || o.respawnAtFrame != 0 || o.addr == s.addr) continue;
        if (!IsEnemyTo(s, o)) continue;
        consider(Target{ o.addr, -1 }, static_cast<float>(o.posX), static_cast<float>(o.posY));
    }
    for (size_t k = 0; k < pets_.size(); ++k) {
        const Creature& cr = pets_[k];
        if (cr.health == 0) continue;
        if (!OwnersEnemy(s.addr, false, cr.owner, cr.owner.empty())) continue;
        consider(Target{ std::string(), static_cast<int>(k) }, static_cast<float>(cr.x), static_cast<float>(cr.y));
    }
    return found;
}

// ── 묶음 6-e(1) 길 찾기 격자 색인 ─────────────────────────────────────
//
// 정답지 `server/object_ai_path.go:16~224`. 길 찾기는 **벽 격자(256×256) 위에서** 돈다.
// 칸마다 깃발을 세워 두고 「여기를 지날 수 있나」를 묻는다.
//
// ⭕ 깃발(같은 파일 31~36행 + `sub50B8E0`) —
//     `0x1`   벽 — **땅으로 걷는 것**을 막는다
//     `0x4`   승강기 · `0x8` 승강기 통로 · `0x20` 나르는 것
//     `0x10`  구멍 — ★ **날아가는 것**을 막는다(땅으로 걷는 것은 지나간다)
//     `0x100` 오브젝트가 차지함
//     `0x200` 날아가는 것(오브젝트 깃발 `0x4000`)에만 걸리는 자리
//     `0x400` 갈래 비트 10 이 없는 것만 막는 자리
//
// ★ **세대(generation)** 로 묵은 자국을 가른다 — 색인을 다시 뜰 때 세대를 하나 올리고,
//   칸의 세대가 지금 세대와 다르면 **오브젝트 자국이 없는 것으로** 본다.
//   그래서 칸을 일일이 지우지 않고도 매번 새로 뜰 수 있다.
// ★ 색인은 **15틱에 한 번**만 다시 뜬다(`IndexObjects`).
//
// ⚠ 정답지 `CheckIndexFlags` 는 격자 밖을 **안 막는다**(널 포인터를 그냥 읽는다).
//   우리는 밖이면 **막힌 것으로** 본다 — 길 찾기가 맵 밖으로 새면 안 된다.

void Server::AIPathResetIndex() {
    aiIndexGen_ = 0;
    aiIndexFrame_ = 0;                                       // `Init`: lastFrame = 0
    aiIndexCalc_ = false;
    for (auto& n : aiIndex_) { n.gen = 0; n.flags = 0; }
}

Server::AIIndexNode* Server::AIPathIndexAt(int x, int y) {
    if (x < 0 || x >= kAIIndexSize || y < 0 || y >= kAIIndexSize) return nullptr;
    return &aiIndex_[static_cast<size_t>(y) * kAIIndexSize + static_cast<size_t>(x)];
}

uint16_t Server::AIPathFlagsAt(int x, int y) {
    const AIIndexNode* n = AIPathIndexAt(x, y);
    return n ? n->flags : 0;
}

// CheckIndexFlags — **막혔나**. 날아가는 것은 **구멍(0x10)** 에, 땅을 걷는 것은 **벽(0x1)** 에 걸린다.
bool Server::AIPathBlockedFlags(bool airborne, int x, int y) {
    const AIIndexNode* n = AIPathIndexAt(x, y);
    if (!n) return true;                                     // 격자 밖은 막힌 것으로 본다
    return airborne ? (n->flags & 0x10) != 0 : (n->flags & 0x1) != 0;
}

// sub50B8E0 — **오브젝트가 막고 있나**. ★ 칸의 **세대가 지금 세대와 다르면 묵은 자국**이라 0.
bool Server::AIPathBlockedByObj(bool airborne, uint32_t subclass, int x, int y) {
    const AIIndexNode* n = AIPathIndexAt(x, y);
    if (!n) return true;
    if (n->gen != aiIndexGen_) return false;                 // 묵은 자국 — 없는 셈
    if (airborne) return (n->flags & 0x200) != 0;            // 날아가는 것은 이 자리만 본다
    if (n->flags & 0x100) return true;                       // 무엇이 차지하고 있다
    if ((subclass >> 10) & 1) return false;                  // 갈래 비트 10 은 이 자리를 지나간다
    return (n->flags & 0x400) != 0;
}

// Nox_xxx_pathfind_preCheckWalls2_50B8A0 — 둘 다 아니어야 지날 수 있다.
bool Server::AIPathCanPass(bool airborne, uint32_t subclass, int x, int y) {
    if (AIPathBlockedFlags(airborne, x, y)) return false;
    return !AIPathBlockedByObj(airborne, subclass, x, y);
}

// IndexObjects — **15틱에 한 번**만 다시 뜬다. 뜰 때 **세대를 하나 올린다**.
//   ⚠ 오브젝트를 칸에 찍는 `IndexObject`(정답지 100줄)는 아직 안 옮겼다 —
//     지금은 **장애물(움직이지 않는 것)만** 찍는다. 소환수·사람이 서로 길을 막는 갈래는 다음 조각.
void Server::AIPathIndexObjects() {
    // ⚠ 정답지 그대로 — **처음(둘 다 0)에도 안 돈다**(`0 - 0 = 0 < 15`).
    //   길 찾기를 처음 쓰는 자리에서 15틱이 지나야 처음 뜬다.
    if (frame_ - aiIndexFrame_ < 15) return;
    aiIndexFrame_ = frame_;
    ++aiIndexGen_;
    for (const auto& o : obstacles_) {
        const int gx = static_cast<int>(o.x / kWallCell);
        const int gy = static_cast<int>(o.y / kWallCell);
        AIIndexNode* n = AIPathIndexAt(gx, gy);
        if (!n) continue;
        n->gen = aiIndexGen_;
        n->flags |= 0x100;                                   // 무엇이 차지하고 있다
    }
    aiIndexCalc_ = true;
}

// MaybeIndexObjects — 이번 프레임에 이미 떴으면 건너뛴다.
void Server::AIPathMaybeIndexObjects() {
    if (aiIndexCalc_) return;
    AIPathIndexObjects();
}

// Sub_50B500 / Sub_50B510 — 「다시 떠야 한다」고 표시한다. 뒤엣것은 틱 관문까지 푼다.
void Server::AIPathInvalidate(bool alsoFrame) {
    aiIndexCalc_ = false;
    if (alsoFrame) aiIndexFrame_ = 0;
}

// 격자 깃발을 다시 굽는다.
//
// ★★ **2026-09-15 정정(6-e(2b·2))** — 여기서 **벽을 굽지 않는다.**
//   정답지 `Sub50AFA0`(`server/object_ai_path.go:512~596`)이 깃발 **0x1** 을 찍는 자리는
//   **오브젝트 모양**(클래스 0x400000 을 칸마다 눌러 보는 자리)뿐이고, **벽은 안 찍는다**.
//   벽은 **`sub_50B870`(칸의 벽 방향 번호 != −1)과 이음매 규칙**으로만 막는다.
//   전에는 벽을 0x1 에 구워 **먼저 통째로** 막아, 이음매 규칙이 아예 효과가 없었다.
//
// ⚠ 그래서 지금 **0x1 을 찍는 것은 아무것도 없다** — 오브젝트 모양 찍기(`IndexObject`, 정답지 100줄)를
//   아직 안 옮겼기 때문이다. 옮기면 여기가 아니라 그쪽이 찍는다.
void Server::AIPathBakeWalls() {
    for (int y = 0; y < kAIIndexSize; ++y)
        for (int x = 0; x < kAIIndexSize; ++x)
            if (AIIndexNode* n = AIPathIndexAt(x, y)) n->flags &= static_cast<uint16_t>(~0x1);
    AIPathBakeObstacles();   // 묶음 6-e(2b·3) — 오브젝트 모양이 0x1(·0x2)을 찍는다
    AIPathBakeWaypoints();   // 묶음 6-j(2) — 길목 칸(0x40)은 여기서 찍는다
}

// ── 묶음 6-e(2) 길 훑기 ───────────────────────────────────────────────
//
// 정답지 `src/unit_ai_path.go:19~330` `nox_xxx_pathFind_50BA00`.
// ★ 이것은 **물결 훑기(너비 우선)** 다 — 한 물결을 다 보고 다음 물결로 간다.
//
// ⭕ 짜임새
//   · `MapIndexLast` 를 **하나 올려** 「이번 훑기에 가 봤다」 자국으로 쓴다.
//     칸의 `visit` 이 그 수와 같으면 이미 가 본 칸이다(6-e(1) 의 세대와 **다른 수다**).
//   · 물결은 **외줄 사슬** — 칸마다 `next`(정답지 `Field8`)로 잇고, `parent`(`Field4`)로 온 길을 남긴다
//   · 이웃 여덟은 blob `0x587000+234216` 표 — **0~3 이 직교, 4~7 이 대각**.
//     시작 자리를 **난수로 돌려서**(`ioff`) 본다. 같은 거리면 늘 같은 쪽으로 쏠리지 않게 하는 장치다
//   · `awayFrom=false` 면 **목표에 가장 가까운** 칸을, `true` 면 **가장 먼** 칸을 best 로 들고 간다
//     (정답지 이름은 `far` 인데, `windows.h` 의 옛 매크로 `far` 와 부딪혀 이름을 바꿨다)
//     (도망갈 때 쓰는 갈래다 — `Nox_xxx_generateRetreatPath_50CA00`)
//   · 끝나는 갈래 셋 — **0** 목표에 닿음 · **1** 자국 자리를 다 씀 · **2** 깊이 한도·훑을 곳 없음.
//     어느 갈래든 **best 에서 길을 만든다**(`Sub_50C320`)
//
// ★★ **시작 자리가 막혔을 때**(`v63`) — 훑기를 **느슨하게** 연다.
//   막힌 칸에서 출발하면 아무 데도 못 가니, 「지날 수 있는 칸」 조건을 잠시 풀고
//   한 칸이라도 빠져나오면(`v64`) **그 물결에서 못 빠져나온 칸을 쳐내고** 다시 조인다.
//   거르개(`fnc`)에도 같은 장치가 따로 있다(`v61`/`v65`).
//
// ⚠ 아직 안 옮긴 곁가지 — **6-e(2b)** 로 미룬다
//   · `preCheckWalls_50C8D0` 시작 칸 미리 열기
//   · 대각선 **이음매** 검사(`Sub_57B500` 벽 모서리 번호 + 문 열쇠) — 지금은 **두 직교 칸이 뚫려야** 대각을 준다
//   · `sub_50B870`(벽 모서리) · `sub_50C830`(물칸 6번 타일)
//   · `Sub_50AC20` 나르는 칸(구멍·전송·승강기)을 자식으로 얹기
//   · 사람·소환수·문은 아직 `obstacles_` 에 없다(묶음 9)

// 이웃 여덟 (blob_587000.dat +234216, Point32 여덟 개를 그대로 읽음)
static const int kAINb[8][2] = {
    { 1, 0 }, { 0, -1 }, { -1, 0 }, { 0, 1 },       // 0~3 직교
    { 1, 1 }, { -1, -1 }, { -1, 1 }, { 1, -1 },     // 4~7 대각
};

// MapTraceObstacles(`server/map.go:453`) — 두 점 사이를 **장애물**이 막는가. 안 막으면 참.
//   ⚠ 정답지는 사람·소환수(적이면 지나침)·문도 본다. 우리 `obstacles_` 에는 아직 안 움직이는 것만 있다
bool Server::MapTraceObstacles(int skipObstacle, float x1, float y1, float x2, float y2) const {
    const float qx0 = std::min(x1, x2), qx1 = std::max(x1, x2);
    const float qy0 = std::min(y1, y2), qy1 = std::max(y1, y2);
    for (size_t i = 0; i < obstacles_.size(); ++i) {
        if (static_cast<int>(i) == skipObstacle) continue;
        const PhysBody& o = obstacles_[i];
        if (!o.hasCollide) continue;                         // FlagNoCollide · AllowOverlap 자리
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
            const float px = o.x, py = o.y;
            if (LineTraceXxx(x1, y1, x2, y2, px + o.boxLT, py + o.boxLB, px + o.boxLB, py + o.boxLT)) return false;
            if (LineTraceXxx(x1, y1, x2, y2, px + o.boxLT, py + o.boxLB, px + o.boxRT, py + o.boxRB)) return false;
            if (LineTraceXxx(x1, y1, x2, y2, px + o.boxRB, py + o.boxRT, px + o.boxRT, py + o.boxRB)) return false;
            if (LineTraceXxx(x1, y1, x2, y2, px + o.boxRB, py + o.boxRT, px + o.boxLB, py + o.boxLT)) return false;
        }
    }
    return true;
}

void Server::AIPathResetVisit() { aiVisit_.clear(); }

// NewVisitNode — 자국 자리는 **1024개**(`alloc.NewClassT("VisitNodes", …, 1024)`). 넘으면 −1.
int Server::AIPathNewVisit(int x, int y, int parent, int next) {
    if (aiVisit_.size() >= kAIVisitMax) return -1;
    AIVisitNode n;
    n.x = static_cast<int16_t>(x);
    n.y = static_cast<int16_t>(y);
    n.parent = parent;
    n.next = next;
    aiVisit_.push_back(n);
    return static_cast<int>(aiVisit_.size()) - 1;
}

// ── 묶음 6-e(3) 길에서 점 뽑기 ────────────────────────────────────────
//
// 정답지 `server/object_ai_path.go:353` `Sub_50C320`.
// ★ 이름과 달리 **길을 다듬는 게 아니라, 칸마다 점을 하나씩 찍는다.**
//   찍는 자리가 **왔던 쪽으로 붙어** 있어서, 걸어가면 저절로 **모서리를 질러간다**.
//
// ⭕ 짜임새
//   · 찾은 칸에서 **온 길을 거꾸로** 되짚으며 점을 쌓고, 끝에 **통째로 뒤집는다**(`swapPoints`)
//   · 첫 점 — 목표에 닿았으면 **정확한 목표 자리**, 아니면 그 칸의 **한가운데(+11.5)**
//   · 그다음부터 칸마다 — 「그 칸에서 **앞 칸 쪽으로** 붙은 점」.
//     `dx`·`dy` 는 **앞 칸에서 이 칸을 뺀 값**(되짚는 방향)이고,
//     음수면 칸의 **오른쪽·아래 끝(+23−2.3)**, 양수면 **왼쪽·위 끝(+2.3)**, 0 이면 **가운데(+11.5)**
//   · ⚠ `dx` 와 `dy` 가 **둘 다 0 이면 점을 안 넣는다** — 정답지 `dx == 0` 갈래에 `else` 가 없다
//   · 점은 **1024개**까지(`alloc.Make([]types.Pointf{}, 1024)`)
//
// ⚠ 나르는 칸(`Flags12 & 2` · `sub50AEA0` · 넷코드를 몬스터 자료에 적기)은 **6-e(2b)**.
//   훑기가 아직 나르는 칸을 자식으로 안 얹으므로 이 갈래가 설 자리가 없다.
// ⚠ `MaybeAppendWorkPath`(점이 담을 곳보다 많으면 상태를 1 로)는 **6-f 가기**에서 붙인다.
void Server::AIPathCollect(int node, bool hasExact, float ex, float ey) {
    aiPathCells_.clear();
    aiPathPts_.clear();
    if (node < 0) return;
    // 칸 목록 — 눈으로 보려고 따로 둔다(정답지에는 없다)
    std::vector<int> rev;
    for (int i = node; i >= 0; i = aiVisit_[static_cast<size_t>(i)].parent) rev.push_back(i);
    for (size_t i = rev.size(); i-- > 0; ) {
        const AIVisitNode& n = aiVisit_[static_cast<size_t>(rev[i])];
        aiPathCells_.push_back({ n.x, n.y });
    }

    auto put = [&](double x, double y) {
        if (aiPathPts_.size() >= kAIPointMax) return;
        aiPathPts_.push_back({ static_cast<float>(x), static_cast<float>(y) });
    };
    if (hasExact) {
        put(ex, ey);
    } else {
        put(aiVisit_[static_cast<size_t>(node)].x * 23.0 + 11.5,
            aiVisit_[static_cast<size_t>(node)].y * 23.0 + 11.5);
    }
    int prev = node;
    for (int it = aiVisit_[static_cast<size_t>(node)].parent; it >= 0;
         prev = it, it = aiVisit_[static_cast<size_t>(it)].parent) {
        const int dx = aiVisit_[static_cast<size_t>(it)].x - aiVisit_[static_cast<size_t>(prev)].x;
        const int dy = aiVisit_[static_cast<size_t>(it)].y - aiVisit_[static_cast<size_t>(prev)].y;
        const double bx = aiVisit_[static_cast<size_t>(it)].x * 23.0;
        const double by = aiVisit_[static_cast<size_t>(it)].y * 23.0;
        if (dx < 0) {
            if (dy > 0)       put(bx + 23.0 - 2.3, by + 2.3);
            else if (dy < 0)  put(bx + 23.0 - 2.3, by + 23.0 - 2.3);
            else              put(bx + 23.0 - 2.3, by + 11.5);
        } else if (dx == 0) {
            if (dy > 0)       put(bx + 11.5, by + 2.3);
            else if (dy < 0)  put(bx + 11.5, by + 23.0 - 2.3);
            // ★ 둘 다 0 이면 **점이 없다** — 정답지에 else 가 없다
        } else {
            if (dy < 0)       put(bx + 2.3, by + 23.0 - 2.3);
            else if (dy == 0) put(bx + 2.3, by + 11.5);
            else              put(bx + 2.3, by + 2.3);
        }
    }
    std::reverse(aiPathPts_.begin(), aiPathPts_.end());      // swapPoints
}

int Server::AIPathFind(bool awayFrom, const PathActor& act, float fromX, float fromY,
                       float toX, float toY, const PathFilter& fnc, int maxDepth) {
    aiPathStatus_ = 0;
    ++aiIndexLast_;
    AIPathMaybeIndexObjects();

    const int sx = static_cast<int>(static_cast<float>(fromX / kWallCell));
    const int sy = static_cast<int>(static_cast<float>(fromY / kWallCell));
    const int gx = static_cast<int>(toX / kWallCell);
    const int gy = static_cast<int>(toY / kWallCell);

    // ⚠ `preCheckWalls_50C8D0`(시작 칸 미리 열기)은 6-e(2b)
    bool relaxWall = !AIPathCanPass(act.airborne, act.subclass, sx, sy);   // v63
    bool relaxFn = (fnc && !fnc(sx, sy));                                  // v61

    AIPathResetVisit();
    const int root = AIPathNewVisit(sx, sy, -1, -1);
    if (root < 0) { aiPathStatus_ = 2; AIPathCollect(-1, false, 0, 0); return 2; }
    if (AIIndexNode* rp = AIPathIndexAt(sx, sy)) rp->visit = aiIndexLast_;

    int cur = awayFrom ? 0 : 999999;
    int best = -1;                                           // v67
    int wave = root;                                         // vn2
    int depth = 0;                                           // v66
    bool escWall = false, escFn = false;                     // v64 · v65

    for (;;) {
        int scan = wave;                                     // vn3
        wave = -1;
        int nextWave = -1;                                   // v77
        while (scan >= 0) {
            const int x1 = aiVisit_[static_cast<size_t>(scan)].x;
            const int y1 = aiVisit_[static_cast<size_t>(scan)].y;
            const int dd = (x1 - gx) * (x1 - gx) + (y1 - gy) * (y1 - gy);
            if (awayFrom) {
                if (dd > cur) { best = scan; cur = dd; }
            } else {
                if (dd < cur) { best = scan; cur = dd; }
                if (!relaxWall && !relaxFn && x1 == gx && y1 == gy) {
                    aiPathStatus_ = 0;
                    AIPathCollect(scan, true, toX, toY);
                    return 0;
                }
            }
            const int ioff = static_cast<int>(NextRandom(8));
            for (int i = 0; i < 8; ++i) {
                const int ii = (i + ioff) % 8;
                const int x2 = x1 + kAINb[ii][0];
                const int y2 = y1 + kAINb[ii][1];
                AIIndexNode* ip = AIPathIndexAt(x2, y2);
                if (!ip || ip->visit == aiIndexLast_) continue;
                ip->visit = aiIndexLast_;                    // 가 봤다고 찍는다

                bool accept = false;
                // ★ 이웃이 **바로 목표 칸**이면, 칸 한가운데에서 **정확한 점까지 줄이 뚫리는지** 본다.
                //   뚫리면 벽 검사를 **건너뛰고** 받아들인다(정답지 LABEL_32).
                if (x2 == gx && y2 == gy) {
                    const float ax = x1 * 23.0f + 11.0f, ay = y1 * 23.0f + 11.0f;
                    if (MapTraceObstacles(act.obstacleSkip, ax, ay, toX, toY) &&
                        MapTraceRay(ax, ay, toX, toY, act.airborne ? 5 : 1))
                        accept = true;
                }
                if (!accept) {
                    // 대각선 **이음매** — 두 옆칸의 **벽 방향 번호**가 그 대각을 막는가(6-e(2b)) ·
                    //   그리고 두 옆칸이 아예 막혔나(정답지도 둘 다 본다)
                    if (ii >= 4) {
                        if (!AIPathDiagOk(ii, x1, y1)) continue;
                        if (AIPathBlockedFlags(act.airborne, x1, y2)) continue;
                        if (AIPathBlockedFlags(act.airborne, x2, y1)) continue;
                    }
                    // ★ `sub_50B870` — **벽이 있는 칸은 못 지난다**. 정답지는 벽을 격자 깃발에
                    //   안 찍고 **오로지 여기서** 막는다(6-e(2b·2)).
                    //   ⚠ 이 관문은 **느슨한 갈래보다 먼저**다 — 벽에 파묻히면 못 빠져나온다
                    if (WallDirAt(x2, y2) != -1) continue;
                    if (relaxWall) {
                        if (AIPathCanPass(act.airborne, act.subclass, x2, y2)) {
                            escWall = true;                  // 빠져나왔다
                        } else if (!AIPathBlockedFlags(act.airborne, x1, y1) &&
                                   AIPathBlockedFlags(act.airborne, x2, y2)) {
                            continue;                        // 정답지 주석: 왜 한쪽만 보는지 모름
                        }
                    } else {
                        if (!AIPathCanPass(act.airborne, act.subclass, x2, y2)) continue;
                        if (fnc) {
                            const bool ok = fnc(x2, y2);
                            if (relaxFn) { if (ok) escFn = true; }
                            else if (!ok) continue;
                        }
                    }
                }
                const int nn = AIPathNewVisit(x2, y2, scan, nextWave);
                if (nn < 0) {                                // 자국 자리를 다 썼다
                    aiPathStatus_ = 1;
                    AIPathCollect(best, false, 0, 0);
                    return 1;
                }
                nextWave = nn;
            }
            // ⚠ `Sub_50AC20` 나르는 칸(구멍·전송·승강기) 얹기는 6-e(2b)
            scan = aiVisit_[static_cast<size_t>(scan)].next;
        }
        wave = nextWave;

        // ★ 느슨하게 열었다가 **한 칸이라도 빠져나왔으면**, 못 빠져나온 칸을 물결에서 쳐낸다
        if (escWall) {
            int prev = -1;
            for (int it = wave; it >= 0; ) {
                const int nx = aiVisit_[static_cast<size_t>(it)].next;
                if (AIPathCanPass(act.airborne, act.subclass,
                                  aiVisit_[static_cast<size_t>(it)].x,
                                  aiVisit_[static_cast<size_t>(it)].y)) {
                    prev = it;
                } else if (prev >= 0) {
                    aiVisit_[static_cast<size_t>(prev)].next = nx;
                } else {
                    wave = nx;
                }
                it = nx;
            }
            relaxWall = false; escWall = false;
        }
        if (escFn) {
            int prev = -1;
            for (int it = wave; it >= 0; ) {
                const int nx = aiVisit_[static_cast<size_t>(it)].next;
                if (fnc(aiVisit_[static_cast<size_t>(it)].x, aiVisit_[static_cast<size_t>(it)].y)) {
                    prev = it;
                } else if (prev >= 0) {
                    aiVisit_[static_cast<size_t>(prev)].next = nx;
                } else {
                    wave = nx;
                }
                it = nx;
            }
            relaxFn = false; escFn = false;
        }
        if (wave < 0) break;                                 // 훑을 곳이 없다
        if (maxDepth != 0 && depth >= maxDepth) {            // 깊이 한도
            aiPathStatus_ = 2;
            AIPathCollect(best, false, 0, 0);
            return 2;
        }
        ++depth;
    }
    aiPathStatus_ = 2;
    AIPathCollect(best, false, 0, 0);
    return 2;
}

// ── 묶음 6-f(1) 길 따라 걷기 ──────────────────────────────────────────
//
// 정답지 `legacy/GAME4_1.c:774` `nox_xxx_creatureActuallyMove_50D3B0`.
// 6-e(3) 이 뽑아 둔 점들 가운데 **하나를 골라 그쪽으로 속도를 준다.**
//
// ⭕ 고르는 법 — 지금 자리에서 **줄이 뚫리는**(`mapTraceRay` 깃발 **132**) 점만 본다.
//   · **8 보다 먼** 점 중에서는 **가장 가까운 것**을 고른다 — 너무 멀리 질러가지 않게
//   · **8 안**으로 들어온 점이 **마지막 점**이면 **다 온 것**이다(참을 돌려준다)
//   · 8 안인데 마지막이 아니면 「지나친 점」으로 적어 둔다 —
//     멀리 보이는 점이 하나도 없을 때만 그리로 간다
//   · 그것마저 없으면 길을 버린다(막혔다)
//
// ★ **보는 쪽은 8씩 안 돌린다** — `facing` 과 `facingWant` 를 **같이** 놓는다(정답지 `+62`·`+63`).
//   바라보기 행동(6-d)과 다른 자리다. 걸을 때는 곧바로 그쪽을 본다.
// ★ 보는 쪽은 **가는 점이 아니라 그 점으로 들어오는 토막**의 방향이다
//   (`pt[i] − pt[i−1]`, 첫 점이면 `pt[1] − pt[0]`).
//
// ⚠ 속도에 곱하는 **깃발 0x4000 갈래**(`*(v1+484)+96` 배)는 아직 안 옮겼다 — 6-g 에서 본다.
bool Server::CreatureActuallyMove(Creature& k) {
    if (k.path.empty()) return false;
    const int n = static_cast<int>(k.path.size());
    if (k.pathInd >= n) { k.path.clear(); return false; }

    int farIdx = -1;                 // v16 — 8 보다 먼 것 중 **가장 가까운** 점
    double farD2 = 10000000.0;       // v17
    int nearIdx = -1;                // v18 — 8 안으로 들어온 마지막 점
    for (int i = k.pathInd; i < n; ++i) {
        const float px = k.path[static_cast<size_t>(i)].first;
        const float py = k.path[static_cast<size_t>(i)].second;
        if (!MapTraceRay(static_cast<float>(k.x), static_cast<float>(k.y), px, py, 132)) continue;
        const double dx = px - k.x, dy = py - k.y;
        const double d2 = dx * dx + dy * dy;
        if (d2 > 64.0) {
            if (farIdx < 0 || farD2 > d2) { farD2 = d2; farIdx = i; }
        } else {
            if (i == n - 1) { k.path.clear(); return true; }   // 다 왔다
            nearIdx = i;
        }
    }
    int pick = farIdx;
    if (farIdx < 0) {
        if (nearIdx < 0) { k.path.clear(); return false; }     // 아무 점도 안 보인다
        pick = nearIdx;
    }
    k.pathInd = pick;

    const double dx = k.path[static_cast<size_t>(pick)].first - k.x;
    const double dy = k.path[static_cast<size_t>(pick)].second - k.y;
    const double len = std::sqrt(dx * dx + dy * dy) + 0.01;    // blob 0x581450+10288 = 0.01
    // ★ 묶음 6-h — **뛰는 중이면 빠르기에 뛰기 배율을 곱한다**(깃발 0x4000 · 몬스터표 +96)
    double spd = k.speedCur;
    if (k.running) {
        const MonsterStats* ms = FindMonster(k.type);
        if (ms && ms->runMul > 0.0) spd *= ms->runMul;
    }

    // 보는 쪽 — 그 점으로 **들어오는 토막**의 방향
    double fx, fy;
    if (pick <= 0) {
        // ⚠ 정답지는 여기서 `pt[1]` 을 읽는다 — 점이 하나뿐이면 없는 자리를 읽는다.
        //   우리는 그때 **그 점 쪽**을 본다(길이 하나면 방향이 그것밖에 없다).
        if (n >= 2) {
            fx = k.path[1].first - k.path[0].first;
            fy = k.path[1].second - k.path[0].second;
        } else {
            fx = dx; fy = dy;
        }
    } else {
        fx = k.path[static_cast<size_t>(pick)].first - k.path[static_cast<size_t>(pick - 1)].first;
        fy = k.path[static_cast<size_t>(pick)].second - k.path[static_cast<size_t>(pick - 1)].second;
    }
    const uint8_t dir = DirFromVec(fx, fy);
    k.facing = dir;
    k.facingWant = dir;                                        // ★ 둘을 같이 놓는다

    k.velX = spd * dx / len;
    k.velY = spd * dy / len;
    return false;
}

// ── 묶음 6-f(2) 길 내기·다시 내기 ─────────────────────────────────────
//
// 정답지 `src/unit_ai_path.go:520~544` + `legacy/GAME4_1.c:877`.
//
// ⭕ **길은 아무 때나 다시 내지 않는다** — 매 틱 새로 내면 256×256 을 매 틱 훑는다.
//   · `creatureSetDetailedPath` 는 **10프레임 관문**을 둔다. 관문에 걸리면 길을 **비우고**
//     결과를 **1**(자리 다 씀)로 적는다 — 그 틱에는 아예 안 움직인다
//   · `creatureSetMovePath` 는 그 위에 **「목표가 얼마나 움직였나」** 를 더 본다 —
//     줄이 뚫리는 갈래에서는 **50 유닛(제곱 2500)** 을 넘게 움직여야 다시 낸다
//
// ⭕ 점은 **31개까지**만 옮겨 담는다(`ud.Path` 가 32칸인데 `appendWorkPath` 가 마지막 칸을 안 쓴다).
//   넘치면 길 찾기 결과가 **0(닿음) → 1** 로 내려앉는다(`MaybeAppendWorkPath`).
//
// ⚠ **길목 길**(`sub_50D2A0` · `sub_50D2E0` · `sub_547F20`)은 미뤘다 —
//   길목(Waypoint)이 우리 맵 자료에 아직 없다. 정답지에서 길목 길이 **실패했을 때와 같은 길**을 탄다.

// nox_xxx_genPathToPoint_50B9A0 + MaybeAppendWorkPath — 낸 점을 크리처의 길로 옮겨 담는다
int Server::CreatureGenPath(Creature& k, float tx, float ty) {
    uint32_t oflags = 0;
    auto tb = typeBodies_.find(k.type);
    if (tb != typeBodies_.end()) oflags = tb->second.flags;
    const bool airborne = (oflags & 0x4000) != 0;
    AIPathFind(false, PathActor{ airborne, 0, -1 },
               static_cast<float>(k.x), static_cast<float>(k.y), tx, ty, PathFilter(), 0);
    // MaybeAppendWorkPath — 담을 곳보다 점이 많으면 **닿았다(0)를 1 로 내린다**
    if (aiPathPts_.size() > kCreaturePathMax && aiPathStatus_ == 0) aiPathStatus_ = 1;
    k.path.clear();
    for (size_t i = 0; i < aiPathPts_.size(); ++i) {
        if (k.path.size() >= kCreaturePathMax - 1) break;   // 마지막 칸은 안 쓴다
        k.path.push_back(aiPathPts_[i]);
    }
    return static_cast<int>(k.path.size());
}

// Nox_xxx_creatureSetDetailedPath_50D220 — **10프레임 관문**을 넘어야 새로 낸다
void Server::CreatureSetDetailedPath(Creature& k, float tx, float ty) {
    k.pathInd = 0;                                   // ud.Field67 = 0
    if (frame_ - k.pathFrame >= 10) {
        k.pathGoalX = tx; k.pathGoalY = ty;          // ud.Field68 — 이때 잡은 목표를 적어 둔다
        CreatureGenPath(k, tx, ty);
        k.pathStatus = static_cast<uint8_t>(aiPathStatus_);
        k.pathFrame = frame_;
    } else {
        k.path.clear();                              // ud.Field2 = 0 — 길이 없는 셈
        k.pathStatus = 1;
    }
}

// nox_xxx_creatureSetMovePath_50D5A0 — **1 이면 다 왔다**
int Server::CreatureSetMovePath(Creature& k, float tx, float ty) {
    const double ddx = tx - k.x, ddy = ty - k.y;
    if (std::sqrt(ddx * ddx + ddy * ddy) + 0.01 <= 8.0) return 1;   // 목표까지 8 안이면 끝

    uint32_t oflags = 0;
    auto tb = typeBodies_.find(k.type);
    if (tb != typeBodies_.end()) oflags = tb->second.flags;
    const uint8_t rayFlags = static_cast<uint8_t>((oflags >> 12) & 4);   // 깃발 0x4000 이면 4
    const bool clear = MapTraceRay(static_cast<float>(k.x), static_cast<float>(k.y), tx, ty, rayFlags);

    if (!clear) {
        // ⚠ 여기서 정답지는 **길목 길**을 먼저 본다. 길목이 없으므로 곧바로 자세한 길로 간다
        if (k.path.empty()) CreatureSetDetailedPath(k, tx, ty);
    } else if (k.path.empty() ||
               ((frame_ - k.pathFrame) > 10 &&
                (static_cast<double>(k.pathGoalX - tx) * (k.pathGoalX - tx) +
                 static_cast<double>(k.pathGoalY - ty) * (k.pathGoalY - ty)) > 2500.0)) {
        CreatureSetDetailedPath(k, tx, ty);
    }
    if (!k.path.empty() && CreatureActuallyMove(k)) { k.path.clear(); return 1; }
    return 0;
}

// ── 묶음 6-f(3) 틱에 잇기 ─────────────────────────────────────────────
//
// ★★ 6-e·6-f(1)(2) 는 **함수만 있고 아무도 안 불렀다.** 이 조각이 **크리처 틱에 건다.**
//
// ⚠ 이것이 **임시 구현을 갈아 끼운다** — 그 전에는 목표를 향해 **곧장 직선으로** 갔다.
//   곧장 가면 **벽에 붙어 버틴다.** 이제 길을 내어 **돌아간다**.
//
// ⭕ 한 틱에 하는 일
//   1. `CreatureSetMovePath` — 다 왔으면 참 · 아니면 길을 내고 **속도를 세운다**
//   2. 그 속도로 **자리를 옮긴다**. 벽은 **x·y 를 따로** 보아 한 축이 막혀도 다른 축으로 미끄러진다
//   · 10프레임 관문에 걸려 길이 비면 **그 틱은 안 움직인다**(정답지도 그렇다)
bool Server::CreatureTickMove(Creature& k, double tx, double ty) {
    if (CreatureSetMovePath(k, static_cast<float>(tx), static_cast<float>(ty)) == 1) {
        k.velX = 0; k.velY = 0;
        return true;                                         // 다 왔다
    }
    if (k.path.empty()) { k.velX = 0; k.velY = 0; return false; }
    CreatureApplyVelocity(k);
    return false;
}

// ── 묶음 6-f(5) 아무 데나 걷기(29) ────────────────────────────────────
//
// 정답지 `legacy/GAME5.c:68` `nox_xxx_mobActionRandomWalk_545020` + `sub_545090`.
// 가기(7)가 **막혔을 때** 올리는 행동이다 — 길을 못 내니 **아무 쪽으로나 조금씩 틀며 걷는다**.
//
// ⭕ 갈 쪽 고르기(`sub_545090`)
//   · **지금 보는 쪽에 ±20 을 더한다** — 한 번에 확 꺾지 않고 **조금씩** 튼다.
//     `randomInt(-20, 20)` 은 **양끝을 다 넣으므로** 41가지다
//   · 0~255 로 감싼다(정답지의 두 줄짜리 감싸기는 `& 0xFF` 과 같다)
//
// ⭕ 걷기(`545020`)
//   · 고른 쪽을 **보는 쪽·돌아가려는 쪽에 같이** 놓는다 — ★ 8씩 도는 게 아니라 **곧바로** 튼다
//   · 속도 = 빠르기 × 그 쪽의 단위 벡터. **길 찾기를 안 쓴다**
//
// ⚠ 안 옮긴 갈래 둘
//   · **물칸(타일 6)을 만나면 90° 꺾기** — 타일 자료가 없다(6-e(2b) 의 `sub_50C830` 과 같은 자리)
//   · 깃발 0x400 이 있으면 그 물칸 검사를 통째로 건너뛴다 · 깃발 0x4000 의 속도 배율은 6-g
//   · `monsterMoveAudio` 는 소리라 안 옮긴다

uint8_t Server::AIRandomWalkDir(Creature& k) {
    const int jitter = static_cast<int>(NextRandom(41)) - 20;   // randomInt(-20, 20)
    return static_cast<uint8_t>((static_cast<int>(k.facing) + jitter) & 0xFF);
}

void Server::AIRandomWalk(Creature& k) {
    const uint8_t d = AIRandomWalkDir(k);
    k.facing = d;
    k.facingWant = d;                                          // ★ 둘을 같이 놓는다
    k.velX = k.speedCur * DirVecX(d);
    k.velY = k.speedCur * DirVecY(d);
}

// ── 묶음 6-g(2b) 때릴 거리를 정하고 사슬을 쌓는다 ─────────────────────
//
// ★★ 6-g(1) 에서 **싸움(15)이 근접(16)을 바로 올린다**고 적었는데 **틀렸다.**
//   싸움은 `nox_xxx_TODOsomeCallingMeleeAttack_531B40`(`legacy/GAME4_3.c:1623`)을 부르고,
//   그것이 **때릴 수 있나**를 보아 갈래를 고른다. 그리고 고른 갈래가
//   `nox_xxx_monsterAction_531C60`(1679행) 으로 **조건과 행동을 한 줄로 쌓는다**.
//
// ⭕ 쌓이는 차례 (아래 → 위) — `monsterAction_531C60`
//     60 새 적 없음 · 42 살아 있나 · [50 쏠 수 있으면 사거리×0.6 안인가] ·
//     45 보이나 · **16 근접 공격** · **26 그쪽 바라보기** · 49 **근접 사거리보다 머나** ·
//     [사람이면 65·40] · **7 그 자리로 가기**
//   ★ 읽는 법 — 맨 위가 **가기(7)** 이고 그 아래 **49(더 머나)** 가 조건이다.
//     즉 **근접 사거리보다 멀면 다가가고**, 닿으면 49 가 깨져 7 과 49 가 빠지며
//     **26(바라보기) → 16(때리기)** 이 드러난다. **이 한 줄이 「쫓아가 때린다」다**
//
// ⭕ 갈래 고르기(`531B40`) — 몬스터 기준
//     `monsterCanMelee_534220` = **근접 사거리(+112) > 0** · `monsterCanShoot_534280` = 몬스터표 **+148**
//   ⚠ **쏠 수 있나(+148)는 아직 못 읽는다** — thing.bin 그 칸을 안 파싱했다. **거짓으로 둔다**.
//     그래서 지금은 「근접이 되면 근접 사슬 · 아니면 `sub_531C10`」 두 갈래만 돈다
//   ⚠ 원거리 갈래(`sub_531D50`)와 주문 갈래(`monsterCanCast_534300`)는 **6-g(3)**
void Server::MonsterPushAttack(Creature& k, const Target& foe, float fx, float fy) {
    const MonsterStats* m = FindMonster(k.type);
    const double reach = (m && m->meleeRange > 0.0) ? m->meleeRange : 0.0;
    const bool canMelee = reach > 0.0;
    // ★ 2026-09-15 — **monster.bin 을 직접 읽어 채웠다**. `monsterCanShoot_534280` 은
    //   몬스터표 **+148(MISSILE_NAME)** 이 비었나만 본다
    const bool canShoot = (m && m->missileName != nullptr);
    const double shootRange = m ? m->missileRange : 0.0;

    auto push = [&](uint8_t act) {
        if (!AIPush(k, act)) return -1;
        k.aiStack[k.aiStackInd].targetAddr = foe.addr;
        k.aiStack[k.aiStackInd].targetPet = foe.pet;
        return k.aiStackInd;
    };

    // ── 갈래 고르기(`531B40`) ──
    //   쏠 수 있으면 — 근접도 되고 **쏘기 사거리의 절반보다 가까우면** 근접 사슬, 아니면 **원거리 사슬**
    if (canShoot) {
        const double ex = fx - k.x, ey = fy - k.y;
        const double dist = std::sqrt(ex * ex + ey * ey);
        if (!(canMelee && dist < shootRange * 0.5)) {
            // sub_531D50 — **원거리 사슬**(아래 → 위)
            //   60 새 적 없음 · 45 보이나 · **17 쏘기** · 26 바라보기 ·
            //   47 쏠 줄이 막혔나 · 49 **쏘기 사거리보다 머나** · 40 또는 · **7 가기**
            push(60);
            push(45);
            const int i17 = push(17);
            if (i17 >= 0) { k.aiStack[i17].argX = fx; k.aiStack[i17].argY = fy; }
            push(26);
            // ⚠ `sub_534710`(깃발 0x40 · 아래 행동 4)은 아직 안 옮겼다 — 늘 거짓으로 본다
            push(47);                                        // 쏠 줄이 막혔나
            const int i49r = push(49);
            if (i49r >= 0) k.aiStack[i49r].argF = static_cast<float>(shootRange);
            AIPush(k, 40);                                   // 또는 — 47 과 49 를 묶는다
            if (push(7) >= 0) {
                k.aiStack[k.aiStackInd].argX = fx;
                k.aiStack[k.aiStackInd].argY = fy;
            }
            return;
        }
    }
    if (!canMelee) {
        // sub_531C10 — 때릴 수단이 없으면 **일단 그 자리로 간다**
        push(60);                                            // 새 적 없음
        push(42);                                            // 살아 있나
        if (push(7) >= 0) {
            k.aiStack[k.aiStackInd].argX = fx;
            k.aiStack[k.aiStackInd].argY = fy;
        }
        return;
    }
    // monsterAction_531C60
    push(60);
    push(42);
    if (canShoot) {
        const int i = push(50);                              // 쏘기 사거리 × 0.6 안인가
        if (i >= 0) k.aiStack[i].argF = static_cast<float>(shootRange * 0.60000002);
    }
    push(45);                                                // 보이나
    AIPush(k, 16);                                           // 근접 공격 — 대상을 안 받는다(정답지도 그렇다)
    push(26);                                                // 그쪽 바라보기
    const int i49 = push(49);                                // **근접 사거리보다 머나**
    if (i49 >= 0) k.aiStack[i49].argF = static_cast<float>(reach);
    // ⚠ 사람이면 65(기력 기다림)·40(또는) 을 더 쌓는다 — 크리처에는 안 쌓는다
    if (push(7) >= 0) {
        k.aiStack[k.aiStackInd].argX = fx;
        k.aiStack[k.aiStackInd].argY = fy;
    }
}

// ── 묶음 6-g(3b) 크리처가 쏘는 것 ────────────────────────────────────
//
// 정답지 `mobActionMissileAtt_532610` 가 `newObjectByTypeID(monsterDef+148)` 로
// **`MISSILE_NAME` 오브젝트를 만들어** 그 쪽으로 속도를 준다.
//   · 피해는 날아가서 **맞을 때** 들어간다(`collideProjectileGeneric_4E87B0`, 종류 **11**)
//   · 그 함수는 **적·아군을 안 가린다** — 맞는 것에 그냥 준다
//   · 양은 `ImpShot` 이면 gamedata **`ImpShotDamage`**, 아니면 `COLLIDE` 의 인자다(둘 다 5)
// ⚠ **앞질러 겨눔**(`projAddVelocitySmth_533080`)은 아직 안 옮겼다 — 곧장 겨눈다
void Server::SpawnCreatureMissile(Creature& k, const char* name, float tx, float ty,
                                  std::vector<OutPacket>& out) {
    (void)out;
    const ProjTypeInfo* pt = ProjType(name);
    if (!pt || pt->type == 0) return;                        // 종류를 모르면 안 만든다(지어내지 않는다)
    const double dx = tx - k.x, dy = ty - k.y;
    const uint8_t dir = DirFromVec(dx, dy);
    MissileObj m;
    m.netCode = nextShotNetCode_++;
    m.type = pt->type;
    m.flags = pt->flags;
    m.posX = m.newX = m.prevX = static_cast<float>(k.x);
    m.posY = m.newY = m.prevY = static_cast<float>(k.y);
    m.dir1 = m.dir2 = dir;
    m.createdFrame = m.frame136 = frame_;
    m.speed = pt->speed;
    m.owner = k.owner;                                       // 점수는 부른 사람 것이다
    m.ud0 = m.ud2 = k.owner;
    m.collideKind = 3;                                       // ProjectileCollide
    m.fromPet = static_cast<int>(&k - pets_.data());         // 제 몸에 안 맞게
    m.collideAmount = pt->collideAmount;
    m.velX = DirVecX(dir) * m.speed;
    m.velY = DirVecY(dir) * m.speed;
    missiles_.push_back(m);
}

// ── 묶음 6-i 소환수 명령 ──────────────────────────────────────────────
//
// 정답지 `nox_xxx_orderUnit_533900`(`legacy/GAME4_3.c:2367`) +
//        `nox_xxx_enactUnitOrder_5339A0`(같은 파일 2394).
//
// ⭕ 명령 여섯 — **0 추방 · 1 관찰 · 2 가만히 · 3 지켜 · 4 따라와 · 5 사냥**
//   · 번호가 **0** 이면 **내 것 전부**에게 준다(정답지는 「고른 것」 깃발 0x80 을 본 뒤 준다)
//   · 2·3·4·5 는 **쌓임을 통째로 비우고** 그 행동 하나를 얹는다 —
//     그래서 하던 싸움이 끊긴다. 이것이 명령이 「선다」는 뜻이다
//   · 3·4·5 는 **못 움직이는 것에는 안 먹는다**(`monsterIsMoveing` 빠르기 >= 0.01)
//   · **사나움**(`Aggression` +1304) — 가만히·지켜 **0.5** · 따라와·사냥 **0.83**
//   · 지켜는 **보는 거리**(`SightRange` +1312)를 **250** 으로 놓고, 쏘는 것이면 뛰기 깃발을 켠다
//
// ⚠ 안 옮긴 것 — 추방(`banishUnit_5017F0`)·관찰(`playerObserveMonster_4DDE80`)은 묶음 6-j·10 ·
//   말풍선(`monsterCmdSend_528BD0`)과 소리 · 「고른 것」 깃발(0x80)
void Server::EnactUnitOrder(Session& src, Creature& k, uint8_t order) {
    if (k.health == 0) return;                               // 죽은 것에는 안 먹는다(깃발 0x8000)
    // `monsterIsMoveing_534320` 그대로 — **지금 빠르기**만 본다(종류표로 눅이지 않는다)
    const bool canMove = k.speedCur >= 0.0099999998f;
    switch (order) {
        case 2:                                              // 가만히
            k.aggression = 0.5f;
            k.running = false;
            AIClearStack(k);
            AIPush(k, 0);
            k.ordered = true;
            break;
        case 3: {                                            // 지켜 — **선 자리를 지킨다**
            if (!canMove) break;
            k.aggression = 0.5f;
            const MonsterStats* m = FindMonster(k.type);
            if (m && m->missileName != nullptr) k.running = true;   // 쏘는 것이면 뛰기 깃발
            k.sightRange = 250.0f;
            AIClearStack(k);
            if (AIPush(k, 4)) {
                k.aiStack[k.aiStackInd].argX = static_cast<float>(k.x);
                k.aiStack[k.aiStackInd].argY = static_cast<float>(k.y);
                k.aiStack[k.aiStackInd].argU32 = k.facing;
            }
            k.ordered = true;
            break;
        }
        case 4:                                              // 따라와
            if (!canMove) break;
            k.aggression = 0.83f;
            k.running = false;
            AIClearStack(k);
            if (AIPush(k, 3)) {
                k.aiStack[k.aiStackInd].argX = static_cast<float>(src.posX);
                k.aiStack[k.aiStackInd].argY = static_cast<float>(src.posY);
                k.aiStack[k.aiStackInd].targetAddr = src.addr;
            }
            k.ordered = true;
            break;
        case 5:                                              // 사냥
            if (!canMove) break;
            k.aggression = 0.83f;
            k.running = false;
            AIClearStack(k);
            AIPush(k, 5);
            k.ordered = true;
            break;
        default: break;                                      // 0 추방 · 1 관찰 — 아직 안 옮겼다
    }
}

void Server::AIClearStack(Creature& k) {
    for (int i = 0; i < Creature::kAIStackMax; ++i) k.aiStack[i] = Creature::AIStackItem{};
    k.aiStackInd = 0;
    k.path.clear();
    k.velX = 0; k.velY = 0;
    aiStackChanged_ = true;
}

void Server::OrderUnit(Session& src, uint16_t netCode, uint8_t order) {
    if (netCode != 0) {
        for (auto& k : pets_)
            if (k.netCode == netCode && k.owner == src.addr) { EnactUnitOrder(src, k, order); return; }
        return;                                              // 남의 것·없는 것에는 안 먹는다
    }
    for (auto& k : pets_)                                    // 번호 0 = 내 것 전부
        if (k.owner == src.addr) EnactUnitOrder(src, k, order);
}

// ── 묶음 6-j(2) 길목 찾기 ─────────────────────────────────────────────
//
// ★★ **앞 기록 정정** — 6-i(3) 에 「우리 맵 자료에 길목이 없다」고 적었는데 **틀렸다**.
//   길목은 `main.cpp:462` 에서 이미 읽어 `SetWaypoints` 로 서버에 실린다.
//   없었던 것은 **고르는 함수**였다.
//
// ⭕ 정답지 `Sub50AFA0`(`server/object_ai_path.go:596`)이 격자에 길목 칸을 **깃발 0x40** 으로 찍고,
//   `Sub_50CB20`(`src/unit_ai_path.go:336`)이 **거기서부터 물결로 훑어** 가장 먼저 닿는
//   길목 칸을 돌려준다. 이웃은 blob `0x587000+234284` 의 **직교 넷**이다(대각 없음).
//
// ⚠ `sub_50B870`(벽 모서리)·`sub_50C830`(물칸)은 아직 안 옮겨 **지날 수 있는 것으로** 본다
// ⚠ 우리 `waypoints_` 는 점멸(4-e)이 쓰려고 `flags2 & 0x80` 에 더해 `flags & 1`(켜짐)까지 걸러 담았다.
//   정답지의 격자 찍기는 **0x80 만** 본다 — 꺼진 길목이 있으면 우리가 덜 찍는다
void Server::AIPathBakeWaypoints() {
    for (const auto& w : waypoints_) {
        const int gx = static_cast<int>(w.first / kWallCell);
        const int gy = static_cast<int>(w.second / kWallCell);
        if (AIIndexNode* n = AIPathIndexAt(gx, gy)) n->flags |= 0x40;
    }
}

bool Server::AIPathFindWaypoint(const Creature& k, float& outX, float& outY) {
    if (waypoints_.empty()) return false;
    ++aiIndexLast_;
    AIPathResetVisit();
    const int sx = static_cast<int>(k.x / kWallCell);
    const int sy = static_cast<int>(k.y / kWallCell);
    int wave = AIPathNewVisit(sx, sy, -1, -1);
    if (wave < 0) return false;
    if (AIIndexNode* rp = AIPathIndexAt(sx, sy)) rp->visit = aiIndexLast_;
    static const int kNb4[4][2] = { { 1, 0 }, { 0, -1 }, { -1, 0 }, { 0, 1 } };
    while (wave >= 0) {
        int next = -1;
        for (int it = wave; it >= 0; it = aiVisit_[static_cast<size_t>(it)].next) {
            const int x1 = aiVisit_[static_cast<size_t>(it)].x;
            const int y1 = aiVisit_[static_cast<size_t>(it)].y;
            if (AIPathFlagsAt(x1, y1) & 0x40) {              // 길목 칸이다
                outX = x1 * 23.0f + 11.0f;
                outY = y1 * 23.0f + 11.0f;
                return true;
            }
            for (int i = 0; i < 4; ++i) {
                const int x2 = x1 + kNb4[i][0], y2 = y1 + kNb4[i][1];
                AIIndexNode* p = AIPathIndexAt(x2, y2);
                if (!p || p->visit == aiIndexLast_) continue;
                p->visit = aiIndexLast_;
                // ★ 정답지의 막힘 관문은 `sub_50B870`(벽 모서리 번호)·`sub_50C830`(물칸)인데
                //   둘 다 아직 안 옮겼다. **우리가 가진 벽 깃발**로 대신 막는다 —
                //   안 막으면 물결이 벽을 넘어가 **담 밖 길목까지 찾아낸다**(시험이 잡았다)
                if (WallDirAt(x2, y2) != -1) continue;       // sub_50B870 — 벽
                const int nn = AIPathNewVisit(x2, y2, it, next);
                if (nn < 0) return false;                    // 자국 자리를 다 썼다
                next = nn;
            }
        }
        wave = next;
    }
    return false;
}

// ── 묶음 6-e(2b) 대각 이음매 ──────────────────────────────────────────
//
// 정답지 `Sub_57B500`(`server/wall.go:839`) — 그 칸 벽의 **방향 번호**(0~10)를 준다.
//   벽이 없거나 격자 밖이면 **−1**. 창·비밀벽·못 여는 문도 −1 이다
//   ⚠ 문·창·비밀벽 깃발(`Flags4`)은 아직 없다(묶음 9) — 지금은 **벽이 있나/없나**로만 줄어든다
int Server::WallDirAt(int x, int y) const {
    if (x < 0 || x >= 256 || y < 0 || y >= 256) return -1;
    if (wallTiles_.empty()) return -1;
    const WallTile& w = wallTiles_[static_cast<size_t>(y) * 256 + static_cast<size_t>(x)];
    return w.present ? static_cast<int>(w.dir) : -1;
}

// 길 훑기 cases 4~7 — **대각으로 갈 때 두 옆칸의 벽 방향이 그 대각을 막는가**.
//   정답지 `nox_xxx_pathFind_50BA00`(`src/unit_ai_path.go:135~200`) 그대로.
//   ★ 번호 집합이 방향마다 다르다 — 벽은 **칸 대각선 선분**이라 어느 대각을 막는지가 방향에 달렸다
bool Server::AIPathDiagOk(int ii, int x1, int y1) const {
    auto in = [](int v, int a, int b, int c, int d) {
        return v == -1 || v == a || v == b || v == c || v == d;
    };
    switch (ii) {
        case 4: {   // (+1,+1)
            const int a = WallDirAt(x1, y1 + 1);
            if (!in(a, 1, 6, 10, 9)) return false;
            const int b = WallDirAt(x1 + 1, y1);
            if (!in(b, 1, 4, 7, 8)) return false;
            return true;
        }
        case 5: {   // (−1,−1)
            const int a = WallDirAt(x1 - 1, y1);
            if (!in(a, 1, 6, 10, 9)) return false;
            const int b = WallDirAt(x1, y1 - 1);
            if (!in(b, 1, 4, 7, 8)) return false;
            return true;
        }
        case 6: {   // (−1,+1)
            const int a = WallDirAt(x1, y1 + 1);
            if (!in(a, 0, 5, 9, 8)) return false;
            const int b = WallDirAt(x1 - 1, y1);
            if (!in(b, 0, 3, 10, 7)) return false;
            return true;
        }
        case 7: {   // (+1,−1)
            const int a = WallDirAt(x1 + 1, y1);
            if (!in(a, 0, 5, 9, 8)) return false;
            const int b = WallDirAt(x1, y1 - 1);
            if (!in(b, 0, 3, 10, 7)) return false;
            return true;
        }
        default: return true;
    }
}

// sub_547DB0 — **점이 그 몸 안에 드는가**. 원은 거리제곱, 상자는 **마름모 네 변**이다.
//   (묶음 4-a2 에서 날아가는 것 닿음 판정에 인라인으로 있던 것을 함수로 뽑았다)
bool Server::BodyContains(const PhysBody& o, float qx, float qy) const {
    if (o.shape == 2) {
        const float dx = o.x - qx, dy = o.y - qy;
        return dy * dy + dx * dx <= o.r * o.r;
    }
    if (o.shape == 3) {
        const float v5 = o.boxLT + o.x, v6 = o.boxLB + o.y;
        if ((v6 - v5 + qx - qy) * 0.70709997f < 0.0f &&
            (o.boxRB + o.y - (o.boxRT + o.x) + qx - qy) * 0.70709997f > 0.0f) {
            const float v7 = o.boxRT + o.x, v8 = o.boxRB + o.y;
            return (v8 + v7 - qx - qy) * 0.70709997f > 0.0f && (v6 + v5 - qx - qy) * 0.70709997f < 0.0f;
        }
    }
    return false;
}

// ── 묶음 6-e(2b·3) 오브젝트 모양 찍기 ────────────────────────────────
//
// 정답지 `Sub50AFA0`(`server/object_ai_path.go:545~580`) — 클래스 **0x400000** 인 것의 몸이
// **걸치는 칸마다** 깃발 **0x1** 을, 오브젝트 깃발 **0x10 이 없으면 0x2** 도 찍는다.
//
// ★ 어떻게 「걸친다」를 보나 — 칸 안의 **아홉 점**(blob `0x587000+234108`,
//   {2.3 · 11.5 · 20.7} 의 3×3)을 차례로 눌러 보아 **하나라도 몸 안에 들면** 그 칸을 찍는다.
//   ⚠ 그래서 **칸보다 작은 것은 칸 한가운데를 비켜 서면 안 찍힐 수 있다** — 정답지 그대로다.
void Server::AIPathBakeObstacles() {
    static const float kS[3] = { 2.3f, 11.5f, 20.7f };
    for (const auto& o : obstacles_) {
        if (!o.hasCollide) continue;
        double ox0, oy0, ox1, oy1;
        BodyRect(o, o.x, o.y, ox0, oy0, ox1, oy1);
        const int gx0 = static_cast<int>(ox0 / kWallCell), gx1 = static_cast<int>(ox1 / kWallCell);
        const int gy0 = static_cast<int>(oy0 / kWallCell), gy1 = static_cast<int>(oy1 / kWallCell);
        for (int gy = gy0; gy <= gy1; ++gy)
            for (int gx = gx0; gx <= gx1; ++gx) {
                AIIndexNode* n = AIPathIndexAt(gx, gy);
                if (!n) continue;
                bool hit = false;
                for (int a = 0; a < 3 && !hit; ++a)
                    for (int b = 0; b < 3 && !hit; ++b)
                        if (BodyContains(o, gx * 23.0f + kS[b], gy * 23.0f + kS[a])) hit = true;
                if (!hit) continue;
                n->flags |= 0x1;
                if (!(o.flags & 0x10)) n->flags |= 0x2;
            }
    }
}

// ── 묶음 7-a 경험치와 레벨 ────────────────────────────────────────────
//
// 정답지 `NetReportExperience`(`server/network.go:724` = `sub_4D81A0`) ·
//        `nox_xxx_plyrGiveExp_4EF3A0`·`sub_4EF2E0_exp_level`(`legacy/server__gamemech__explevel.c`).
//
// ⭕ 와이어 — `6e <경험치 u32>` **5바이트** · `NetSendPacketXxx0(…, 0, 1)` 이라 **중요**.
//   ★ 경험치는 오브젝트 안에서 **float** 인데 와이어에는 **u32 로 잘라** 싣는다
//   ★ **순번(`cc`)은 안 붙는다.** `NetSendPacketXxx0` 은 여섯째 인자를 **0** 으로 넘기고
//     (`server/network.go:101`), `netImportant_4E5770` 이 그 자리(+184)를 보고 갈라
//     0 이면 `27 <시각> aa` 봉투에 **본문 그대로**, 1 이면 `cc <순번> <길이>` 를 씌운다(`GAME3_3.c:2024`).
//     그래서 우리도 `EmitSeq` 가 아니라 `EmitImportantRaw` 다 — 확인(`ab`)을 안 기다리고,
//     다시 실려도 경험치는 **더하기가 아니라 절댓값**이라 같은 값이 한 번 더 들어올 뿐이다
//
// ⭕ 레벨 — `XPTable` 의 **다음 칸 값 이하**로 경험치가 차면 한 단계 오른다.
//   ⚠ **한 번에 한 단계만** 오른다(정답지가 `while` 이 아니라 `if` 다) — 크게 한 번 받아도 한 칸이다
//   ⚠ 천장은 **10**(`legacy/defs.h:26`)
//   ⚠ 칸 번호는 **0부터**고 gamedata.bin 이 스스로 `First entry is for level "0"` 이라 적어 두었다.
//     그런데 코드가 보는 것은 `XPTable[레벨 + 1]` 이라, **레벨 1 이 다음으로 가려면 셋째 칸(18200)** 이다.
//     ⭕ 표의 뜻과 코드가 한 칸 어긋나 보이지만 **코드 쪽을 그대로 옮긴다**(`sub_4EF2E0` 원문)
//
// ⛔ 정답지가 더 하는데 **여기서는 안 옮기는 것** 넷 —
//   ① `sub_56FA40`·`sub_56F980` — 경험치·레벨의 **XOR 그림자 사본**(`GAME5_2.c:616`). 메모리 조작 잡는 장치라 서버 규칙이 아니다
//   ② `gameGet_4DB1B0() && sub_4DB1C0()` 관문 — **혼자 하기 맵 갈아타는 중**이면 레벨을 안 올린다.
//      두 값은 글자 고르는 화면이 세운다(`gui_selchar.go:1000`) — 붙어 있는 서버엔 없는 상태라 관문이 늘 열려 있다
//   ③ 글줄 "health.c:gainpoints"·"LevelUP" — 문자열표를 타는 글이라 묶음 10 으로
//   ④ 소리 902 — 묶음 10 으로
//
// ⚠ **죽여서 받는 경험치는 없다.** `plyrGiveExp` 를 부르는 곳은 **스크립트**(`builtin.c:355`)뿐이다 —
//   퀘스트가 주는 것이지 몬스터를 잡아서 저절로 오르지 않는다. 그래서 여기서는 **주는 길만** 옮긴다
void Server::SendExperience(Session& s, std::vector<OutPacket>& out) {
    const uint32_t xp = static_cast<uint32_t>(s.experience);
    uint8_t msg[5];
    msg[0] = static_cast<uint8_t>(Op::REPORT_EXPERIENCE);
    msg[1] = static_cast<uint8_t>(xp);
    msg[2] = static_cast<uint8_t>(xp >> 8);
    msg[3] = static_cast<uint8_t>(xp >> 16);
    msg[4] = static_cast<uint8_t>(xp >> 24);
    EmitImportantRaw(s, msg, sizeof(msg), out);
}

// sub_4EF2E0_exp_level — 한 단계 올릴 수 있으면 올린다(**한 단계만**)
bool Server::PlayerCheckLevel(Session& s) {
    if (s.level >= kPlayerMaxLevel) return false;
    const size_t next = static_cast<size_t>(s.level) + 1;
    if (next >= sizeof(kXPTable) / sizeof(kXPTable[0])) return false;
    if (kXPTable[next] > s.experience) return false;
    ++s.level;
    // `plrReadVals_4EEDC0(a1, 1)` — 딸린 값 다시 셈.
    //   ⚠ 우리 온라인 값은 **직업으로 고정**이라(접속 때 `HealthOnline` 따위) 레벨로 바뀌는 것이 없다.
    //     그래서 다시 셀 것이 없고 **「바뀌었다」 표시만** 세운다(스냅샷 꼬리가 싣고 끈다)
    s.statsDirty = true;
    return true;
}

void Server::PlayerGiveExp(Session& s, float amount, std::vector<OutPacket>& out) {
    s.experience += amount;
    SendExperience(s, out);
    PlayerCheckLevel(s);
}

// ── 묶음 7-b 주문 받기 ────────────────────────────────────────────────
//
// 정답지 `nox_xxx_spellGrantToPlayer_4FB550`(`legacy/server__magic__plyrspel.c:46`) ·
//        `nox_xxx_netSendSpellAward_4D7F90`(`GAME3_2.c`).
//
// ⭕ 와이어 — `6f <주문 번호> <그 주문의 지금 레벨> <알림 | 0x80>` **4바이트**.
//   ★ `netSendPacket1_4E5390(…, 0, 1)` — **Packet1** 이라 여섯째 인자가 **1**,
//     즉 `cc <순번> <길이>` **순번 봉투**에 들어간다. 경험치(`6e`)와 반대다 —
//     주문 레벨은 **놓치면 안 되는 상태**라 확인(`ab`)을 받아야 한다
//   ★ 셋째 칸은 **보낼 때 읽은 레벨**이다(올린 **뒤**). 클라가 그 값을 그대로 제 표에 넣는다
//     (`netSpellRewardCli_45CFE0` — `SpellLvl[번호] = a2`)
//   ★ 넷째 칸 — 아래 7비트가 `알림`(0 이면 조용히), **0x80** 이 넷째 인자다.
//     클라는 `& 0x80` 을 따로 떼어 넘긴다(`cdecode.c:1551~1559`)
//
// ⭕ 올리는 규칙
//   · 주문 번호는 **1~136**. 벗어나면 "AwardSpellError" 글줄만 보내고 실패
//   · **천장** — 협동(2048)·퀘스트(4096)면 **3**, 그 밖은 **5**. 이미 천장이면 "MaxSpellLevel" 글줄만
//   · 한 칸 올리고(`++`) 천장으로 자른다 · `forceLevel` 이 있으면 **그 값으로 덮는다**
//   · **무리 주문** — 깃발 `0x1000`/`0x4000`/`0x10000` 을 가진 주문을 받으면
//     짝이 되는 `0x2000`/`0x8000`/`0x20000` 을 가진 **성한 주문 전부**가 같이 오른다
//     (『보호 무리』처럼 한 장에 여러 개가 묶인 것)
//   · 알림이면 소리 226 과 보상 알림을 **나와 남들 모두**에게 → 둘 다 묶음 10(소리)·미이식
//
// ⚠ 우리 서버는 **온라인 아레나**라 협동·퀘스트 깃발이 꺼져 있다 ⇒ 천장은 늘 **5**.
//   퀘스트 전용 「한 번만 받는 주문 17종」 관문도 같은 이유로 안 탄다(그래도 옮겨 둔다)
void Server::SendSpellAward(Session& s, uint8_t spell, uint8_t announce, bool extra,
                            std::vector<OutPacket>& out) {
    uint8_t msg[4];
    msg[0] = static_cast<uint8_t>(Op::REPORT_SPELL_AWARD);
    msg[1] = spell;
    msg[2] = s.spellLvl[spell];                          // 올린 **뒤**의 레벨
    msg[3] = static_cast<uint8_t>(extra ? (announce | 0x80) : announce);
    EmitSeq(s, msg, sizeof(msg), out);                   // Packet1 = 순번 봉투
}

// 퀘스트에서 **한 번만** 받는 주문들(`plyrspel.c:80~82`) — 이미 1 이상이면 더 안 준다
static bool SpellQuestOnce(uint8_t sp) {
    switch (sp) {
        case 34: case 134: case 45: case 46: case 47: case 48: case 49:
        case 117: case 118: case 119: case 120: case 121: case 122:
        case 123: case 124: case 125: case 19:
            return true;
        default: return false;
    }
}

bool Server::SpellGrantToPlayer(Session& s, uint8_t spell, uint8_t announce, bool extra,
                                uint8_t forceLevel, std::vector<OutPacket>& out) {
    if (spell == 0 || spell >= 137) return false;        // "AwardSpellError"(글줄은 묶음 10)
    const bool coopOrQuest = questTotal_ > 0 || questStage_ > 0;   // 2048·4096 자리
    const uint8_t cap = coopOrQuest ? 3 : 5;
    if (s.spellLvl[spell] == cap) return false;          // "MaxSpellLevel"
    // 퀘스트면 목록의 17종은 한 번만
    if (coopOrQuest && SpellQuestOnce(spell) && s.spellLvl[spell] != 0) return false;

    ++s.spellLvl[spell];
    if (s.spellLvl[spell] > 5) s.spellLvl[spell] = 5;
    if (coopOrQuest && s.spellLvl[spell] > 3) s.spellLvl[spell] = 3;
    if (forceLevel) s.spellLvl[spell] = forceLevel;

    // 무리 주문 — 이끄는 깃발이 있으면 딸린 깃발을 가진 성한 주문을 모두 올린다
    uint32_t member = 0;
    const uint32_t fl = SpellFlags(spell);
    if (fl & 0x1000)       member = 0x2000;
    else if (fl & 0x4000)  member = 0x8000;
    else if (fl & 0x10000) member = 0x20000;
    if (member) {
        for (int i = 1; i < 137; ++i) {
            if (!(SpellFlags(static_cast<uint8_t>(i)) & member)) continue;
            if (!spellDefs_[i].valid) continue;
            if (forceLevel) s.spellLvl[i] = forceLevel;
            else ++s.spellLvl[i];
            if (s.spellLvl[i] > 5) s.spellLvl[i] = 5;
            if (coopOrQuest && s.spellLvl[i] > 3) s.spellLvl[i] = 3;
        }
    }
    SendSpellAward(s, spell, announce, extra, out);
    return true;
}

// ── 묶음 7-c 점수와 죽은 횟수 ─────────────────────────────────────────
//
// 정답지 `nox_xxx_netReportLesson_4D8EF0`(`GAME3_2.c:4471`) = `NetReportLesson`(`server/network.go:369`) ·
//        `nox_xxx_playerUpdateScore_54D980`(`GAME5.c:5221`) · `changeScore_4D8E90` ·
//        `playerSubLessons_4D8EC0` · `playerIncrementElimDeath_4D8D40`.
//
// ⭕ 와이어 — `4e <넷코드 u16> <점수 u32> <죽은 횟수 u32>` **11바이트**.
//   ★ `netSendPacket1_4E5390(**255**, …, 0, 1)` — 첫 인자 255 라 **모두에게** 가고,
//     Packet**1** 이라 `cc` **순번 봉투**에 든다. 남의 점수도 내 화면에 떠야 하므로 그렇다
//   ★ 「Lessons」는 이름만 수업이고 실은 **점수**다(`ns.Player GetScore/ChangeScore` 가 이 자리를 읽고 쓴다).
//     둘째 값 +2140 은 **죽은 횟수**(`playerIncrementElimDeath_4D8D40`)
//
// ⭕ 아레나 셈법 — 판 갈래로 셈이 갈린다(`GAME5.c:5078~5085`):
//     **256 아레나 → `playerUpdateScore_54D980`**(우리 것) · 16 왕 → Kotr · 1024 탈락전 → ElimDeath
//   `playerUpdateScore(죽은이, 죽인이, 도운이, a4)` 의 갈래 —
//     · 죽인이가 **자기 자신**이거나 **아무도 없으면** → 죽은이 점수 **−1**. ⚠ **죽은 횟수는 안 올린다**
//     · 죽인이가 **사람이 아니면**(몬스터·함정) → 죽은 횟수 **+1** 만. 점수는 그대로
//     · 죽인이가 **사람**이고 **같은 편**이면 → 죽인이 점수 **−1** · 죽은 횟수 +1
//     · 그 밖(남) → 죽인이 점수 **+1** · 죽은 횟수 +1
//     · **도운이**가 죽인이와도 죽은이와도 편이 다르면 → 도운이 점수 **+1**(`LABEL_36`)
//   바뀐 사람마다 `4e` 를 **따로** 보낸다(한 번 죽으면 최대 세 통 — 죽인이·죽은이·도운이)
void Server::SendLesson(Session& s, std::vector<OutPacket>& out) {
    uint8_t msg[11];
    msg[0] = static_cast<uint8_t>(Op::REPORT_LESSON);
    msg[1] = static_cast<uint8_t>(s.netCode);
    msg[2] = static_cast<uint8_t>(s.netCode >> 8);
    const uint32_t sc = static_cast<uint32_t>(s.score);
    msg[3] = static_cast<uint8_t>(sc);
    msg[4] = static_cast<uint8_t>(sc >> 8);
    msg[5] = static_cast<uint8_t>(sc >> 16);
    msg[6] = static_cast<uint8_t>(sc >> 24);
    msg[7] = static_cast<uint8_t>(s.deaths);
    msg[8] = static_cast<uint8_t>(s.deaths >> 8);
    msg[9] = static_cast<uint8_t>(s.deaths >> 16);
    msg[10] = static_cast<uint8_t>(s.deaths >> 24);
    // 첫 인자 255 = 모두에게 · Packet1 = 순번 봉투
    for (auto& kv : sessions_) {
        if (kv.second.inGame) EmitSeq(kv.second, msg, sizeof(msg), out);
    }
}

void Server::ChangeScore(Session& s, int32_t delta, std::vector<OutPacket>& out) {
    s.score += delta;                                      // +2136 (`changeScore_4D8E90` / `playerSubLessons_4D8EC0`)
    SendLesson(s, out);
}

void Server::IncrementDeaths(Session& s, std::vector<OutPacket>& out) {
    ++s.deaths;                                            // +2140 (`playerIncrementElimDeath_4D8D40`)
    SendLesson(s, out);
}

// `playerUpdateScore_54D980` — 아레나(깃발 256)의 죽음 셈
void Server::PlayerUpdateScore(Session& victim, std::vector<OutPacket>& out) {
    Session* killer = nullptr;
    if (!victim.lastHit.player.empty()) {
        auto k = sessions_.find(victim.lastHit.player);
        if (k != sessions_.end() && k->second.inGame) killer = &k->second;
    }
    Session* assist = nullptr;
    if (victim.assistSet && frame_ - victim.assistFrame < 10u * kServerTickRate) {
        auto h = sessions_.find(victim.assistBy);
        if (h != sessions_.end() && h->second.inGame &&
            h->second.addr != victim.lastHit.player && h->second.addr != victim.addr) assist = &h->second;
    }
    // 죽인 것이 사람이 아니었나 — +520 사슬이 몬스터면 `!(flags & 4)` 자리다
    const bool byNonPlayer = killer == nullptr &&
                             (victim.lastHit.cls == 2 || (victim.lastHit.cls == 3 && victim.lastHit.ownerCls == 2));

    if (killer == &victim || (killer == nullptr && !byNonPlayer && assist == nullptr)) {
        ChangeScore(victim, -1, out);                      // LABEL_31 — 죽은 횟수는 **안** 올린다
        return;
    }
    if (killer != nullptr) {
        const bool sameTeam = victim.team != 0 && killer->team == victim.team;
        ChangeScore(*killer, sameTeam ? -1 : +1, out);
    }
    IncrementDeaths(victim, out);
    // LABEL_36 — 도운 사람은 죽인이·죽은이와 **편이 다를 때만** 받는다
    if (assist != nullptr) {
        const bool sameAsKiller = killer != nullptr && killer->team != 0 && assist->team == killer->team;
        const bool sameAsVictim = victim.team != 0 && assist->team == victim.team;
        if (!sameAsKiller && !sameAsVictim) ChangeScore(*assist, +1, out);
    }
}

// ── 묶음 7-d 능력 ─────────────────────────────────────────────────────
//
// 정답지 `serverAbilities`(`src/spells_abil.go:135~394`) ·
//        `nox_xxx_abilityRewardServ_4FB9C0`·`nox_xxx_netAbilityReport_4D8060`(`legacy`).
//
// ⭕ 와이어 넷 — 앞 셋은 **본인에게만**.
//   · `cd <능력 번호> <레벨 | 0x80(알림)>` **3바이트** — 받았다. `netSendPacket1` 이라 **순번 봉투**
//   · `ce <능력 번호> <상태>` **3바이트** — 쉬는 시간 상태. `NetSendPacketXxx0` 이라 **순번 없는 중요**
//   · `cf <능력 번호> <켜짐 0/1>` **3바이트** — 지속이 끝났다. 같이 순번 없는 중요
//   · `eb <능력 번호>` **2바이트** — 되돌림. **6 이면 전부**. 같이 순번 없는 중요
//   ★ 받았다(`cd`)만 순번이 붙는다 — **놓치면 능력이 사라지는** 값이라서다.
//     쉬는 시간·켜짐은 다음 틱이면 또 오므로 맨몸으로 간다
//
// ⭕ 능력 레벨은 **주문 레벨 표의 1~5 칸**이다(`SpellLvl[번호]`) — 따로 두지 않는다.
//   받으면 **한 번에 5** 로 올라간다(`*v6 = 5`) · 이미 있으면 "use.c:HadAbility" 만 보내고 실패
//
// ⭕ 쓰기(`Do`)의 관문 차례 — 하나라도 걸리면 그 자리에서 끝난다.
//   ① 번호 1~5 · 죽었거나 사라졌으면 · 사람이 아니면 · **워리어가 아니면** 조용히 끝
//   ② **같이 못 켜는 짝** — 광전사↔함성·작살 / 함성↔광전사·작살 / 작살↔함성·광전사 → 알림(2,2)
//   ③ 그 능력이 **이미 켜져 있으면** → 알림(2,2)
//   ④ 상태 12 이거나 (협동이 아닌데) **떠 있으면** → 알림(2,6)
//   ⑤ (온라인이 아니거나 퀘스트면) 레벨 0 이면 → 알림(2,3). ⚠ 우리는 늘 온라인이라 안 탄다
//   ⑥ 광전사인데 **짐이 넘치면**(+3656) → 알림(2,7)
//   ⑦ **쉬는 시간이 남았으면** → 알림(2,2)
//   그 뒤 쉬는 시간을 걸고(`ce` 상태 0), 지속이 0보다 크면 지속 목록에 얹고, 효과를 낸다
//
// ⛔ 효과 다섯(`do()` — 광전사·함성·작살·살금살금·늑대의 눈)은 **다음 조각(7-e)**.
//   소리(226 따위)와 문자열 글줄은 묶음 10.
void Server::SendAbilityAward(Session& s, uint8_t abil, bool announce, std::vector<OutPacket>& out) {
    uint8_t msg[3];
    msg[0] = static_cast<uint8_t>(Op::REPORT_ABILITY_AWARD);
    msg[1] = abil;
    msg[2] = static_cast<uint8_t>(announce ? (s.spellLvl[abil] | 0x80) : s.spellLvl[abil]);
    EmitSeq(s, msg, sizeof(msg), out);                   // Packet1 = 순번 봉투
}

void Server::SendAbilityState(Session& s, uint8_t abil, uint8_t state, std::vector<OutPacket>& out) {
    const uint8_t msg[3] = { static_cast<uint8_t>(Op::REPORT_ABILITY_STATE), abil, state };
    EmitImportantRaw(s, msg, sizeof(msg), out);
}

void Server::SendAbilityActive(Session& s, uint8_t abil, bool active, std::vector<OutPacket>& out) {
    const uint8_t msg[3] = { static_cast<uint8_t>(Op::REPORT_ACTIVE_ABILITIES), abil,
                             static_cast<uint8_t>(active ? 1 : 0) };
    EmitImportantRaw(s, msg, sizeof(msg), out);
}

void Server::SendAbilityReset(Session& s, uint8_t abil, std::vector<OutPacket>& out) {
    const uint8_t msg[2] = { static_cast<uint8_t>(Op::RESET_ABILITIES), abil };
    EmitImportantRaw(s, msg, sizeof(msg), out);
}

// `nox_xxx_abilityRewardServ_4FB9C0` — 능력 하나를 준다
bool Server::AbilityAward(Session& s, uint8_t abil, bool announce, std::vector<OutPacket>& out) {
    if (abil == 0 || abil >= 6) return false;            // "AwardAbilityError"(글줄은 묶음 10)
    if (s.spellLvl[abil] != 0) return false;             // "use.c:HadAbility"
    s.spellLvl[abil] = 5;                                // `*v6 = 5` — 한 번에 천장까지
    SendAbilityAward(s, abil, announce, out);
    return true;
}

bool Server::AbilityIsActive(const Session& s, uint8_t abil) const {
    return abil < kAbilityCount && s.abilActive[abil];
}

// `serverAbilities::Do` — 능력을 쓴다
bool Server::AbilityDo(Session& s, uint8_t abil, std::vector<OutPacket>& out) {
    if (abil == 0 || abil >= 6) return false;
    if (s.respawnAtFrame != 0) return false;             // 죽었거나 사라진 것
    if (s.charClass != CharClass::Warrior) return false; // 워리어만
    // ② 같이 못 켜는 짝 — 광전사·함성·작살 셋은 서로를 막는다
    //   ⚠ 정답지는 `IsActiveVal`(켜짐 값까지) 과 `IsActive`(있기만) 를 섞어 쓰는데,
    //     목록에 얹는 자리가 늘 `Active: 1` 이라 **둘이 같다**. 그래서 하나로 본다
    const bool exclusive = (abil == 1 || abil == 2 || abil == 3);
    if (exclusive) {
        for (uint8_t o = 1; o <= 3; ++o)
            if (o != abil && AbilityIsActive(s, o)) { InformSpell(s, 2, 2, out); return false; }
    }
    if (AbilityIsActive(s, abil)) { InformSpell(s, 2, 2, out); return false; }   // ③
    // ④ 상태 12 이거나 (협동이 아닌데) **떠 있으면**
    //   ⚠ 떠 있는 것(`FlagAirborne`)은 **사람에게는 아직 안 올겼다** —
    //     날아가는 것의 깃발이지 사람 깃발이 아니다. 사람을 띄우는 길(작살 끌림 따위)을 옮길 때 같이 넣는다
    if (s.moveState == 12) { InformSpell(s, 2, 6, out); return false; }
    // ⑤ (온라인이 아니거나 퀘스트면) 레벨 0 은 못 쓴다 — ⚠ 우리는 늘 온라인이라 이 관문을 안 탄다
    const bool quest = questTotal_ > 0 || questStage_ > 0;
    if (quest && s.spellLvl[abil] == 0) { InformSpell(s, 2, 3, out); return false; }
    if (abil == 1 && s.overweight) { InformSpell(s, 2, 7, out); return false; }   // ⑥ 짐 넘침(+3656)
    if (s.abilCooldown[abil] != 0) { InformSpell(s, 2, 2, out); return false; }   // ⑦

    s.abilCooldown[abil] = kAbilityDelay[abil];
    if (kAbilityDelay[abil] != 0) SendAbilityState(s, abil, 0, out);
    if (kAbilityDuration[abil] > 0) {
        s.abilActive[abil] = true;
        s.abilActiveUntil[abil] = frame_ + kAbilityDuration[abil];
    }
    AbilityEffect(s, abil, out);                          // 7-e — `do()`. ⛔ 소리는 묶음 10
    return true;
}

// `serverAbilities::Update` — 틱마다 쉬는 시간을 줄이고, 지속이 다한 것을 끈다
void Server::AbilityUpdate(Session& s, std::vector<OutPacket>& out) {
    for (uint8_t i = 1; i < kAbilityCount; ++i) {
        if (s.abilCooldown[i] == 0) continue;
        if (--s.abilCooldown[i] == 0) SendAbilityState(s, i, 1, out);   // 다 쉬었다
    }
    const bool gone = s.respawnAtFrame != 0;
    for (uint8_t i = 1; i < kAbilityCount; ++i) {
        if (!s.abilActive[i]) continue;
        // ⚠ 죽었거나 사라졌으면 **알리지 않고** 목록에서만 뺀다(정답지 `if (!obj.Flags().HasAny(...))`)
        if (!gone) {
            if (frame_ <= s.abilActiveUntil[i]) continue;               // 아직 남았다
            SendAbilityActive(s, i, false, out);
            if (i == 1) PlayerSetState(s, 13, frame_);                  // 광전사는 끝나면 상태 13
        }
        s.abilActive[i] = false;
        s.abilActiveUntil[i] = 0;
    }
}

// `serverAbilities::ResetAbility` — 하나를 되돌린다
void Server::AbilityReset(Session& s, uint8_t abil, std::vector<OutPacket>& out) {
    if (abil >= kAbilityCount) return;
    s.abilCooldown[abil] = 0;
    SendAbilityReset(s, abil, out);
    if (s.abilActive[abil]) {
        SendAbilityActive(s, abil, false, out);
        s.abilActive[abil] = false;
        s.abilActiveUntil[abil] = 0;
    }
}

// `serverAbilities::CancelAbilities` — 전부 되돌린다. ★ `eb` 에 **6**(전부)을 싣는다
void Server::AbilityCancelAll(Session& s, std::vector<OutPacket>& out) {
    for (uint8_t i = 0; i < kAbilityCount; ++i) s.abilCooldown[i] = 0;
    SendAbilityReset(s, 6, out);
    for (uint8_t i = 1; i < kAbilityCount; ++i) {
        if (!s.abilActive[i]) continue;
        SendAbilityActive(s, i, false, out);
        s.abilActive[i] = false;
        s.abilActiveUntil[i] = 0;
    }
}

// ── 묶음 7-e 능력 효과 넷 ─────────────────────────────────────────────
//
// 정답지 `serverAbilities::do`(`spells_abil.go:346`) 가 부르는 다섯 중 **넷**.
//   · 광전사 돌진 `nox_xxx_warriorBerserker_53FEB0`(`spell_berserk.go:11`)
//   · 함성       `nox_xxx_warriorWarcry_53FF40`(`spell_warcry.go:13`)
//   · 살금살금   `nox_xxx_warriorTreadLightly_5400B0`(`spell_tread.go:10`)
//   · 늑대의 눈  `nox_xxx_warriorInfravis_540110`(`spell_infra.go:16`)
//   ⛔ 작살(`harpoon.Do`)은 화살 오브젝트·걸림·끌기·끊김이 딸린 한 벌이라 **다음 조각**이다.
//
// ★★ **`cf 1`(켜졌다)은 여기서 나간다.** 7-d 의 `Do` 는 목록에 얹기만 하고 안 보냈다 —
//   정답지도 넷 다 **끝에서** `netAbilReportActive(…, true)` 를 부른다.
//
// ⭕ 광전사 — 걸던 주문 전부 끊기 · (혼란이면 보는 쪽을 흔들기) · 상태 **1** ·
//   투명(0)·무적(23) 끄기 · 타원 방패(주문 51) 끊기
// ⭕ 함성 — 상태 **1** · 둘레 **300** 안의 **적**에게 무효화(주문 55)를 레벨 5 로 건다.
//   거르기 — 워리어는 건너뜀 · 관전자는 건너뜀 · 몬스터는 주문을 쓸 줄 알아야 함 ·
//   **편이 같으면 건너뜀** · 눈이 닿아야(`MapTraceVision`) 함. 끝에 투명·무적·타원 방패도 끈다
// ⭕ 살금살금 — 달리는 중(상태 5)이면 상태 **13** 으로 내리고, 발소리 죽이기(31)를 **능력 레벨**로 건다
// ⭕ 늑대의 눈 — 밤눈(21)을 **능력 레벨**로 건다
void Server::AbilityEffect(Session& s, uint8_t abil, std::vector<OutPacket>& out) {
    const Target self{ s.addr, -1 };
    switch (abil) {
        case 1: {   // 광전사 돌진
            PlayerCancelSpells(s.addr, out);               // cancelAllSpells_4FEE90
            if (s.buffs.Has(3)) {                          // ENCHANT_CONFUSED — 보는 쪽을 흔든다
                //   `Direction2 = Dir16(Direction1 + 4 × IntClamp(-8, 8))` — 양끝 포함이라 **17가지**
                //   ⚠ 정답지는 `RoundDir` 을 안 거쳐 u16 에 260 같은 값이 남을 수 있지만,
                //     읽는 자리가 모두 한 바이트로 자르므로 우리 uint8 돌아감과 같다
                const int k = static_cast<int>(NextRandom(17)) - 8;
                s.facing = static_cast<uint8_t>(static_cast<int>(s.facingPrev) + 4 * k);
            }
            PlayerSetState(s, 1, frame_);
            BuffOff(self, 0);                              // 투명
            BuffOff(self, 23);                             // 무적
            DurCancelFor(51, s.addr, out);                 // 타원 방패
            break;
        }
        case 2: {   // 함성
            PlayerSetState(s, 1, frame_);
            const double dist = 300.0;
            for (auto& kv : sessions_) {
                Session& o = kv.second;
                if (&o == &s || !o.inGame || o.respawnAtFrame != 0) continue;
                if (o.charClass == CharClass::Warrior) continue;   // 워리어는 안 걸린다
                if (o.observer) continue;                          // +3680 & 1
                if (s.team != 0 && o.team == s.team) continue;      // 같은 편
                const double dx = o.posX - s.posX, dy = o.posY - s.posY;
                if (dx * dx + dy * dy > dist * dist) continue;
                if (!MapTraceVision(static_cast<float>(o.posX), static_cast<float>(o.posY),
                                    static_cast<float>(s.posX), static_cast<float>(s.posY))) continue;
                SpellArg arg;
                arg.obj = Target{ o.addr, -1 };
                arg.x = static_cast<float>(o.posX);
                arg.y = static_cast<float>(o.posY);
                SpellAccept(55, s, s.addr, arg, 5, out);   // 무효화 · 레벨 5
            }
            BuffOff(self, 0);
            BuffOff(self, 23);
            DurCancelFor(51, s.addr, out);
            break;
        }
        case 4: {   // 살금살금
            if (s.moveState == 5) PlayerSetState(s, 13, frame_);   // 달리는 중이면 내린다
            ApplyEnchant(self, 31, static_cast<int>(kAbilityDuration[4]), s.spellLvl[4]);
            break;
        }
        case 5: {   // 늑대의 눈
            ApplyEnchant(self, 21, static_cast<int>(kAbilityDuration[5]), s.spellLvl[5]);
            break;
        }
        case 3:     // 작살 — 7-f
            // ★ 작살은 **`cf 1` 을 안 보낸다.** 정답지 `harpoon.Do` 는 그것을 안 부르고,
            //   대신 틀에서 `netHarpoonAttach`(`9e 07`)로 **줄이 걸렸다**를 알린다.
            //   지속도 0 이라 지속 목록에도 안 올라간다 — 그래서 여기서 나간다
            HarpoonDo(s, out);
            return;
        default: return;
    }
    SendAbilityActive(s, abil, true, out);                 // ★ 켜졌다는 여기서 나간다
}

// ── 묶음 7-f 작살 — 쏘기와 걸림 ───────────────────────────────────────
//
// 정답지 `abilityHarpoon`(`src/spell_harpoon.go`) · `NetHarpoonAttach/Break`(`server/network.go`).
//
// ⭕ 와이어 둘 — 둘 다 `9e`(지속 주문 그림)에 **갈래 번호만 다르다**.
//   · 걸림 `9e 07 00 <주인 넷코드 u16> <화살 넷코드 u16>` **7바이트**
//   · 끊김 `9e 0e 00 <주인 넷코드 u16> <화살 넷코드 u16>` **7바이트**
//   `NetSendPacketXxx1(255, …)` 라 **모두에게** 가고 **순번 봉투**에 든다 — 줄이 남에게도 보여야 해서다
//
// ⭕ 쏘기(`Do`) — 상태 **32** 로 바꾸고 `HarpoonBolt` 을 **보는 쪽**으로 쏜다.
//   화살은 몸 반지름 **+1** 앞에서 태어나고, 빠르기는 thing.bin `SPEED` 다
//
// ⭕ 닿음(`Collide`) — 갈래가 넷 중 하나다.
//   · **벽**(대상 없음) → 그 칸에 피해 1(종류 11)을 주고 **소리 없이 끊긴다**
//   · 대상이 **죽었거나 자기 자신** → 아무 일도 안 한다(화살은 그대로 난다)
//   · 대상이 **적이 아니면** → 피해만 주고 **끊긴다**
//   · **적이면** → 피해 1 을 주고 **꽂힌다**. 화살은 `NO_COLLIDE` 가 되고 도운 사람으로 기록된다
//   ⚠ 정답지는 `CallDamage` 의 **돌려준 값**도 본다(막혔으면 끊김) — 우리 `HurtTarget` 은 값을 안 돌려준다.
//     ⇒ 지금은 **편 가르기만** 본다. 막기 갈래는 그 함수가 값을 돌려주게 될 때 같이 옮긴다
//
// ⛔ 끌기와 끊김 관문(거리·수명·안 움직임·시야)은 **다음 조각(7-g)**.
void Server::SendHarpoonFx(uint8_t code, uint16_t n1, uint16_t n2, std::vector<OutPacket>& out) {
    const uint8_t msg[7] = { static_cast<uint8_t>(Op::FX_DURATION_SPELL), code, 0,
                             static_cast<uint8_t>(n1), static_cast<uint8_t>(n1 >> 8),
                             static_cast<uint8_t>(n2), static_cast<uint8_t>(n2 >> 8) };
    for (auto& kv : sessions_) if (kv.second.inGame) EmitSeq(kv.second, msg, sizeof(msg), out);
}

// 지금 그 사람의 작살 화살을 찾는다 — 없으면 nullptr
Server::MissileObj* Server::HarpoonBoltOf(const Session& s) {
    if (s.harpoonBoltNc == 0) return nullptr;
    for (auto& m : missiles_) if (m.netCode == s.harpoonBoltNc && !m.destroyed) return &m;
    return nullptr;
}

// `abilityHarpoon::Do` — 화살을 만들어 쏜다
void Server::HarpoonDo(Session& s, std::vector<OutPacket>& out) {
    PlayerSetState(s, 32, frame_);                           // PlayerState32
    const ProjTypeInfo* pt = ProjType("HarpoonBolt");
    if (!pt || pt->type == 0) return;                        // 종류를 모르면 안 만든다
    HarpoonBreak(s, /*emitSound=*/false, out);               // 남은 것이 있으면 먼저 치운다
    MissileObj m;
    m.netCode = nextShotNetCode_++;
    m.type = pt->type;
    m.flags = pt->flags;
    const double r = static_cast<double>(kPlayerRadius) + 1.0;   // `u.Shape.Circle.R + 1.0`
    m.posX = m.newX = m.prevX = static_cast<float>(s.posX + DirVecX(s.facingPrev) * r);
    m.posY = m.newY = m.prevY = static_cast<float>(s.posY + DirVecY(s.facingPrev) * r);
    m.dir1 = m.dir2 = s.facingPrev;                          // `u.Direction1`
    m.createdFrame = m.frame136 = frame_;
    m.speed = pt->speed;
    m.owner = m.ud0 = m.ud2 = s.addr;
    m.collideKind = 4;                                       // HarpoonCollide
    m.collideAmount = static_cast<uint8_t>(kHarpoonDamage);
    m.velX = DirVecX(m.dir1) * m.speed;
    m.velY = DirVecY(m.dir1) * m.speed;
    missiles_.push_back(m);
    s.harpoonBoltNc = m.netCode;
    s.harpoonTarget = Target{};
    s.harpoonFrame35 = 0;                                    // 아직 걸림을 안 알렸다
    s.harpoonFrame38 = 0;
}

// `abilityHarpoon::breakForOwner` — 줄을 끊는다
void Server::HarpoonBreak(Session& s, bool emitSound, std::vector<OutPacket>& out) {
    (void)emitSound;                                         // 소리 `SoundHarpoonBroken` 은 묶음 10
    if (s.harpoonBoltNc == 0) return;
    // `DisableAbility` — 끊김 와이어 + 켜짐 끄기
    SendHarpoonFx(14, s.netCode, s.harpoonBoltNc, out);
    if (s.abilActive[3]) {
        SendAbilityActive(s, 3, false, out);
        s.abilActive[3] = false;
        s.abilActiveUntil[3] = 0;
    }
    for (size_t i = missiles_.size(); i-- > 0;) {
        if (missiles_[i].netCode != s.harpoonBoltNc) continue;
        DestroyMissile(i, out);
        break;
    }
    s.harpoonBoltNc = 0;
    s.harpoonTarget = Target{};
    s.harpoonFrame35 = 0;
    s.harpoonFrame38 = 0;
}

// `abilityHarpoon::Collide` — 화살이 무엇엔가 닿았다. 꽂히면 true(화살을 살려 둔다)
bool Server::HarpoonCollide(MissileObj& m, const Target& targ, std::vector<OutPacket>& out) {
    auto oit = sessions_.find(m.owner);
    if (oit == sessions_.end()) return false;
    Session& own = oit->second;
    if (targ.none()) {                                       // 벽 — 그 칸에 피해를 주고 조용히 끊는다
        HarpoonBreak(own, /*emitSound=*/false, out);
        return false;
    }
    if (!targ.addr.empty() && targ.addr == m.owner) return true;   // 자기에게는 안 꽂힌다
    AttackSrc src;
    src.x = m.posX; src.y = m.posY;
    src.byMonsterType = m.type;
    HurtTarget(targ, kHarpoonDamage, out, m.owner, 11, &src);      // DamageImpact = 11
    if (!TargetIsEnemy(own, targ)) {                         // 적이 아니면 피해만 주고 끊긴다
        HarpoonBreak(own, /*emitSound=*/false, out);
        return false;
    }
    own.harpoonTarget = targ;
    TargetXY(targ, own.harpoonTargX, own.harpoonTargY);
    own.harpoonFrame38 = frame_;
    m.flags |= 0x40;                                         // FlagNoCollide(비트 6 · ThingDb kFlagNames) — 더는 안 부딪힌다
    MarkAssist(m.owner, targ);                               // sub_4E7540
    return true;
}

// ── 묶음 7-g 작살 끌기와 끊김 관문 ────────────────────────────────────
//
// 정답지 `abilityHarpoon::Update`(`spell_harpoon.go:193`) · `UpdatePlayer`(`:186`).
//
// ⭕ **끌기** — 꽂혀 있으면 매 틱 상대에게 `applyForce(주인 자리, **−10**)` 을 준다.
//   `objectApplyForce_52DF80` 의 셈 그대로 —
//     `dp = 상대자리 − 주인자리` · `r = |dp| + 0.1` · `f = 10 × 힘 / 무게` · `ForceVec += dp × f / r`
//   힘이 **음수**라 상대는 주인 **쪽으로** 끌려온다. 그리고 매 틱 도운 사람으로 기록된다
//
// ⭕ **끊김 관문** — 꽂힌 뒤에는 다섯 가지로 끊긴다(하나라도 걸리면 그 자리에서 끝).
//   ① 주인과 상대가 **375 보다 멀어짐** ② **20 보다 가까워짐**(다 끌어왔다)
//   ③ 줄이 선 지 **150프레임**(`MaxHarpoonExistence`)
//   ④ **30틱마다** 보아 상대가 거리제곱 **1 도 안 움직였으면**(빠져나갈 수 없는 자리에 낀 것)
//   ⑤ 주인↔상대 **시야가 막히면**(벽 추적 9)  · 그리고 상대가 죽으면
//   꽂히기 **전**에는 관문이 하나다 — 주인에게서 **300 보다 멀리 날면** 끊긴다
//
// ⭕ 꽂힌 화살은 **상대 자리에 붙어 다닌다**(자리·이전자리·새자리를 모두 상대에게 맞추고 속도·힘을 0 으로).
//
// ★ `9e 07`(걸림)은 **첫 갱신 틱**에 나간다(`frame35 == 0`) — 꽂혔는지와 상관없다.
//   그래서 수명 150 도 **꽂힌 때가 아니라 그 첫 틱부터** 센다
//
// ⛔ 안 옮긴 것 — **겨눈 쪽으로 휘기**. 정답지는 살아 있는 커서 자리(`pl.CursorPos()`)로
//   `spellFlySearchTarget` 을 돌려 화살을 그쪽으로 튼다. 우리는 **시전 순간 커서**(`castCursorX/Y`)만 있어
//   같은 것이 아니다 — 커서를 매 틱 받게 될 때 같이 옮긴다. 소리 셋은 묶음 10
void Server::HarpoonUpdate(Session& s, std::vector<OutPacket>& out) {
    if (s.harpoonBoltNc == 0) return;
    MissileObj* bolt = HarpoonBoltOf(s);
    if (!bolt) { HarpoonBreak(s, /*emitSound=*/true, out); return; }
    if (s.respawnAtFrame != 0) { HarpoonBreak(s, /*emitSound=*/true, out); return; }   // 주인이 죽었다

    const double dx0 = static_cast<double>(bolt->posX) - s.posX;
    const double dy0 = static_cast<double>(bolt->posY) - s.posY;
    const double dist = std::sqrt(dx0 * dx0 + dy0 * dy0);

    if (!s.harpoonTarget.none()) {
        float tx = 0, ty = 0;
        if (!TargetXY(s.harpoonTarget, tx, ty)) { HarpoonBreak(s, true, out); return; }   // 상대가 사라졌다
        if (!s.harpoonTarget.addr.empty()) {
            auto it = sessions_.find(s.harpoonTarget.addr);
            if (it == sessions_.end() || !it->second.inGame || it->second.respawnAtFrame != 0) {
                HarpoonBreak(s, true, out); return;
            }
        }
        if (dist > kHarpoonMaxDist) { HarpoonBreak(s, true, out); return; }               // ①
        if (dist < kHarpoonMinDist) { HarpoonBreak(s, false, out); return; }              // ②
        if (s.harpoonFrame35 != 0 && frame_ - s.harpoonFrame35 > kHarpoonLifetime) {      // ③
            HarpoonBreak(s, true, out); return;
        }
        if (frame_ - s.harpoonFrame38 > 30u) {                                           // ④ 30틱마다 본다
            s.harpoonFrame38 = frame_;
            const double mx = static_cast<double>(s.harpoonTargX) - tx;
            const double my = static_cast<double>(s.harpoonTargY) - ty;
            if (mx * mx + my * my < 1.0) { HarpoonBreak(s, true, out); return; }
            s.harpoonTargX = tx;
            s.harpoonTargY = ty;
        }
        if (!MapTraceRay(static_cast<float>(s.posX), static_cast<float>(s.posY), tx, ty, 0x9)) {   // ⑤
            HarpoonBreak(s, true, out); return;
        }
        // 화살을 상대 자리에 붙인다
        bolt->newX = bolt->posX = bolt->prevX = tx;
        bolt->newY = bolt->posY = bolt->prevY = ty;
        bolt->velX = bolt->velY = 0.0f;
        bolt->forceX = bolt->forceY = 0.0f;
        // 끌어당기기 — `applyForce(주인 자리, −힘)`(`UpdatePlayer`)
        MarkAssist(s.addr, s.harpoonTarget);                 // sub_4E7540 — 매 틱 도운 사람으로
        HarpoonPull(s, s.harpoonTarget, tx, ty);
    } else if (dist > kHarpoonMaxFlight) {
        HarpoonBreak(s, true, out); return;                  // 안 걸린 채 너무 멀리 날았다
    }
    if (s.harpoonFrame35 == 0) {                             // 첫 틱에 「걸림」을 알린다
        SendHarpoonFx(7, s.netCode, s.harpoonBoltNc, out);
        s.harpoonFrame35 = frame_;
    }
}

// `objectApplyForce_52DF80` — 힘이 음수면 `vec` 쪽으로 끌려온다
void Server::HarpoonPull(const Session& owner, const Target& targ, float tx, float ty) {
    const double dpx = static_cast<double>(tx) - owner.posX;
    const double dpy = static_cast<double>(ty) - owner.posY;
    const double r = std::sqrt(dpx * dpx + dpy * dpy) + 0.1;
    double mass = kPlayerMass;
    if (targ.addr.empty() && targ.pet >= 0 && static_cast<size_t>(targ.pet) < pets_.size()) {
        const auto tb = typeBodies_.find(pets_[static_cast<size_t>(targ.pet)].type);
        mass = (tb != typeBodies_.end() && tb->second.mass > 0.0f) ? tb->second.mass : 1.0;
    }
    if (mass <= 0.0) mass = 1.0;
    const double f = 10.0 * (-kHarpoonForce) / mass;
    const double fx = dpx * f / r;
    const double fy = dpy * f / r;
    if (!targ.addr.empty()) {
        auto it = sessions_.find(targ.addr);
        if (it == sessions_.end()) return;
        if (it->second.respawnAtFrame != 0) return;          // 죽은 것은 안 밀린다(isObjectMovable)
        it->second.forceX += fx;
        it->second.forceY += fy;
    } else if (targ.pet >= 0 && static_cast<size_t>(targ.pet) < pets_.size()) {
        // ⚠ 소환수는 아직 ForceVec 적분이 없다(걸음을 AI 가 직접 옮긴다) — 자리로 바로 옮긴다
        pets_[static_cast<size_t>(targ.pet)].x += fx;
        pets_[static_cast<size_t>(targ.pet)].y += fy;
    }
}

// ── 묶음 7-h 안내서(짐승 두루마리) ────────────────────────────────────
//
// 정답지 `nox_xxx_awardBeastGuide_4FAE80`·`nox_xxx_netReportGuideAward_4D8000`
//        (`legacy/server__magic__plyrgide.c`).
//
// ⭕ 와이어 — `d1 <안내서 번호> <알림>` **3바이트**. `netSendPacket1` 이라 **순번 봉투**.
//   ★ 셋째 칸의 **0x80 은 이 길에서는 안 선다** — 부르는 자리가 넷째 인자를 늘 **0** 으로 넘긴다.
//     클라도 `& 0x7F` 로 벗겨 읽는다(`cdecode.c:2576`)
//
// ⭕ 주기 — 번호는 **1~40**(벗어나면 "AwardGuideError" 글줄). 이미 있으면 **조용히 실패**한다
//   (주문·능력과 달리 글줄도 안 보낸다). 없으면 **1** 로 세운다. 안내서는 레벨이 없고 **있다/없다**뿐이다
//
// ⭕⭕ **무리 안내서** — 정답지가 `0x587000 + 216292` 의 표를 훑는다.
//   그 자리는 **포인터 배열**이고, 실측으로 **칸이 하나**뿐이다(둘째 칸이 0 이라 거기서 끝난다).
//   그 하나가 가리키는 목록은 `24, 7, 8, 25, 26, 0` —
//   ⇒ **24 번을 받으면 7·8·25·26 도 같이** 1 이 된다. `d1` 은 **받은 번호 하나만** 간다
//   (⭕ `blob_587000.dat` 오프셋 216268 을 바이트로 읽어 확인했다)
static const uint8_t kGuideGroupLead = 24;
static const uint8_t kGuideGroupRest[] = { 7, 8, 25, 26 };

void Server::SendGuideAward(Session& s, uint8_t guide, bool announce, std::vector<OutPacket>& out) {
    const uint8_t msg[3] = { static_cast<uint8_t>(Op::REPORT_GUIDE_AWARD), guide,
                             static_cast<uint8_t>(announce ? 1 : 0) };
    EmitSeq(s, msg, sizeof(msg), out);                   // Packet1 = 순번 봉투
}

bool Server::GuideAward(Session& s, uint8_t guide, bool announce, std::vector<OutPacket>& out) {
    if (guide == 0 || guide >= 41) return false;         // "AwardGuideError"(글줄은 묶음 10)
    if (s.guideLvl[guide] != 0) return false;            // 이미 있으면 조용히
    s.guideLvl[guide] = 1;
    // ⛔ 소리 227 과 보상 알림(나·남들)은 묶음 10
    if (guide == kGuideGroupLead) {
        for (uint8_t g : kGuideGroupRest) s.guideLvl[g] = 1;
    }
    SendGuideAward(s, guide, announce, out);
    return true;
}

// ── 묶음 6-a AI 행동 쌓기 ─────────────────────────────────────────────
//
// 정답지 `server/object_ai.go`(밀기·빼기) + `unit_ai.go:575~610`(매 틱 굴리기).
// 몬스터는 **할 일을 쌓아 두고 맨 위를 한다.** 여기까지가 틀이고, 행동 하나하나는 다음 조각이다.
//
// ⭕ 행동 갈래는 **0~38**(`common/unit/ai` ActionType).
//   0 놀기 · 4 지키기 · 5 사냥 · 7 가기 · 14 주인에게 · 15 싸움 · 16 근접 · 24 달아나기 ·
//   26 바라보기 · 29 아무 데나 걷기 · 30 죽어가는 중 · 31 죽음 · 38 없음
//
// ★ **조건(Condition)** 갈래가 따로 있다 — 빼기가 맨 위를 내린 뒤 **그 아래 조건들도 같이** 내린다.
//   ⭕ `ActionType.IsCondition()`(`common/unit/ai/ai.go:22` · `monsterActionIsCondition_50A010`) —
//     **40 `DEPENDENCY_OR` ~ 71 `DEPENDENCY_NOT_MOVED`** 가 조건이다.
//     즉 **0~38 은 할 일**이고 **40~71 은 그 할 일에 붙는 조건**이다(39 는 비어 있다).
//     보기 — 42 살아 있나 · 45 보이나 · 49 더 머나 · 53 보이는 적 · 64 성치 않나 · 70 안 답답한가
bool Server::AIIsCondition(uint8_t act) {
    return act >= 40 && act <= 71;
}

// monsterActionReset_50A0C0 — 행동을 바꿀 때 딸린 셈들을 0 으로 하고 **바꾼 프레임을 적는다**.
//   ⭕ 정답지는 `+124` 와 `+137` 에 **같은 프레임**을 적는다(`object_ai.go:60~61`).
//   ⚠ 함께 0 이 되는 셈들(Field2·67·74·91·120…)은 우리 `Creature` 에 아직 없다 — 생기면 여기에 붙인다.
void Server::AIActionReset(Creature& k) {
    k.aiActionFrame = frame_;
    k.aiMorphReady = false;
}

// monsterPushAction_50A260 — 할 일을 하나 올린다. 못 올리면 거짓.
//   · 쌓임이 꽉 차면 안 올린다
//   · 맨 위가 **죽음(31)** 이면 **죽어가는 중(30)** 말고는 안 올린다
//   · 맨 위가 **놀기(0)** 이고 바닥이면 **덮어쓴다**(쌓지 않는다)
//   · 그 밖에는 맨 위를 **취소(Cancel)** 하고 그 위에 올린다
bool Server::AIPush(Creature& k, uint8_t act, const Target& targ, float x, float y) {
    if (k.aiStackInd >= Creature::kAIStackMax - 1) return false;
    int ind = k.aiStackInd;
    const uint8_t cur = k.aiStack[ind].action;
    if (cur == 31 && act != 30) return false;                // 죽은 것에는 안 올린다
    if (cur == 0 && ind == 0) {
        ind = -1;                                            // 바닥의 놀기는 덮어쓴다
    } else if (!AIIsCondition(cur) && k.aiStack[ind].started) {
        AICancel(k, cur);
    }
    ++ind;
    k.aiStack[ind] = Creature::AIStackItem{};
    k.aiStack[ind].action = act;
    k.aiStack[ind].targetAddr = targ.addr;
    k.aiStack[ind].targetPet = targ.pet;
    k.aiStack[ind].argX = x;
    k.aiStack[ind].argY = y;
    k.aiStackInd = ind;
    AIActionReset(k);
    aiStackChanged_ = true;
    return true;
}

// monsterPopAction_50A3A0 — 맨 위를 끝내고 내린다.
//   내린 뒤 **그 아래 조건 항목들도 같이** 내린다. 다 내려가면 바닥에 **놀기(0)** 가 남는다.
int Server::AIPop(Creature& k) {
    const uint8_t cur = k.aiStack[k.aiStackInd].action;
    if (!AIIsCondition(cur) && k.aiStack[k.aiStackInd].started) AIEnd(k, cur);
    aiStackChanged_ = true;
    --k.aiStackInd;
    for (; k.aiStackInd >= 0; --k.aiStackInd)
        if (!AIIsCondition(k.aiStack[k.aiStackInd].action)) break;
    AIActionReset(k);
    const int si = k.aiStackInd;
    if (si < 0) {
        k.aiStackInd = 0;
        k.aiStack[0] = Creature::AIStackItem{};              // 바닥은 놀기(0)
    }
    return si;
}

// `unit_ai.go:581~603` — 매 틱. 맨 위가 아직 **안 시작했으면 시작**하고,
//   시작이 쌓임을 바꿨으면 **다시 맨 위를 본다**. 그러고 **마지막 맨 위를 갱신**한다.
//   ⚠ 앞에 붙는 것(체력 회복·시야 갱신·`monsterMainAIFn_547210` 두뇌·애니)은 다음 조각이다.
void Server::AITick(Creature& k, std::vector<OutPacket>& out) {
    const bool changedPrev = aiStackChanged_;
    uint8_t curAct = 0;
    for (;;) {
        Creature::AIStackItem& cur = k.aiStack[k.aiStackInd];
        curAct = cur.action;
        if (cur.started) {
            if (!aiStackChanged_) aiStackChanged_ = changedPrev;
            break;
        }
        cur.started = true;
        aiStackChanged_ = false;
        AIStart(k, curAct, out);
        if (!aiStackChanged_) {
            aiStackChanged_ = changedPrev;
            break;
        }
    }
    AIUpdate(k, curAct, out);
}

// ── 행동 하나하나(`server/object_ai_actions.go`) ──────────────────────
//
// ⭕ 정답지에서 **Go 로 옮겨진 행동은 여섯뿐**이다 — 기다리기(1) · 상대 기다리기(2) ·
//   상자로 변신(33) · 제 모습으로(34) · 떨구기(12, 아무것도 안 함) · 찾기(13, 아무것도 안 함).
//   나머지(가기 7 · 싸움 15 · 바라보기 26 …)는 아직 **C 안에** 있다 — 다음 조각이다.
//
// 시작은 넷 다 **아무 일도 안 한다**(정답지 `Start(_ *Object) {}`).
// sub_50A040(`legacy/GAME4_1.c:391`) — 맨 위 **아래**의 행동. ★ **조건은 건너뛴다**.
//   없으면 38(없음). 가기(7)가 「내 아래가 호위(3)인가」를 이것으로 묻는다
uint8_t Server::AIActionBelow(const Creature& k) {
    for (int i = k.aiStackInd - 1; i >= 0; --i)
        if (!AIIsCondition(k.aiStack[i].action)) return k.aiStack[i].action;
    return 38;
}

// nox_xxx_monsterIsActionScheduled_50A090 — 그 행동이 **쌓임 어딘가에** 있나
bool Server::AIHasAction(const Creature& k, uint8_t act) {
    for (int i = 0; i <= k.aiStackInd && i < Creature::kAIStackMax; ++i)
        if (k.aiStack[i].action == act) return true;
    return false;
}

// 세워 둔 속도로 자리를 옮긴다. 벽은 **x·y 를 따로** 보아 한 축이 막혀도 미끄러진다
void Server::CreatureApplyVelocity(Creature& k) {
    const double nx = k.x + k.velX, ny = k.y + k.velY;
    if (!WallAt(nx, k.y)) k.x = nx;
    if (!WallAt(k.x, ny)) k.y = ny;
}

void Server::AIStart(Creature& k, uint8_t act, std::vector<OutPacket>& out) {
    (void)out;
    switch (act) {
        case 17: {   // ── 묶음 6-g(3) 쏘기 시작 — `sub_532540`(`legacy/GAME4_3.c:2119`)
            // 근접과 **같은 꼴**이다 — 쉬는 시간(+512)이 끝났으면 휘두르기를 시작하고
            // 다음 때를 **쏘기 간격**(`monsterDef+220`·`+224`)으로 잡는다.
            // ⚠ 근접과 달리 **쉬는 중에 아무것도 안 올린다**(정답지에 그 갈래가 없다)
            const MonsterStats* m = FindMonster(k.type);
            if (frame_ >= k.nextHitFrame) {
                k.attackStartFrame = frame_;
                const int lo = (m && m->missileMin > 0) ? m->missileMin : 30;
                const int hi = (m && m->missileMax > 0) ? m->missileMax : lo;
                const int span = (hi >= lo) ? (hi - lo + 1) : 1;
                k.nextHitFrame = frame_ + static_cast<uint32_t>(lo) + NextRandom(static_cast<uint32_t>(span));
            }
            break;
        }
        case 31: {   // ── 묶음 6-j 죽음 시작 — `mobActionDead1_544D80`
            // **속도·힘을 0 으로** 두고 오브젝트 깃발에 `0x18` 을 얹는다(시체는 안 움직인다)
            //   ⚠ 풀려난 넋(`createReleasedSoul_544E60`)·좀비 시간표는 안 옮겼다
            k.velX = 0; k.velY = 0;
            k.path.clear();
            break;
        }
        case 16: {   // ── 묶음 6-g(2) 근접 공격 시작 — `mobActionMelee1_532130`(`legacy/GAME4_3.c:1943`)
            //
            // ⭕ **쉬는 시간이 끝났나**(`+512`)로 갈린다.
            //   · 끝났으면 — **휘두르기를 시작**하고 다음 쉬는 시간을 `randomInt(최소, 최대)` 로 잡는다
            //   · 아직이면 — **「대상이 가까이 있으면」 조건(50)** 을 깔고 **그때까지 기다리기(1)** 를 올린다.
            //     조건의 자는 **근접 사거리 × 1.2** 다(`monsterDef+112 * 1.2`)
            //
            // ⚠ **앞에 아군이 있나** 검사(`sub_532390` — 앞 60° 안에서 가장 가까운 것)와
            //   그때 미는 **각도 바라보기(27)·때(41)·도망(24)** 갈래는 아직 안 옮겼다 — 6-g(2b)
            // ⚠ 사람의 기력 깎기(깃발 0x10)와 소리는 안 옮긴다
            const MonsterStats* m = FindMonster(k.type);
            if (frame_ >= k.nextHitFrame) {
                k.attackStartFrame = frame_;                 // +136 휘두르기 시작 프레임
                const int lo = (m && m->minDelay > 0) ? m->minDelay : 30;
                const int hi = (m && m->maxDelay > 0) ? m->maxDelay : lo;
                const int span = (hi >= lo) ? (hi - lo + 1) : 1;   // randomInt 는 양끝 포함
                k.nextHitFrame = frame_ + static_cast<uint32_t>(lo) + NextRandom(static_cast<uint32_t>(span));
            } else {
                const double reach = (m && m->meleeRange > 0.0) ? m->meleeRange : 0.0;
                if (AIPush(k, 50)) {
                    k.aiStack[k.aiStackInd].argF = static_cast<float>(reach * 1.2);
                    k.aiStack[k.aiStackInd].targetAddr = k.enemyAddr;
                    k.aiStack[k.aiStackInd].targetPet = k.enemyPet;
                }
                if (AIPush(k, 1)) k.aiStack[k.aiStackInd].argU32 = k.nextHitFrame;
            }
            break;
        }
        default: break;
    }
}

// sub_545240(`GAME5.c:154`) — **한 점을 바라보게 돌린다**. 다 돌았으면 참(그러면 부르는 쪽이 뺀다).
//   · 그 점까지의 방향을 단위로 만든다(길이에 **0.001** 을 더해 0 나눗셈을 막는다 — 정답지 그대로)
//   · 지금 쪽과 **외적**을 재 어느 쪽으로 돌지 정하고 `돌아가려는 쪽`을 **8** 만큼 돌린다
//   · 지금 쪽과 **내적이 0.9 를 넘으면** 다 본 것이다(`sub_534120`)
bool Server::AIFacePoint(Creature& k, float tx, float ty) {
    const float dx = tx - static_cast<float>(k.x);
    const float dy = ty - static_cast<float>(k.y);
    const float len = std::sqrt(dx * dx + dy * dy) + 0.001f;
    const float nx = dx / len, ny = dy / len;
    const float cx = DirVecX(k.facing), cy = DirVecY(k.facing);
    if (ny * cx - nx * cy >= 0.0f) k.facingWant = static_cast<uint8_t>(k.facingWant + 8);
    else                            k.facingWant = static_cast<uint8_t>(k.facingWant - 8);
    return (cy * ny + cx * nx) > 0.89999998f;
}

// 갱신 — 여기가 알맹이다.
void Server::AIUpdate(Creature& k, uint8_t act, std::vector<OutPacket>& out) {
    Creature::AIStackItem& st = k.aiStack[k.aiStackInd];
    switch (act) {
        case 25:     // 자리를 바라보기(`sub_545210`) — 인자로 받은 점
            if (AIFacePoint(k, st.argX, st.argY)) AIPop(k);
            break;
        case 26: {   // 무엇을 바라보기(`sub_545300`) — **대상이 없으면 그냥 뺀다**
            float tx = 0, ty = 0;
            const Target targ{ st.targetAddr, st.targetPet };
            if (!TargetXY(targ, tx, ty)) { AIPop(k); break; }
            if (AIFacePoint(k, tx, ty)) AIPop(k);
            break;
        }
        case 27:     // 각도를 바라보기(`sub_545340`) — 인자가 **방향 번호**다.
            //   그 번호의 단위 벡터를 「내 앞의 한 점」으로 삼으면 같은 셈이 된다
            if (AIFacePoint(k, static_cast<float>(k.x) + DirVecX(static_cast<int>(st.argU32)) * 100.0f,
                            static_cast<float>(k.y) + DirVecY(static_cast<int>(st.argU32)) * 100.0f))
                AIPop(k);
            break;
        case 1:      // 기다리기 — **그 프레임이 되면** 스스로 빠진다(`지금 >= 인자`)
            if (frame_ >= st.argU32) AIPop(k);
            break;
        case 2:      // 상대 기다리기 — **행동을 바꾼 프레임 + 인자**를 **넘기면** 빠진다
            //   ⚠ 기다리기(1)는 `>=`, 이쪽은 `>` 다. 정답지가 그렇게 갈라 놓았다
            if (frame_ > k.aiActionFrame + st.argU32) AIPop(k);
            break;
        case 33:     // 상자로 변신 — 「됐다」가 서면 빠지고 **변신 표시를 켠다**
            if (k.aiMorphReady) { AIPop(k); k.morphed = true; }
            break;
        case 34:     // 제 모습으로 — 빠지고 변신 표시를 **끈다**
            if (k.aiMorphReady) { AIPop(k); k.morphed = false; }
            break;
        case 7: {    // ── 묶음 6-f(4) 가기 — `nox_xxx_mobActionMoveTo_5443F0`(`legacy/GAME4_3.c:9487`)
            // ① **못 움직이면 그냥 뺀다**(`monsterIsMoveing_534320` = 빠르기 >= 0.01)
            if (!(k.speedCur >= 0.0099999998f)) { AIPop(k); break; }
            // ★ 묶음 6-h — **바로 아래가 호위(3)면 걷기·뛰기를 갈아탄다**(`sub_50A040 == 3`).
            //   잣대는 **호위 거리 × 3**(그보다 가까우면 걷기)과 **거기에 +30**(그보다 멀면 뛰기).
            //   ⚠ 그 사이는 **그대로 둔다** — 경계에서 달렸다 걸었다 하지 않게 하는 틈이다
            if (AIActionBelow(k) == 3) {
                const double near2 = k.escortDist * 3.0;
                const double far2 = near2 + 30.0;
                const double gx2 = st.argX - k.x, gy2 = st.argY - k.y;
                const double d2 = gx2 * gx2 + gy2 * gy2;
                if (d2 >= near2 * near2) { if (d2 > far2 * far2) k.running = true; }
                else k.running = false;
            }
            if (CreatureSetMovePath(k, st.argX, st.argY) != 1) break;   // 아직 가는 중

            // ② 길 찾기 **결과(+284)** 로 갈래가 갈린다
            const uint8_t ps = k.pathStatus;
            // **막힌 것으로 본다** — 결과 2 이거나, 결과 1 이 **5초 안에 두 번** 났을 때
            const bool stuck = (ps == 2) ||
                               (ps == 1 && (frame_ - k.pathStuckFrame) < static_cast<uint32_t>(5 * 30));
            if (ps == 1) k.pathStuckFrame = frame_;
            // ③ **제대로 닿았으면**(결과 0) 그쪽을 보고 **행동을 뺀다**
            //   ⚠ `sub_547F10`(길목 모드)과 쌓임 항목 `+12` 갈래는 아직 안 옮겼다
            if (ps == 0) {
                // ★ `mobCalcDir_533CC0` 은 **돌아가려는 쪽(+126)만** 세운다 —
                //   보는 쪽(+124)은 건드리지 않는다(그쪽은 딴 데서 8씩 쫓아간다)
                k.facingWant = DirFromVec(st.argX - k.x, st.argY - k.y);
                AIPop(k);
                break;
            }
            // ④ **막혔으면** — 2~4초짜리 **때(41) 조건**을 깔고 **아무 데나 걷기(29)** 를 올린다
            if (stuck) {
                const int idx = AIPush(k, 41) ? k.aiStackInd : -1;
                if (idx >= 0)
                    // `randomInt(2*FPS, 4*FPS)` — ★ **양끝을 다 넣는다**(`IntClamp`).
                    //   60~120 이니 값이 **61가지**다
                    k.aiStack[idx].argU32 = frame_ + 60 + NextRandom(61);
                AIPush(k, 29);
            }
            // ⑤ 결과가 0 이 아니면 **잠깐 기다린다**(0.5~1초)
            AIPush(k, 1);
            k.aiStack[k.aiStackInd].argU32 = frame_ + 15 + NextRandom(16);   // randomInt(FPS/2, FPS) = 15~30
            break;
        }
        case 3: {    // ── 묶음 6-h 호위 — `nox_xxx_mobActionEscort_546430`(`legacy/GAME5.c:924`)
            //
            // ⭕ 「주인 곁에 있어라」다. 셋으로 갈린다 —
            //   · 호위할 것이 **없으면 뺀다**(`monsterGetObjEscortName_546600`)
            //   · **지금 상대가 있으면 싸움(15)** 을 올린다(그쪽이 알아서 쫓고 때린다)
            //   · **너무 멀면**(호위 거리 + 30) 조건을 깔고 **가기(7)** 를 민다
            //     — 44 공격받고 있지 않나 · 56 보이는 적이 없나 · 49 **호위 거리보다 머나**
            //   · 가까우면 아무 일도 안 한다(치유 갈래 `mobHealSomeone_5411A0` 은 아직 안 옮겼다)
            //
            // ⚠ 우리 크리처의 호위 대상은 **부른 사람**이다 — 정답지의 이름표가 `**PLAYER**` 인 갈래
            // ⚠ `monsterCanAttackAtWill_534390`·`sub_5343C0` 는 안 옮겼다 — 소환수는 **늘 싸울 수 있다**로 본다
            const Target master{ k.owner, -1 };
            float mx = 0, my = 0;
            if (k.owner.empty() || !TargetXY(master, mx, my)) { AIPop(k); break; }
            st.argX = mx; st.argY = my;                       // +4·+8 — 호위 대상 자리
            const Target foe{ k.enemyAddr, k.enemyPet };
            float ex = 0, ey = 0;
            if (!foe.none() && TargetXY(foe, ex, ey)) {
                if (AIPush(k, 15)) {
                    k.aiStack[k.aiStackInd].argX = ex;
                    k.aiStack[k.aiStackInd].argY = ey;
                    k.aiStack[k.aiStackInd].argU32 = frame_;
                }
                break;
            }
            const double ddx = k.x - mx, ddy = k.y - my;
            const double lim = k.escortDist + 30.0;
            if (ddx * ddx + ddy * ddy <= lim * lim) break;    // 곁에 있다 — 가만히
            AIPush(k, 44);                                    // 공격받고 있지 않나
            AIPush(k, 56);                                    // 보이는 적이 없나
            if (AIPush(k, 49)) {                              // 호위 거리보다 머나
                k.aiStack[k.aiStackInd].argF = k.escortDist;
                k.aiStack[k.aiStackInd].targetAddr = k.owner;
                k.aiStack[k.aiStackInd].targetPet = -1;
            }
            if (AIPush(k, 7)) {
                k.aiStack[k.aiStackInd].argX = mx;
                k.aiStack[k.aiStackInd].argY = my;
            }
            break;
        }
        case 30:     // ── 묶음 6-j 죽어가는 중 — `sub_544D60`(`legacy/GAME4_3.c:9846`)
            // 하는 일이 하나뿐이다 — **죽는 애니가 끝나면 빠진다**(`+483`).
            //   빠지면 아래에 깔아 둔 **죽음(31)** 이 드러난다
            // ⚠ 크리처 애니 칸이 없어 **첫 갱신에 바로 빠진다**(근접 16·쏘기 17 과 같은 자리)
            AIPop(k);
            break;
        case 31:     // ── 묶음 6-j 죽음 — `mobActionDead2_544EC0`
            // 갱신 목록에서 빼고, 종류표가 그러라고 하면 **치운다**.
            //   ⚠ 좀비의 되살아나기(`mobRaiseZombie_534AB0`)와 불타 사라지기는 안 옮겼다
            k.deadDone = true;
            break;
        case 4: {    // ── 묶음 6-i(2) 지키기 — `nox_xxx_mobActionGuard_546010`(`legacy/GAME5.c:738`)
            //
            // ⭕ 「이 자리를 지켜라」다. 쌓임 항목의 `+4·+8` 이 **지킬 자리**, `+12` 가 **볼 쪽**이다.
            //   · **지금 상대가 있으면 싸움(15)** 을 올린다(쫓고 때리는 것은 그쪽 몫)
            //   · 아니면 **16프레임에 한 번**만 자리를 살핀다(`(프레임 + 번호) & 0xF`) —
            //     지킬 자리에서 **8 넘게** 벗어났으면 **56·59·44** 를 깔고 **가기(7)** 로 되돌아간다
            //   ⚠ 매 틱 살피지 않는 것이 핵심이다 — 번호를 더해 **개체마다 살피는 틱이 어긋난다**
            //
            // ⚠ 안 옮긴 것 — `sub_5343C0`(못 싸울 때의 43·52·40 갈래) · 흉내쟁이·식물 갈래 ·
            //   `monsterLookAtDamager_5466B0`(때린 쪽 돌아보기) · 둘레 둘러보기(`sub_5466F0`·`sub_545DA0`)
            const Target foe{ k.enemyAddr, k.enemyPet };
            float ex = 0, ey = 0;
            if (!foe.none() && TargetXY(foe, ex, ey)) {
                if (AIPush(k, 15)) {
                    k.aiStack[k.aiStackInd].argX = ex;
                    k.aiStack[k.aiStackInd].argY = ey;
                    k.aiStack[k.aiStackInd].argU32 = frame_;
                }
                break;
            }
            if (((frame_ + k.netCode) & 0xF) != 0) break;    // 이 틱은 안 살핀다
            const double px = st.argX - k.x, py = st.argY - k.y;
            if (px * px + py * py <= 64.0) break;            // 제자리다
            AIPush(k, 56);                                   // 보이는 적이 없나
            AIPush(k, 59);                                   // 귀에 걸리는 소리가 없나
            AIPush(k, 44);                                   // 공격받고 있지 않나
            const float postX = st.argX, postY = st.argY;
            if (AIPush(k, 7)) {
                k.aiStack[k.aiStackInd].argX = postX;
                k.aiStack[k.aiStackInd].argY = postY;
                k.aiStack[k.aiStackInd].argU32 = 0;
            }
            break;
        }
        case 5: {    // ── 묶음 6-i(3) 사냥 — `nox_xxx_mobActionHunt_5449D0`(`legacy/GAME4_3.c:9692`)
            // 하는 일이 거의 없다 — **사나움을 0.83 으로 놓고 떠돌기(10)를 민다**.
            //   `+4` 에 0(아직 고른 길목 없음) · `+12` 의 낮은 바이트에 **0x80**
            //   (길목을 못 찾으면 `sub_50CB20` 으로 한 번 더 찾아보라는 표시)
            k.aggression = 0.83f;
            if (AIPush(k, 10)) {
                k.aiStack[k.aiStackInd].argX = 0;
                k.aiStack[k.aiStackInd].argU32 = 0x80;
            }
            break;
        }
        case 10: {   // ── 묶음 6-i(3) 떠돌기 — `nox_xxx_mobActionRoam_5457E0`(`legacy/GAME5.c:428`)
            //
            // ⭕ 정답지의 떠돌기는 **길목(Waypoint)을 하나 골라 그리로 걸어간다**.
            //   · 그 전에 **지금 상대가 있으면 싸움(15)** 을 올린다
            //   · 길목을 **하나도 못 찾으면** — 「Cannot find any waypoints」를 찍고
            //     **스스로 빠진 뒤 1~2초 기다리기(1)** 를 올린다
            //
            // ⚠ **우리 맵 자료에는 길목이 없다**(6-e(2b) 의 길목 길과 같은 구멍).
            //   그래서 늘 위의 **「길목 없음」 갈래**를 탄다 — 정답지가 같은 자리에서 하는 것 그대로다.
            //   길목을 읽게 되면 그때 고르기·걸어가기가 살아난다
            const Target foe{ k.enemyAddr, k.enemyPet };
            float ex = 0, ey = 0;
            if (!foe.none() && TargetXY(foe, ex, ey)) {
                if (AIPush(k, 15)) {
                    k.aiStack[k.aiStackInd].argX = ex;
                    k.aiStack[k.aiStackInd].argY = ey;
                    k.aiStack[k.aiStackInd].argU32 = frame_;
                }
                break;
            }
            // ★ 묶음 6-j(2) — **길목을 물결로 찾아** 그리로 걸어간다(`Sub_50CB20`)
            float wx = 0, wy = 0;
            if (AIPathFindWaypoint(k, wx, wy)) {
                if (AIPush(k, 7)) {
                    k.aiStack[k.aiStackInd].argX = wx;
                    k.aiStack[k.aiStackInd].argY = wy;
                    k.aiStack[k.aiStackInd].argU32 = 0;
                }
                break;
            }
            AIPop(k);                                        // 길목이 없다 — 스스로 빠지고
            if (AIPush(k, 1))                                // 1~2초 기다린다(randomInt(FPS, 2*FPS))
                k.aiStack[k.aiStackInd].argU32 = frame_ + 30 + NextRandom(31);
            break;
        }
        case 15: {   // ── 묶음 6-g(1) 싸움 — `nox_xxx_mobActionFight_531EC0`(`legacy/GAME4_3.c:1823`)
            //
            // ⭕ 싸움은 **대상을 쫓는 행동이 아니라, 대상이 없으면 마지막 자리로 가 보는 행동**이다.
            //   · **10초 넘게 대상 소식이 없으면** 스스로 빠진다(쌓임 항목 `+12` 가 마지막 소식 때다)
            //   · 대상이 있으면 그때를 적고 **근접 공격(16)** 을 올린다
            //   · 대상이 없으면 **마지막으로 본 자리**(`+4`·`+8`)로 **가기(7)** 를 민다.
            //     이미 그 자리(8 안)면 그냥 빠진다
            //
            // ⚠ 아직 안 옮긴 갈래 — 스스로 버프 걸기(`monsterBuffSelf_540B90`)·
            //   공격 주문(`monsterCastOffensive_540F20`)은 **6-g(3)** · `sub_534710`(깃발 0x40·아래 행동 4) ·
            //   마지막 자리 둘레 30 안에 그 적이 보이나(`unitsGetInCircle` + `sub_532040`)
            if (frame_ - st.argU32 > static_cast<uint32_t>(10 * 30)) { AIPop(k); break; }
            const Target foe{ k.enemyAddr, k.enemyPet };
            float fx = 0, fy = 0;
            if (!foe.none()) {
                st.argU32 = frame_;                          // 소식이 있었다고 적는다
                // ⚠ `checkIsKillable_528190` 은 **있나**로만 대신한다 — 온전한 조건은 뒤 조각
                if (!TargetXY(foe, fx, fy)) { AIPop(k); break; }
                MonsterPushAttack(k, foe, fx, fy);           // 6-g(2b) — 조건 사슬 + 가기(7)
                break;
            }
            // 대상이 없다 — **마지막으로 본 자리**로 가 본다
            const float lastX = st.argX, lastY = st.argY;
            const double dx = lastX - k.x, dy = lastY - k.y;
            if (dx * dx + dy * dy < 64.0) { AIPop(k); break; }   // 이미 그 자리다
            AIPush(k, 56);                                   // 「보이는 적이 없으면」 조건을 깐다
            if (AIPush(k, 7)) {
                k.aiStack[k.aiStackInd].argX = lastX;
                k.aiStack[k.aiStackInd].argY = lastY;
                k.aiStack[k.aiStackInd].argU32 = 0;          // 정답지 `v3[3] = 0`
            }
            break;
        }
        case 17: {   // 쏘기 — `mobActionMissileAtt_532610` (묶음 6-g(3b))
            // ⭕ 정답지는 애니의 **쏘는 칸**(`monsterDef+216`)에서 `MISSILE_NAME` 오브젝트를
            //   **만들어 날린다**. 이제 우리도 **진짜로 만든다** — 피해는 그것이 맞을 때 들어간다
            // ⚠ **애니 칸이 없어** 「휘두르기를 시작한 그 틱」을 그 자리로 삼는다.
            //   이 막이 없으면 쉬는 시간이 안 지켜져 매 틱 쏜다
            // ⚠ **앞질러 겨눔**(`projAddVelocitySmth_533080`)은 아직 안 옮겼다 — 지금은 곧장 겨눈다
            const MonsterStats* m = FindMonster(k.type);
            const Target foe{ k.enemyAddr, k.enemyPet };
            if (!m || m->missileName == nullptr || foe.none()) { AIPop(k); break; }
            if (k.attackStartFrame != frame_) { AIPop(k); break; }
            float tx = 0, ty = 0;
            if (!TargetXY(foe, tx, ty)) { AIPop(k); break; }
            SpawnCreatureMissile(k, m->missileName, tx, ty, out);
            AIPop(k);
            break;
        }
        case 16: {   // 근접 공격 — `mobActionMeleeAtt_532440`
            // ⚠⚠ **여기가 정답지와 갈리는 자리다.** 정답지는 애니 칸으로 굴린다 —
            //   「**때리는 칸**(`monsterDef+108`)에 닿으면 한 번 때리고, **애니가 끝나면**(+483) 뺀다」.
            //   우리는 **크리처 애니 칸이 없다**(thing.bin 몬스터 애니를 아직 안 읽는다).
            //   그래서 **한 틱에 때리고 뺀다**. ★ 때리는 **간격**은 Start 가 쉬는 시간으로 이미 맞춘다 —
            //   달라지는 것은 「휘두르는 동안 몇 틱 붙들려 있나」뿐이다
            const MonsterStats* m = FindMonster(k.type);
            const int dmg = (m && m->meleeDamage > 0) ? m->meleeDamage : 0;
            const Target foe{ k.enemyAddr, k.enemyPet };
            if (dmg == 0 || foe.none()) { AIPop(k); break; }   // 때릴 수 없다(정답지도 뺀다)
            // ★ 쏘기(17)와 같은 막 — 휘두르기를 시작한 틱에만 맞힌다
            if (k.attackStartFrame != frame_) { AIPop(k); break; }
            AttackSrc src;
            src.x = static_cast<float>(k.x);
            src.y = static_cast<float>(k.y);
            src.byMonsterType = k.type;
            src.byPet = static_cast<int>(&k - pets_.data());
            HurtTarget(foe, dmg, out, k.owner, 0, &src);
            AIPop(k);
            break;
        }
        case 29:     // 아무 데나 걷기 — 6-f(5). **스스로 안 빠진다**(아래 깔린 때(41) 조건이 뺀다)
            AIRandomWalk(k);
            break;
        default: break;   // 12·13 은 아무 일도 안 한다 · 나머지는 아직 안 옮겼다
    }
}

// 끝내기 — 넷 다 아무 일도 안 한다(정답지 `End(_ *Object) {}`).
void Server::AIEnd(Creature& k, uint8_t act) { (void)k; (void)act; }

// 취소 — ★ **기다리기 둘만** 스스로 빠진다(`Cancel` 이 `MonsterPopAction`).
//   변신·떨구기·찾기는 취소돼도 가만히 있는다.
//   ★ 바라보기 넷(25·26·27·28)도 스스로 빠진다 — 등록표가 `Cancel: nox_ai_action_pop_532100`
//     을 달아 두었다(`legacy/unit_ai.go:57~60`).
void Server::AICancel(Creature& k, uint8_t act) {
    if (act == 1 || act == 2 || (act >= 25 && act <= 28)) AIPop(k);
}

// nox_xxx_mobActionDependency(`unit_ai.go:44~262`) — 쌓임에 붙은 **조건**을 **위에서부터** 본다.
//
// 조건은 할 일에 **붙어 다니는 꼬리표**다(갈래 40~71). 틀리면 그 조건과 **그것이 붙은 할 일까지**
// 한꺼번에 빼 버린다 — 「보이는 동안만 쫓아라」 같은 것을 이 한 줄로 만든다.
//
//   · 조건이 아니면 지나간다(그때 `또는` 묶음이 끊긴다)
//   · **40 `또는`** — 그 아래로 이어지는 조건들을 **하나만 맞아도** 되게 묶는다.
//     묶음의 끝(아래가 조건이 아닌 자리)에서 **하나도 안 맞았으면** 뺀다
//   · 틀리면 — `자리 < i` 가 되거나 쌓임이 **바닥 놀기**가 될 때까지 뺀다. 그러고 **맨 위부터 다시** 본다
//
// ⭕ 옮긴 조건(우리 상태로 충실히 잴 수 있는 것):
//   41 때 · 42 살아 있나 · 45 보이나 · 46 안 보이나 · 49 더 머나 · 50 더 가까운가 ·
//   51 자리가 더 머나 · 52 자리가 더 가까운가 · 61 안 끊기나(늘 참) · 62 마법이 걸렸나 · 71 안 움직였나
// ⚠ 안 옮긴 조건 — 43·44 맞고 있나(피격 기록) · 47 사선이 막혔나 · 48 보이는 자리인가 ·
//   53~58 보이는 적·친구·먹이(몬스터 시야 목록이 아직 없다) · 59 소리 · 60 새 적 ·
//   63·66 적 거리 · 64 성치 않나 · 65 기력 · 67 커서 아래 · 68 몰렸나 · 69 안전한 자리 · 70 답답한가.
//   **이들은 늘 참으로 둔다** — 없는 것을 거짓으로 만들면 멀쩡한 할 일이 사라진다.
void Server::AIProcDeps(Creature& k, std::vector<OutPacket>& out) {
    bool isOR = false;
    bool okPrev = false;
    for (int i = k.aiStackInd; i >= 0; --i) {
        bool ok = true;
        Creature::AIStackItem& st = k.aiStack[i];
        const uint8_t typ = st.action;
        if (!AIIsCondition(typ)) { isOR = false; continue; }

        const Target targ{ st.targetAddr, st.targetPet };
        float tx = 0, ty = 0;
        const bool hasT = TargetXY(targ, tx, ty);
        const float dx = static_cast<float>(k.x) - st.argX;
        const float dy = static_cast<float>(k.y) - st.argY;

        switch (typ) {
            case 40:                                         // 또는
                isOR = true;
                okPrev = false;
                continue;
            case 41: ok = st.argU32 > frame_; break;         // 때 — 그 프레임 전까지만
            case 42: ok = hasT && TargetHealth(targ) > 0; break;               // 살아 있나
            case 45: ok = hasT && CanInteractFrom(k, targ); break;             // 보이나
            case 46: ok = !(hasT && CanInteractFrom(k, targ)); break;          // 안 보이나
            case 47: {                                                          // 쏠 줄이 막혔나
                // DEPENDENCY_BLOCKED_LINE_OF_FIRE — **막혀 있는 동안 참**이다.
                //   원거리 사슬에서 49(더 머나)와 **또는(40)** 으로 묶여 「멀거나 막혔으면 다가간다」가 된다.
                //   ⚠ 안 옮겼을 때는 `default: 늘 참`이라 **그 묶음이 절대 안 깨져** 임프가
                //     사거리를 지나쳐 몸에 붙을 때까지 걸어갔다(2026-09-15 6-g(3b) 에서 잡힘)
                if (!hasT) { ok = false; break; }
                ok = !MapTraceVision(static_cast<float>(k.x), static_cast<float>(k.y), tx, ty);
                break;
            }
            case 49: {                                                          // 더 머나
                if (!hasT) { ok = false; break; }
                const float ex = static_cast<float>(k.x) - tx, ey = static_cast<float>(k.y) - ty;
                ok = std::sqrt(ex * ex + ey * ey) > st.argF;
                break;
            }
            case 50: {                                                          // 더 가까운가
                if (!hasT) { ok = false; break; }
                const float ex = static_cast<float>(k.x) - tx, ey = static_cast<float>(k.y) - ty;
                ok = std::sqrt(ex * ex + ey * ey) <= st.argF;
                break;
            }
            case 51: ok = std::sqrt(dx * dx + dy * dy) >= st.argF; break;      // 자리가 더 머나
            case 52: ok = std::sqrt(dx * dx + dy * dy) <= st.argF; break;      // 자리가 더 가까운가
            case 62: ok = k.buffs.Has(static_cast<int>(st.argU32)); break;     // 마법이 걸렸나
            case 71: ok = (k.x == k.prevX && k.y == k.prevY); break;           // 안 움직였나
            default: ok = true; break;   // ⚠ 안 옮긴 조건 — 늘 참(없는 것을 거짓으로 만들지 않는다)
        }

        if (!isOR) {
            if (ok) continue;
            // 틀렸다 — 아래에서 뺀다
        } else {
            if (ok) okPrev = true;
            if (i > 0 && AIIsCondition(k.aiStack[i - 1].action)) continue;   // 묶음이 아직 안 끝났다
            if (okPrev) continue;                                            // 하나라도 맞았다
        }
        for (;;) {
            AIPop(k);
            if (k.aiStackInd < i || (k.aiStackInd == 0 && k.aiStack[0].action == 0)) break;
        }
        i = k.aiStackInd + 1;   // 맨 위부터 다시 본다(다음 돎에서 −1 되어 맨 위가 된다)
        AIActionReset(k);
    }
    (void)out;
}

// 소환수에서 대상까지 볼 수 있나. `CanInteract` 는 사람 기준이라 소환수 자리로 한 번 더 싸맨다.
//   ⚠ 지금은 **주인의 눈**으로 갈음한다 — 소환수 눈으로 보는 시야 추적은 6-f 에서.
bool Server::CanInteractFrom(const Creature& k, const Target& t) const {
    auto o = sessions_.find(k.owner);
    if (o == sessions_.end()) return true;
    return CanInteract(o->second, t);
}

// ── 묶음 5-h 금 ───────────────────────────────────────────────────────
//
// sub_4D8870(`GAME3_2.c:4150`) — `4a <금 u32>` **5바이트**. `netSendPacket0(…, 1)` 이라 **중요(cc)** 다.
//   ⚠ 충전 알림(`64`)과 다르다 — 그쪽은 마지막 인자가 0 이라 중요가 아니었다.
void Server::SendGold(Session& s, std::vector<OutPacket>& out) {
    uint8_t msg[5];
    msg[0] = static_cast<uint8_t>(Op::REPORT_GOLD);
    msg[1] = static_cast<uint8_t>(s.gold);
    msg[2] = static_cast<uint8_t>(s.gold >> 8);
    msg[3] = static_cast<uint8_t>(s.gold >> 16);
    msg[4] = static_cast<uint8_t>(s.gold >> 24);
    EmitImportantRaw(s, msg, sizeof(msg), out);
}

// playerAddGold_4FA590(`GAME4.c:1862`) — 갱신자료 `+2164` 에 더하고 알린다.
void Server::PlayerAddGold(Session& s, uint32_t v, std::vector<OutPacket>& out) {
    s.gold += v;
    SendGold(s, out);
}

// playerSubGold_4FA5D0(`GAME4.c:1870`) — 뺀다. ★ **모자라면 0 으로 내린다**(음수로 안 간다).
void Server::PlayerSubGold(Session& s, uint32_t v, std::vector<OutPacket>& out) {
    s.gold = (s.gold >= v) ? (s.gold - v) : 0;
    SendGold(s, out);
}

// ── 묶음 5-e 충전 완드 ────────────────────────────────────────────────
//
// netReportCharges_4D82B0(`GAME3_2.c:3830`) — `64 <번호 2> <지금> <최대>` **5바이트**.
//   ⭕ 짜임은 디컴파일 그대로다. `netSendPacket1(…, 0, 0)` 이라 **중요(cc)가 아니다**.
void Server::SendCharges(Session& s, const Session::Carried& c, std::vector<OutPacket>& out) {
    uint8_t msg[5];
    msg[0] = static_cast<uint8_t>(Op::REPORT_CHARGES);
    msg[1] = static_cast<uint8_t>(c.netCode);
    msg[2] = static_cast<uint8_t>(c.netCode >> 8);
    msg[3] = c.charges;
    msg[4] = c.chargesMax;
    Emit(s, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);
}

// useWandCastSpell_53F4F0(`GAME4_3.c:7934`) — 완드로 주문을 건다.
//   ★ 이것이 묶음 4 에서 「충전 지팡이로 걸면 마나 대신 충전을 쓴다」로 미뤄 둔 바로 그 길이다.
//     **마나를 아예 안 본다** — 주문을 곧장 `spellAccept` 로 넣는다(세기 4 고정).
//   · **최대 != 0 이고 지금 == 0** 이면 못 쓴다(소리 222 — 묶음 10)
//   · 재사용 대기 — `지금 프레임 − 마지막 − (대기 − 2×준비효과)`. ⚠ 준비효과(아이템 수식)는 아직 없어 0
//   · 주문이 받아들여져야 **마지막 프레임을 갱신하고 충전을 깎는다**(실패하면 안 깎는다)
//   · 최대 != 0 일 때만 깎고 `REPORT_CHARGES` 를 보낸다(= 0 은 무한 완드)
// ⚠ 안 옮긴 갈래 — 정답지는 `a2[2] & 0x1000 && a2[3] & 0x4040000` 이면 **안 깎는다**.
//   그 두 비트가 무엇인지 못 짚어(오브젝트 깃발·클래스로 보이나 확증 없음) 늘 깎는 쪽으로 두었다.
bool Server::UseWandCast(Session& s, Session::Carried& c, std::vector<OutPacket>& out) {
    if (c.wandSpell == 0) return false;
    if (c.chargesMax != 0 && c.charges == 0) return false;    // 다 썼다
    // ⚠ 「아직 안 썼다」를 따로 안 둔다 — 정답지도 `+104` 가 0 인 채로 `지금 − 0 >= 대기` 를 본다.
    //   즉 **판이 시작하고 대기만큼 지나야 첫 발이 나간다.** 그게 원본 버릇이다.
    if (frame_ - c.wandLastFrame < c.wandCooldown) return false;

    SpellArg arg;
    // ★ 겨눈 자리는 커서, **대상은 커서가 집은 것**. 집은 것이 없으면 **시전자 자신**이다
    //   (`GAME4_3.c:7967~7972` — `+288` 이 0 이면 `v11[0] = a1`). 이걸 빼면 방패처럼
    //   대상이 있어야 도는 주문이 완드로는 영영 안 걸린다.
    arg.obj = FindObjectAtCursor(s);
    if (arg.obj.none()) arg.obj = Target{ s.addr, -1 };
    arg.x = static_cast<float>(s.aimX);
    arg.y = static_cast<float>(s.aimY);
    if (!SpellAccept(c.wandSpell, s, s.addr, arg, /*lvl=*/4, out)) return false;

    c.wandLastFrame = frame_;
    if (c.chargesMax != 0) {
        --c.charges;
        SendCharges(s, c, out);
    }
    return false;   // ★ 완드는 **안 없어진다** — 충전만 준다
}

// ── 묶음 5-d 먹을거리·버섯·사이다 ─────────────────────────────────────
//
// useConsume_53EE10(`object_use.go:13~55`) — 먹는 것.
//   · 먹을거리(CLASS 에 FOOD)가 아니면 아무 일도 안 난다
//   · **물약 갈래(`POTION`)는 여기서 빠진다** — 그쪽은 `usePotion` 이 맡는다
//   · **체력이 가득이면 안 먹는다**(그대로 남는다)
//   · `USE` 값만큼 체력을 움직이고 없어진다
//     ★ 값이 **음수인 것이 있다** — thing.bin 실측 `USE = ConsumeUse -15`.
//       먹으면 깎이는 음식이다. 「회복량」이라고 부르면 이 갈래를 놓친다
// ⚠ 안 옮긴 것 — 남녀·먹을거리 갈래별 소리(`SoundHumanFemaleEatApple` 따위)는 묶음 10.
bool Server::UseConsume(Session& s, const Session::Carried& c, std::vector<OutPacket>& out) {
    if (!c.isFood) return false;                             // CLASS 에 FOOD 가 아님
    if (c.foodBits & kFoodPotion) return false;              // 물약은 usePotion 담당
    if (c.useAmount == 0) return false;                      // UseData 없음
    if (s.health >= s.maxHealth) return false;               // 가득이면 안 먹는다
    UnitAdjustHP(Target{ s.addr, -1 }, c.useAmount, out);
    return true;
}

// useMushroom_53ECE0(`object_use.go:57~70`) — 버섯.
//   · 독이 있으면 **풀고** 글 `Use.c:MushroomClean` · 없으면 글 `Use.c:MushroomConfuse`
//   · ★ **어느 쪽이든 10초 혼란(세기 5)** 이 걸린다. 독을 풀어도 어지럽다
//   · 늘 없어진다(체력이 가득이어도)
bool Server::UseMushroom(Session& s, std::vector<OutPacket>& out) {
    if (s.poison != 0) {
        RemovePoison(Target{ s.addr, -1 }, out);
        SendPriMsg(s, "Use.c:MushroomClean", out);
    } else {
        SendPriMsg(s, "Use.c:MushroomConfuse", out);
    }
    ApplyEnchant(Target{ s.addr, -1 }, 3, 10 * static_cast<int>(kServerTickRate), 5);   // ENCHANT_CONFUSED
    return true;
}

// useCiderConfuse_53EF00(`object_use.go:72~85`) — 사이다.
//   · **5초 혼란(세기 4)** + 글 `Use.c:CiderConfuse`
//   · 그러고 나서 먹기(`useConsume`)를 부른다 — 체력이 가득이면 안 채워지지만
//     ★ **사이다는 그래도 없어진다**(정답지가 `useConsume` 의 답과 무관하게 지운다)
bool Server::UseCiderConfuse(Session& s, const Session::Carried& c, std::vector<OutPacket>& out) {
    ApplyEnchant(Target{ s.addr, -1 }, 3, 5 * static_cast<int>(kServerTickRate), 4);    // ENCHANT_CONFUSED
    SendPriMsg(s, "Use.c:CiderConfuse", out);
    UseConsume(s, c, out);
    return true;
}

// ── 묶음 5-c 줍는 순간 마시기 ─────────────────────────────────────────
//
// playerClassCanUseItem_57B3D0 + sub_57B370(`object_pickup.go:107~131`) —
//   **그 직업이 이 물건을 쓸 수 있나.** 먹을거리 갈래는 표가 아니라 **셈**으로 나온다:
//     `허용 = ~(SUBCLASS >> 5) | 0xFE`  →  `(1 << 직업) & 허용` 이 0 이 아니면 쓸 수 있다
//   `SUBCLASS >> 5` 의 맨 아랫비트가 **MANA_POTION**(갈래표 다섯째)이다. 그래서
//     · 마나 물약이면 허용 = 0xFE → 워리어(1 << 0 = 1)만 걸러진다
//     · 그 밖이면  허용 = 0xFF → 누구나 쓴다
//   ★ 즉 이 셈 한 줄이 뜻하는 것은 **「워리어는 마나 물약을 못 든다」** 하나다.
// ⚠ 무기·완드·방어구 갈래는 **수식표**(`Modif`)를 봐야 한다 — 그 표가 아직 없어 안 옮겼다.
//   여기(`PotionPickup`)로 오는 것은 먹을거리뿐이라 이 조각에서는 걸리지 않는다.
bool Server::ClassCanUseFood(CharClass cls, uint32_t foodBits) {
    const uint32_t allow = (~(foodBits >> 5) | 0xFEu) & 0xFFu;
    return ((1u << static_cast<int>(cls)) & allow) != 0;
}

// pickupPotion_4F37D0(`object_pickup.go:133~181`) — 물약을 주울 때.
//   돌려주는 값 — **−1 못 줍는다 · 0 그냥 가방으로 · 1 그 자리에서 마셨다**
//
// ★ 가방에서 쓸 때(`UsePotion`)와 **문턱이 다르다**:
//     가방  — `지금 < 최대`         (모자라기만 하면 마신다)
//     줍기  — `회복량 + 지금 < 최대` (**넘칠 것 같으면 안 마시고 가방으로 간다**)
//   아까운 물약을 버리지 않게 하는 규칙이다. 둘을 같게 만들면 가득 차기 직전에
//   50 짜리를 주워 1 만 채우고 날리게 된다.
//
// ⚠ 안 옮긴 것 — 버프 물약 갈래는 **줍기에는 없다**(그냥 가방으로) · 소리는 묶음 10 ·
//   `CheckXxx`(사람마다 켜지는 비트 `playersXxx`)는 무엇을 뜻하는지 못 짚어 안 옮겼다.
//   그 비트가 켜지면 줍는 순간 **아무것도 안 마시고** 바로 가방으로 간다.
int Server::PickupPotion(Session& s, const WorldStatic& w, std::vector<OutPacket>& out) {
    // ① 직업 검사 — **온라인이고 퀘스트가 아닐 때만**(gameFlags 0x2000 && !0x1000).
    //   우리 서버는 늘 온라인 아레나라 퀘스트 갈래만 빼면 된다.
    const bool quest = (questTotal_ > 0 || questStage_ > 0);
    if (!quest && !ClassCanUseFood(s.charClass, w.foodBits)) {
        SendPriMsg(s, "pickup.c:ObjectEquipClassFail", out);
        return -1;
    }

    bool consumed = false;
    // ② 체력 — **회복량까지 더해도 최대 아래**일 때만 마신다
    if ((w.foodBits & kFoodHealthPotion) && w.useAmount > 0) {
        const int dhp = static_cast<int>(w.useAmount * ClassHealthMult(s.charClass));
        if (dhp + static_cast<int>(s.health) < static_cast<int>(s.maxHealth)) {
            UnitAdjustHP(Target{ s.addr, -1 }, dhp, out);
            consumed = true;
        }
    }
    // ③ 마나 — 같은 꼴
    if ((w.foodBits & kFoodManaPotion) && w.useAmount > 0) {
        const int dmp = static_cast<int>(w.useAmount * ClassManaMult(s.charClass));
        if (dmp + static_cast<int>(s.mana) < static_cast<int>(s.maxMana)) {
            PlayerManaAdd(s, dmp);
            consumed = true;
        }
    }
    // ④ 독 해제 — 독이 있으면 **바로 풀고 끝난다**(정답지가 여기서 곧장 돌아간다)
    if ((w.foodBits & kFoodCurePoisonPotion) && s.poison != 0) {
        RemovePoison(Target{ s.addr, -1 }, out);
        return 1;
    }
    return consumed ? 1 : 0;
}

// ── 묶음 5-b 물약 쓰기 ────────────────────────────────────────────────
//
// usePotion_53EF70(`object_use.go:115~180`) — 가방에서 물약을 쓸 때.
//   ★ 갈래는 **`SUBCLASS` 비트**가 정한다(`FoodClassBits`). 한 물약이 여러 갈래를 겸할 수 있고
//     정답지도 `if` 를 줄줄이 늘어놓아 **겹치면 다 적용한다**.
//   · 죽었으면 아무 일도 안 난다
//   · 체력 물약 — **지금 체력 < 최대**일 때만. 양 = `USE` 값 × **직업 배수**
//   · 마나 물약 — **지금 마나 < 최대**일 때만. 양 = `USE` 값 × **직업 배수**
//   · 독 해제 물약 — 독이 있을 때만 푼다
//   · 버프 물약 **여덟** — 빠르게(9) · 투명(0) · 불 보호(17) · 전기 보호(20) · 독 보호(18) ·
//     무적(23) · 어둠보기(21) · 흡혈(13). 전부 **120초 · 세기 3**
//   · 방패 물약 — **주문 51**(SPELL_SHIELD)을 `USE` 값을 세기로 걸음
//     ⚠ 26 은 **버프 번호**(ENCHANT_SHIELD)지 주문 번호가 아니다 — 둘을 헷갈리면 엉뚱한 주문이 나간다
//   · 하나라도 걸렸으면 물약이 없어진다. 아무것도 안 걸렸으면 **그대로 남는다**
//
// ⚠ 안 옮긴 것 — 소리(RestoreHealth·RestoreMana·CurePoison) 는 묶음 10.
bool Server::UsePotion(Session& s, const Session::Carried& c, std::vector<OutPacket>& out) {
    if (s.respawnAtFrame != 0) return false;                 // 죽은 사람은 못 쓴다
    const uint32_t fb = c.foodBits;
    bool consumed = false;

    // 체력 — 가득 차 있으면 **안 마신다**(정답지가 여기서 `consumed` 를 안 켠다)
    if ((fb & kFoodHealthPotion) && c.useAmount > 0 && s.health < s.maxHealth) {
        const int dhp = static_cast<int>(c.useAmount * ClassHealthMult(s.charClass));
        UnitAdjustHP(Target{ s.addr, -1 }, dhp, out);
        consumed = true;
    }
    // 마나 — 같은 꼴
    if ((fb & kFoodManaPotion) && c.useAmount > 0 && s.mana < s.maxMana) {
        const int dmp = static_cast<int>(c.useAmount * ClassManaMult(s.charClass));
        PlayerManaAdd(s, dmp);
        consumed = true;
    }
    // 독 해제 — **독이 있을 때만**
    if ((fb & kFoodCurePoisonPotion) && s.poison != 0) {
        RemovePoison(Target{ s.addr, -1 }, out);
        consumed = true;
    }
    // 버프 여덟 — 전부 120초 · 세기 3(`object_use.go:147~163`)
    {
        static const struct { uint32_t bit; int buff; } kBuffs[] = {
            { kFoodHastePotion,           9 },               // ENCHANT_HASTED
            { kFoodInvisibilityPotion,    0 },               // ENCHANT_INVISIBLE
            { kFoodFireProtectPotion,    17 },               // ENCHANT_PROTECT_FROM_FIRE
            { kFoodShockProtectPotion,   20 },               // ENCHANT_PROTECT_FROM_ELECTRICITY
            { kFoodPoisonProtectPotion,  18 },               // ENCHANT_PROTECT_FROM_POISON
            { kFoodInvulnerabilityPotion,23 },               // ENCHANT_INVULNERABLE
            { kFoodInfravisionPotion,    21 },               // ENCHANT_INFRAVISION
            { kFoodVampirismPotion,      13 },               // ENCHANT_VAMPIRISM
        };
        for (const auto& b : kBuffs) {
            if (!(fb & b.bit)) continue;
            ApplyEnchant(Target{ s.addr, -1 }, b.buff, 120 * static_cast<int>(kServerTickRate), 3);
            consumed = true;
        }
    }
    // 방패 물약 — `USE` 값이 **세기**다
    if ((fb & kFoodShieldPotion) && c.useAmount > 0) {
        SpellArg arg;
        arg.obj = Target{ s.addr, -1 };
        arg.x = static_cast<float>(s.posX);
        arg.y = static_cast<float>(s.posY);
        SpellAccept(/*SPELL_SHIELD=*/51, s, s.addr, arg, c.useAmount, out);
        consumed = true;
    }
    return consumed;
}

// ── 묶음 4-f 소환 ──────────────────────────────────────────────────────
//
// SummonSize(`object_monster.go:652`) — thing.bin 몬스터 갈래로 정한다.
//   `SMALL_MONSTER` 1 · `MEDIUM_MONSTER` 2 · `LARGE_MONSTER` 4. 모르면 1.
int Server::SummonSizeOf(uint16_t type) const {
    const auto tb = typeBodies_.find(type);
    if (tb == typeBodies_.end()) return 1;
    const uint32_t sub = tb->second.subclass;
    if (sub & 0x4) return 4;                                 // LARGE_MONSTER
    if (sub & 0x2) return 2;                                 // MEDIUM_MONSTER
    return 1;                                                // SMALL_MONSTER(또는 모름)
}

// countControlledCreatures_500D10 — 그 사람이 거느린 것들의 **크기 합**.
int Server::CountControlledCreatures(const std::string& owner) const {
    int n = 0;
    for (const auto& k : pets_) {
        if (k.owner != owner) continue;
        if (k.health == 0) continue;
        n += SummonSizeOf(k.type);
    }
    return n;
}

// summonStart_500DA0(`GAME4.c:4060`) — 켤 때
//   · 시전자가 없거나 죽었으면, 또는 **글리프가 건 것**이면 취소
//   · **거느린 크기 합 + 이번 크기 > 4** 면 글(`Summon.c:CreatureControlFailed`)을 내고 취소
//   · 걸리면 **크기별 시전 시간**을 잡는다 — `SummonDuration`(아레나 60·210·375) 을
//     작은 것 [0] · 중간 [1] · 큰 것 [2] 로 쓴다
//   ⚠ 안 옮긴 것 — 퀘스트의 「길잡이(Guide)가 있어야 부른다」 검사 · 부르는 자리 고르기(sub_500F40)는 앞 자리로 갈음
int Server::DurSummonCreate(DurSpell& d, std::vector<OutPacket>& out) {
    auto c = sessions_.find(d.caster);
    if (c == sessions_.end() || !c->second.inGame) return 1;
    if (c->second.respawnAtFrame != 0) return 1;
    if (d.flag20) return 1;                                  // 글리프가 건 것은 안 된다
    const uint16_t type = SummonType(d.spell);
    if (type == 0) return 1;
    const int size = SummonSizeOf(type);
    if (CountControlledCreatures(d.caster) + size > kSummonLimit) {
        SendPriMsg(c->second, "Summon.c:CreatureControlFailed", out);
        return 1;
    }
    const int idx = (size >= 4) ? 2 : (size >= 2 ? 1 : 0);
    d.frame68 = frame_ + static_cast<uint32_t>(BalanceFloatInd("SummonDuration", idx));
    return 0;
}

// summonFinish_5010D0(`GAME4.c:4230`) — 그 프레임이 되면 **한 마리** 세우고 끝난다.
//   ★ 한도를 **여기서 다시 본다** — 부르는 동안 다른 것을 부르면 넘칠 수 있다.
int Server::DurSummonTick(DurSpell& d, std::vector<OutPacket>& out) {
    auto c = sessions_.find(d.caster);
    if (c == sessions_.end() || !c->second.inGame) return 1;
    if (c->second.respawnAtFrame != 0) return 1;
    if (d.frame68 - 1 != frame_) return 0;                   // 아직 부르는 중
    const uint16_t type = SummonType(d.spell);
    if (type == 0) return 1;
    const int size = SummonSizeOf(type);
    if (CountControlledCreatures(d.caster) + size > kSummonLimit) {
        SendPriMsg(c->second, "Summon.c:CreatureControlFailed", out);
        return 1;
    }
    Session& s = c->second;
    const double rad = s.facing * 3.14159265358979 / 128.0;  // 시전자 앞
    SpawnCreature(type, static_cast<float>(s.posX + kPetReach * std::cos(rad)),
                  static_cast<float>(s.posY + kPetReach * std::sin(rad)), d.caster);
    if (!pets_.empty()) pets_.back().facing = s.facing;
    return 1;
}

// ── 묶음 4-c1 밀기·당기기·땅흔들기 ─────────────────────────────────────
//
// mapPushUnitsAround_52E040(`GAME4_2.c:10477`) + unitPushAroundFn_52E0E0(`:10507`)
//   가운데에서 `outer` 안에 있는 **움직일 수 있는** 것마다 힘을 준다. 힘은 **×10** 해서 넣는다(정답지 `a3a[3] = a4 * 10.0`).
//   · 가운데→그것 **선 추적(0)** 이 막히면 안 민다(벽 너머로는 안 밀린다)
//   · 거리 d = √(dx²+dy²) **+ 0.1**(0 나눗셈 막기) · d > outer 면 건너뜀
//   · d ≤ inner 면 힘 그대로 · 그 밖은 `(1 − (d−inner)/(outer−inner))` 로 줄어든다
//   · 실제로 더하는 것은 **힘 ÷ 질량** 을 방향으로 나눈 것 — ForceVec 에 **더한다**(덮어쓰지 않는다)
//   ⚠ outer 가 inner 보다 작으면 inner 를 outer 로 쓴다(정답지 a3a[1] 고르기)
void Server::PushUnitsAround(float cx, float cy, float outer, float inner, float power) {
    if (outer < inner) outer = inner;
    const float force = power * 10.0f;
    auto apply = [&](float ox, float oy, float mass, double& fx, double& fy) {
        if (!MapTraceRay(cx, cy, ox, oy, 0x0)) return;       // 벽 너머로는 안 민다
        const float dx = ox - cx, dy = oy - cy;
        const float d = std::sqrt(dx * dx + dy * dy) + 0.1f;
        if (d > outer) return;
        const float f = (d > inner && outer > inner)
                      ? (1.0f - (d - inner) / (outer - inner)) * force
                      : force;
        const float per = f / (mass > 0.0f ? mass : 1.0f);
        fx += static_cast<double>(per * dx / d);             // ForceVec 에 **더한다**
        fy += static_cast<double>(per * dy / d);
    };
    for (auto& kv : sessions_) {
        Session& o = kv.second;
        if (!o.inGame || o.respawnAtFrame != 0) continue;    // 죽은 것은 안 밀린다(isObjectMovable 깃발 갈래)
        apply(static_cast<float>(o.posX), static_cast<float>(o.posY), static_cast<float>(kPlayerMass), o.forceX, o.forceY);
    }
    for (auto& k : pets_) {
        const auto tb = typeBodies_.find(k.type);
        const float mass = (tb != typeBodies_.end()) ? tb->second.mass : 1.0f;
        double fx = 0, fy = 0;
        apply(static_cast<float>(k.x), static_cast<float>(k.y), mass, fx, fy);
        // ⚠ 소환수는 아직 ForceVec 적분이 없다(걸음을 AI 가 직접 옮긴다) — 힘을 자리로 바로 옮겨 둔다
        k.x += fx;
        k.y += fy;
    }
}

// castPull_52BFA0(`GAME4_2.c:9337`) · castPush_52C000(`:9350`) — 시전자 자리에서 둘레 600·안쪽 10 로 민다(당기기는 **음수**)
int Server::CastPushPull(const Session& caster, int lvl, bool pull) {
    const double coeff = BalanceFloat(pull ? "PullPowerCoeff" : "PushPowerCoeff");
    const float power = static_cast<float>(pull ? -(coeff * lvl) : (coeff * lvl));
    PushUnitsAround(static_cast<float>(caster.posX), static_cast<float>(caster.posY), 600.0f, 10.0f, power);
    return 1;                                                // 소리는 10
}

// earthquakeSend_4D9110(`GAME3_2.c:4579`) — 가운데에서 **300 안**(거리² < 90000)에 있는 사람마다
//   `97 <세기>` 2바이트를 Kind1(틱 목록)로. 세기는 `(1 − 거리²/90000) × jiggle` 로 멀수록 약해진다
void Server::SendJiggle(float cx, float cy, int jiggle, std::vector<OutPacket>& out) {
    for (auto& kv : sessions_) {
        Session& o = kv.second;
        if (!o.inGame) continue;
        const float dx = cx - static_cast<float>(o.posX), dy = cy - static_cast<float>(o.posY);
        const double d2 = static_cast<double>(dx) * dx + static_cast<double>(dy) * dy;
        if (d2 >= 90000.0) continue;
        const int v = static_cast<int>((1.0 - d2 / 90000.0) * static_cast<double>(jiggle));
        const uint8_t msg[2] = { static_cast<uint8_t>(Op::FX_JIGGLE), static_cast<uint8_t>(v) };
        Emit(o, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);
    }
}

// castEquake_52DE40(`GAME4_2.c:10418`) + equakeDamage_52DEC0(`:10437`) — 둘레 안에 **거리에 반비례하는** 피해(종류 11) + 화면 흔들기
//   피해 = EarthquakeDamage[세기−1] × (1 − 거리/EarthquakeRange) · 자기 자신·공중(0x4000)·체력 0 은 뺀다
int Server::CastEarthquake(Session& caster, int lvl, std::vector<OutPacket>& out) {
    const float range = static_cast<float>(BalanceFloat("EarthquakeRange"));
    const double base = BalanceFloatInd("EarthquakeDamage", lvl - 1);
    const float cx = static_cast<float>(caster.posX), cy = static_cast<float>(caster.posY);
    auto hurtAt = [&](const Target& t, float ox, float oy) {
        const float dx = ox - cx, dy = oy - cy;
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (dist > range) return;
        const double dmg = base * (1.0 - static_cast<double>(dist) / static_cast<double>(range));
        if (dmg <= 0.0) return;
        AttackSrc src;
        src.x = cx; src.y = cy;
        HurtTarget(t, static_cast<int>(dmg), out, caster.addr, 11, &src);   // 피해 종류 11
    };
    std::vector<std::string> who;
    for (const auto& kv : sessions_) {
        if (!kv.second.inGame || kv.first == caster.addr) continue;         // 자기 자신은 뺀다
        if (kv.second.respawnAtFrame != 0 || kv.second.health == 0) continue;
        who.push_back(kv.first);
    }
    for (const auto& addr : who) {
        auto it = sessions_.find(addr);
        if (it == sessions_.end()) continue;
        hurtAt(Target{ addr, -1 }, static_cast<float>(it->second.posX), static_cast<float>(it->second.posY));
    }
    for (size_t i = 0; i < pets_.size(); ++i) {
        if (pets_[i].health == 0) continue;
        hurtAt(Target{ std::string(), static_cast<int>(i) }, static_cast<float>(pets_[i].x), static_cast<float>(pets_[i].y));
    }
    const int jiggle = static_cast<int>(BalanceFloatInd("EarthquakeJiggle", lvl - 1));
    SendJiggle(cx, cy, jiggle, out);                         // 화면 흔들기 — 소리는 10
    return 1;
}

// ── 묶음 4-c2a 독 ────────────────────────────────────────────────────
//
// 자료 — +540 남은 양 · 체력자료 +16 시작 프레임 · +542 감쇠 계수기
uint8_t* Server::PoisonOf(const Target& t) {
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        return (it != sessions_.end() && it->second.inGame) ? &it->second.poison : nullptr;
    }
    if (t.pet < 0 || static_cast<size_t>(t.pet) >= pets_.size()) return nullptr;
    return &pets_[static_cast<size_t>(t.pet)].poison;
}

// netReportObjectPoison_4D7F40(`GAME3_2.c:3771`) — `da <번호2> <켜짐>` 4바이트 Packet0.
//   사람은 이 메시지가 아니라 **상태 비트 1024**(`6a`)로 알린다 — 소환수는 주인에게 이 메시지로.
void Server::SetPoisonAmount(const Target& t, int amount, std::vector<OutPacket>& out) {
    uint8_t* pp = PoisonOf(t);
    if (!pp) return;
    const bool wasZero = (*pp == 0);
    if (wasZero && amount > 0) {                            // 0 에서 켜질 때 시작 프레임을 적는다
        if (!t.addr.empty()) sessions_.at(t.addr).poisonStart = frame_;
        else pets_[static_cast<size_t>(t.pet)].poisonStart = frame_;
    }
    *pp = static_cast<uint8_t>(amount);
    if (!t.addr.empty()) {
        Session& s = sessions_.at(t.addr);
        // 상태 비트 1024 — 켜고 끔(관전 1 과 함께 싣는다)
        const uint32_t bits = (s.observer ? 1u : 0u) | (amount != 0 ? 1024u : 0u);
        BroadcastClientStatus(s.netCode, bits, out);
        s.poisonDecay = (amount != 0) ? 1000 : 0;
    } else {
        Creature& k = pets_[static_cast<size_t>(t.pet)];
        auto oi = sessions_.find(k.owner);                  // 주인에게만 알린다(+508)
        if (oi != sessions_.end() && oi->second.inGame) {
            const uint8_t msg[4] = { static_cast<uint8_t>(Op::REPORT_OBJECT_POISON),
                                     static_cast<uint8_t>(k.netCode), static_cast<uint8_t>(k.netCode >> 8),
                                     static_cast<uint8_t>(amount != 0 ? 1 : 0) };
            EmitImportantRaw(oi->second, msg, sizeof(msg), out);
        }
        k.poisonDecay = (amount != 0) ? 1000 : 0;
    }
}

// activatePoison_4EE7E0(`GAME3_3.c:7270`) — 독을 얹는다. 얹었으면 1
//   거절: 대상 없음 · **무적(23)** · 관전 중인 사람 · 몬스터 특정 갈래(+12 & 0x200)
//   ⚠ 저항 굴림(getPoisonDmg_4E0040 × 100)은 **입은 것에서 나온다** — 아이템 묶음. 지금은 저항 0 으로 둔다(지어내지 않는다)
//   새 양 = 지금 + add 이고 cap 을 넘지 않는다 · 지금이 이미 cap 을 넘으면 그대로 둔다
bool Server::ActivatePoison(const Target& t, int add, int cap, std::vector<OutPacket>& out) {
    uint8_t* pp = PoisonOf(t);
    if (!pp) return false;
    const Buffs* b = BuffsOf(t);
    if (b && b->Has(23)) return false;                      // 무적이면 안 걸린다
    if (!t.addr.empty()) {
        const Session& s = sessions_.at(t.addr);
        if (s.observer || s.respawnAtFrame != 0) return false;
    }
    const int cur = static_cast<int>(*pp);
    int next = cur;
    if (cur + add <= cap)      next = cur + add;
    else if (cur <= cap)       next = cap;
    if (next != cur) SetPoisonAmount(t, next, out);          // 소리 100 은 10
    return true;
}

// updatePoison_4EE8F0(`GAME3_3.c:7333`) — 양을 깎고, 0 이 되면 끈다(사람은 글 "PoisonFade")
void Server::UpdatePoison(const Target& t, int amount, std::vector<OutPacket>& out) {
    uint8_t* pp = PoisonOf(t);
    if (!pp) return;
    if (static_cast<int>(*pp) > amount) { *pp = static_cast<uint8_t>(*pp - amount); return; }
    SetPoisonAmount(t, 0, out);
    if (!t.addr.empty()) {
        sessions_.at(t.addr).poisonStart = 0;
        SendPriMsg(sessions_.at(t.addr), "Health.c:PoisonFade", out);
    } else {
        pets_[static_cast<size_t>(t.pet)].poisonStart = 0;
    }
}

// removePoison_4EE9D0(`GAME3_3.c:7375`) — 통째로 없앤다(해독 주문·물약)
void Server::RemovePoison(const Target& t, std::vector<OutPacket>& out) {
    uint8_t* pp = PoisonOf(t);
    if (!pp || *pp == 0) return;
    SetPoisonAmount(t, 0, out);
    if (!t.addr.empty()) sessions_.at(t.addr).poisonStart = 0;
    else pets_[static_cast<size_t>(t.pet)].poisonStart = 0;
}

// 독 틱(`server.go:175~201`) — 감쇠와 피해가 한자리에 있다
//   ① 감쇠 — +542 를 매 틱 줄이고 0 이 되면 **독 1 감소** 뒤 다시 1000(≈33초에 1)
//   ② 피해 — 시작 뒤 **60틱 유예**를 지나면, 독이 8 을 넘으면 매 틱 · 아니면 `128 >> (양−1)` 틱마다 1(종류 5)
void Server::PoisonTick(std::vector<OutPacket>& out) {
    std::vector<Target> list;
    for (const auto& kv : sessions_) if (kv.second.inGame && kv.second.poison) list.push_back(Target{ kv.first, -1 });
    for (size_t i = 0; i < pets_.size(); ++i) if (pets_[i].poison) list.push_back(Target{ std::string(), static_cast<int>(i) });
    for (const Target& t : list) {
        uint8_t* pp = PoisonOf(t);
        if (!pp || *pp == 0) continue;
        uint16_t* decay = nullptr;
        uint32_t start = 0;
        bool dead = false;
        if (!t.addr.empty()) {
            Session& s = sessions_.at(t.addr);
            decay = &s.poisonDecay; start = s.poisonStart; dead = (s.respawnAtFrame != 0);
        } else {
            Creature& k = pets_[static_cast<size_t>(t.pet)];
            decay = &k.poisonDecay; start = k.poisonStart;
        }
        if (decay && *decay > 0) {                          // ① 감쇠
            if (--*decay == 0) {
                if (!dead) UpdatePoison(t, 1, out);
                if (PoisonOf(t) && *PoisonOf(t)) *decay = 1000;
            }
        }
        uint8_t* pp2 = PoisonOf(t);
        if (!pp2 || *pp2 == 0) continue;
        const int amt = static_cast<int>(*pp2);
        if (TargetHealth(t) <= 0) continue;
        if (start != 0 && (frame_ - start) <= 60) continue;  // ② 시작 뒤 60틱은 안 아프다
        const bool hit = (amt > 8) || (frame_ % static_cast<uint32_t>(128u >> (amt - 1)) == 0);
        if (!hit) continue;
        HurtTarget(t, 1, out, std::string(), 5, nullptr);    // 피해 1 · 종류 5(독) · ⚠ 퀘스트는 2
    }
}

// castPoison_52C720(`GAME4_2.c:9567`) — 대상 없으면 0 · 독을 세기만큼(상한도 세기) 얹고 도운 사람 기록
int Server::CastPoison(const std::string& obj3, const Target& targ, int lvl, std::vector<OutPacket>& out) {
    if (!PoisonOf(targ)) return 0;
    ActivatePoison(targ, lvl, lvl, out);
    MarkAssist(obj3, targ);                                  // sub_4E7540
    return 1;
}

// ── 묶음 4-c3 — 주문이 놓는 오브젝트 ─────────────────────────────────
//
// 정답지는 `newObjectWithTypeInd(이름번호)` 로 만들고 `createAt_4DAA50` 로 자리에 놓은 뒤
// 수명을 건다(불꽃은 unitSetDecayTime, 독구름은 갱신자료 첫 칸). 우리는 한 목록(placed_)에 담는다.
Server::PlacedObj* Server::PlaceObject(uint16_t type, float x, float y, const std::string& owner, uint32_t life, uint8_t kind,
                                       uint8_t dmgAmount, uint8_t dmgType, float radius) {
    if (type == 0 || life == 0) return nullptr;
    PlacedObj o;
    o.netCode = nextShotNetCode_++;
    o.type = type;
    o.x = x; o.y = y;
    o.owner = owner;
    o.life = life;
    o.nextHitFrame = frame_;                                 // 놓자마자 한 번 판정한다(+136 초깃값 0)
    o.kind = kind;
    o.dmgAmount = dmgAmount;
    o.dmgType = dmgType;
    o.radius = radius;
    o.createdFrame = frame_;
    placed_.push_back(o);
    return &placed_.back();
}

void Server::DestroyPlaced(size_t i, std::vector<OutPacket>& out) {
    if (i >= placed_.size()) return;
    const uint16_t nc = placed_[i].netCode;
    const uint8_t msg[3] = { static_cast<uint8_t>(Op::DESTROY_OBJECT), static_cast<uint8_t>(nc), static_cast<uint8_t>(nc >> 8) };
    for (auto& kv : sessions_) if (kv.second.inGame) EmitImportantRaw(kv.second, msg, sizeof(msg), out);
    placed_.erase(placed_.begin() + static_cast<long>(i));
}

// updateToxicCloud_53D850(`GAME4_3.c:6855`) + sub_53D8C0(`:6872`)
//   · 판정 프레임(+136)이 지났으면 둘레 **75** 안을 훑어 사람·몬스터에게 **3~10 피해(종류 5)** ·
//     놓은 이의 적이면 **독 1(상한 1)** 도 얹는다 · 벽 뒤(선 추적 5)는 건너뛴다
//   · 다음 판정은 **5~10틱 뒤** 무작위 · 수명은 매 틱 하나씩 줄고 0 이면 지운다
void Server::UpdatePlaced(std::vector<OutPacket>& out) {
    for (size_t i = 0; i < placed_.size();) {
        PlacedObj& o = placed_[i];
        if (o.kind == 1 && frame_ >= o.nextHitFrame) {
            const float r2 = 75.0f * 75.0f;
            auto hit = [&](const Target& t, float tx, float ty) {
                const float dx = tx - o.x, dy = ty - o.y;
                if (dx * dx + dy * dy > r2) return;
                if (!MapTraceRay(tx, ty, o.x, o.y, 0x5)) return;     // 벽 뒤는 안 닿는다
                const int dmg = 3 + static_cast<int>(NextRandom(8));  // randomInt(3,10)
                HurtTarget(t, dmg, out, o.owner, 5, nullptr);         // 피해 종류 5(독)
                bool enemy = false;                                   // 놓은 이의 적이면 독도
                auto oi = sessions_.find(o.owner);
                if (oi != sessions_.end()) enemy = TargetIsEnemy(oi->second, t);
                if (enemy) ActivatePoison(t, 1, 1, out);
            };
            std::vector<std::string> who;
            for (const auto& kv : sessions_) if (kv.second.inGame && kv.second.respawnAtFrame == 0) who.push_back(kv.first);
            for (const auto& addr : who) {
                auto it = sessions_.find(addr);
                if (it == sessions_.end()) continue;
                hit(Target{ addr, -1 }, static_cast<float>(it->second.posX), static_cast<float>(it->second.posY));
            }
            for (size_t k = 0; k < pets_.size(); ++k)
                hit(Target{ std::string(), static_cast<int>(k) }, static_cast<float>(pets_[k].x), static_cast<float>(pets_[k].y));
            o.nextHitFrame = frame_ + 5 + NextRandom(6);              // randomInt(5,10)
        }
        // ── kind 2 — 닿음 피해(collideDamage_4E9430) ──────────────────
        //   몸이 겹친 틱마다 부른다. 피해 = **양 >> 1** · 단 양이 1 이면 홀수 프레임에만 1(짝수 틱은 0).
        //   ⚠ 갈래·팀을 안 가린다 — 불꽃 FLAGS 에 NO_COLLIDE_OWNER 가 없어 **시전자·같은 편도 탄다**
        //     (sub_548360 `v11 & 0x400 && unitsHaveSameTeam` 가지를 안 탄다).
        if (o.kind == 2) PlacedCollide(o, out);
        // ── kind 6 — 바깥으로 나가는 불꽃(updateFlameCleanse_53D510) ──────────
        //   ① 수명(+136)이 지났거나 ② **시작점에서 지금 자리까지 선 추적(65)이 막혔거나**
        //   ③ 만든 지 3틱이 넘었는데 **한 틱 동안 안 움직였으면** 사라진다.
        //   움직임은 감쇠 없이 속도를 그대로 더한다(+112 = 0).
        if (o.kind == 6) {
            const float px = o.x, py = o.y;
            o.x += o.velX;
            o.y += o.velY;
            const bool stuck = (frame_ - o.createdFrame > 3u) && (px == o.x) && (py == o.y);
            if (frame_ >= o.expireFrame || !MapTraceRay(o.originX, o.originY, o.x, o.y, 65) || stuck) {
                DestroyPlaced(i, out);
                continue;
            }
            PlacedCollide(o, out);
        }
        // ── kind 3 — 떨어지는 운석(MeteorUpdate = meteorExplode_53D6E0) ──────
        //   하는 일이 하나다: **높이(+104)가 0 이하면** 터뜨리고 지운다.
        //   높이는 시전 때 255 로 띄우고(unitRaise) z 속도를 −MeteorSpeed(8)로 준다.
        if (o.kind == 3) {
            o.z += o.zVel;
            if (o.z <= 0.0f) { MeteorExplode(o, out); DestroyPlaced(i, out); continue; }
        }
        // ── kind 5 — 떨어지는 주먹(FistUpdate 53D400 + FistCollide 4EADF0) ───
        //   높이 물리는 `updateFallLogic_51B870` 의 **0x800000 갈래**를 그대로 쓴다:
        //     높이 += z속도 · 높이 ≥ 0 이면 z속도 −= 0.5(중력) ·
        //     아니면 높이 = 0 · z속도 = −z속도 × (되튐 9.0) × 0.1 · 그래도 2 미만이면 멈춘다
        //   처음 땅에 닿을 때 **화면 흔들기 30** 을 보내고 「닿았음」을 적는다(+16 비트 31).
        //   닿음 피해는 **닿은 뒤에만** — 몸이 겹친 틱마다 `FistOfVengeanceDamage`(50·100·200·300·400) 종류 **2(짓눌림)**.
        //   수명은 3초(90틱).
        if (o.kind == 5) {
            o.z += o.zVel;
            if (o.z >= 0.0f) {
                o.zVel -= 0.5f;
            } else {
                o.z = 0.0f;
                o.zVel = -o.zVel * 9.0f * 0.1f;              // +116 = 9.0(되튐)
                if (o.zVel < 2.0f) { o.z = 0.0f; o.zVel = 0.0f; }
            }
            if (o.z <= 0.0f && !o.landed) {
                o.landed = true;
                SendJiggle(o.x, o.y, 30, out);               // earthquakeSend_4D9110(자리, 30)
            }
            if (o.z <= 0.0f && o.damage > 0) {
                const float reach = o.radius + static_cast<float>(kBodyRadius);
                const float rr = reach * reach;
                auto touch = [&](const Target& t, float tx, float ty) {
                    const float dx = tx - o.x, dy = ty - o.y;
                    if (dx * dx + dy * dy > rr) return;
                    HurtTarget(t, o.damage, out, o.owner, 2, nullptr);   // 종류 2 = 짓눌림
                };
                std::vector<std::string> who;
                for (const auto& kv : sessions_) if (kv.second.inGame && kv.second.respawnAtFrame == 0) who.push_back(kv.first);
                for (const auto& addr : who) {
                    auto it = sessions_.find(addr);
                    if (it == sessions_.end()) continue;
                    touch(Target{ addr, -1 }, static_cast<float>(it->second.posX), static_cast<float>(it->second.posY));
                }
                for (size_t k = 0; k < pets_.size(); ++k)
                    touch(Target{ std::string(), static_cast<int>(k) }, static_cast<float>(pets_[k].x), static_cast<float>(pets_[k].y));
            }
            if (frame_ - o.createdFrame > 3u * static_cast<uint32_t>(kServerTickRate)) { DestroyPlaced(i, out); continue; }
        }
        // ── kind 7 — 거미 낳는 자리(updateArachnaphobia_53DA60) ──────────────
        //   판정 프레임(+136)이 지나면 **SmallSpider 한 마리**를 제자리에 낳고 다음은 **1~5틱 뒤**.
        //   만든 지 **3초(90틱)** 가 넘으면 사라진다. ⚠ 낳은 뒤에도 검사하므로 마지막 틱에도 한 마리 더 나온다.
        if (o.kind == 7) {
            if (frame_ > o.nextHitFrame) {
                const float sx = o.x, sy = o.y;
                const std::string owner = o.owner;
                SpawnCreature(NamedType("SmallSpider"), sx, sy, owner);
                placed_[i].nextHitFrame = frame_ + 1 + NextRandom(5);   // randomInt(1,5)
            }
            if (frame_ - placed_[i].createdFrame > 3u * static_cast<uint32_t>(kServerTickRate)) {
                DestroyPlaced(i, out);
                continue;
            }
        }
        // ── kind 4 — 유성우 뿌리개(updateMeteorShower_53D5A0) ────────────────
        //   만든 뒤 **5초(150틱)** 동안 **4~8틱마다** 운석 하나를 무작위 자리에 떨군다.
        //   자리 = 방향 무작위(0~255) · 거리 = `무작위실수(4,12)`의 **제곱**(16~144).
        //   그 자리까지 선 추적(traceRay 5)이 막히면 그 번만 거른다. 5초가 지나면 스스로 지워진다.
        if (o.kind == 4) {
            if (frame_ - o.createdFrame >= 5u * static_cast<uint32_t>(kServerTickRate)) { DestroyPlaced(i, out); continue; }
            if (frame_ >= o.nextHitFrame) {
                // ⚠ 아래에서 `placed_` 에 새로 밀어 넣는다 — 벡터가 옮겨 앉으면 `o` 가 죽는다.
                //   그래서 쓸 값을 **먼저 복사**하고, 되쓸 때는 자리(i)로 다시 잡는다.
                const float ox = o.x, oy = o.y;
                const std::string owner = o.owner;
                const int dmg = o.damage;
                const int dir = static_cast<int>(NextRandom(256));
                const float r = 4.0f + NextRandomFloat() * 8.0f;         // randomFloat(4,12)
                const float d = r * r;                                   // ★ **제곱**이 거리다(16~144)
                const float mx = ox + d * DirVecX(dir);
                const float my = oy + d * DirVecY(dir);
                if (MapTraceRay(ox, oy, mx, my, 0x5)) {
                    if (PlacedObj* mo = PlaceObject(NamedType("Meteor"), mx, my, owner, kPlacedNoLife, /*kind=*/3)) {
                        mo->z = 255.0f;                                  // unitRaise_4E46F0(255)
                        mo->zVel = -static_cast<float>(BalanceFloat("MeteorSpeed"));
                        mo->damage = dmg;                                // 뿌리개가 든 값을 그대로
                    }
                }
                placed_[i].nextHitFrame = frame_ + 4 + NextRandom(5);    // randomInt(4,8)
            }
        }
        // ⚠ `o` 는 위 갈래에서 `placed_` 가 커졌으면 이미 죽은 참조다 — 자리(i)로 다시 잡는다
        if (placed_[i].life > 0 && placed_[i].life != kPlacedNoLife) --placed_[i].life;
        if (placed_[i].life == 0) { DestroyPlaced(i, out); continue; }
        ++i;
    }
}

// castToxicCloud_52DB60(`GAME4_2.c:10279`) — 시전자→커서 **선 추적**이 통하면 그 자리에 독구름을 놓는다
//   막히면 글(`a9` 코드 0 · 값 2 「거기엔 못 놓는다」)을 내고 **0**(받아들이지 않음)
//   수명 = ToxicCloudLifetime × 초당 틱
int Server::CastToxicCloud(Session& caster, const SpellArg& arg, std::vector<OutPacket>& out) {
    const float cx = static_cast<float>(caster.posX), cy = static_cast<float>(caster.posY);
    if (!MapTraceRay(cx, cy, arg.x, arg.y, 0x5)) {           // traceRay_5374B0
        InformSpell(caster, 0, 2, out);                      // netInformTextMsg(0, 2)
        return 0;
    }
    const uint16_t type = NamedType("ToxicCloud");
    const uint32_t life = static_cast<uint32_t>(BalanceFloat("ToxicCloudLifetime") * static_cast<double>(kServerTickRate));
    PlaceObject(type, arg.x, arg.y, caster.addr, life, /*kind=*/1);
    return 1;                                                // 소리는 10
}

// castBurn_52C3E0(`GAME4_2.c:9471`) — 시전자→커서 **선 추적**이 통하면 그 자리에 불꽃(MediumFlame)을 놓는다
//   막히면 글(코드 0 · 값 2)을 내고 **0** · 통하면 불꽃 + 불똥 터짐(반지름 64) + 소리
//   ★ 수명 = `BurnDuration`(= 300) **그대로가 틱이다** — 독구름과 달리 초당 틱을 **안 곱한다**
//     (`unitSetDecayTime_511660` 은 +136 에 `지금 프레임 + a2` 를 넣는다. 다른 부르는 자리는 전부 `gameFPS() × N` 인데
//      불꽃만 `float2int(BurnDuration)` 을 날로 넘긴다).
//   ⚠ 아직 안 옮긴 갈래 — 대상이 **글리프**면 선 추적 없이 글리프 자리에 놓는다. 글리프가 아직 없다(묶음 5).
int Server::CastBurn(Session& caster, const SpellArg& arg, std::vector<OutPacket>& out) {
    const float cx = static_cast<float>(caster.posX), cy = static_cast<float>(caster.posY);
    if (!MapTraceRay(cx, cy, arg.x, arg.y, 0x5)) {           // traceRay_5374B0
        InformSpell(caster, 0, 2, out);                      // netInformTextMsg(0, 2)
        return 0;
    }
    const uint16_t type = NamedType("MediumFlame");
    const uint32_t life = static_cast<uint32_t>(BalanceFloat("BurnDuration"));
    // thing.bin 의 MediumFlame — `COLLIDE = DamageCollide 2 DAMAGE_FLAME` · `EXTENT = CIRCLE 6`
    PlaceObject(type, arg.x, arg.y, caster.addr, life, /*kind=*/2, /*dmgAmount=*/2, /*dmgType=*/1, /*radius=*/6.0f);
    return 1;
}

// castFireball_52C790(`GAME4_2.c:9580`) — 앞으로 날리는 불덩이. **겨눈 주문이 아니다**(Magic 을 안 쓴다).
//   ★ 종류가 세기마다 다르다 — 정답지 표 `0x587000+258864+4×세기`:
//     1 Fireball · 2 StrongFireball · **3·4·5 모두 TitanFireball**
//   나는 자리 = 시전자 자리 + (몸 반지름 × 2) × 방향 + 시전자 속도.
//   그 자리까지 선 추적(5)이 막히면 **시전자 제자리에** 만든다(실패가 아니다 — 벽에 붙어 쏘면 제 발밑에서 터진다).
//   빠르기 = `FireballSpeedCoeff[세기-1]`(1.6·2.0·2.4·2.5·2.6) × 그 종류의 SPEED 기본값.
int Server::CastFireball(Session& caster, int lvl, std::vector<OutPacket>& out) {
    (void)out;
    if (lvl < 1) lvl = 1;
    if (lvl > 5) lvl = 5;
    static const char* const kName[6] = { "", "Fireball", "StrongFireball", "TitanFireball", "TitanFireball", "TitanFireball" };
    const ProjTypeInfo* pt = ProjType(kName[lvl]);
    if (!pt || pt->type == 0) return 1;                      // newObjectByTypeID 가 0 이면 정답지도 1 을 돌려준다
    const int dir = caster.facingPrev;                       // +124 Direction1
    const float px = static_cast<float>(caster.posX), py = static_cast<float>(caster.posY);
    const float reach = static_cast<float>(kBodyRadius) * 2.0f;   // +176 반지름 두 배
    float sx = static_cast<float>(static_cast<double>(reach) * DirVecX(dir) + px) + static_cast<float>(caster.velX);
    float sy = static_cast<float>(static_cast<double>(reach) * DirVecY(dir) + py) + static_cast<float>(caster.velY);
    if (!MapTraceRay(px, py, sx, sy, 0x5)) { sx = px; sy = py; }  // 막히면 제자리
    MissileObj m;
    m.netCode = nextShotNetCode_++;
    if (nextShotNetCode_ == 0) nextShotNetCode_ = 4096;
    m.type = pt->type;
    m.flags = pt->flags;
    m.posX = m.newX = m.prevX = sx;
    m.posY = m.newY = m.prevY = sy;
    m.dir1 = m.dir2 = static_cast<uint8_t>(dir);
    m.createdFrame = m.frame136 = frame_;
    m.speed = static_cast<float>(BalanceFloatInd("FireballSpeedCoeff", lvl - 1)) * pt->speed;
    m.owner = caster.addr;
    m.ud0 = m.ud2 = caster.addr;
    m.spell = 27;
    m.level = lvl;
    m.collideKind = 1;                                       // SparkExplosionCollide
    m.collideAmount = pt->collideAmount;
    m.velX = DirVecX(dir) * m.speed + static_cast<float>(caster.velX);
    m.velY = DirVecY(dir) * m.speed + static_cast<float>(caster.velY);
    missiles_.push_back(m);
    return 1;
}

// castMeteor_52D9D0(`GAME4_2.c:10192`) — 커서 자리 **하늘(높이 255)** 에 운석을 띄우고 떨어뜨린다.
//   ① 그 사람이 이미 띄운 운석이 있으면 **거절**하고 글(`ExecSpel.c:TooManyMeteors`)을 보낸다
//   ② 시전자→커서 선 추적(깃발 **73**, 협동 모드면 9)이 막히면 글(0·2)을 내고 0
//   ③ 갱신자료 첫 칸 = `MeteorDamage[세기-1]`(45·65·85·105·125) · z 속도 = **−MeteorSpeed(8)**
int Server::CastMeteor(Session& caster, const SpellArg& arg, int lvl, std::vector<OutPacket>& out) {
    for (const auto& p : placed_) {
        if (p.kind == 3 && p.owner == caster.addr) {
            SendPriMsg(caster, "ExecSpel.c:TooManyMeteors", out);   // netPriMsgToPlayer_4DA2C0
            return 0;
        }
    }
    const float cx = static_cast<float>(caster.posX), cy = static_cast<float>(caster.posY);
    if (!MapTraceRay(cx, cy, arg.x, arg.y, 73)) {
        InformSpell(caster, 0, 2, out);
        return 0;
    }
    if (PlacedObj* mo = PlaceObject(NamedType("Meteor"), arg.x, arg.y, caster.addr, kPlacedNoLife, /*kind=*/3)) {
        mo->z = 255.0f;                                      // unitRaise_4E46F0(255)
        mo->zVel = -static_cast<float>(BalanceFloat("MeteorSpeed"));
        mo->damage = static_cast<int>(BalanceFloatInd("MeteorDamage", lvl - 1));
    }
    return 1;
}

// castMeteorShower_52D8A0(`GAME4_2.c:10112`) — 커서 자리에 **뿌리개**(보이지 않는 MeteorShower)를 놓는다.
//   선 추적은 운석과 같다. 갱신자료 첫 칸에 같은 `MeteorDamage[세기-1]` 을 담아 떨어뜨릴 운석에 물려준다.
//   ⚠ 운석과 달리 **「하나만」 제한이 없다**(정답지에 그 검사가 없다).
int Server::CastMeteorShower(Session& caster, const SpellArg& arg, int lvl, std::vector<OutPacket>& out) {
    const float cx = static_cast<float>(caster.posX), cy = static_cast<float>(caster.posY);
    if (!MapTraceRay(cx, cy, arg.x, arg.y, 73)) {
        InformSpell(caster, 0, 2, out);
        return 0;
    }
    if (PlacedObj* so = PlaceObject(NamedType("MeteorShower"), arg.x, arg.y, caster.addr, kPlacedNoLife, /*kind=*/4))
        so->damage = static_cast<int>(BalanceFloatInd("MeteorDamage", lvl - 1));
    return 1;
}

// newObjectWithTypeInd_4E3450 + createAt_4DAA50 — 몬스터 하나를 그 자리에 만든다.
//   ⚠ 움직임·AI 는 묶음 6·7 이다. 여기서는 **나오는 것까지**가 산출물이다.
void Server::SpawnCreature(uint16_t type, float x, float y, const std::string& owner) {
    if (type == 0) return;
    Creature k;
    k.owner = owner;
    k.type = type;
    k.netCode = nextShotNetCode_++;
    if (nextShotNetCode_ == 0) nextShotNetCode_ = 4096;
    k.x = x;
    k.y = y;
    k.health = static_cast<uint16_t>(TuningFor(type).health);
    pets_.push_back(k);
}

// spellArachna_52DC80(`GAME4_2.c:10327`) — 시전자→커서 선 추적(5)이 통하면 그 자리에
//   `ArachnaphobiaFocus`(보이지 않는 자리표)를 놓는다. 막히면 글(0·2)을 내고 0.
int Server::CastArachnaphobia(Session& caster, const SpellArg& arg, std::vector<OutPacket>& out) {
    const float cx = static_cast<float>(caster.posX), cy = static_cast<float>(caster.posY);
    if (!MapTraceRay(cx, cy, arg.x, arg.y, 0x5)) {           // traceRay_5374B0
        InformSpell(caster, 0, 2, out);
        return 0;
    }
    if (PlacedObj* ao = PlaceObject(NamedType("ArachnaphobiaFocus"), arg.x, arg.y, caster.addr, kPlacedNoLife, /*kind=*/7))
        ao->nextHitFrame = frame_;                           // +136 초깃값 — 놓자마자 한 마리
    return 1;
}

// castPixies_540440(`GAME4_3.c:8299`) — 시전자 둘레에 **픽시**를 `PixieCount[세기-1]`(2·3·4·5·6) 마리까지 채운다.
//   · 이미 제 픽시가 그만큼 있으면 **아무것도 안 만든다**(모자란 만큼만 채운다)
//   · 자리 = 시전자 + (시전자 반지름 + 4) × 무작위 방향 · 선 추적(5)이 막히면 그 마리는 거른다
//   · 속도 0 으로 나오고, 수명 = 지금 + 초당틱 × 무작위(30, 90)
//   ⚠ **날아다니는 AI 는 아직 없다**(`updatePixie_53CD20` — 적 찾기·떼 짓기·주인 따라가기·되돌아오기).
//     그건 유닛 움직임 묶음(6·7)이다. 여기서는 **나오는 것·수명·닿음 피해**까지가 산출물이다.
int Server::CastPixieSwarm(uint8_t spell, Session& caster, int lvl, std::vector<OutPacket>& out) {
    (void)out;
    const uint16_t type = NamedType("Pixie");
    if (type == 0) return 1;
    int have = 0;
    for (const auto& m : missiles_) if (m.type == type && m.owner == caster.addr) ++have;
    const int want = static_cast<int>(BalanceFloatInd("PixieCount", lvl - 1));
    if (have >= want) return 1;                              // 이미 다 차 있으면 소리만
    const float cx = static_cast<float>(caster.posX), cy = static_cast<float>(caster.posY);
    const float dist = static_cast<float>(kBodyRadius) + 4.0f;   // +176 반지름 + 4
    for (int n = have; n < want; ++n) {
        const int dir = static_cast<int>(NextRandom(256));
        const float px = cx + dist * DirVecX(dir);
        const float py = cy + dist * DirVecY(dir);
        if (!MapTraceRay(cx, cy, px, py, 0x5)) continue;
        MissileObj m;
        m.netCode = nextShotNetCode_++;
        if (nextShotNetCode_ == 0) nextShotNetCode_ = 4096;
        m.type = type;
        m.flags = 0x4000;                                    // thing FLAGS = AIRBORNE
        m.posX = m.newX = m.prevX = px;
        m.posY = m.newY = m.prevY = py;
        m.dir1 = m.dir2 = static_cast<uint8_t>(dir);
        m.createdFrame = m.frame136 = frame_;
        m.speed = 0.0f;                                      // 속도 0 으로 나온다(움직임은 AI 몫 — 묶음 6·7)
        m.owner = caster.addr;
        m.ud0 = m.ud2 = caster.addr;
        m.spell = spell;
        m.level = lvl;
        m.collideKind = 2;                                   // PixieCollide
        m.collideAmount = 8;                                 // thing.bin `COLLIDE = PixieCollide 8`
        m.expireFrame = frame_ + static_cast<uint32_t>(kServerTickRate) * (30u + NextRandom(61u));   // randomInt(30,90) 초
        missiles_.push_back(m);
    }
    return 1;
}

// 놓인 것의 **닿음 함수** — 몸이 겹친 것에 한 틱마다 한 번.
//   · `collideDamage_4E9430` — 피해 = **양 >> 1**, 단 양이 1 이면 홀수 프레임만 1. 갈래·팀을 안 가린다
//   · `collideManadrain_4E9490` — **사람에게만**, 마나를 **양만큼** 깎는다(피해가 아니다)
void Server::PlacedCollide(PlacedObj& o, std::vector<OutPacket>& out) {
    if (o.dmgAmount == 0) return;
    const int dmg = (o.dmgAmount == 1 && (frame_ & 1u)) ? 1 : (o.dmgAmount >> 1);
    const float reach = o.radius + static_cast<float>(kBodyRadius);
    const float rr = reach * reach;
    auto touch = [&](const Target& t, float tx, float ty) {
        const float dx = tx - o.x, dy = ty - o.y;
        if (dx * dx + dy * dy > rr) return;
        if (o.collideFn == 1) {
            if (t.addr.empty()) return;                      // 사람만(+8 & 4)
            auto it = sessions_.find(t.addr);
            if (it == sessions_.end() || ManaOnline(it->second.charClass) == 0) return;   // 마나 없는 직업은 거른다
            PlayerManaSub(it->second, static_cast<int>(o.dmgAmount));
            return;
        }
        HurtTarget(t, dmg, out, o.owner, o.dmgType, nullptr);
    };
    std::vector<std::string> who;
    for (const auto& kv : sessions_) if (kv.second.inGame && kv.second.respawnAtFrame == 0) who.push_back(kv.first);
    for (const auto& addr : who) {
        auto it = sessions_.find(addr);
        if (it == sessions_.end()) continue;
        touch(Target{ addr, -1 }, static_cast<float>(it->second.posX), static_cast<float>(it->second.posY));
    }
    for (size_t k = 0; k < pets_.size(); ++k)
        touch(Target{ std::string(), static_cast<int>(k) }, static_cast<float>(pets_[k].x), static_cast<float>(pets_[k].y));
}

// spellCastCleansingFlame_52D5C0(`GAME4_2.c:10031`) — 시전자 둘레에 **불꽃 48개**를 뿌린다.
//   · 온라인(협동 아님)이면 **세기를 4로 못 박는다**
//   · 불꽃마다 `세기 − 무작위(0,1)` 로 종류를 고르고, 0 이하면 그 번은 안 만든다
//   · 종류표 열 칸 — 붉은 다섯(주문 10 CleansingFlame) · 파란 다섯(주문 11 CleansingManaFlame).
//     ★ 붉은 것은 `DamageCollide`(피해), **파란 것은 `ManaDrainCollide`(마나 빨기)** 다
//   · 자리 = 시전자 + (불꽃 반지름 + 시전자 반지름 + 4) × 방향(무작위 0~255).
//     시작점→그 자리 선 추적(65)이 막히면 그 불꽃은 **안 만든다**
//   · 속도 = 방향 × 4 · 수명 = 지금 + 무작위(3초, 6초)
int Server::CastCleansingFlame(uint8_t spell, Session& caster, int lvl, std::vector<OutPacket>& out) {
    const bool quest = (questTotal_ > 0 || questStage_ > 0);
    if (!quest) lvl = 4;                                     // gameFlags(2048) 밖 = 온라인
    // 표 열 칸 — 0~4 붉은 · 5~9 파란. 정답지 셈은 `2487756 + 4×((주문 != 10 ? 5 : 0) + v8)` = 칸 (off + v8 − 1)
    static const char* const kName[10] = {
        "SmallFlameCleanse", "SmallFlameCleanse", "MediumFlameCleanse", "FlameCleanse", "LargeFlameCleanse",
        "SmallBlueFlameCleanse", "SmallBlueFlameCleanse", "MediumBlueFlameCleanse", "BlueFlameCleanse", "LargeBlueFlameCleanse",
    };
    static const float kRadius[10] = { 4, 4, 6, 8, 12, 4, 4, 6, 8, 12 };
    static const uint8_t kAmount[10] = { 1, 1, 2, 3, 4, 3, 3, 6, 9, 12 };
    const int off = (spell != 10) ? 5 : 0;
    const bool manaFlame = (off == 5);
    // ★ 이 사람의 불꽃이 하나라도 남아 있으면 **거절**한다(글 `plyrspel.c:TooManySpells`)
    for (const auto& p : placed_) {
        if (p.kind == 6 && p.owner == caster.addr) {
            SendPriMsg(caster, "plyrspel.c:TooManySpells", out);
            return 0;
        }
    }
    const float cx = static_cast<float>(caster.posX), cy = static_cast<float>(caster.posY);
    for (int n = 0; n < 48; ++n) {
        const int v8 = lvl - static_cast<int>(NextRandom(2));   // randomInt(0,1)
        if (v8 < 1) continue;
        const int idx = off + v8 - 1;
        if (idx < 0 || idx >= 10) continue;
        const int dir = static_cast<int>(NextRandom(256));
        const float dist = kRadius[idx] + static_cast<float>(kBodyRadius) + 4.0f;
        const float fx = cx + dist * DirVecX(dir);
        const float fy = cy + dist * DirVecY(dir);
        if (!MapTraceRay(cx, cy, fx, fy, 65)) continue;
        PlacedObj* fo = PlaceObject(NamedType(kName[idx]), fx, fy, caster.addr, kPlacedNoLife, /*kind=*/6,
                                    kAmount[idx], /*dmgType=*/1, kRadius[idx]);
        if (!fo) continue;
        fo->collideFn = manaFlame ? 1 : 0;
        fo->velX = DirVecX(dir) * 4.0f;
        fo->velY = DirVecY(dir) * 4.0f;
        fo->originX = cx;
        fo->originY = cy;
        fo->expireFrame = frame_ + 3u * static_cast<uint32_t>(kServerTickRate)
                        + NextRandom(3u * static_cast<uint32_t>(kServerTickRate) + 1u);   // randomInt(3×FPS, 6×FPS)
    }
    return 1;
}

// castFist_52D3C0(`GAME4_2.c:9956`) — 커서 자리 하늘에 **주먹**을 띄워 떨어뜨린다. 운석과 같은 꼴이되,
//   ① 그 사람이 이미 띄운 주먹이 있으면 글(`ExecSpel.c:TooManyFists`)과 함께 거절
//   ② 세기별 종류 — 1 SmallFist(둘레 25) · 2 MediumFist(30) · **3·4·5 LargeFist(35)**
//   ③ 갱신자료 첫 칸 = `FistOfVengeanceDamage[세기-1]`(50·100·200·300·400) · z 속도 = −FistSpeed(10)
//   ④ 깃발에 0x800000 을 얹는다 — 떨어짐 논리에서 **되튀는 갈래**가 이 비트다
int Server::CastFist(Session& caster, const SpellArg& arg, int lvl, std::vector<OutPacket>& out) {
    if (lvl < 1) lvl = 1;
    if (lvl > 5) lvl = 5;
    for (const auto& p : placed_) {
        if (p.kind == 5 && p.owner == caster.addr) {
            SendPriMsg(caster, "ExecSpel.c:TooManyFists", out);
            return 0;
        }
    }
    const float cx = static_cast<float>(caster.posX), cy = static_cast<float>(caster.posY);
    if (!MapTraceRay(cx, cy, arg.x, arg.y, 73)) {
        InformSpell(caster, 0, 2, out);
        return 0;
    }
    static const char* const kName[6] = { "", "SmallFist", "MediumFist", "LargeFist", "LargeFist", "LargeFist" };
    static const float kRadius[6]     = { 0.0f, 25.0f, 30.0f, 35.0f, 35.0f, 35.0f };   // EXTENT CIRCLE
    if (PlacedObj* fo = PlaceObject(NamedType(kName[lvl]), arg.x, arg.y, caster.addr, kPlacedNoLife, /*kind=*/5,
                                    /*dmgAmount=*/0, /*dmgType=*/0, kRadius[lvl])) {
        fo->z = 255.0f;                                      // unitRaise_4E46F0(255)
        fo->zVel = -static_cast<float>(BalanceFloat("FistSpeed"));
        fo->damage = static_cast<int>(BalanceFloatInd("FistOfVengeanceDamage", lvl - 1));
    }
    return 1;
}

// meteorExplode_53D6E0(`GAME4_3.c:6540`) — 높이가 0 이하가 된 운석이 하는 일.
//   · 그을음 · **화면 흔들기 10**(earthquakeSend) · `MeteorExplode` 그림 오브젝트 · 소리 87
//   · 둘레 피해 : 바깥 **80** · 안쪽 **30** · 피해 = 갱신자료 첫 칸 · 종류 **7**(폭발)
//   ⚠ 아직 안 옮긴 것 — 그을음 · 소리 · 벽 피해(mapDamageToWalls)
void Server::MeteorExplode(PlacedObj& o, std::vector<OutPacket>& out) {
    const float x = o.x, y = o.y;
    const int dmg = o.damage;
    const std::string owner = o.owner;
    SendJiggle(x, y, 10, out);                               // earthquakeSend_4D9110(자리, 10)
    DamageUnitsAround(x, y, 80.0f, 30.0f, dmg, 7, owner, Target{}, out);
    // 터짐 그림은 **1초 뒤 스스로 사라지는**(OneSecondDieUpdate) 장식 오브젝트다
    PlaceObject(NamedType("MeteorExplode"), x, y, owner, static_cast<uint32_t>(kServerTickRate), /*kind=*/0);
}

// mapDamageUnitsAround_4E25B0(Go `game.go:1407`) — 둘레 피해. rr = max(r1, r2)
//   · 자기(날아간 것)와 **이미 직접 맞은 것**은 건너뛴다 · 벽 너머(선 추적 1)는 안 닿는다
//   · 안쪽 r2 까지는 온전, 그 밖은 `1 − (거리 − r2) / (rr − r2)` 로 줄어든다
//   ⚠ 벽 피해(mapDamageToWalls_534FC0)는 아직 안 옮김 — 벽이 부서지는 묶음(9)에서
void Server::DamageUnitsAround(float cx, float cy, float r1, float r2, int dmg, uint8_t dtype,
                               const std::string& by, const Target& skip, std::vector<OutPacket>& out) {
    const float rr = (r1 < r2) ? r2 : r1;
    if (rr <= 0.0f || dmg <= 0) return;
    auto hit = [&](const Target& t, float tx, float ty) {
        if (!skip.none() && t.addr == skip.addr && t.pet == skip.pet) return;
        const float dx = tx - cx, dy = ty - cy;
        const float dist = std::sqrt(dx * dx + dy * dy);
        if (dist > rr) return;
        if (!MapTraceRay(cx, cy, tx, ty, 0x1)) return;
        float rdmg = static_cast<float>(dmg);
        if (dist >= r2 && rr > r2) rdmg *= 1.0f - (dist - r2) / (rr - r2);
        HurtTarget(t, static_cast<int>(rdmg), out, by, dtype, nullptr);
    };
    std::vector<std::string> who;
    for (const auto& kv : sessions_) if (kv.second.inGame && kv.second.respawnAtFrame == 0) who.push_back(kv.first);
    for (const auto& addr : who) {
        auto it = sessions_.find(addr);
        if (it == sessions_.end()) continue;
        hit(Target{ addr, -1 }, static_cast<float>(it->second.posX), static_cast<float>(it->second.posY));
    }
    for (size_t k = 0; k < pets_.size(); ++k)
        hit(Target{ std::string(), static_cast<int>(k) }, static_cast<float>(pets_[k].x), static_cast<float>(pets_[k].y));
}

// fireballCollide_4E9AC0(`GAME3_3.c:4160`) — 닿은 자리에서 터진다. 인자 한 바이트가 크기를 다 정한다.
//   · 밀기 : 둘레 **양 × 0.66667** · 안쪽 0 · 힘 50
//   · 직접 맞은 것 : **양 >> 1** 피해(종류 1 불)
//   · 둘레 피해 : 바깥 **양 × 0.33333** · 안쪽 **15** · 같은 **양 >> 1** · 직접 맞은 것은 건너뜀
//   · 불똥 그림 : `netSparkExplosionFx_5231B0` = **`93 <x u16> <y u16> <양 i8>` 6바이트**를 화면 안 모두에게
//     ⚠ 옛 자리표시는 여기에 `85`(FireBoom)를 보냈다 — **짐작이었고, 정답지는 `93` 이다.**
//   ⚠ 아직 안 옮긴 것 — 그을음(sMakeScorch) · 소리 42 · 벽 피해(mapDamageToWalls)
void Server::FireballExplode(MissileObj& m, const Target& hit, std::vector<OutPacket>& out) {
    const int amount = static_cast<int>(m.collideAmount);
    if (amount <= 0) return;
    PushUnitsAround(m.posX, m.posY, static_cast<float>(static_cast<double>(amount) * 0.66666669), 0.0f, 50.0f);
    const int dmg = amount >> 1;
    if (!hit.none()) HurtTarget(hit, dmg, out, m.owner, 1, nullptr);
    DamageUnitsAround(m.posX, m.posY, static_cast<float>(static_cast<double>(amount) * 0.33333334), 15.0f,
                      dmg, 1, m.owner, hit, out);
    const int16_t ex = static_cast<int16_t>(m.posX), ey = static_cast<int16_t>(m.posY);
    const uint8_t fx[6] = {
        static_cast<uint8_t>(Op::FX_SPARK_EXPLOSION),
        static_cast<uint8_t>(ex), static_cast<uint8_t>(ex >> 8),
        static_cast<uint8_t>(ey), static_cast<uint8_t>(ey >> 8),
        static_cast<uint8_t>(amount),
    };
    SendFxAllCli(m.posX, m.posY, fx, sizeof(fx), out);
    ++fxSent_;
}

// changeOwner_52BE40(`spell_counter.go:82`) — MISSILE+MissileMagic 만(날리는 Magic 은 모두) · 대상이 새 주인이면 대상 = 옛 주인 · 주인 = 새 주인 · 만든 프레임 = 지금(소리 → 10)
//   ⚠ 자리바꾸기(SWAP 115) 주문이면 cspellRemoveSpell_52BC90(4-e)
void Server::MissileChangeOwner(MissileObj& m, const std::string& own, uint32_t frame) {
    if (m.ud1.addr == own && !own.empty()) {
        m.ud1 = Target{ m.ud0, -1 };
        m.ud0 = own;
        m.owner = own;
        m.createdFrame = frame;
    }
}

// ★ 자리표시 효과 — 옛 시전 자리에 있던 투사체(27·24·43)·소환(75~87)을 그대로 옮겨 둔다. 4-c·4-d·4-f 에서 걷어낸다.
bool Server::SpellEffectPlaceholder(uint8_t spell, Session& s, const SpellArg& arg, std::vector<OutPacket>& out) {
    (void)out;
    if (const uint16_t stype = SummonType(spell)) {
        Creature k;
        k.owner = s.addr;
        k.type = stype;
        k.netCode = nextShotNetCode_++;
        if (nextShotNetCode_ == 0) nextShotNetCode_ = 4096;
        k.facing = s.facing;
        const double rad = s.facing * 3.14159265358979 / 128.0;
        k.x = s.x + kPetReach * std::cos(rad);
        k.y = s.y + kPetReach * std::sin(rad);
        k.health = static_cast<uint16_t>(TuningFor(k.type).health);
        pets_.push_back(k);
        return true;
    }
    // ★ 자리표시 투사체(shots_)는 **걷어냈다**(주문 27·43·24 가 모두 정답지로 옮겨졌다).
    //   효과 함수를 아직 안 옮긴 주문은 위 소환 갈래에 걸리지 않으면 **아무 일도 안 한다** —
    //   부르는 쪽이 「안 옮김」으로 세고 적는다.
    (void)arg;
    return false;
}

// ── 날아가는 오브젝트 (묶음 4-a2) ─────────────────────────────────────────
//
// 정답지: 투사체(MISSILE)는 5×0.2 물리에 안 든다 — 따로 목록(Objs.MissileList)을 틱마다
//   갱신 함수 → 움직임(sub_51B810) → 선 추적 맞힘(projectileTraceHit_537850 → 충돌 함수 둘) → PrevPos·PosVec 넘김(server_update.go:72~88).
//   모양은 전부 EXTENT = CENTER(점)라 모양 충돌(sub_548220)에 안 걸리고, 선 추적으로만 맞는다.

namespace {
// sub_57F2A0(`GAME5_2.c:2061`) — 칸 안에서 점이 어느 삼각형 쪽인가. ⚠ LOBYTE 대입이 윗바이트를 남기는 것까지 그대로(칸 밖이면 0~3 밖 값)
int32_t WallTriSide(float px, float py, int gx, int gy) {
    const int32_t v3 = static_cast<int32_t>(px - static_cast<float>(23 * gx));
    int32_t v4 = static_cast<int32_t>(py - static_cast<float>(23 * gy));
    const uint32_t cond = (22 - v3 <= v4) ? 1u : 0u;
    if (v3 <= v4) {
        uint32_t t = (static_cast<uint32_t>(v4) & ~0xFFu) | cond;
        uint32_t v6 = t - 1u;
        v6 = (v6 & ~0xFFu) | ((v6 & 0xFFu) & 0xFDu);
        return static_cast<int32_t>(v6 + 3u);
    }
    const uint32_t t = (static_cast<uint32_t>(v4) & ~0xFFu) | cond;
    return static_cast<int32_t>(t + 1u);
}
// sub_57F1D0(`GAME5_2.c:2027`) — 칸 안(23 나머지) 사분면 비트
uint8_t WallQuadBits(float hx, float hy) {
    uint8_t v1 = 0;
    const int v2 = static_cast<int>(hx);
    const uint8_t v7 = static_cast<uint8_t>(static_cast<int>(hy) % 23);
    const double v3 = static_cast<double>(static_cast<uint8_t>(v2 % 23));
    if (v3 >= 11.5) {
        if (static_cast<double>(v7) >= 11.5) v1 = 4;
        if (static_cast<double>(v7) <= 11.5) v1 |= 1;
    }
    if (v3 <= 11.5) {
        if (static_cast<double>(v7) >= 11.5) v1 |= 8;
        if (static_cast<double>(v7) <= 11.5) v1 |= 2;
    }
    return v1;
}
const float kWallDiag = 0.70709997f;
}  // namespace

// sub_57CDB0(`GAME5_2.c:1757~2020`) — 벽 칸 모양(0~10) · 출발점 삼각형 · 맞은 점 사분면으로 벽 법선(대각선 넷 중 하나). 모르면 false(법선 안 씀)
static bool WallNormal(int shape, int32_t v9, uint8_t v4, float& nx, float& ny) {
    auto set = [&](float x, float y) { nx = x; ny = y; return true; };
    switch (shape) {
        case 0: if (v9 != 1 && v9) return set(kWallDiag, kWallDiag); return set(-kWallDiag, -kWallDiag);
        case 1: if (v9 == 1 || v9 == 2) return set(kWallDiag, -kWallDiag); return set(-kWallDiag, kWallDiag);
        case 2:
            switch (v9) {
                case 0: return set(-kWallDiag, (v4 & 2) ? kWallDiag : -kWallDiag);
                case 1: if (!(v4 & 1)) return set(kWallDiag, -kWallDiag); return set(-kWallDiag, -kWallDiag);
                case 2: return set(kWallDiag, (v4 & 1) ? kWallDiag : -kWallDiag);
                case 3: if (!(v4 & 4)) return set(kWallDiag, kWallDiag); return set(-kWallDiag, kWallDiag);
                default: return true;           // 정답지 default: return 1 — 법선을 안 적는다(앞 값 그대로)
            }
        case 3:
            if (!v9) return set(-kWallDiag, (v4 & 2) ? kWallDiag : -kWallDiag);
            if (v9 != 1) return set(kWallDiag, kWallDiag);
            if (!(v4 & 1)) return set(kWallDiag, -kWallDiag);
            return set(-kWallDiag, -kWallDiag);
        case 4:
            if (v9 == 1) { if (!(v4 & 1)) return set(kWallDiag, -kWallDiag); return set(-kWallDiag, -kWallDiag); }
            if (v9 == 2) return set(kWallDiag, (v4 & 1) ? kWallDiag : -kWallDiag);
            return set(-kWallDiag, kWallDiag);
        case 5:
            if (v9 == 2) return set(kWallDiag, (v4 & 1) ? kWallDiag : -kWallDiag);
            if (v9 == 3) { if (!(v4 & 4)) return set(kWallDiag, kWallDiag); return set(-kWallDiag, kWallDiag); }
            return set(-kWallDiag, -kWallDiag);
        case 6:
            if (!v9) return set(-kWallDiag, (v4 & 2) ? kWallDiag : -kWallDiag);
            if (v9 == 3) { if (!(v4 & 4)) return set(kWallDiag, kWallDiag); return set(-kWallDiag, kWallDiag); }
            return set(kWallDiag, -kWallDiag);
        case 7:
            if (v9 == 1) { if (!(v4 & 1)) return set(kWallDiag, -kWallDiag); return set(-kWallDiag, -kWallDiag); }
            if (v4 & 1) return set(kWallDiag, kWallDiag);
            return set(-kWallDiag, kWallDiag);
        case 8:
            if (v9 == 2) return set(kWallDiag, (v4 & 1) ? kWallDiag : -kWallDiag);
            return set(-kWallDiag, (v4 & 1) ? -kWallDiag : kWallDiag);
        case 9:
            if (v9 == 3) { if (!(v4 & 4)) return set(kWallDiag, kWallDiag); return set(-kWallDiag, kWallDiag); }
            if (v4 & 4) return set(kWallDiag, -kWallDiag);
            return set(-kWallDiag, -kWallDiag);
        case 10:
            if (v9) { if (v4 & 2) return set(kWallDiag, -kWallDiag); return set(kWallDiag, kWallDiag); }
            return set(-kWallDiag, (v4 & 2) ? kWallDiag : -kWallDiag);
        default: return false;
    }
}

// server/spells.go:360 Nox_xxx_spellFlySearchTarget — 사람 시전자·공격 주문이면 커서 오브젝트가 적이면 그것 ·
//   아니면 중심(pos 가 있고 선 추적 1|3 통과면 pos, 아니면 날아가는 것 자리) 둘레 dist 에서 가장 가까운 대상
//   CanInteract = CanSee(버프 — 눈멂·투명·적외선, 4-a4) + 시야선 · 몬스터 생성기(8)는 없음
Target Server::SpellFlySearchTarget(bool havePos, float px, float py, float mslX, float mslY, const std::string& mslOwner,
                                    const std::string& mslSelf, uint32_t sflags, float dist, const Session* self, const Buffs* mslBuffs) const {
    // a5 = 0 — 날아가는 것 자신(mslSelf: 만들 때는 시전자)은 뺀다
    if (self && (sflags & 0x20)) {
        const Target cur = FindObjectAtCursor(*self);
        if (!cur.none() && TargetIsEnemy(*self, cur) && (mslSelf.empty() || cur.addr != mslSelf)) return cur;
    }
    float cx = mslX, cy = mslY;
    if (havePos) {
        if (MapTraceRay(mslX, mslY, px, py, 0x5)) { cx = px; cy = py; }
    }
    auto ownerIt = sessions_.find(mslOwner);
    const Session* owner = (ownerIt != sessions_.end()) ? &ownerIt->second : nullptr;
    const float dist2 = dist * dist;
    float minDist = 1e+08f;
    Target found;
    auto consider = [&](const Target& t, float ox, float oy, const Buffs* tb, double tvx, double tvy) {
        if ((sflags & 0x20) && owner && !TargetIsEnemy(*owner, t)) return;
        const float dx = cx - ox, dy = cy - oy;
        const float odist = dy * dy + dx * dx;
        if (odist > dist2) return;
        if (!CanSeeBuffs(mslBuffs, tb, tvx, tvy, 0) || !MapTraceVision(ox, oy, mslX, mslY)) return;   // CanInteract(msl, it)
        if (owner && (!CanSeeBuffs(&owner->buffs, tb, tvx, tvy, 0) ||
                      !MapTraceVision(ox, oy, static_cast<float>(owner->posX), static_cast<float>(owner->posY)))) return;
        if (odist < minDist) { found = t; minDist = odist; }
    };
    for (const auto& kv : sessions_) {
        const Session& o = kv.second;
        if (!o.inGame || o.observer || o.respawnAtFrame != 0) continue;         // 관전 · DEAD
        if (!mslSelf.empty() && o.addr == mslSelf) continue;
        consider(Target{ o.addr, -1 }, static_cast<float>(o.posX), static_cast<float>(o.posY), &o.buffs, o.velX, o.velY);
    }
    for (size_t i = 0; i < pets_.size(); ++i) {
        const auto tb = typeBodies_.find(pets_[i].type);
        if (tb != typeBodies_.end() && (tb->second.subclass & 0xC000)) continue;   // 몬스터 SUBCLASS 0x8000·0x4000
        consider(Target{ std::string(), static_cast<int>(i) }, static_cast<float>(pets_[i].x), static_cast<float>(pets_[i].y), &pets_[i].buffs, 0.0, 0.0);   // ⚠ 소환수 속도는 6
    }
    return found;
}

// createSpellFly_4FDDA0(`GAME4.c:2880`)
bool Server::CreateSpellFly(Session& s, Target target, uint8_t spell, std::vector<OutPacket>& out) {
    (void)out;
    if (magicType_.type == 0) return false;
    if (target.none()) {
        // 사람 — 커서(playerinfo +2284/+2288) 둘레 600 에서 고른다(날아가는 것 = 시전자)
        target = SpellFlySearchTarget(true, static_cast<float>(s.aimX), static_cast<float>(s.aimY),
                                      static_cast<float>(s.posX), static_cast<float>(s.posY), s.addr, s.addr, SpellFlags(spell), 600.0f, &s, &s.buffs);
    }
    const int dir = s.facingPrev;                            // +124 Direction1
    const float r = static_cast<float>(kBodyRadius) + 4.0f;  // +176 반지름 + 4
    const float px = static_cast<float>(s.posX), py = static_cast<float>(s.posY);
    const float sx = static_cast<float>(static_cast<double>(r) * DirVecX(dir) + px) + static_cast<float>(s.velX);
    const float sy = static_cast<float>(static_cast<double>(r) * DirVecY(dir) + py) + static_cast<float>(s.velY);
    if (!MapTraceRay(px, py, sx, sy, 0x5)) return false;     // 몸 앞이 벽이면 안 만든다(정답지는 이때 0 을 돌려 fizzle)
    MissileObj m;
    m.netCode = nextShotNetCode_++;
    if (nextShotNetCode_ == 0) nextShotNetCode_ = 4096;
    m.type = magicType_.type;
    m.flags = magicType_.flags;
    m.posX = m.newX = m.prevX = sx;
    m.posY = m.newY = m.prevY = sy;
    m.dir1 = m.dir2 = static_cast<uint8_t>(dir);
    m.createdFrame = m.frame136 = frame_;
    m.speed = magicType_.speed;
    m.owner = s.addr;
    m.ud0 = s.addr;
    m.ud1 = target;
    m.ud2 = s.addr;
    m.spell = spell;
    m.level = SpellPower(s, spell);
    m.velX = DirVecX(dir) * m.speed + static_cast<float>(s.velX);
    m.velY = DirVecY(dir) * m.speed + static_cast<float>(s.velY);
    // 시전자가 적외선(21)이면 날아가는 것에도(buffApplyTo — 유닛이 아니라 알림 없음) · 시전 소리는 10
    if (s.buffs.Has(21)) {
        m.buffs.bits |= 1u << 21;
        m.buffs.dur[21] = s.buffs.dur[21];
        m.buffs.power[21] = s.buffs.power[21];
    }
    std::printf("[날리기] 프레임 %u %s 주문 %u(%s) Magic 번호 %u 자리 (%.1f,%.1f) 대상 %s\n", frame_, s.addr.c_str(), spell,
                SpellName(spell) ? SpellName(spell) : "?", m.netCode, sx, sy,
                target.none() ? "없음" : (!target.addr.empty() ? target.addr.c_str() : "소환수"));
    std::fflush(stdout);
    missiles_.push_back(m);
    return true;
}

// netReportDestroyObject_5289D0(`GAME4_2.c:7630`) — 보던 사람에게 `31 <번호>` Packet0 · 날아가는 것은 갱신에 늘 실리므로 게임 안 모두
void Server::DestroyMissile(size_t i, std::vector<OutPacket>& out) {
    if (i >= missiles_.size()) return;
    const uint16_t nc = missiles_[i].netCode;
    const uint8_t msg[3] = { static_cast<uint8_t>(Op::DESTROY_OBJECT), static_cast<uint8_t>(nc), static_cast<uint8_t>(nc >> 8) };
    for (auto& kv : sessions_) if (kv.second.inGame) EmitImportantRaw(kv.second, msg, sizeof(msg), out);
    missiles_.erase(missiles_.begin() + static_cast<long>(i));
}

// ── 묶음 4-a4 버프 틀 ─────────────────────────────────────────────────
//   정답지 GAME4.c:3538~3650(buffApplyTo · GetBuffTimer · GetPower · ClearBuffs · spellBuffOff · updateUnitBuffs) · GAME3_3.c:1269(setUnitBuffFlags)
//   · spells_buffs.go(castBuffSpell) · server/object.go:76(CanSee) · GAME3_3.c:2593(sub_4E7540)
//   ⚠ 소리(On/Off · 26 · 779)는 10 · Hecubah/Necromancer 갈래는 몬스터(7) · 보호 CRC 없음

Buffs* Server::BuffsOf(const Target& t) {
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        return (it != sessions_.end() && it->second.inGame) ? &it->second.buffs : nullptr;
    }
    return (t.pet >= 0 && static_cast<size_t>(t.pet) < pets_.size()) ? &pets_[static_cast<size_t>(t.pet)].buffs : nullptr;
}
const Buffs* Server::BuffsOf(const Target& t) const {
    return const_cast<Server*>(this)->BuffsOf(t);
}
uint16_t Server::UnitNetCode(const Target& t) const {
    if (!t.addr.empty()) {
        auto it = sessions_.find(t.addr);
        return (it != sessions_.end() && it->second.inGame) ? it->second.netCode : 0;
    }
    return (t.pet >= 0 && static_cast<size_t>(t.pet) < pets_.size()) ? pets_[static_cast<size_t>(t.pet)].netCode : 0;
}

// setUnitBuffFlags_4E48F0 — 비트를 넣고 보는 사람 모두에게 `5a` 를 올린다(더티 0x800000)
void Server::SetBuffFlags(const Target& t, uint32_t bits) {
    Buffs* b = BuffsOf(t);
    if (!b) return;
    b->bits = bits;
    MarkDirty(UnitNetCode(t), PendingReport::What::Enchant);
}

// buffApplyTo_4FF380
void Server::ApplyEnchant(const Target& t, int buff, int dur, int power) {
    if (buff < 0 || buff >= 32) return;
    Buffs* b = BuffsOf(t);
    if (!b) return;
    // ⚠ Hecubah 반마법(29) 무시 · 퀘스트 Hecubah/Necromancer 혼란(3) · 몬스터 공포(11) 소리 갈래는 몬스터(7)
    if (!t.addr.empty() && sessions_.at(t.addr).respawnAtFrame != 0) return;   // +16 & 0x8022(DEAD)
    if (!b->Has(buff) || b->dur[buff] != 0) {
        if (buff != 0) BuffOff(t, 0);                    // 새 버프는 투명을 끈다
        b->dur[buff] = static_cast<uint16_t>(dur);
        b->power[buff] = static_cast<uint8_t>(power);
        SetBuffFlags(t, b->bits | (1u << buff));
        // 소리 On(주문 정의) → 10
    }
}

// spellBuffOff_4FF5B0
void Server::BuffOff(const Target& t, int buff) {
    if (buff < 0 || buff >= 32) return;
    Buffs* b = BuffsOf(t);
    if (!b || !b->Has(buff)) return;
    SetBuffFlags(t, b->bits & ~(1u << buff));
    b->dur[buff] = 0;
    b->power[buff] = 0;
    // 16·30 아니면 소리 Off → 10
}

// unitClearBuffs_4FF580
void Server::ClearBuffs(const Target& t) {
    Buffs* b = BuffsOf(t);
    if (!b) return;
    SetBuffFlags(t, 0);
    for (int i = 0; i < 32; ++i) { b->dur[i] = 0; b->power[i] = 0; }
}

// updateUnitBuffs_4FF620 — updateUnitsCCC(적분 뒤) 에서 유닛마다
void Server::UpdateUnitBuffs(const Target& t, std::vector<OutPacket>& out) {
    Buffs* b = BuffsOf(t);
    if (!b || b->bits == 0) return;
    for (int v = 0; v < 32; ++v) {
        b = BuffsOf(t);
        if (!b) return;
        if (!b->Has(v)) continue;
        // 16(DEATH) 이면 남은 틱 % 30 == 29 마다 소리 26 → 10
        if (b->dur[v] == 0) continue;
        --b->dur[v];
        if (b->dur[v] != 0) continue;
        if (v == 7) {
            // ETHEREAL — 깃발 &~0x40(NO_COLLIDE) ⚠ 유닛 깃발 칸이 없어 효과 묶음(4-b)에서
        } else if (v == 16) {
            // DEATH — +520 = 0 · +524 = 13 · unitDamageClear_4EE5E0(9999999: 방어·무적 안 봄) · 소리 779(10) · 사람이면 제거전 죽음·교훈(8)
            if (!t.addr.empty()) {
                Session& s = sessions_.at(t.addr);
                s.lastHit = Session::LastHit{};
                s.lastDamageType = 13;
                const Session::LastHit none;
                Damage(t.addr, 9999999, out, std::string(), &none, /*ignoreInvuln=*/true);
            } else {
                KillPet(static_cast<size_t>(t.pet), out);   // 몬스터 죽음 함수(monsterCallDieFn) — 순번이 바뀌므로 여기서 끝
                return;
            }
        }
        BuffOff(t, v);
        b = BuffsOf(t);
        if (b) b->power[v] = 0;
    }
    // 9(HASTED) 면 SpeedCur × 1.25(4-b3) — 사람만(소환수 걸음은 AI 6)
    b = BuffsOf(t);
    if (b && b->Has(9) && !t.addr.empty()) sessions_.at(t.addr).speedCur *= 1.25f;
}

// nox_xxx_playerConfusedGetDirection_4F7A40(`GAME4.c:1187`) — (프레임 + 번호) % 40 를 0~20 삼각파로 · (세기 + 3) × (파 − 10) 만큼 흔들어 0~255
uint8_t Server::ConfusedDirection(const Session& s, uint32_t frame) const {
    const int v1 = s.facing;
    const int v2 = s.buffs.power[3];
    int v3 = static_cast<int>((frame + static_cast<uint32_t>(s.netCode)) % 0x28u);
    if (v3 > 20) v3 = 40 - v3;
    int v4 = (v2 + 3) * (v3 - 10) + v1;
    if (v4 < 0) v4 += static_cast<int>((static_cast<unsigned>(255 - v4) >> 8) << 8);
    if (v4 >= 256) v4 += -256 * static_cast<int>(static_cast<unsigned>(v4) >> 8);
    return static_cast<uint8_t>(v4);
}

// CanSee(server/object.go:76) — obj·targ 버프만 본다(시야선은 부르는 쪽 MapTraceVision)
bool Server::CanSeeBuffs(const Buffs* obj, const Buffs* targ, double targVelX, double targVelY, int flags) {
    if (obj && obj->Has(2)) return false;                                    // BLINDED
    if (!(flags & 0x1) && targ && targ->Has(0)) {                            // INVISIBLE
        const bool infra = obj && obj->Has(21);                              // 퀘스트 Hecubah/Necromancer 갈래는 7
        // ⚠ 픽시(주인 적외선) 갈래는 4-c
        if (!infra && std::fabs(targVelX) <= 6.0 && std::fabs(targVelY) <= 6.0) return false;
    }
    // ⚠ 폭탄병(Bomber) ↔ 살금살금(TreadLightly) 능력은 7
    return true;
}

// netSendFxAllCli_523030 — 자리가 화면(반폭 640·반높이 480 + 50) 안인 사람에게 Kind1
void Server::SendFxAllCli(float x, float y, const uint8_t* msg, size_t n, std::vector<OutPacket>& out) {
    for (auto& kv : sessions_) {
        const Session& o = kv.second;
        if (!o.inGame) continue;
        if (std::fabs(x - o.posX) < 640.0 + 50.0 && std::fabs(y - o.posY) < 480.0 + 50.0)
            Emit(kv.second, /*reliable=*/false, /*stream=*/0, msg, n, out);
    }
}

// sub_4E7540 — 사람이 사람에게(서로 다름) 걸면 대상 +3600=1 · +3604=건 이 번호 · +3608=지금
void Server::MarkAssist(const std::string& orig, const Target& targ) {
    if (orig.empty() || targ.addr.empty() || orig == targ.addr) return;
    auto o = sessions_.find(orig);
    auto t = sessions_.find(targ.addr);
    if (o == sessions_.end() || t == sessions_.end()) return;
    t->second.assistSet = true;
    t->second.assistBy = orig;
    t->second.assistFrame = frame_;
}

// castBuffSpell(spells_buffs.go:25)
int Server::CastBuffSpell(uint8_t spell, int enc, int lvl, const Target& targ, const BuffConf& opts, std::vector<OutPacket>& out) {
    (void)spell;
    Buffs* b = BuffsOf(targ);
    if (!b) return 0;
    if (opts.once && b->Has(enc)) return 0;
    int dur = opts.dur;
    if (!opts.durOpt.empty()) dur = static_cast<int>(BalanceFloat(opts.durOpt));
    const bool quest = (questTotal_ > 0 || questStage_ > 0);
    if (!opts.durOptMulQuest.empty() && quest) dur = static_cast<int>(static_cast<double>(dur) * BalanceFloat(opts.durOptMulQuest));
    if (opts.durInSec) dur *= static_cast<int>(kServerTickRate);
    if (opts.durLevelMul) dur *= lvl;
    if (opts.defensive && !targ.addr.empty()) DurCancelOffensiveFor(targ.addr, out);
    ApplyEnchant(targ, enc, dur, lvl);
    if (!opts.orig.empty() && opts.offensive) MarkAssist(opts.orig, targ);
    if (opts.pointFx != 0) {                                  // netSendPointFx_522FF0 — `<fx> <x2> <y2>` 화면 안 사람
        float px = 0, py = 0;
        if (!targ.addr.empty()) { px = static_cast<float>(sessions_.at(targ.addr).posX); py = static_cast<float>(sessions_.at(targ.addr).posY); }
        else { px = static_cast<float>(pets_[static_cast<size_t>(targ.pet)].x); py = static_cast<float>(pets_[static_cast<size_t>(targ.pet)].y); }
        const uint16_t ix = static_cast<uint16_t>(static_cast<int>(px)), iy = static_cast<uint16_t>(static_cast<int>(py));
        const uint8_t msg[5] = { opts.pointFx, static_cast<uint8_t>(ix), static_cast<uint8_t>(ix >> 8), static_cast<uint8_t>(iy), static_cast<uint8_t>(iy >> 8) };
        SendFxAllCli(px, py, msg, sizeof(msg), out);
    }
    return 1;
}

// ── 묶음 4-a3 지속 주문 틀 ─────────────────────────────────────────────
//   정답지 server/spells_dur.go(DurSpell 칸 · 목록 · 취소) + spells_dur.go(New · 틱 · 지우기) + GAME4.c:3369~3525
//   ⚠ 소리(On/Cast/Off)는 10 · 미니맵 표시 풀기(417470)는 미니맵 묶음 · 몬스터 주인(MonsterCancelDurSpell)은 7 · 글리프 주인은 4-e

static bool SpellIsSummon(uint8_t sp) { return sp >= 75 && sp <= 114; }   // SPELL_SUMMON_BAT ~ SPELL_SUMMON_URCHIN_SHAMAN

// spellsDuration.New(spells_dur.go:88)
bool Server::DurNew(uint8_t spell, const std::string& a2, const std::string& obj3, bool u3Glyph, const SpellArg& arg, int lvl,
                    const DurFns& fns, uint32_t dt, std::vector<OutPacket>& out) {
    // u2 == nil || u2 죽음·지워짐 && u3 != nil && u3 글리프 아님 → false (u3 는 늘 있다 — 시전자 또는 날아가는 것)
    auto u2It = sessions_.find(obj3);
    if (obj3.empty() || u2It == sessions_.end() || !u2It->second.inGame) return false;
    const Session& u2 = u2It->second;
    if (u2.respawnAtFrame != 0 && !u3Glyph) return false;
    if ((spell == 59 || spell == 43) && DurActive(spell, obj3)) return true;   // 플라스마·연쇄 번개는 이미 있으면 새로 안 만듦
    DurCancelFor(spell, obj3, out);
    DurOnNewSpell(out);
    DurSpell d;
    d.id = ++durLastId_;
    d.spell = spell;
    d.level = lvl;
    d.obj12 = a2;
    d.caster = obj3;
    d.flag20 = u3Glyph;                                      // 글리프면 Obj24·Pos = 글리프 자리(4-e)
    d.posX = static_cast<float>(u2.posX);
    d.posY = static_cast<float>(u2.posY);
    d.target = arg.obj;
    d.pos2X = arg.x;
    d.pos2Y = arg.y;
    d.frame60 = frame_;
    d.frame64 = frame_;
    d.frame68 = dt + frame_;
    d.fns = fns;
    durSpells_.push_front(d);                                // Add — 머리에
    // 소리 — 겨눔(0x4)이면 On · 아니면 Cast(10)
    DurSpell& p = durSpells_.front();
    if (fns.create == nullptr || (this->*fns.create)(p, out) == 0) return true;
    DurCancel(p, out);
    return false;
}

// onNewSpell — 취소 깃발 붙은 것을 전부 지운다
void Server::DurOnNewSpell(std::vector<OutPacket>& out) {
    for (auto it = durSpells_.begin(); it != durSpells_.end();) {
        auto next = std::next(it);
        if (it->flags88 & 1) DurDestroy(it, out);
        it = next;
    }
}

// destroyDurSpell(spells_dur.go:39)
void Server::DurDestroy(std::list<DurSpell>::iterator it, std::vector<OutPacket>& out) {
    // 소리 Off(10)
    if (it->fns.destroy) (this->*(it->fns.destroy))(*it, out);
    auto c = sessions_.find(it->caster);
    if (!it->caster.empty() && c != sessions_.end() && c->second.inGame) {
        // 워리어가 광전사 돌진 중이 아니면 상태 13 ⚠ 능력(광전사)은 묶음 7 — 지금은 늘 13
        PlayerSetState(c->second, kPlayerStateIdle, frame_);
    }
    durSpells_.erase(it);                                    // Unlink · FreeRecursive(하위 포함)
}

// CancelSpell(server/spells_dur.go:181) = nox_xxx_spellCancelSpellDo_4FE9D0
void Server::DurCancel(DurSpell& d, std::vector<OutPacket>& out) {
    auto c = sessions_.find(d.caster);
    if (!d.caster.empty() && c != sessions_.end() && c->second.inGame)
        ReportSpellStat(c->second, d.spell, (d.spell == 43) ? 0 : 15, out);
    if (d.spell == 43) {
        for (const auto& sub : d.sub108) if (!sub.target.none()) NetStopRaySpell(sub, sub.target, out);
    } else if (!d.target.none()) {
        NetStopRaySpell(d, d.target, out);
    }
    d.flags88 |= 1;
}

// CancelFor(server/spells_dur.go:169)
void Server::DurCancelFor(uint8_t spell, const std::string& obj, std::vector<OutPacket>& out) {
    for (auto& d : durSpells_) {
        if ((d.spell == spell && d.caster == obj) || (SpellIsSummon(spell) && SpellIsSummon(d.spell) && d.caster == obj))
            DurCancel(d, out);
    }
}

// CancelOffensiveFor(server/spells_dur.go:159)
void Server::DurCancelOffensiveFor(const std::string& obj, std::vector<OutPacket>& out) {
    for (auto& d : durSpells_)
        if (d.caster == obj && (SpellFlags(d.spell) & 0x20)) DurCancel(d, out);
}

// Sub4FEE50 — 안 취소된 · 글리프 아닌 · 그 주문 · 그 주인
bool Server::DurActive(uint8_t spell, const std::string& obj) const {
    for (const auto& d : durSpells_)
        if (!d.flag20 && d.spell == spell && d.caster == obj && !(d.flags88 & 1)) return true;
    return false;
}

// nox_xxx_playerCancelSpells_4FEAE0 — 그 주인의 것 전부
void Server::PlayerCancelSpells(const std::string& obj, std::vector<OutPacket>& out) {
    for (auto& d : durSpells_) if (d.caster == obj) DurCancel(d, out);
}

// nox_xxx_cancelAllSpells_4FEE90 — 걷기·뛰기 입력이면 붙들고 있는 주문(번개 24 · 연쇄 번개 43 · 큰 치유 35 · 생명 흘리기 8 · 마나 흡수 22 · 플라스마 59 · 타원 방패 67)
void Server::CancelChannelSpells(const std::string& obj, std::vector<OutPacket>& out) {
    for (auto& d : durSpells_) {
        if (d.caster != obj) continue;
        const uint8_t v = d.spell;
        if (v == 24 || v == 43 || v == 35 || v == 8 || v == 22 || v == 59 || v == 67) DurCancel(d, out);
    }
}

// sub_4FF2D0 — 안 취소된 · 그 주문 · 대상이 그것
const Server::DurSpell* Server::DurFindByTarget(uint8_t spell, const Target& target) const {
    for (const auto& d : durSpells_) {
        if ((d.flags88 & 1) || d.spell != spell || d.target.none()) continue;
        if (d.target.addr == target.addr && d.target.pet == target.pet) return &d;
    }
    return nullptr;
}

// NetStopRaySpell(server/spells.go:458) — `9e <갈래> <값> <u1 번호2> <u2 번호2>` Packet1 모두
void Server::NetStopRaySpell(const DurSpell& d, const Target& who, std::vector<OutPacket>& out) {
    auto c = sessions_.find(d.caster);
    if (d.caster.empty() || c == sessions_.end() || !c->second.inGame) return;
    auto netOf = [&](const Target& t) -> uint16_t {
        if (!t.addr.empty()) {
            auto it = sessions_.find(t.addr);
            return (it != sessions_.end() && it->second.inGame) ? it->second.netCode : 0;
        }
        return (t.pet >= 0 && static_cast<size_t>(t.pet) < pets_.size()) ? pets_[static_cast<size_t>(t.pet)].netCode : 0;
    };
    if (who.none() || netOf(who) == 0) return;
    uint8_t typ = 0, val = 0;
    uint16_t n1 = d.fromUnit.none() ? c->second.netCode : netOf(d.fromUnit);
    uint16_t n2 = netOf(who);
    switch (d.spell) {
        case 43:                                             // 연쇄 번개 — 하위마다
            for (const auto& sub : d.sub108) NetStopRaySpell(sub, sub.target, out);
            return;
        case 59: typ = 8; val = c->second.facingPrev; break;             // 플라스마 — 주인 Direction1
        case 9: typ = 9; val = static_cast<uint8_t>(d.level); break;     // 매혹
        case 7: typ = 10; val = static_cast<uint8_t>(d.level); break;    // 연쇄 번개 조각
        case 22: typ = 12; val = static_cast<uint8_t>(d.level); break;   // 마나 흡수
        case 24: typ = 11; val = static_cast<uint8_t>(d.level); break;   // 번개
        case 35:                                             // 큰 치유 — 자기에게면 없음 · 받는 쪽이 앞
            if (!d.target.addr.empty() && d.target.addr == d.caster) return;
            typ = 13; val = static_cast<uint8_t>(d.level);
            std::swap(n1, n2);
            break;
        default: return;
    }
    const uint8_t msg[7] = { static_cast<uint8_t>(Op::FX_DURATION_SPELL), typ, val, static_cast<uint8_t>(n1), static_cast<uint8_t>(n1 >> 8),
                             static_cast<uint8_t>(n2), static_cast<uint8_t>(n2 >> 8) };
    for (auto& kv : sessions_) if (kv.second.inGame) EmitSeq(kv.second, msg, sizeof(msg), out);
    // 미니맵 표시 풀기(Nox_xxx_netUnmarkMinimapSpec_417470 · 2) — 미니맵 표시 묶음
}

// spellCastByPlayer(spells_dur.go:62) — 틱마다 castByBook 다음
void Server::SpellCastByPlayer(uint32_t frame, std::vector<OutPacket>& out) {
    auto alive = [&](const std::string& a) {
        auto s = sessions_.find(a);
        return s != sessions_.end() && s->second.inGame;
    };
    for (auto it = durSpells_.begin(); it != durSpells_.end();) {
        auto next = std::next(it);
        DurSpell& d = *it;
        if (d.flags88 & 1) { DurDestroy(it, out); it = next; continue; }
        if (!d.caster.empty() && (!alive(d.caster) || sessions_.at(d.caster).respawnAtFrame != 0)) d.caster.clear();   // DEAD|DESTROYED
        if (!d.obj12.empty() && !alive(d.obj12)) d.obj12.clear();                                                      // DESTROYED
        if (d.caster.empty() && !d.flag20) { DurCancel(d, out); it = next; continue; }
        if ((d.frame68 != d.frame60 && d.frame68 <= frame) || (d.fns.update && (this->*d.fns.update)(d, out) != 0))
            DurCancel(d, out);
        it = next;
    }
}

// 시험용 create · update · destroy
int Server::DebugDurCreate(DurSpell& d, std::vector<OutPacket>& out) { (void)d; (void)out; return debugDurCreateRet_; }
int Server::DebugDurUpdate(DurSpell& d, std::vector<OutPacket>& out) {
    (void)d; (void)out;
    ++debugDurUpdates_;
    return (debugDurUpdateStopAt_ > 0 && debugDurUpdates_ >= debugDurUpdateStopAt_) ? 1 : 0;
}
void Server::DebugDurDestroyFn(DurSpell& d, std::vector<OutPacket>& out) { (void)d; (void)out; ++debugDurDestroys_; }
bool Server::DebugDurNew(const std::string& caster, uint8_t spell, uint32_t dt, int createRet, int updateStopAt, bool withDestroy,
                         const Target& target, std::vector<OutPacket>& out) {
    if (sessions_.find(caster) == sessions_.end()) return false;
    DurFns f;
    if (createRet >= 0) { debugDurCreateRet_ = createRet; f.create = &Server::DebugDurCreate; }
    if (updateStopAt >= 0) { debugDurUpdateStopAt_ = updateStopAt; debugDurUpdates_ = 0; f.update = &Server::DebugDurUpdate; }
    if (withDestroy) f.destroy = &Server::DebugDurDestroyFn;
    SpellArg arg;
    arg.obj = target;
    return DurNew(spell, caster, caster, false, arg, 3, f, dt, out);
}

// 날아가는 것들 한 틱 — updateUnitsCallUpdate(server_update.go:72) 의 투사체 몫. 사람 적분 **앞**에서 부른다
void Server::UpdateMissiles(uint32_t frame, std::vector<OutPacket>& out) {
    for (size_t i = 0; i < missiles_.size();) {
        MissileObj& m = missiles_[i];
        // 만든 틱에는 안 돈다(ObjectsAddPending 뒤) — 우리 틀은 시전 목록(SpellCastByBook)이 이 뒤에 돌아 저절로 그렇다
        // ①-a ProjectileUpdate 53AC10(`object_update.go:938`) — 불덩이 같은 **보통 투사체**의 주기 함수.
        //   하는 일이 하나뿐이다: 만든 뒤 **40틱**이 지나면 제 닿음 함수를 대상 없이(0,0) 부르고 지운다.
        //   ⚠ 아래 ①-b(SpellProjectileUpdate)의 수명 15/45·대상 찾기·따라가는 힘은 **Magic 전용**이다 —
        //     불덩이에 씌우면 15틱 만에 사라져 멀리 못 간다(실측: 세기 1 불덩이가 280 을 못 갔다).
        if (m.collideKind == 1) {
            if (frame - m.createdFrame > 40u) {
                FireballExplode(m, Target{}, out);
                DestroyMissile(i, out);
                continue;
            }
        } else if (m.collideKind == 3) {
            // ①-d 묶음 6-g(3b) — `ImpShot` 도 `UPDATE = ProjectileUpdate` 다(불덩이와 같은 40틱).
            //   때가 되면 **대상 없이** 제 닿음 함수를 부르는데, 그쪽은 맵에 피해를 주고 사라진다.
            //   ⚠ 맵 피해(`damageToMap_534BC0`)는 아직 안 옮겼다 — 지금은 그냥 지운다
            if (frame - m.createdFrame > 40u) { DestroyMissile(i, out); continue; }
        } else if (m.collideKind == 4) {
            // ①-e 묶음 7-f 작살 — `UPDATE = HarpoonUpdate`(`updateHarpoon_54F380`).
            //   ★ 아래 ①-b(주문 투사체)로 떨어지면 **15틱 만에 사라지고** 엉뚱한 상대를 찾아 휜다.
            //     여기서는 **그냥 날게** 둔다 — 거리·수명·시야 관문과 끌기는 다음 조각(7-g)이다
        } else if (m.collideKind == 2) {
            // ①-c updatePixie_53CD20 — 여기서는 **수명만** 본다.
            //   ⚠ 적 찾기·떼 짓기·주인 따라가기·되돌아오기는 유닛 움직임 묶음(6·7)이다.
            if (m.expireFrame != 0 && frame > m.expireFrame) { DestroyMissile(i, out); continue; }
        } else
        // ①-b SpellProjectileUpdate 53B940(`GAME4_3.c:5764`)
        {
            const uint32_t life = m.ud1.none() ? 15u : 45u;   // ⭕ gamedata ARENA UnTargeted/TargetedSpellLifetime
            if (frame - m.createdFrame > life) {
                const int16_t fx = static_cast<int16_t>(m.posX), fy = static_cast<int16_t>(m.posY);
                const uint8_t msg[5] = { static_cast<uint8_t>(Op::FX_RICOCHET), static_cast<uint8_t>(fx), static_cast<uint8_t>(fx >> 8),
                                         static_cast<uint8_t>(fy), static_cast<uint8_t>(fy >> 8) };
                for (auto& kv : sessions_) {                  // netSendFxAllCli_523030 — 화면(반폭+50) 안인 사람만 · Kind1
                    const Session& o = kv.second;
                    if (!o.inGame) continue;
                    if (std::fabs(m.posX - o.posX) < 640.0 + 50.0 && std::fabs(m.posY - o.posY) < 480.0 + 50.0)
                        Emit(kv.second, /*reliable=*/false, /*stream=*/0, msg, sizeof(msg), out);
                }
                DestroyMissile(i, out);
                continue;
            }
            auto alive = [&](const std::string& a) {
                auto it = sessions_.find(a);
                return it != sessions_.end() && it->second.inGame;
            };
            if (!m.ud0.empty() && !alive(m.ud0)) m.ud0.clear();
            if (!m.ud2.empty() && !alive(m.ud2)) m.ud2.clear();
            if (!m.ud1.addr.empty()) {
                auto it = sessions_.find(m.ud1.addr);
                if (it == sessions_.end() || !it->second.inGame || it->second.respawnAtFrame != 0) m.ud1 = Target{};   // 0x20 · 0x8000
            } else if (m.ud1.pet >= 0 && static_cast<size_t>(m.ud1.pet) >= pets_.size()) {
                m.ud1 = Target{};
            }
            if (m.ud1.none()) {
                if (frame - m.frame136 > static_cast<uint32_t>(kServerTickRate >> 2) || m.frame136 == m.createdFrame) {
                    auto ownIt = sessions_.find(m.ud0);
                    m.ud1 = SpellFlySearchTarget(false, 0, 0, m.posX, m.posY, m.owner, std::string(), SpellFlags(m.spell), 600.0f,
                                                 ownIt != sessions_.end() ? &ownIt->second : nullptr, &m.buffs);
                    m.frame136 = frame;
                }
            }
            if (!m.ud1.none()) {
                float tx = 0, ty = 0;
                if (!m.ud1.addr.empty()) { tx = static_cast<float>(sessions_.at(m.ud1.addr).posX); ty = static_cast<float>(sessions_.at(m.ud1.addr).posY); }
                else { tx = static_cast<float>(pets_[static_cast<size_t>(m.ud1.pet)].x); ty = static_cast<float>(pets_[static_cast<size_t>(m.ud1.pet)].y); }
                const float v18 = tx - m.posX;
                const float v17 = ty - m.posY;
                const float v11 = static_cast<float>(std::sqrt(static_cast<double>(v17) * v17 + static_cast<double>(v18) * v18) + 0.10000000149011612);
                m.damping = 0.89999998f;                     // 1063675494
                m.forceX = v18 * m.speed / v11;
                m.forceY = v17 * m.speed / v11;
            } else {
                const float v13 = static_cast<float>(std::sqrt(static_cast<double>(m.velX) * m.velX + static_cast<double>(m.velY) * m.velY) + 0.10000000149011612);
                m.damping = 0.89999998f;
                m.forceX = m.speed * m.velX / v13;
                m.forceY = m.speed * m.velY / v13;
            }
        }
        // ② 떨어짐(updateFallLogic) — 높이는 묶음 5·9
        // ③ 움직임 sub_51B810(`GAME4_1.c:4799`)
        m.posX = m.newX; m.posY = m.newY;
        m.velX = (m.forceX + m.velX) * m.damping;
        m.velY = (m.forceY + m.velY) * m.damping;
        m.newX += m.velX; m.newY += m.velY;
        // ④ 선 추적 맞힘 projectileTraceHit_537850(`GAME4_3.c:3386`) → 충돌 함수 둘(sub_537770)
        bool erased = false;
        {
            enum { HitNone, HitPlayer, HitPet, HitObstacle } hk = HitNone;
            std::string hAddr; int hIdx = -1;
            float hx = 0, hy = 0;                            // 맞은 것 자리
            float onx = 0, ony = 0;                          // 오브젝트 법선(점 − 그 자리)
            // sub_54E730(날아가는 것, 대상) — 깃발 거르기. 대상 NO_COLLIDE(0x40) 는 안 맞음 · MISSILE_HIT(0x80) 는 늘 맞음 ·
            //   BELOW|SHORT(0x11) 와 AIRBORNE(0x4000) 은 서로 안 맞음 · 어느 쪽이든 NO_COLLIDE_OWNER(0x400) 면 같은 편(주인 사슬)은 안 맞음
            //   ⚠ 몬스터가 쏜 투사체 ↔ 몬스터 갈래(+508 주인 클래스 2)는 몬스터 시전(묶음 7) · 충돌 함수 없음은 hasCollide 로
            auto passFlags = [&](uint32_t of, bool sameTeam) {
                if (of & 0x40) return false;
                if (of & 0x80) return true;
                if ((m.flags & 0x11) && (of & 0x4000)) return false;
                if ((of & 0x11) && (m.flags & 0x4000)) return false;
                if (((m.flags | of) & 0x400) && sameTeam) return false;
                return true;
            };
            auto ownIt = sessions_.find(m.owner);
            const uint8_t ownTeam = (ownIt != sessions_.end()) ? ownIt->second.team : 0;
            // sub_54E810 — 점이 모양 안에 드는 것(원 거리² ≤ R² · 상자 · 문 선분은 9) · 마지막 것
            auto pointHit = [&](float qx, float qy) {
                bool any = false;
                for (const auto& kv : sessions_) {
                    const Session& o = kv.second;
                    if (!o.inGame || o.observer) continue;   // 관전 NO_COLLIDE(0x40)
                    if (!passFlags(0, o.addr == m.owner || (ownTeam != 0 && o.team == ownTeam))) continue;   // NewPlayer FLAGS NULL
                    const float dx = static_cast<float>(o.posX) - qx, dy = static_cast<float>(o.posY) - qy;
                    if (dy * dy + dx * dx > static_cast<float>(kBodyRadius * kBodyRadius)) continue;
                    hk = HitPlayer; hAddr = o.addr; hx = static_cast<float>(o.posX); hy = static_cast<float>(o.posY); any = true;
                }
                for (size_t k = 0; k < pets_.size(); ++k) {
                    // ★ **쏜 것 자신은 건너뛴다** — 발사체는 그 자리에서 생기므로 안 그러면
                    //   나가자마자 제 몸에 맞는다. 정답지는 부모 사슬을 타고 올라가 가른다
                    //   (`findParentChainPlayer_4EC580`)
                    if (m.fromPet >= 0 && static_cast<int>(k) == m.fromPet) continue;
                    const auto tb = typeBodies_.find(pets_[k].type);
                    if (tb == typeBodies_.end() || !tb->second.hasCollide || tb->second.shape != 2) continue;
                    if (!passFlags(tb->second.flags, !pets_[k].owner.empty() && pets_[k].owner == m.owner)) continue;
                    const float dx = static_cast<float>(pets_[k].x) - qx, dy = static_cast<float>(pets_[k].y) - qy;
                    if (dy * dy + dx * dx > tb->second.r * tb->second.r) continue;
                    hk = HitPet; hIdx = static_cast<int>(k); hx = static_cast<float>(pets_[k].x); hy = static_cast<float>(pets_[k].y); any = true;
                }
                for (size_t k = 0; k < obstacles_.size(); ++k) {
                    const PhysBody& o = obstacles_[k];
                    if (!passFlags(o.flags, false)) continue;
                    if (!BodyContains(o, qx, qy)) continue;
                    hk = HitObstacle; hIdx = static_cast<int>(k); hx = o.x; hy = o.y; any = true;
                }
                return any;
            };
            const float ddx = m.newX - m.posX, ddy = m.newY - m.posY;
            const double d2 = static_cast<double>(ddy) * ddy + static_cast<double>(ddx) * ddx;
            if (d2 <= 36.0) {
                if (pointHit(m.newX, m.newY)) { onx = m.posX - hx; ony = m.posY - hy; }
            } else {
                const int steps = static_cast<int>(std::sqrt(d2 * 0.027777778)) + 1;
                const float stx = ddx / static_cast<float>(steps), sty = ddy / static_cast<float>(steps);
                float lx = m.posX, ly = m.posY, qx = m.posX, qy = m.posY;
                for (int k = 0; k < steps; ++k) {
                    qx += stx; qy += sty;
                    if (pointHit(qx, qy)) { onx = lx - hx; ony = ly - hy; break; }
                    lx = qx; ly = qy;
                }
            }
            TraceHit th;
            bool wall = false;
            float wnx = 0, wny = 0;
            if (!MapTraceRay(m.posX, m.posY, m.newX, m.newY, 0x5, &th)) {
                wall = WallNormal(WallShapeAt(th.gx, th.gy), WallTriSide(m.posX, m.posY, th.gx, th.gy),
                                  WallQuadBits(th.x, th.y), wnx, wny);
                m.newX = m.posX; m.newY = m.posY;
            }
            bool objHit = (hk != HitNone);
            bool wallHit = wall;
            if (objHit && wall) {
                const double od = (m.posX - hx) * (m.posX - hx) + (m.posY - hy) * (m.posY - hy);
                const double wd = (m.posX - th.x) * (m.posX - th.x) + (m.posY - th.y) * (m.posY - th.y);
                if (od < wd) wallHit = false; else objHit = false;
            }
            // SpellProjectileCollide 4E9500(`GAME3_3.c:3925`) — 날아가는 것 쪽. 맞은 것 쪽 충돌 함수(PlayerCollide 등)는 광전사·버프 갈래뿐이라 4-a4·7
            auto reflectFrom = [&](float ox, float oy) {     // projectileReflect_4E0A70
                const double v3 = static_cast<double>(m.posX) - ox, v4 = static_cast<double>(m.posY) - oy;
                m.dir1 = static_cast<uint8_t>(m.dir1 + 128);
                const float v13 = static_cast<float>(std::sqrt(v4 * v4 + v3 * v3) + 0.1);
                const double v6 = (v4 * m.velY + v3 * m.velX) / v13;
                const float v9 = static_cast<float>(v6 * v3 / v13);
                const float v10 = static_cast<float>(v6 * v4 / v13);
                const float v11 = static_cast<float>(-v4);
                const double v7 = (static_cast<double>(v11) * m.velX + v3 * m.velY) / v13;
                const float v12 = static_cast<float>(v7 * v11 / v13);
                m.velX = v12 - v9;
                m.velY = static_cast<float>(v3 * v7 / v13) - v10;
                m.newX = m.prevX; m.newY = m.prevY;
            };
            if (wallHit) {
                // 7-f — 작살은 벽에 닿으면 **대상 없이** 제 닿음 함수를 타 조용히 끊긴다.
                // ⚠ `HarpoonCollide` 가 `HarpoonBreak` 을 통해 이미 지웠으면 목록이 줄어 색인 i 가 다음 것을 가리킨다.
                //   그러나 주인을 못 찾는 따위로 **안 지워졌을 수도** 있으므로 여기서 한 번 더 확인한다 —
                //   그러지 않으면 `continue` 가 같은 색인을 다시 보아 **무한 고리**가 된다(돌연변이 HP9 가 드러냈다)
                if (m.collideKind == 4) {
                    const uint16_t nc = m.netCode;
                    HarpoonCollide(m, Target{}, out);
                    if (i < missiles_.size() && missiles_[i].netCode == nc) DestroyMissile(i, out);
                    erased = true;
                    continue;
                }
                if (m.collideKind == 1) {                    // 불덩이는 벽에서도 터진다(fireballCollide 의 a2 == 0 갈래)
                    FireballExplode(m, Target{}, out);
                    DestroyMissile(i, out);
                    erased = true;
                    continue;
                }
                // collideReflect_57B810 — 법선 부호 곱으로 속도 성분 맞바꿈
                if (wny * wnx <= 0.0f) std::swap(m.velX, m.velY);
                else { const float v3 = m.velX; m.velX = -m.velY; m.velY = -v3; }
            } else if (objHit) {
                bool dead = false;
                if (hk == HitPlayer) dead = (sessions_.at(hAddr).respawnAtFrame != 0);
                if (!dead) {
                    bool handled = false;
                    if (hk == HitPlayer) {
                        Session& t = sessions_.at(hAddr);
                        auto changeOwner = [&]() { MissileChangeOwner(m, t.addr, frame); };
                        const bool front = (TestTwoPointsAndDirection(static_cast<float>(t.posX), static_cast<float>(t.posY),
                                                                      t.facingPrev, m.prevX, m.prevY) & 1) != 0;
                        if (t.moveState == 16 && front) {            // 방패 올림 앞쪽 — 되받아침(소리 878 → 10)
                            reflectFrom(hx, hy);
                            changeOwner();
                            handled = true;
                        } else if (t.moveState == kPlayerStateIdle && (WeaponEquipOf(t) & 0x400) && front) {
                            reflectFrom(hx, hy);                     // 대검 가만 앞쪽 — 되받아침 + 상태 18~20(소리 890 → 10)
                            changeOwner();
                            PlayerSetState(t, static_cast<uint8_t>(18 + NextRandom(3)), frame);
                            t.animFrame = static_cast<uint8_t>(playerAnimFrames_[PlayerAnimByte(t)][0] - 1);
                        }
                        // LABEL_13 — ⚠ 반전(Inversion)은 든 물건의 마법부여 함수(아이템 5)
                        //   되받아치기 방패(27) 버프 · 앞쪽(날아가는 것 지금 자리 +56) → 주인·대상 맞바꿈(소리 122 → 10) · 되받아침 없이 그대로
                        if (!handled && t.buffs.Has(27) &&
                            (TestTwoPointsAndDirection(static_cast<float>(t.posX), static_cast<float>(t.posY), t.facingPrev, m.posX, m.posY) & 1)) {
                            if (m.collideKind == 1) {
                                // 불덩이는 제 닿음 함수 안에서 **되받아치고 주인을 넘긴다** — 터지지 않는다(소리 122)
                                //   `projectileReflect_4E0A70` → `unitClearOwner_4EC300` → `unitSetOwner_4EC290(맞은 이, 불덩이)`
                                reflectFrom(hx, hy);
                                m.owner = t.addr;
                                m.ud0 = m.ud2 = t.addr;
                            } else {
                                changeOwner();
                            }
                            handled = true;
                        }
                    }
                    // collidePixie_4EA080 — **적일 때만** 인자만큼 피해(종류 11 충격)를 주고 제가 사라진다
                    if (!handled && m.collideKind == 2) {
                        const Target tgt = (hk == HitPlayer) ? Target{ hAddr, -1 } : Target{ std::string(), hIdx };
                        auto ownIt = sessions_.find(m.owner);
                        const bool enemy = (ownIt != sessions_.end()) && TargetIsEnemy(ownIt->second, tgt);
                        if (enemy) {
                            HurtTarget(tgt, static_cast<int>(m.collideAmount), out, m.owner, 11, nullptr);
                            DestroyMissile(i, out);
                            erased = true;
                        }
                        handled = true;
                    }
                    // collideProjectileGeneric_4E87B0 — ★ **적·아군을 안 가린다**(픽시와 다르다).
                    //   맞는 것에 **종류 11** 피해. 양은 `ImpShot` 이면 gamedata `ImpShotDamage`,
                    //   아니면 `COLLIDE` 의 인자다(둘 다 5 라 값은 같다)
                    if (!handled && m.collideKind == 3) {
                        const Target tgt = (hk == HitPlayer) ? Target{ hAddr, -1 } : Target{ std::string(), hIdx };
                        // ⭕ 정답지는 피해를 줄 때 **a2 로 발사체 자신**을 넘긴다
                        //   (`(*(a2+716))(a2, 부모사람, **발사체**, 피해, 11)`) — 죽음 알림 그림이 그것이다
                        AttackSrc src;
                        src.x = m.posX; src.y = m.posY;
                        src.byMonsterType = m.type;
                        HurtTarget(tgt, static_cast<int>(m.collideAmount), out, m.owner, 11, &src);
                        DestroyMissile(i, out);
                        erased = true;
                        handled = true;
                    }
                    // 7-f — 작살. 꽂히면 화살을 **살려 둔다**(줄이 된다)
                    if (!handled && m.collideKind == 4) {
                        const Target tgt = (hk == HitPlayer) ? Target{ hAddr, -1 } : Target{ std::string(), hIdx };
                        if (!HarpoonCollide(m, tgt, out)) { erased = true; }
                        handled = true;
                    }
                    if (!handled && m.collideKind == 1) {
                        // 불덩이 — 대상을 안 가린다(맞는 것마다 터진다). 되받아치기 방패(27)는 위에서 이미 handled
                        const Target tgt = (hk == HitPlayer) ? Target{ hAddr, -1 } : Target{ std::string(), hIdx };
                        FireballExplode(m, tgt, out);
                        DestroyMissile(i, out);
                        erased = true;
                        handled = true;
                    }
                    if (!handled) {
                        const bool isTarget = (hk == HitPlayer && m.ud1.addr == hAddr) || (hk == HitPet && m.ud1.pet == hIdx);
                        if (isTarget) {
                            auto casterIt = sessions_.find(m.ud2);
                            if (casterIt != sessions_.end()) {
                                SpellArg arg;
                                arg.obj = (hk == HitPlayer) ? Target{ hAddr, -1 } : Target{ std::string(), hIdx };
                                SpellAccept(m.spell, casterIt->second, m.ud0, arg, m.level, out);   // (ud[3], ud[2], ud[0], 날아가는 것, 대상)
                            }
                            DestroyMissile(i, out);
                            erased = true;
                        } else {
                            reflectFrom(hx, hy);
                        }
                    }
                }
                (void)onx; (void)ony;
            }
        }
        if (erased) continue;
        // ⑤ 넘김(server_update.go:80~85)
        missiles_[i].dir1 = missiles_[i].dir2;
        missiles_[i].prevX = missiles_[i].posX; missiles_[i].prevY = missiles_[i].posY;
        missiles_[i].posX = missiles_[i].newX; missiles_[i].posY = missiles_[i].newY;
        missiles_[i].forceX = missiles_[i].forceY = 0;
        ++i;
    }
}

// 든 무기의 종류 번호 — 죽음 알림의 무기 그림(3-d). 기본 장비는 직업 표, 시험용 바꿔 들기는 main 이 채운 이름 표
uint16_t Server::WeaponWireOf(const Session& s) const {
    if (s.debugWeapon < 0) return LoadoutFor(s.charClass).weapon;
    const auto it = weaponWire_.find(static_cast<uint32_t>(s.debugWeapon));
    return it == weaponWire_.end() ? 0 : it->second;
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
                const bool entered = PlayerSetState(s, kPlayerStateAttack, frame, &out);
                if (!entered) s.stamina = static_cast<uint8_t>(s.stamina + cost);   // sub_4F7DB0 되돌림
                std::printf("[공격] 프레임 %u %s 무기 0x%X %s · 기력 %d 깎음 → %u\n", frame, s.addr.c_str(), equip,
                            entered ? "상태 1 들어감" : "쉬는 시간이라 안 들어감(기력 되돌림)", cost, s.stamina);
            } else {
                std::printf("[공격] 프레임 %u %s 무기 0x%X 기력 모자람 %u < %d\n", frame, s.addr.c_str(), equip, s.stamina, cost);
            }
            std::fflush(stdout);
        }
        // 투명(0)·무적(23) 끄기 · 타원 방패(67) 취소 — 무기 갈래는 **클릭마다** 끈다(`:1629~1631`)
        BuffOff(Target{ s.addr, -1 }, 0);
        BuffOff(Target{ s.addr, -1 }, 23);
        DurCancelFor(67, s.addr, out);                     // spellCancelDurSpell_4FEB10(67)
        return;
    }
    if (s.moveState != kPlayerStateAttack) {
        const bool entered = PlayerSetState(s, kPlayerStateAttack, frame, &out);
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
    const Target t = FindObjectAtCursor(s);
    if (t.none()) return false;
    return !TargetIsEnemy(s, t);
}

// findObjectAtCursor_54AF40 — 커서 둘레 100 · 시야선 · 몸 안에 커서 · 가장 아래(y 큰) 것. (3-a 에서 CursorOnFriend 안에 옮긴 것을 떼어 냄 · 4-a)
Target Server::FindObjectAtCursor(const Session& s) const {
    Target found;
    if (!s.hasAim) return found;
    const float cx = static_cast<float>(s.aimX), cy = static_cast<float>(s.aimY);
    float best = 0.0f;
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
        if (o.buffs.Has(0) && !s.buffs.Has(21)) continue;  // 투명이면 적외선일 때만(`GAME5.c:3588`, 4-b1)
        if (consider(static_cast<float>(o.posX), static_cast<float>(o.posY), static_cast<float>(kBodyRadius))) {
            found = Target{ o.addr, -1 };
        }
    }
    for (size_t i = 0; i < pets_.size(); ++i) {
        const Creature& k = pets_[i];
        auto tb = typeBodies_.find(k.type);
        if (tb == typeBodies_.end() || tb->second.shape != 2) continue;   // 상자 몸 몬스터는 map_57B850(아직 없음)
        if (k.buffs.Has(0) && !s.buffs.Has(21)) continue;  // 투명이면 적외선일 때만(4-b1)
        if (consider(static_cast<float>(k.x), static_cast<float>(k.y), tb->second.r)) found = Target{ std::string(), static_cast<int>(i) };
    }
    return found;
}

// IsEnemyTo(사람, 대상) — 사람이면 팀 규칙 · 소환수면 주인 기준(맵 몬스터는 늘 적 · 내 것은 친구)
bool Server::TargetIsEnemy(const Session& s, const Target& t) const {
    if (!t.addr.empty()) {
        if (t.addr == s.addr) return false;
        auto it = sessions_.find(t.addr);
        return it != sessions_.end() && IsEnemyTo(s, it->second);
    }
    if (t.pet < 0 || static_cast<size_t>(t.pet) >= pets_.size()) return false;
    const Creature& k = pets_[static_cast<size_t>(t.pet)];
    if (k.owner.empty()) return true;
    if (k.owner == s.addr) return false;
    return IsEnemyTo(s.addr, k.owner);
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
        src.obj = ai.weapon;                                // +28 무기 오브젝트 — 죽음 알림의 무기 그림(3-d)
        src.objType = ai.weapon ? WeaponWireOf(s) : 0;
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
        if (!CanSeeBuffs(&s.buffs, nullptr, 0.0, 0.0, 1) || !MapTraceVisionSkip(ax, ay, ox, oy, obstacleIdx)) return false;   // unitCanInteractWith(…, 1) — 눈멂만(4-a4)
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
            kv.second.score = 0;
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
            if (wasInGame) PlayerCancelSpells(addr, out);   // 오브젝트 지움 → playerCancelSpells(object.go:70, 4-a3)
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
            k.x = kv.second.x + (debugPetWild_ ? debugDummyDx_ : 80);   // 허수아비는 주먹 사거리(30) 안(기본 25)
            k.y = kv.second.y;
            k.health = static_cast<uint16_t>(TuningFor(k.type).health);
            pets_.push_back(k);
            debugOwnPetNetCode_ = k.netCode;
            std::printf("[진단] 프레임 %u 에 %s %s 임프(번호 %u · 체력 %u)를 사람(%d,%d) 오른쪽 +%d 에 고정\n",
                        frame_, kv.second.addr.c_str(), debugPetWild_ ? "옆에 주인 없는" : "소유",
                        static_cast<unsigned>(k.netCode), static_cast<unsigned>(k.health),
                        kv.second.x, kv.second.y, debugPetWild_ ? debugDummyDx_ : 80);
            std::fflush(stdout);
            break;
        }
    }
    if (debugEnchantBuff_ >= 0 && !debugEnchantDone_ && firstInGameFrame_ != 0 && frame_ >= firstInGameFrame_ + debugEnchantAfter_) {
        for (auto& kv : sessions_) {
            if (!kv.second.inGame || kv.second.observer) continue;
            ApplyEnchant(Target{ kv.first, -1 }, debugEnchantBuff_, 0, debugEnchantPower_);
            std::printf("[진단] 프레임 %u 에 %s 버프 %d(세기 %d) 걸음 → 비트 0x%08X\n", frame_, kv.first.c_str(), debugEnchantBuff_,
                        debugEnchantPower_, kv.second.buffs.bits);
            std::fflush(stdout);
            debugEnchantDone_ = true;
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
    //    그 틱의 시전 목록도 함께(spellBookReact_4FCB70 — 정답지는 유닛 갱신 뒤 · server.go:341)
    for (int step = 0; step < ticks; ++step) {
        UpdatePlayerMovement(frame_ - static_cast<uint32_t>(ticks - 1 - step), out);
        SpellCastByBook(frame_ - static_cast<uint32_t>(ticks - 1 - step), out);
        SpellCastByPlayer(frame_ - static_cast<uint32_t>(ticks - 1 - step), out);   // 4-a3 지속 주문(spellBookReact_4FCB70)
    }

    // ── 묶음 6-f(6) 불려 나온 것들을 **쌓임으로** 움직인다 ────────────────
    //
    // ★★ 2026-09-15 이전에는 이 자리가 **임시 구현**이었다 — 가장 가까운 적에게
    //   곧장 다가가 사거리에서 때렸다. 이제 **AI 행동 쌓기**(6-a~6-g)가 그 일을 한다.
    //
    // ⭕ 한 틱에 하는 일
    //   1. 그 몬스터의 **빠르기**를 세운다(thing/monster.bin 값)
    //   2. **가장 가까운 적**을 골라 `enemyAddr` 에 적는다 — 싸움(15)이 이것을 본다
    //      ⚠ 정답지의 「본 것」 갱신(`unitUpdateSightMB_5281F0` 따위)은 아직 안 옮겼다.
    //        지금은 **아주 멀리까지 보는** 임시 규칙이다(전과 같다)
    //   3. **싸움(15)이 쌓임에 없으면 쌓는다** — 그 안에서 쫓기·때리기·쏘기가 갈린다
    //   4. **조건을 훑고**(`AIProcDeps`) **쌓임을 굴린다**(`AITick`)
    //   5. 행동이 세워 둔 **속도로 자리를 옮긴다**
    //
    // ⚠ 속도는 **틱마다 0 으로 되돌린다** — 정답지는 물리가 깎지만 우리는 그 자리가 없다.
    //   안 되돌리면 때리는 동안에도 지난 속도로 밀려간다
    // ⚠ 때리다 상대가 죽으면 **목록이 바뀐다** — 크기가 바뀌면 그 걸음을 접는다
    for (int step = 0; step < ticks && !pets_.empty(); ++step) {
        const size_t before = pets_.size();
        for (size_t pi = 0; pi < pets_.size(); ++pi) {
            Creature& k = pets_[pi];
            if (debugOwnPetNetCode_ != 0 && k.netCode == debugOwnPetNetCode_) continue;   // 진단 임프 — 고정
            const PetTuning tune = TuningFor(k.type);
            k.speedCur = static_cast<float>(tune.speed);
            // ★★ **보는 쪽은 매 프레임 「돌아가려는 쪽」을 그대로 옮겨 쓴다**
            //   (`server_update.go:81` · `server.go:167` — `obj.Direction1 = obj.Direction2`).
            //   점진 회전은 **want 를 8씩 돌려서** 생기고, 보는 쪽은 한 프레임 뒤따라온다.
            //   ⚠ 이 한 줄이 없으면 바라보기(26)의 「다 봤나」가 **영영 안 참이 되어** 제자리에 선다
            //     (2026-09-15 틱 교체에서 잡혔다 — 6-d 는 want 쪽만 옮겼었다)
            k.facing = k.facingWant;

            // ★ **보이는 적만 「지금 상대」로 삼는다** — 조건 45(보이나)가 같은 것을 보기 때문이다.
            //   안 보이는 것을 상대로 잡아 두면, 45 가 깨져 사슬이 빠지고 싸움이 다시 쌓기를
            //   **매 틱 되풀이**해 제자리에 선다(2026-09-15 벽 시험에서 잡혔다).
            //   정답지도 **알아본 적**만 `+1196` 에 넣는다
            //   ⚠ 잣대는 **크리처 자리에서 보는 선 추적**이다. `CanInteractFrom` 은 **주인 시점**이라
            //     크리처 시야로는 틀린다(주인이 보면 소환수도 본다는 셈이 된다)
            const Target t = debugNoAutoFight_ ? Target{} : NearestTarget(k.owner, k.x, k.y, 1e9);
            float sx = 0, sy = 0;
            const bool sees = !t.none() && TargetXY(t, sx, sy) &&
                              MapTraceVision(static_cast<float>(k.x), static_cast<float>(k.y), sx, sy);
            if (sees) {
                k.enemyAddr = t.addr;
                k.enemyPet = t.pet;
                // ★ **할 일이 아예 없을 때만** 쌓는다(바닥이 놀기 하나).
                //   ⚠ 정답지는 **적을 알아본 순간**(`unitUpdateSightMB_5281F0` 둘레)에 한 번 쌓는다.
                //     그 「알아보기」를 아직 안 옮겨서, **놀고 있으면 쌓는다**로 대신한다.
                //     이렇게 두면 이미 굴러가는 사슬을 매 틱 덮어쓰지 않는다
                //   ★★ 묶음 6-h(2) — **주인이 있으면 싸움이 아니라 호위(3)** 를 쌓는다.
                //     정답지에서 **싸움을 올리는 쪽이 호위**다(`mobActionEscort_546430`) —
                //     호위가 바닥에 남아 **서 있는 명령**이 되고, 싸움이 끝나면 다시 주인에게 붙는다.
                //     주인이 없는 것(퀘스트 몬스터)은 호위가 스스로 빠지므로 **싸움을 바로 쌓는다**
                //   ⚠ **사람이 명령을 준 것은 안 덮는다**(6-i) — 「가만히」가 바닥에 놀기를 남기므로
                //     이 관문이 없으면 다음 틱에 호위로 되돌아가 명령이 안 선다
                if (!k.ordered && k.aiStackInd == 0 && k.aiStack[0].action == 0) {
                    if (!k.owner.empty()) {
                        AIPush(k, 3);
                    } else if (AIPush(k, 15)) {
                        k.aiStack[k.aiStackInd].argX = sx;          // 마지막으로 본 자리
                        k.aiStack[k.aiStackInd].argY = sy;
                        k.aiStack[k.aiStackInd].argU32 = frame_;    // 마지막 소식 때
                    }
                } else {
                    // ★ **보이는 동안에는 「마지막으로 본 자리」를 계속 고쳐 적는다.**
                    //   정답지는 알아보기 쪽이 이 자리를 갱신한다. 안 고치면 적이 옮겨가도
                    //   **처음 본 자리로만 달려간다**(2026-09-15 소환수 죽음 시험에서 잡혔다)
                    for (int i = 0; i <= k.aiStackInd && i < Creature::kAIStackMax; ++i)
                        if (k.aiStack[i].action == 15) { k.aiStack[i].argX = sx; k.aiStack[i].argY = sy; }
                }
            } else if (!debugNoAutoFight_) {
                k.enemyAddr.clear();
                k.enemyPet = -1;
                // ★ 적이 안 보여도 **주인이 있으면 호위**는 선다 — 그래야 따라다닌다
                if (!k.ordered && !k.owner.empty() && k.aiStackInd == 0 && k.aiStack[0].action == 0) AIPush(k, 3);
            }

            k.velX = 0; k.velY = 0;
            AIProcDeps(k, out);
            if (pets_.size() != before) break;
            AITick(k, out);
            if (pets_.size() != before) break;
            if (k.velX != 0.0 || k.velY != 0.0) CreatureApplyVelocity(k);
        }
        if (pets_.size() != before) break;
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

    // ★ 09-14 갱신 알림 정답지화 — 접속 직후 HUD(`45`·`43`·`dd`·`de`)를 다섯 번 되풀이하던 자리표시를 걷어냈다.
    //   정답지는 스냅샷 꼬리(첫 playerReportAnything 몫 dd·de·48·43 — SnapshotTail.cpp #21·#22)뿐이고, 그 뒤는 바뀔 때만(PlayerReportAnything).
    //   되풀이가 필요했던 까닭(cc 가 접속 직후 막힘)은 7821fc7 대기열로 해소됐다.

    UpdateFlags(out);
    UpdateProps(out);   // 문·오벨리스크 (P5)

    // (4-a4) 무적 켜짐·꺼짐 비교 루프는 걷어냄 — 버프가 바뀌는 자리(setUnitBuffFlags)가 곧바로 `5a` 를 올린다

    UpdatePlaced(out);                                     // 4-c3 놓인 오브젝트(독구름 …)의 주기 함수·수명
    PoisonTick(out);                                       // 4-c2a 독 — 감쇠와 피해(server.go 의 유닛 갱신 자리)

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
                const std::vector<uint8_t> tail = BuildSnapshotTail(s.netCode, s.charClass, s.tickFrame, s.tickFrame);   // 3-c: 받는 사람의 지금 프레임
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
            // 스냅샷 꼬리에 방어값 `49` 가 들어 있다 — 그것이 첫 playerReportAnything 몫이다(+232 = +228).
            //   녹음 서두는 위저드 녹음이라 그 값(0x3e6b8520)이다
            // +2184·+10·+6 도 같은 자리 — 꼬리의 dd·de·48(#21) 과 43(#22) 이 첫 playerReportAnything 몫이다.
            // ★★ **「내가 무엇을 알렸나」를 적어야지 「지금 값」을 적으면 안 된다.**
            //   꼬리는 `dd`·`43` 에 **직업 최대치**(HealthOnline)를 싣는다 — 정품은 접속 때 늘 가득이라 둘이 같지만,
            //   `--start-health` 처럼 서버 값이 다르면 어긋난다. 「지금 값」을 적어 두면 PlayerReportAnything 이
            //   「안 바뀌었다」고 보고 **영영 `43` 을 안 보낸다** → 클라 체력바가 가득인 채로 굳는다
            //   (2026-09-14 b5a_heal 화면에서 잡음 — 전에는 HUD 5회 되풀이가 이걸 덮어 가리고 있었다).
            const CharClass tailCls = firstSnapshot_.empty() ? s.charClass : CharClass::Wizard;
            s.armorReported = ArmorValueFloat(tailCls);
            s.statsDirty = false;
            s.healthReported = static_cast<uint16_t>(HealthOnline(tailCls));   // 꼬리가 실은 값
            s.manaPrev      = static_cast<uint16_t>(ManaOnline(tailCls));      // 꼬리 `de`/`45` 가 실은 값
            continue;
        }

        // 자기 몸 갱신 몫 — 바뀐 것만 알린다(playerReportAnything_4D9900 · 묶음 3-d). 스냅샷을 기다리는 동안은 아니다
        if (s.snapshotIn < 0) PlayerReportAnything(s, out);

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

        // ★ 2026-09-13 묶음 3-c — 여기서 45틱 뒤 저절로 되살리던 것을 걷어냈다. 부활은 걷기 틱의 상태 4 가 한다
        //   (UpdatePlayerMovement · PlayerRespawn — 클릭하거나 자동 부활 설정일 때).

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
            // netPlayerObjSend_518C30 a4 갈래 — 보는 사람 s 에게 o 의 `42` 증감(마지막 피해 뒤 2틱 · 음수만 · Packet0)
            ReportHealthDeltaTo(s, o.netCode, o.health, o.lastDamageFrame, o.hpReportedTo, out);

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

        // ★ 4-a2 날아가는 오브젝트(Magic 등) — SIMPLE 이라 붙는 바이트가 없다
        for (const auto& mo : missiles_) {
            ObjEntry e;
            FillEntry(s.aliases, mo.netCode, mo.type, s.tickFrame,
                      static_cast<int>(mo.posX), static_cast<int>(mo.posY), mo.dir1, e,
                      /*complex=*/false);
            Relativize(e, mo.netCode, static_cast<int>(mo.posX), static_cast<int>(mo.posY),
                       baseX, baseY);
            m.objects.push_back(e);
        }

        // 4-c3 놓인 오브젝트(독구름 …) — SIMPLE 이라 붙는 바이트가 없다
        for (const auto& po : placed_) {
            ObjEntry e;
            FillEntry(s.aliases, po.netCode, po.type, s.tickFrame,
                      static_cast<int>(po.x), static_cast<int>(po.y), 0, e, /*complex=*/false);
            Relativize(e, po.netCode, static_cast<int>(po.x), static_cast<int>(po.y), baseX, baseY);
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
        for (auto& k : pets_) {
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
            ReportHealthDeltaTo(s, k.netCode, k.health, k.lastDamageFrame, k.hpReportedTo, out);   // netSendComplexObject_518960 a3 갈래(+412+2×i)
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
