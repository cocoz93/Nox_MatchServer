# where.py — 중간자 기록에서 각 클라의 마지막 좌표를 읽는다.
#
#   python tools/where.py <덤프>
#
# 좌표는 C->S 0x3f(매 틱 입력) 안에 0xac 하위 메시지로 실린다: ac <x:2LE> <y:2LE>.
#
# 주의 두 가지 (둘 다 한 번씩 당했다):
#  1) **0xac 은 움직일 때만 나온다.** 가만히 선 클라는 좌표를 안 보낸다.
#     좌표를 알고 싶으면 짧게 한 번 걷게 한 뒤 읽어라.
#  2) **키는 세션마다 다르다.** 접속(포트)마다 ACCEPTED(0x1f)의 마지막 바이트를 따로 잡는다.
#     중간자가 키를 바꿔치기한 세션과 안 바꾼 세션이 섞이면 하나의 키로는 못 푼다.
import sys, re

def main():
    path = sys.argv[1]
    lines = open(path, encoding='utf-8', errors='replace').read().split('\n')

    pkts = []          # (시각, 방향, 상대, 원본바이트)
    for i, ln in enumerate(lines):
        m = re.match(r'\s*(\S+)\s+(C->S|S->C)\s+(\S+)\s+len=(\d+)\s+op=0x([0-9a-f]+)', ln)
        if not m:
            continue
        hx = lines[i + 1].strip() if i + 1 < len(lines) else ''
        if not re.fullmatch(r'([0-9a-f]{2}\s*)+', hx or ''):
            continue
        pkts.append((m.group(1), m.group(2), m.group(3),
                     bytes(int(v, 16) for v in hx.split())))

    # 세션별 키 — 수락 메시지의 마지막 바이트
    keys = {}
    for t, d, peer, b in pkts:
        # 5번은 플레이어 번호(첫째 01, 둘째 02 …)라 고정으로 보면 안 된다
        if d == 'S->C' and len(b) == 10 and b[:5] == b'\x80\x00\x1f\x00\x01':
            keys[peer] = b[9]

    last = {}
    for t, d, peer, raw in pkts:
        if d != 'C->S' or peer not in keys:
            continue
        b = bytes(x ^ keys[peer] for x in raw)
        if len(b) < 8 or b[2] != 0x3f:
            continue
        for j in range(3, len(b) - 4):
            if b[j] != 0xac:
                continue
            x = b[j + 1] | (b[j + 2] << 8)
            y = b[j + 3] | (b[j + 4] << 8)
            if 500 < x < 6000 and 500 < y < 6000:
                last[peer] = (t, x, y)
            break

    for peer, k in sorted(keys.items()):
        mark = '  좌표 없음(안 움직였다)' if peer not in last else ''
        print('세션 %-28s 키=0x%02x%s' % (peer, k, mark))
    print()
    for peer, (t, x, y) in sorted(last.items(), key=lambda kv: kv[1][0]):
        print('%-28s %s  x=%-5d y=%-5d' % (peer, t, x, y))

    if len(last) == 2:
        (p1, (t1, x1, y1)), (p2, (t2, x2, y2)) = sorted(last.items(), key=lambda kv: kv[1][0])
        dx, dy = x1 - x2, y1 - y2
        sx, sy = dx - dy, (dx + dy) // 2
        print()
        print('%s → %s : 월드 (%+d, %+d) / 화면 (%+d, %+d)' % (p2, p1, dx, dy, sx, sy))
        print('  화면에서 %s %s 쪽' % ('오른쪽' if sx > 0 else '왼쪽',
                                        '아래' if sy > 0 else '위'))

main()
