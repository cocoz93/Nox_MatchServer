# field_diff.py — 같은 오프코드 패킷들을 겹쳐놓고 "어느 자리가 변하는지" 본다.
#
# 매 틱 오는 메시지(입력·상태)의 구조를 알아내는 도구.
# 자리마다 값의 가짓수를 세면, 고정 칸 / 변하는 칸 / 카운터가 갈린다.
#
#   python tools/field_diff.py <덤프> --op 0x3f --dir C->S [--from 01:10:00 --to 01:11:00]
import sys, re, collections

def main():
    path = sys.argv[1]
    want_op = None; want_dir = None; t_from = None; t_to = None; show = 6
    for i, a in enumerate(sys.argv):
        if a == '--op' and i + 1 < len(sys.argv): want_op = int(sys.argv[i+1], 16)
        if a == '--dir' and i + 1 < len(sys.argv): want_dir = sys.argv[i+1]
        if a == '--from' and i + 1 < len(sys.argv): t_from = sys.argv[i+1]
        if a == '--to' and i + 1 < len(sys.argv): t_to = sys.argv[i+1]
        if a == '--show' and i + 1 < len(sys.argv): show = int(sys.argv[i+1])

    lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
    pkts = []
    for i, ln in enumerate(lines):
        m = re.match(r'\s*(\S+)\s+(C->S|S->C)\s+\S+\s+len=(\d+)\s+op=0x([0-9a-f]+)', ln)
        if not m or i + 1 >= len(lines): continue
        hexs = lines[i+1].strip()
        if not re.fullmatch(r'([0-9a-f]{2}\s*)+', hexs): continue
        pkts.append({'time': m.group(1), 'dir': m.group(2), 'raw': [int(x,16) for x in hexs.split()]})

    key = 0; key_at = -1
    for idx, p in enumerate(pkts):
        if p['dir'] == 'S->C' and len(p['raw']) >= 3 and p['raw'][2] == 0x1f and p['raw'][0] & 0x80:
            key = p['raw'][-1]; key_at = idx; break

    sel = []
    for idx, p in enumerate(pkts):
        b = [x ^ key for x in p['raw']] if (key_at >= 0 and idx > key_at) else p['raw']
        if len(b) < 3: continue
        if want_op is not None and b[2] != want_op: continue
        if want_dir and p['dir'] != want_dir: continue
        if t_from and p['time'] < t_from: continue
        if t_to and p['time'] > t_to: continue
        sel.append((p['time'], b))

    if not sel:
        print('해당 패킷 없음'); return
    print(f'대상 {len(sel)}개  (오프코드 0x{want_op:02x}, {want_dir}, {sel[0][0]} ~ {sel[-1][0]})')
    L = min(len(b) for _, b in sel)
    maxL = max(len(b) for _, b in sel)
    print(f'길이 {L} ~ {maxL}')
    print()
    print('자리  가짓수  가장흔한값   설명')
    for i in range(L):
        vals = collections.Counter(b[i] for _, b in sel)
        top, n = vals.most_common(1)[0]
        kind = ('고정' if len(vals) == 1 else
                '거의고정' if n / len(sel) > 0.9 else
                '몇가지' if len(vals) <= 8 else
                '카운터/값')
        print(f'[{i:2}]  {len(vals):5}   0x{top:02x} ({n*100//len(sel)}%)   {kind}')
    print()
    print(f'=== 표본 {show}개 ===')
    step = max(1, len(sel)//show)
    for t, b in sel[::step][:show]:
        print(f'  {t}  ' + ' '.join(f'{x:02x}' for x in b))

main()
