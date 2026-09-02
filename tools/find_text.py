# find_text.py — 복호화한 기록에서 사람이 읽을 수 있는 글자를 뽑는다.
#
# 채팅은 평문으로 실리므로, 이걸로 "언제 무엇을 했는지" 표지판을 찾는다.
#
#   python tools/find_text.py <덤프파일> [--min 3]
import sys, re

def main():
    path = sys.argv[1]
    minlen = 3
    for i, a in enumerate(sys.argv):
        if a == '--min' and i + 1 < len(sys.argv):
            minlen = int(sys.argv[i + 1])

    lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
    pkts = []
    for i, ln in enumerate(lines):
        m = re.match(r'\s*(\S+)\s+(C->S|S->C)\s+\S+\s+len=(\d+)\s+op=0x([0-9a-f]+)', ln)
        if not m or i + 1 >= len(lines):
            continue
        hexs = lines[i + 1].strip()
        if not re.fullmatch(r'([0-9a-f]{2}\s*)+', hexs):
            continue
        pkts.append({'time': m.group(1), 'dir': m.group(2),
                     'raw': [int(x, 16) for x in hexs.split()]})

    key = 0
    key_at = -1
    for idx, p in enumerate(pkts):
        if p['dir'] == 'S->C' and len(p['raw']) >= 3 and p['raw'][2] == 0x1f and p['raw'][0] & 0x80:
            key = p['raw'][-1]; key_at = idx; break

    print(f'패킷 {len(pkts)}개, XOR 키 0x{key:02x}')
    print()
    print('=== 글자가 들어 있는 패킷 ===')
    seen = 0
    for idx, p in enumerate(pkts):
        b = [x ^ key for x in p['raw']] if (key_at >= 0 and idx > key_at) else p['raw']
        if len(b) < 3:
            continue
        op = b[2]
        body = bytes(b[3:])
        # 1바이트 글자
        for m in re.finditer(rb'[\x20-\x7e]{%d,}' % minlen, body):
            s = m.group().decode('latin1')
            if len(set(s)) < 2:      # 같은 글자 반복은 잡음
                continue
            print(f'  {p["time"]} {p["dir"]} 0x{op:02x} len={len(b)}  "{s}"')
            seen += 1
            break
        else:
            # 2바이트(UTF-16LE) 글자
            for m in re.finditer(rb'(?:[\x20-\x7e]\x00){%d,}' % minlen, body):
                s = m.group().decode('utf-16le')
                print(f'  {p["time"]} {p["dir"]} 0x{op:02x} len={len(b)}  [16비트] "{s}"')
                seen += 1
                break
    print()
    print(f'글자 있는 패킷 {seen}개')

main()
