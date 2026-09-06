#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""updscan.py — `0xa4 UPDATE_STREAM` 뒤에 무엇이 딸려 오는지 센다.

왜 파이썬이냐 — 같은 고침을 Go 쪽(`tools/dumpscan/updstream.go`)에도 넣었는데
**이 PC 에 Go 가 없어서 빌드를 못 한다**(2026-09-07 확인: go.exe 가 어디에도 없고
설치 기록도 없다. 모듈 캐시 `C:\\Users\\USER\\go\\pkg\\mod` 만 남아 있다).
판정을 미룰 수 없어 같은 계산을 파이썬으로 따로 세웠다. 둘은 서로의 대조군이다.

## 무엇을 재나

opennox 디코더는 `0xa4` 가 길이 -1 이라 **패킷 나머지를 통째로 먹는다**
(`netmsg/decoder.go:57-62`). 그래서 한 패킷에서 `0xa4` 뒤에 오는 메시지가 다 사라진다.

여기서는 `0xa4` 를 클라 원본 파서(`GAME2_3.c:1580`, `PROTOCOL.md` 「짜임」 절)대로 뜯어
**그 뒤에 남는 바이트의 오프코드**를 센다. 남는 게 있으면 삼킨 것이 있었다는 뜻이다.

## 쓰는 법

    python tools/updscan.py _evidence/CAP_server_0907.txt _evidence/types.tsv
    python tools/updscan.py <덤프> <종류표> --dir SEND --show 12

종류표는 `/debug/nox/types` 에서 뽑은 `ind <탭> 복합(0/1) <탭> 이름` 이다.
게임 데이터라 `_evidence/` 아래 두고 커밋하지 않는다.
"""
import argparse
import re
import sys
from collections import Counter, defaultdict

# opennox `netmsg/codes.go` 의 이름표. 세는 데 필요한 만큼만.
OPNAME = {}

TIMESTAMP = 0x27
UPDATE_STREAM = 0xA4
SERVER_ACCEPT = 0x01
PLAYER_INPUT = 0x3F
TRY_SPELL = 0x79
SERVER_CMD = 0xBB
SEQ_IMPORTANT = 0xCC
TEXT_MESSAGE = 0xA8
MAX_COORD = 6000
ALIAS_TTL = 60          # 프레임. alias.go 와 같은 값
STEP_BUDGET = 20000     # 되짚기가 지수로 터지지 않게 (앞선 도구에서 겪은 함정)

LINE = re.compile(
    r"^(\d\d:\d\d:\d\d\.\d\d\d)\s+(SEND|RECV)\s+(\S+)\s+len=(\d+)\s+\|\s+([0-9a-f ]+?)\s+\|"
)


try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass


def harvest_keys(path):
    """수락 패킷에서 상대별 XOR 키를 미리 모은다.

    아직 평문일 때 `1f 00 | 01 <ID 4바이트> <키>` 로 온다 (msg_accept.go:63-65 —
    MsgServerAccept 는 ID 4바이트 다음 바이트가 XorKey 다). 실측 원문 —

        80 00 1f 00 01 01 00 00 00 60      ← 마지막 0x60 이 키
    """
    # ⚠ 두 방향을 다 훑는다. 서버 덤프에서는 수락이 SEND 에 있지만
    #   **클라 덤프에서는 RECV 에 있다.** 어느 쪽이든 수락 시점엔 아직 평문이다.
    keys = {}
    for _t, _d, peer, raw in read_dump(path, ""):
        b = raw[2:]
        for j in range(2, len(b) - 5):
            if b[j] == 0x01 and b[j - 1] == 0x00 and b[j - 2] == 0x1F:
                k = b[j + 5]
                if k:
                    keys[peer] = k
    return keys


def load_names(codes_go):
    """codes.go 의 `MSG_X = Op(N)` 줄에서 번호→이름을 읽는다. 없으면 빈 표."""
    try:
        with open(codes_go, encoding="utf-8") as f:
            for ln in f:
                m = re.match(r"\s*(MSG_\w+)\s*=\s*Op\((\d+)\)", ln)
                if m:
                    OPNAME[int(m.group(2))] = m.group(1)
    except OSError:
        pass


def load_types(path):
    """ind → 복합인가. 와이어 종류 번호가 곧 ind 다."""
    complex_of = {}
    name_of = {}
    with open(path, encoding="utf-8") as f:
        for ln in f:
            ln = ln.rstrip("\r\n")
            if not ln or ln.startswith("#"):
                continue
            c = ln.split("\t")
            if len(c) < 2:
                continue
            ind = int(c[0])
            complex_of[ind] = c[1].strip() == "1"
            if len(c) >= 3:
                name_of[ind] = c[2]
    return complex_of, name_of


def read_dump(path, want_dir):
    for ln in open(path, encoding="utf-8", errors="replace"):
        m = LINE.match(ln)
        if not m:
            continue
        t, d, peer, _n, hexs = m.groups()
        if want_dir and d != want_dir:
            continue
        yield t, d, peer, bytes.fromhex(hexs.replace(" ", ""))


class Aliases:
    """별칭 → 종류. 2초(60프레임) 임대라 만료를 흉내 낸다."""

    def __init__(self):
        self.t = {}          # alias -> (typ, deadline)

    def set(self, alias, typ, frame):
        if 0 < alias < 0xFF:
            self.t[alias] = (typ, frame + ALIAS_TTL)

    def get(self, alias, frame):
        v = self.t.get(alias)
        if not v:
            return None
        typ, dl = v
        if frame > dl:
            return None
        return typ


def parse_entries(buf, i, x, y, complex_of, al, frame, budget):
    """항목 목록을 뜯는다. 끝난 자리(00 00 00 다음)를 돌려주고, 못 뜯으면 None.

    복합 오브젝트의 덧붙이 바이트 수에 갈래가 하나 남아 있어(`sprite[112] & 4`)
    되짚기를 한다. 예산을 두어 지수로 터지지 않게 막는다.
    """
    budget[0] -= 1
    if budget[0] <= 0:
        return None
    n = len(buf)
    if i + 3 > n:
        return None
    if buf[i] == 0 and buf[i + 1] == 0 and buf[i + 2] == 0:
        return i + 3

    j = i
    absolute = False
    if buf[j] == 0:
        absolute = True
        j += 1
        if j >= n:
            return None
    alias = buf[j]
    j += 1
    typ = None
    if alias == 0xFF:
        if j + 4 > n:
            return None
        typ = int.from_bytes(buf[j + 2:j + 4], "little")
        netcode = int.from_bytes(buf[j:j + 2], "little")
        j += 4
    else:
        netcode = None
        typ = al.get(alias, frame)

    if absolute:
        if j + 4 > n:
            return None
        x = int.from_bytes(buf[j:j + 2], "little")
        y = int.from_bytes(buf[j + 2:j + 4], "little")
        j += 4
    else:
        if j + 2 > n:
            return None
        x += int.from_bytes(buf[j:j + 1], "little", signed=True)
        y += int.from_bytes(buf[j + 1:j + 2], "little", signed=True)
        j += 2
    if not (0 <= x <= MAX_COORD and 0 <= y <= MAX_COORD):
        return None

    # 덧붙이 바이트 — 복합이 아니면 0. 종류를 모르면 어쩔 수 없이 갈래를 연다.
    if typ is None:
        cands = [0, 1, 2, 3]
    elif complex_of.get(typ, False):
        if j >= n:
            return None
        base = 2 if (buf[j] & 0x80) else 1
        cands = [base, base + 1]
    else:
        cands = [0]

    for extra in cands:
        if j + extra > n:
            continue
        saved = dict(al.t)
        if alias == 0xFF and netcode:
            al.set(alias, typ, frame)     # 전체 번호로 온 것은 별칭을 새로 뗀다
        r = parse_entries(buf, j + extra, x, y, complex_of, al, frame, budget)
        if r is not None:
            return r
        al.t = saved
    return None


def parse_update(buf, i, complex_of, al, frame):
    """`0xa4` 하나를 뜯어 **끝난 자리**를 돌려준다. 못 뜯으면 None."""
    n = len(buf)
    j = i + 1                       # 오프코드
    if j >= n:
        return None
    if buf[j] == 0xFF:
        if j + 5 > n:
            return None
        j += 5
    else:
        j += 1
    if j + 5 > n:
        return None
    x = int.from_bytes(buf[j:j + 2], "little")
    y = int.from_bytes(buf[j + 2:j + 4], "little")
    flags = buf[j + 4]
    j += 5
    if flags & 0x80:
        j += 1
    j += 1                          # 애니메이션 상태
    if j > n:
        return None
    return parse_entries(buf, j, x, y, complex_of, al, frame, [STEP_BUDGET])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("types")
    ap.add_argument("--dir", default="SEND")
    ap.add_argument("--show", type=int, default=8)
    ap.add_argument("--codes", default="")
    ap.add_argument("--xor", default="", help="XOR 키를 직접 준다 (예 0x60). 안 주면 수락 패킷에서 찾는다")
    ap.add_argument("--tally", default="", help="opennox 의 noxnet 폴더 경로. 주면 삼킴을 걷어낸 전수 집계를 낸다")
    ap.add_argument("--pick", default="", help="이 오프코드들의 원문을 뽑는다 (쉼표, 예 0x42,0x80)")
    ap.add_argument("--client", action="store_true", help="클라 쪽에서 잡은 덤프다 (방향이 뒤집힌다)")
    a = ap.parse_args()

    if a.codes:
        load_names(a.codes)
    complex_of, name_of = load_types(a.types)

    xor_key = harvest_keys(a.dump) if not a.xor else {}
    fixed_key = int(a.xor, 0) if a.xor else 0
    print(f"XOR 키: {a.xor if a.xor else {p: hex(k) for p, k in xor_key.items()}}")

    if a.tally:
        name_of2, cli, srv = build_lengths(a.tally)
        return tally(a, complex_of, xor_key, fixed_key, name_of2, cli, srv)

    frame = 0
    al = Aliases()

    tail_ops = Counter()          # 삼켜지던 오프코드
    tail_bytes = 0
    samples = defaultdict(list)
    n_upd = n_ok = n_bad = 0
    n_pkt = 0

    for t, d, peer, raw in read_dump(a.dump, a.dir):
        if len(raw) < 3:
            continue
        n_pkt += 1
        k = fixed_key or xor_key.get(peer, 0)
        if k and d == "SEND":
            raw = bytes(c ^ k for c in raw)
        buf = raw[2:]

        i = 0
        while i < len(buf):
            op = buf[i]

            if op == TIMESTAMP:
                i += 3
                continue
            if op == UPDATE_STREAM:
                n_upd += 1
                frame += 1
                end = parse_update(buf, i, complex_of, al, frame)
                if end is None:
                    n_bad += 1
                    break
                n_ok += 1
                rest = buf[end:]
                if rest:
                    tail_ops[rest[0]] += 1
                    tail_bytes += len(rest)
                    if len(samples[rest[0]]) < a.show:
                        samples[rest[0]].append((t, rest[:16].hex(" ")))
                i = end
                continue
            break

    print(f"{a.dir} 패킷 {n_pkt}개 · 0xa4 {n_upd}건 · 뜯음 {n_ok} · 못 뜯음 {n_bad}")
    if n_upd:
        print(f"뜯기 성공률 {100.0*n_ok/n_upd:.1f}%")
    print()
    if not tail_ops:
        print("★ 0xa4 뒤에 남는 바이트가 없다 — 삼킨 것이 없었다.")
        return
    total = sum(tail_ops.values())
    print(f"★★ 0xa4 뒤에 메시지가 더 있었다 — {total}자리, {tail_bytes}바이트가 사라지고 있었다\n")
    print(f"{'오프코드':>10} {'개수':>8}  이름")
    print("-" * 60)
    for op, c in tail_ops.most_common(30):
        print(f"      0x{op:02x} {c:8d}  {OPNAME.get(op, '?')}")
    print()
    for op, c in tail_ops.most_common(4):
        print(f"── 0x{op:02x} {OPNAME.get(op, '?')} 표본")
        for t, h in samples[op]:
            print(f"   {t}  {h}")
        print()


# ─────────────────────────────────────────────────────────────────────────────
# 길이표 — opennox 원본에서 그대로 뽑는다
#
# 왜 뽑아 쓰나 — 손으로 옮겨 적으면 그 순간 정본이 둘이 되고, 나중에 어느 쪽이
# 맞는지 다투게 된다. 원본을 읽어 만들면 다툴 일이 없다.
#
# 세 군데서 모은다 —
#   codes.go 의 `MSG_X = Op(N)`        → 이름 ↔ 번호
#   codes.go 의 `opLen` 지도            → 구조체가 없는 것들의 본문 길이
#   msg_*.go 의 `EncodeSize()` 상수 반환 → 구조체가 있는 것들의 본문 길이
#
# ★ 방향이 갈리는 것은 넷뿐이다 — `0xaa IMPORTANT` 와 `0xab IMPORTANT_ACK` 의
#   Cli/Srv 짝. 나머지 37개는 `Register` 로 양쪽 공통이다. 그래서 표를 둘로 나눈다.
#   서버 덤프에서 SEND(서버→클라)는 **클라가 읽는 표**, RECV 는 **서버가 읽는 표**다.
# ─────────────────────────────────────────────────────────────────────────────

RE_NETOP = re.compile(
    r"func \(\w*\s*\*(\w+)\) NetOp\(\) netmsg\.Op \{\s*return netmsg\.(MSG_\w+)\s*\}")
RE_SIZE = re.compile(
    r"func \(\w*\s*\*(\w+)\) EncodeSize\(\) int \{\s*return (-?\d+)\s*\}")
RE_REG = re.compile(r"netmsg\.(Register|RegisterClient|RegisterServer)\(&(\w+)\{\}")
RE_CONST = re.compile(r"^\s*(MSG_\w+)\s*=\s*Op\((\d+)\)", re.M)
RE_OPLEN = re.compile(r"^\s*(MSG_\w+):\s*(-?\d+),", re.M)


def build_lengths(noxnet_dir):
    """(번호→이름, SEND용 길이표, RECV용 길이표)를 만든다. 길이는 오프코드를 포함한 전체."""
    import glob
    import os

    codes = open(os.path.join(noxnet_dir, "netmsg", "codes.go"), encoding="utf-8").read()
    num_of = {m.group(1): int(m.group(2)) for m in RE_CONST.finditer(codes)}
    name_of = {v: k for k, v in num_of.items()}
    oplen = {m.group(1): int(m.group(2)) for m in RE_OPLEN.finditer(codes)}

    struct_op, struct_size, reg = {}, {}, {}
    for path in glob.glob(os.path.join(noxnet_dir, "*.go")):
        src = open(path, encoding="utf-8").read()
        for m in RE_NETOP.finditer(src):
            struct_op[m.group(1)] = m.group(2)
        for m in RE_SIZE.finditer(src):
            struct_size[m.group(1)] = int(m.group(2))
        for m in RE_REG.finditer(src):
            reg[m.group(2)] = m.group(1)

    def table(kind):
        """kind = 'RegisterClient' 또는 'RegisterServer'. 공통 등록은 양쪽에 다 들어간다."""
        t = {}
        for name, n in num_of.items():
            if name in oplen and oplen[name] >= 0:
                t[n] = 1 + oplen[name]
        for st, how in reg.items():
            if how != "Register" and how != kind:
                continue
            op = struct_op.get(st)
            if not op or op not in num_of:
                continue
            if st in struct_size:
                t[num_of[op]] = 1 + struct_size[st]
            else:
                t.pop(num_of[op], None)   # 길이가 가변이면 표에서 뺀다
        return t

    # UPDATE_STREAM 은 여기서 따로 뜯으므로 표에 두지 않는다.
    cli, srv = table("RegisterClient"), table("RegisterServer")
    cli.pop(UPDATE_STREAM, None)
    srv.pop(UPDATE_STREAM, None)
    return name_of, cli, srv


def tally(a, complex_of, xor_key, fixed_key, name_of, cli, srv):
    """삼킴을 걷어낸 상태로 오프코드를 전수로 센다."""
    # 어느 표를 쓰나 — **읽는 쪽**이 정한다.
    #   서버 덤프: SEND(서버→클라)는 클라가 읽으니 cli, RECV 는 srv
    #   클라 덤프: 방향이 뒤집히므로 --client 로 맞바꾼다
    if a.client:
        lens = srv if a.dir == "SEND" else cli
    else:
        lens = cli if a.dir == "SEND" else srv
    pick = {int(x, 0) for x in a.pick.split(",") if x.strip()} if a.pick else set()
    picked = defaultdict(list)
    counts = Counter()
    inner = Counter()          # 0xcc 껍데기 안에 들어 있던 것
    stuck = Counter()          # 길이를 몰라 멈춘 자리
    frame = 0
    al = Aliases()
    n_pkt = n_stuck = 0

    for _t, d, peer, raw in read_dump(a.dump, a.dir):
        if len(raw) < 3:
            continue
        n_pkt += 1
        k = fixed_key or xor_key.get(peer, 0)
        if k and d == "SEND":
            raw = bytes(c ^ k for c in raw)
        buf = raw[2:]
        i = 0
        while i < len(buf):
            # 패킷 꼬리의 0 은 채움이지 메시지가 아니다.
            # 안 걸러 내면 `0x00 MSG_SERVER_CONNECT` 로 세어 버린다(0906 서버 RECV 에서 21,759건).
            if not any(buf[i:]):
                break
            op = buf[i]
            if op == UPDATE_STREAM:
                frame += 1
                end = parse_update(buf, i, complex_of, al, frame)
                if end is None:
                    stuck[op] += 1
                    n_stuck += 1
                    break
                counts[op] += 1
                # 항목 목록 뒤의 채움 0 하나를 흡수한다 (PROTOCOL.md 「종결자 3 대 4」 참조).
                # 서버→클라에 오프코드 0x00 은 없으므로 안전하다.
                if end < len(buf) and buf[end] == 0:
                    end += 1
                i = end
                continue
            # 길이표에 안 담기는 가변길이 둘. 근거는 dumpscan/fix.go 에 적혀 있다 —
            #   0x3f PLAYER_INPUT : 오프코드 다음 바이트가 본문 길이 (msg_player_input.go:62)
            #   0x79 TRY_SPELL    : 구조체도 길이표도 없다. 실측 22바이트(오프코드 포함)
            if op == PLAYER_INPUT:
                ln = 2 + buf[i + 1] if i + 1 < len(buf) else None
            elif op == TRY_SPELL:
                ln = 22
            elif op == SEQ_IMPORTANT:
                # ★ 또 하나의 껍데기다 (msg_seq_important.go) —
                #     cc | ID 2바이트(LE) | 길이 1바이트 | 속 메시지 그 길이만큼
                #   `0xaa` 와 같은 짜임이라 **안에 한 층이 더 있다.** 속도 함께 센다.
                ln = 4 + buf[i + 3] if i + 3 < len(buf) else None
                if ln is not None and i + ln <= len(buf) and ln > 4:
                    sub = buf[i + 4]
                    inner[sub] += 1
                    if sub in pick and len(picked[sub]) < a.show:
                        picked[sub].append((_t, peer + " (0xcc 속)", buf[i + 4:i + ln].hex(" ")))
            elif op == TEXT_MESSAGE:
                # msg_text.go — 머리 10바이트(번호2·깃발1·x2·y2·크기1·지속2) + 데이터.
                # 데이터 길이는 `크기`인데 깃발에 UTF8(0x2)·Localized(0x8) 가 없으면 UTF-16 이라 두 배다.
                if i + 8 < len(buf):
                    dsz = buf[i + 8] if (buf[i + 3] & 0x0A) else buf[i + 8] * 2
                    ln = 11 + dsz
                else:
                    ln = None
            elif op == SERVER_CMD:
                # msg_server_cmd.go — 본문은 ID 1 + NetCode 2 + 길이 1 + 2*sz. 전체 5 + 2*sz.
                #
                # ⚠⚠ 2026-09-07 정정 — 한때 `7 + 2*sz` 로 두었다(Encode 가 뒤에 0 을 둘 더 쓰고
                #   Decode 는 안 세므로 그 둘이 메시지에 든다고 본 것). **아니었다.**
                #   클라 덤프에서 `bb 00 d8 02 07` + 14바이트가 **정확히 패킷 끝**이라
                #   더 먹을 2바이트가 없다. 09-07 서버 덤프에서 그 둘이 맞아 보였던 것은
                #   **거기 꼬리에 채움 0 이 두 개 있었기 때문**이다. 우연히 들어맞았다.
                #
                #   A/B (멈춘 자리 수) —
                #     클라 0906 SEND   7+2sz: 15,708   →   5+2sz: 8
                #     서버 0906 RECV   7+2sz: 15,766   →   5+2sz: 7,872
                #
                # ★ 그러므로 `dumpscan/fix.go` 의 `serverCmdTrailer = 2` 도 보편 규칙이 아니다.
                #
                # ★★ 그리고 그 2바이트의 정체가 밝혀졌다 — **보내는 클라가 갈린다.**
                #
                #     sz=7  남는 것 없음   15,699 전수 정확   ← OpenNox 클라
                #     sz=3  2바이트 남음                      ← 정품 클라
                #           `00 c8` 23,713 · `00 00` 21,246 · `00 b8` 7,800 · `80 00` 6 · `00 ee` 1
                #
                #   같은 덤프 안에서도 상대별로 갈린다(0906 서버 RECV: sz=7 이 15,699 로
                #   클라 덤프의 건수와 **정확히 같다** — 그 하나가 OpenNox 클라다).
                #   → 진행 기록의 미결 「0xbb 뒤 00 b8」이 이것이었다.
                #
                #   그 2바이트는 늘 **패킷 끝에 정확히 2바이트만** 남는다. 그걸로 가른다.
                ln = 5 + 2 * buf[i + 4] if i + 4 < len(buf) else None
                if ln is not None and len(buf) - (i + ln) == 2:
                    ln += 2
            else:
                ln = lens.get(op)
            if ln is None or i + ln > len(buf):
                stuck[op] += 1
                n_stuck += 1
                break
            counts[op] += 1
            if op in pick and len(picked[op]) < a.show:
                picked[op].append((_t, peer, buf[i:i + ln].hex(" ")))
            i += ln

    total = sum(counts.values())
    print(f"{a.dir} 패킷 {n_pkt}개 → 메시지 {total}개  ·  길이를 몰라 멈춘 자리 {n_stuck}곳")
    print(f"길이표 {len(lens)}종 (opennox 원본에서 뽑음)\n")
    print(f"{'코드':>6} {'개수':>9}  이름")
    print("-" * 58)
    for op, c in counts.most_common(60):
        print(f"  0x{op:02x} {c:9d}  {name_of.get(op, '?')}")
    if inner:
        print(f"\n★ 0xcc SEQ_IMPORTANT 껍데기 안에 들어 있던 것 (합 {sum(inner.values())}개) —")
        for op, c in inner.most_common(25):
            print(f"  0x{op:02x} {c:9d}  {name_of.get(op, '?')}")

    if stuck:
        print(f"\n멈춘 자리의 오프코드 (길이 미상이거나 잘림) —")
        for op, c in stuck.most_common(15):
            print(f"  0x{op:02x} {c:9d}  {name_of.get(op, '?')}")

    # 이번 밤 내내 물어 온 것들을 따로 짚는다.
    for op in sorted(picked):
        print(f"\n── 0x{op:02x} {name_of.get(op, '?')} 원문 {len(picked[op])}개 (전체 {counts[op]}개)")
        for t, peer, h in picked[op]:
            print(f"   {t}  {peer}  {h}")

    # ⚠ 껍데기(0xcc) 속에 든 것을 안 더하면 표가 거짓말을 한다.
    #   실제로 `0x43 REPORT_PLAYER_HEALTH` 86건이 전부 껍데기 속에 있었고,
    #   겉 개수만 보면 0으로 보여 「체력 절대값은 안 온다」로 잘못 읽힌다.
    print("\n★ 체력·죽음·줍기 계열 (겉 + 0xcc 속) —")
    for op in (0x41, 0x42, 0x43, 0x45, 0x48, 0x49, 0xDD, 0xDE, 0xE8, 0xE9,
               0x73, 0x4B, 0x4C, 0x60, 0x61):
        o, n = counts.get(op, 0), inner.get(op, 0)
        mark = f"  (겉 {o} + 속 {n})" if n else ""
        print(f"  0x{op:02x} {o + n:9d}  {name_of.get(op, '?')}{mark}")

if __name__ == "__main__":
    sys.exit(main())
