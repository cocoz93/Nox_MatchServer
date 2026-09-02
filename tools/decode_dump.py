# decode_dump.py — 중간자 기록을 XOR 복호화해서 진짜 오프코드를 센다.
#
# 접속 수락(ACCEPTED) 메시지의 마지막 바이트가 XOR 키다. 그 뒤 트래픽은 전부 암호화된다.
# 기록 파일은 평문 그대로 저장되므로, 여기서 풀어서 다시 읽는다.
#
#   python tools/decode_dump.py <덤프파일> [--dump-op 0x2b] [--limit 5]
import sys, re, collections

def main():
    path = sys.argv[1]
    want_op = None
    limit = 3
    for i, a in enumerate(sys.argv):
        if a == '--dump-op' and i + 1 < len(sys.argv):
            want_op = int(sys.argv[i + 1], 16)
        if a == '--limit' and i + 1 < len(sys.argv):
            limit = int(sys.argv[i + 1])

    lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
    pkts = []
    for i, ln in enumerate(lines):
        m = re.match(r'\s*(\S+)\s+(C->S|S->C)\s+(\S+)\s+len=(\d+)\s+op=0x([0-9a-f]+)', ln)
        if not m:
            continue
        if i + 1 >= len(lines):
            continue
        hexs = lines[i + 1].strip()
        if not re.fullmatch(r'([0-9a-f]{2}\s*)+', hexs):
            continue
        pkts.append({
            'time': m.group(1), 'dir': m.group(2), 'peer': m.group(3),
            'len': int(m.group(4)),
            'raw': [int(x, 16) for x in hexs.split()],
        })

    # 키 찾기: ACCEPTED(암호화 전, 오프코드 0x1f) 의 마지막 바이트
    key = 0
    key_at = -1
    for idx, p in enumerate(pkts):
        if p['dir'] == 'S->C' and len(p['raw']) >= 3 and p['raw'][2] == 0x1f and p['raw'][0] & 0x80:
            key = p['raw'][-1]
            key_at = idx
            break
    print(f'패킷 {len(pkts)}개')
    if key_at < 0:
        print('접속 수락 메시지를 못 찾음 — 복호화 없이 셈')
    else:
        print(f'XOR 키 = 0x{key:02x}  (수락 메시지는 {key_at}번째, 그 뒤부터 암호화)')
    print()

    cnt = collections.Counter()
    samples = collections.defaultdict(list)
    for idx, p in enumerate(pkts):
        enc = key_at >= 0 and idx > key_at
        b = [x ^ key for x in p['raw']] if enc else p['raw'][:]
        p['dec'] = b
        if len(b) < 3:
            continue
        op = b[2]
        cnt[(p['dir'], op)] += 1
        if len(samples[(p['dir'], op)]) < limit:
            samples[(p['dir'], op)].append((p['time'], b))

    print('=== 진짜 오프코드 집계 ===')
    print(f'{"방향":<6}{"오프코드":<10}{"건수":>8}   {"평균크기":>8}')
    tot = 0
    for (d, op), c in sorted(cnt.items(), key=lambda kv: -kv[1]):
        sizes = [len(x[1]) for x in samples[(d, op)]]
        avg = sum(sizes) // len(sizes) if sizes else 0
        print(f'{d:<6}0x{op:02x}{"":<6}{c:>8}   {avg:>8}')
        tot += c
    print(f'{"":<6}{"합계":<8}{tot:>8}')
    print()
    print(f'오프코드 종류: {len(set(op for _, op in cnt))} 가지')

    if want_op is not None:
        print()
        print(f'=== 0x{want_op:02x} 표본 ===')
        for (d, op), lst in samples.items():
            if op != want_op:
                continue
            for t, b in lst:
                hexs = ' '.join(f'{x:02x}' for x in b)
                txt = ''.join(chr(x) if 32 <= x < 127 else '.' for x in b)
                print(f'  {d} {t} len={len(b)}')
                print(f'    {hexs}')
                print(f'    {txt}')

main()
