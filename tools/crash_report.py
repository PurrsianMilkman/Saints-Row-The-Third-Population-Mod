r"""Turn the crash blocks in SR3Population.log into readable reports.

    python tools\crash_report.py ["C:\Games\Saints Row 3\SR3Population.log"]

For every FAULT / FATAL block: the function containing eip (+ a name if known), the disassembly around eip, the memory classification
the plugin logged, and the caller chain - stack candidates are kept only when the instruction right before them is a call (real return
addresses), each shown with that call instruction and its function.
"""
import sys, os, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from re_helpers import *

# addresses identified so far (DX11 exe). Extend as functions get named.
KNOWN = {
    0xEB4F45: 'CRT memset (SSE path)', 0xEA67D0: 'CRT memset', 0xDB1640: 'linear mempool alloc', 0xDB1440: 'sub-mempool ctor (carves from parent)',
    0xDB1400: 'linear mempool ctor', 0xDB9950: 'block mempool ctor', 0xDB99D0: 'block pool can_alloc', 0x778910: 'empty function (shared no-op / warning)',
    0x9D99C0: 'get local player', 0x906510: 'ambient car base count (player-in-vehicle aware)', 0x90E2C0: 'population spawn pass',
    0x904890: 'spawn budget gate', 0x8EFF50: 'spawn tracker reserve', 0x8F03D0: 'spawn tracker handle lookup', 0x8F0180: 'spawn tracker init',
    0x47E030: 'object system: per-type list sizing (sums get_capacity)', 0x47E560: 'object system: objects heap size',
    0x5DDCB0: 'render: walk per-type dirty bitsets (0x5901B20)', 0x5DF6C0: 'renderlib PC setup (class caps)', 0x5C6EE0: 'effect per-frame update',
    0x79A160: 'havok hk_custom_character job queue', 0x8BE050: 'action-node pass target collection', 0x709F40: 'loader using 1 MB sub-pool of "permanent"',
    0xA966C0: 'vehicle customisation slot alloc', 0xAC2580: 'wheel-list pool alloc', 0xAC2170: 'tyre-record pool alloc', 0xAF1240: 'seat registry submit',
    0x4B4120: 'anim instance alloc', 0x4B3F20: 'anim controller alloc', 0x49D080: 'anim state ring pop', 0x9E7810: 'is human in vehicle (reads +0x16C0)',
}

def name_of(a):
    s = fstart(a)
    n = KNOWN.get(s) or KNOWN.get(a)
    return s, (f'  [{n}]' if n else '')

def call_before(ret):
    """instruction ending exactly at `ret` if it is a call, else None"""
    for back in (2, 3, 5, 6, 7):
        s = ret - back
        f = e.off(s)
        if f is None: continue
        ins = list(e.md.disasm(e.d[f:f + back], s))
        if len(ins) == 1 and ins[0].address + ins[0].size == ret and ins[0].mnemonic == 'call':
            return ins[0]
    return None

def main(path):
    lines = open(path, errors='replace').read().splitlines()
    blocks, cur = [], None
    for ln in lines:
        if ln.startswith(('FAULT', 'FATAL', 'CRASH')):
            cur = [ln]; blocks.append(cur)
        elif cur is not None and ln.startswith('  '):
            cur.append(ln)
        else:
            cur = None
    if not blocks: print('no FAULT / FATAL blocks in', path); return
    if not any(b[0].startswith('FATAL') for b in blocks):
        print('No FATAL block: the game caught the fault in its own handler. If the game died, the LAST fault below is the fatal one.\n')
    for b in blocks:
        print('=' * 110); print(b[0])
        for ln in b[1:]:
            if not ln.startswith(('  stack', '  code at eip')): print(ln)
        m = re.search(r'eip ([0-9a-f]{8})', b[0]); eip = int(m.group(1), 16)
        if 0x401000 <= eip < 0x101F000:
            s, nm = name_of(eip)
            print(f'\n  fault in function {s:08x}{nm}  (eip = fn+0x{eip - s:x})')
            show(eip, 8, 3)
        st = [ln for ln in b if ln.startswith('  stack')]
        if st:
            print('\n  call chain (verified return addresses):')
            for v in re.findall(r'\b([0-9a-f]{8})\b', st[0].split(':', 1)[1]):
                ret = int(v, 16)
                c = call_before(ret)
                if not c: continue
                s, nm = name_of(c.address)
                print(f'    {c.address:08x}  {c.mnemonic} {c.op_str:<28} in fn {s:08x}{nm}')

if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else r'C:\Games\Saints Row 3\SR3Population.log')
