// UpdateStream.cpp — 상태 갱신 뜯기. 짜임과 근거는 UpdateStream.h 에 적어두었다.
#include "nox/UpdateStream.h"

#include <cstring>

namespace nox {

namespace {

uint16_t RdU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

// 항목을 하나씩 따라간다. 끝 표식에 닿으면 참.
//
// ★ 갈래를 여는 자리가 하나 있다 — 복합 오브젝트의 마지막 덧붙이 바이트다.
//   클라는 `sprite[112] & 4` 로 가르는데 그 값이 패킷에 없다. 그래서 두 갈래를
//   다 넣어 보고 **끝 표식까지 닿는 쪽**을 고른다.
//
// ⚠ 갈래를 함부로 넓히면 "아무 데나 끼워 맞춰" 100% 가 나온다. 09-05 에 그렇게
//   속은 적이 있다(좌표라고 읽은 값이 `(1,64)` 로 2만 7천 번). 그래서 갈래는
//   **복합인 것에만, 두 가지로만** 연다. 복합이 아니면 0 으로 못박는다.
bool Walk(const uint8_t* data, size_t n, int x, int y,
          const AliasTable* aliases, std::vector<ObjEntry>& objs, size_t& used) {
    if (n < 3) return false;
    if (data[0] == 0 && data[1] == 0 && data[2] == 0) {
        used = 3;
        return true;   // 끝 표식
    }

    const uint8_t* p = data;
    size_t left = n;
    ObjEntry e;

    uint8_t id = p[0];
    if (id == 0) {          // 0 이 앞에 붙으면 절대 좌표 모드
        e.absolute = true;
        ++p; --left;
        if (left < 1) return false;
        id = p[0];
    }
    e.alias = id;
    ++p; --left;

    if (id == 0xff) {
        if (left < 4) return false;
        e.netCode = RdU16(p);
        e.type = RdU16(p + 2);
        p += 4; left -= 4;
    } else if (aliases) {
        uint16_t nc = 0, ty = 0;
        if (aliases->Lookup(id, nc, ty)) {
            e.netCode = nc;
            e.type = ty;
        }
    }

    if (e.absolute) {
        if (left < 4) return false;
        x = RdU16(p);
        y = RdU16(p + 2);
        p += 4; left -= 4;
    } else {
        if (left < 2) return false;
        e.dx = static_cast<int8_t>(p[0]);
        e.dy = static_cast<int8_t>(p[1]);
        x += e.dx;
        y += e.dy;
        p += 2; left -= 2;
    }
    if (x < 0 || x > kMaxCoord || y < 0 || y > kMaxCoord) return false;
    e.x = x;
    e.y = y;

    // 덧붙이 바이트 — 복합이 아니면 없다.
    int cands[4];
    int nCands = 0;
    const bool known = ThingKnown(e.type);
    const bool complex = known && ThingIsComplex(e.type);
    if (!known && e.type == 0) {
        // 종류를 모른다(별칭 표에 없는 자리). 어쩔 수 없이 갈래를 연다.
        cands[nCands++] = 0;
        cands[nCands++] = 1;
        cands[nCands++] = 2;
        cands[nCands++] = 3;
    } else if (complex) {
        if (left < 1) return false;
        int base = 1;                       // 방향·애니 1B
        if (p[0] & 0x80) ++base;            // 최상위 비트면 프레임 1B 더
        cands[nCands++] = base;
        cands[nCands++] = base + 1;         // `sprite[112] & 4` 는 아직 모른다
    } else {
        cands[nCands++] = 0;                // 복합이 아니면 한 바이트도 안 붙는다
    }

    for (int i = 0; i < nCands; ++i) {
        const int extra = cands[i];
        if (static_cast<size_t>(extra) > left) continue;
        const size_t before = objs.size();
        e.extra = extra;
        for (int j = 0; j < extra && j < 3; ++j) e.extraBytes[j] = p[j];
        objs.push_back(e);
        size_t sub = 0;
        if (Walk(p + extra, left - extra, x, y, aliases, objs, sub)) {
            used = static_cast<size_t>(p - data) + extra + sub;
            return true;
        }
        objs.resize(before);
    }
    return false;
}

}  // namespace

UpdateMsg DecodeUpdate(const uint8_t* data, size_t n, const AliasTable* aliases) {
    UpdateMsg m;
    const uint8_t* p = data;
    size_t left = n;

    // ── 머리
    if (left < 1) return m;
    if (p[0] == 0xff) {
        if (left < 5) return m;
        m.headAlias = 0xff;
        m.headNetCode = RdU16(p + 1);
        m.headType = RdU16(p + 3);
        p += 5; left -= 5;
    } else {
        m.headAlias = p[0];
        ++p; --left;
    }
    if (left < 6) return m;
    m.x = RdU16(p);
    m.y = RdU16(p + 2);
    m.flags = p[4];
    p += 5; left -= 5;
    if (m.flags & 0x80) {          // 스프라이트 프레임이 한 바이트 더
        if (left < 1) return m;
        m.headFrame = p[0];
        ++p; --left;
    }
    if (left < 1) return m;
    m.headAnim = p[0];
    ++p; --left;                   // 애니메이션 상태 한 바이트

    // ── 항목. 좌표는 직전 항목에서 이어 쌓인다.
    size_t used = 0;
    if (!Walk(p, left, m.x, m.y, aliases, m.objects, used)) {
        m.objects.clear();
        return m;
    }
    m.consumed = static_cast<size_t>(p - data) + used;
    m.ok = true;
    return m;
}

// ── 다시 짜기 ────────────────────────────────────────────
//
// 뜯을 때와 **정확히 거울**이어야 한다. 한 자리라도 어긋나면 왕복 검사에서 걸린다.
size_t EncodeUpdate(const UpdateMsg& m, uint8_t* out, size_t cap) {
    size_t k = 0;
    auto need = [&](size_t n) { return k + n <= cap; };
    auto putU16 = [&](uint16_t v) {
        out[k++] = static_cast<uint8_t>(v);
        out[k++] = static_cast<uint8_t>(v >> 8);
    };

    // ── 머리
    if (m.headAlias == 0xff) {
        if (!need(5)) return 0;
        out[k++] = 0xff;
        putU16(m.headNetCode);
        putU16(m.headType);
    } else {
        if (!need(1)) return 0;
        out[k++] = m.headAlias;
    }
    if (!need(5)) return 0;
    putU16(static_cast<uint16_t>(m.x));
    putU16(static_cast<uint16_t>(m.y));
    out[k++] = m.flags;
    if (m.flags & 0x80) {
        if (!need(1)) return 0;
        out[k++] = m.headFrame;
    }
    if (!need(1)) return 0;
    out[k++] = m.headAnim;

    // ── 항목
    for (const auto& e : m.objects) {
        if (e.absolute) {
            if (!need(1)) return 0;
            out[k++] = 0;               // 절대 좌표 모드 표시
        }
        if (e.alias == 0xff) {
            if (!need(5)) return 0;
            out[k++] = 0xff;
            putU16(e.netCode);
            putU16(e.type);
        } else {
            if (!need(1)) return 0;
            out[k++] = e.alias;
        }
        if (e.absolute) {
            if (!need(4)) return 0;
            putU16(static_cast<uint16_t>(e.x));
            putU16(static_cast<uint16_t>(e.y));
        } else {
            if (!need(2)) return 0;
            out[k++] = static_cast<uint8_t>(e.dx);
            out[k++] = static_cast<uint8_t>(e.dy);
        }
        if (e.extra > 0) {
            if (!need(static_cast<size_t>(e.extra))) return 0;
            for (int j = 0; j < e.extra && j < 3; ++j) out[k++] = e.extraBytes[j];
        }
    }

    // ── 끝 표식
    if (!need(3)) return 0;
    out[k++] = 0;
    out[k++] = 0;
    out[k++] = 0;
    return k;
}

}  // namespace nox
