# spellmap.py — 주문 **번호 ↔ 상수 ↔ 내부 키 ↔ 표시 이름** 대응표를 만든다.
#
# 왜 필요한가 — `0x79 TRY_SPELL` 은 본문 첫 바이트에 **번호만** 싣는다.
# 번호를 이름으로 바꿔야 로그가 읽히는데, **이름이 세 가지**라 그냥은 안 이어진다:
#
#     번호  opennox 상수            게임 내부 키      게임 표시 이름
#      51   SPELL_SHIELD           MagicShield      Force Field
#      24   SPELL_LIGHTNING        EnergyBolt       Energy Bolt      ★
#      43   SPELL_CHAIN_LIGHTNING  Lightning        Lightning        ★
#
# ★ 24 와 43 을 조심하라 — **opennox 상수 이름만 보고 「Lightning」을 찾으면 24 를 고르는데,
#   게임에서 「Lightning」은 43 이다.** 상수 이름은 게임 이름이 아니다.
#
# ⚠ 표시 이름은 **게임 데이터**다(`nox.csf`). `thing.bin` 사물 이름과 같은 취급으로
#   저장소에 넣지 않는다. 필요할 때 이 도구로 그 자리에서 뽑아 쓴다.
#
# 쓰는 법 (게임이 설치된 PC 에서):
#     python tools/spellmap.py                     # 표를 찍는다
#     python tools/spellmap.py --tsv out.tsv       # 파일로
#     python tools/spellmap.py --console list.txt  # 콘솔 `list spells` 출력을 함께 대조
#
# 어떻게 잇나 — 세 출처를 겹친다.
#   ① opennox `spell` 패키지 상수 → 번호       (server/src/SpellNames.cpp, -genspells 로 생성)
#   ② 게임 `nox.csf` 의 `thing.db:<키>` 라벨   → 내부 키와 표시 이름
#   ③ 텔넷 콘솔 `list spells`                  → 게임이 실제로 쓰는 순서와 마나 비용
#
# ①의 상수 이름을 CamelCase 로 바꾸면 대개 ②의 키와 같다(90/115). 다른 24개는
# **표시 이름으로 되짚어** 채운다. ③은 순서를 지키는 정렬(DP)로 ①에 붙여 번호를 준다.
import argparse, os, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
CSF_CANDIDATES = [
    r"C:\Program Files\GOG Galaxy\Games\Nox\_backup_before_korean\nox.csf",
    r"C:\Program Files\GOG Galaxy\Games\Nox\nox.csf",
]


def read_csf(path):
    """CSF 파서 — 태그가 ` rtS`(대문자 아님), 문자열은 **바이트 반전 UTF-16LE**."""
    d = open(path, "rb").read()
    off, out = 0x18, {}
    while off < len(d) - 8:
        if d[off:off + 4] == b" LBL":
            nstr, ln = struct.unpack_from("<II", d, off + 4)
            label = d[off + 12:off + 12 + ln].decode("ascii", "replace")
            q = off + 12 + ln
            if d[q:q + 4] in (b" rtS", b" RTS", b"WRTS", b"wrtS"):
                wide = d[q:q + 4] in (b"WRTS", b"wrtS")
                slen, = struct.unpack_from("<I", d, q + 4)
                raw = d[q + 8:q + 8 + slen * 2]
                out[label] = bytes((~b) & 0xFF for b in raw).decode("utf-16-le", "replace")
                q += 8 + slen * 2
                if wide:
                    el, = struct.unpack_from("<I", d, q)
                    q += 4 + el
            off = q
        else:
            off += 1
    return out


def read_ids():
    """SpellNames.cpp 에서 번호 → 상수 이름."""
    ids = []
    p = os.path.join(REPO, "server", "src", "SpellNames.cpp")
    for line in open(p, encoding="utf-8-sig"):
        m = re.match(r'\s*/\*\s*(\d+)\s*\*/\s*"([^"]*)",', line)
        if m and m.group(2):
            ids.append((int(m.group(1)), m.group(2)))
    return ids


def read_console(path):
    """콘솔 `list spells` 출력 → [(표시 이름, 마나 비용)] (순서 그대로)."""
    con = []
    for line in open(path, encoding="utf-8-sig"):
        m = re.match(r"^([^\t]+?)\s*\t+Cost:(\d+),", line.rstrip("\n"))
        if m:
            con.append((m.group(1).strip(), int(m.group(2))))
    return con


def camel(const):
    return "".join(w.capitalize() for w in const.replace("SPELL_", "").split("_"))


def norm(s):
    return " ".join(re.sub(r"[^A-Z0-9 ]", " ", s.upper().replace("SPELL_", "").replace("_", " ")).split())


def score(a, b):
    na, nb = norm(a), norm(b)
    if na == nb:
        return 10
    if nb.startswith(na) or na.startswith(nb):     # 콘솔 이름은 20자에서 잘린다
        return 8
    ta, tb = set(na.split()), set(nb.split())
    ov = len(ta & tb)
    return 0 if not ov else 2 + 4 * ov / max(len(ta), len(tb))


def align(con, ids):
    """순서를 지키는 정렬 — 콘솔은 번호 순서를 지키되 일부를 건너뛴다."""
    NC, NI, NEG = len(con), len(ids), -1e9
    dp = [[NEG] * (NI + 1) for _ in range(NC + 1)]
    bt = [[None] * (NI + 1) for _ in range(NC + 1)]
    for j in range(NI + 1):
        dp[0][j], bt[0][j] = 0, "skipI"
    for i in range(1, NC + 1):
        for j in range(1, NI + 1):
            best, how = dp[i][j - 1], "skipI"
            s = score(con[i - 1][0], ids[j - 1][1])
            c = dp[i - 1][j - 1] + (s if s > 0 else -3)
            if c > best:
                best, how = c, "match"
            c = dp[i - 1][j] - 6
            if c > best:
                best, how = c, "skipC"
            dp[i][j], bt[i][j] = best, how
    pairs, i, j = [], NC, NI
    while i > 0:
        if bt[i][j] == "match":
            pairs.append((con[i - 1], ids[j - 1]))
            i, j = i - 1, j - 1
        elif bt[i][j] == "skipC":
            pairs.append((con[i - 1], None))
            i -= 1
        else:
            j -= 1
    pairs.reverse()
    return pairs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csf", default=None)
    ap.add_argument("--console", default=None, help="텔넷 `list spells` 출력 파일")
    ap.add_argument("--tsv", default=None)
    a = ap.parse_args()

    csf_path = a.csf or next((p for p in CSF_CANDIDATES if os.path.exists(p)), None)
    if not csf_path:
        print("nox.csf 를 못 찾았다. --csf 로 알려 달라.", file=sys.stderr)
        return 2
    things = {k.split(":", 1)[1]: v.strip() for k, v in read_csf(csf_path).items()
              if k.startswith("thing.db:")}
    byLower = {k.lower(): k for k in things}
    byVal = {}
    for k, v in things.items():
        byVal.setdefault(v, []).append(k)

    ids = read_ids()
    con = read_console(a.console) if a.console else None
    order = {ids[i][0]: i for i in range(len(ids))}
    cost = {}
    if con:
        for (cname, ccost), idp in align(con, ids):
            if idp:
                cost[idp[0]] = ccost

    rows = []
    for num, const in ids:
        # ★ 표시 이름으로 되짚는 것이 **정본**이다 — 상수 이름을 CamelCase 한 키는 편의일 뿐이고,
        #   24(LIGHTNING)처럼 남의 키와 부딪히는 것이 있다.
        key = byLower.get(camel(const).lower())
        name = things.get(key) if key else None
        rows.append({"id": num, "const": const, "key": key or "", "name": name or "", "cost": cost.get(num)})

    # 콘솔 표시 이름이 있으면 그쪽을 따른다 (부딪힘 해소)
    if con:
        conByNum = {}
        for (cname, _), idp in align(con, ids):
            if idp:
                conByNum[idp[0]] = cname
        for r in rows:
            cn = conByNum.get(r["id"])
            if not cn:
                continue
            if r["name"] and (r["name"] == cn or r["name"].startswith(cn)):
                continue
            hits = byVal.get(cn) or [k for k, v in things.items() if v.startswith(cn)]
            if hits:
                r["key"], r["name"], r["fixed"] = hits[0], things[hits[0]], True

    named = [r for r in rows if r["name"]]
    fixed = [r for r in rows if r.get("fixed")]
    print("주문 %d개 · 표시 이름 붙음 %d · 상수 키와 달라 되짚은 것 %d" % (len(rows), len(named), len(fixed)))
    print()
    print("%-5s %-34s %-24s %-24s %s" % ("번호", "opennox 상수", "게임 내부 키", "표시 이름", "마나"))
    print("-" * 100)
    for r in rows:
        if not r["name"]:
            continue
        mark = " ★" if r.get("fixed") else ""
        print("%-5d %-34s %-24s %-24s %s%s" % (
            r["id"], r["const"], r["key"], r["name"],
            "" if r["cost"] is None else "%3d" % r["cost"], mark))

    if a.tsv:
        with open(a.tsv, "w", encoding="utf-8") as f:
            f.write("id\tconst\tkey\tname\tcost\n")
            for r in rows:
                f.write("%d\t%s\t%s\t%s\t%s\n" % (
                    r["id"], r["const"], r["key"], r["name"],
                    "" if r["cost"] is None else r["cost"]))
        print("\n%s 에 썼다" % a.tsv)
    return 0


if __name__ == "__main__":
    sys.exit(main())
