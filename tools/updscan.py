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
    keys = {}
    for _t, _d, peer, raw in read_dump(path, "SEND"):
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
    a = ap.parse_args()

    if a.codes:
        load_names(a.codes)
    complex_of, name_of = load_types(a.types)

    xor_key = harvest_keys(a.dump) if not a.xor else {}
    fixed_key = int(a.xor, 0) if a.xor else 0
    print(f"XOR 키: {a.xor if a.xor else {p: hex(k) for p, k in xor_key.items()}}")
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


if __name__ == "__main__":
    sys.exit(main())
