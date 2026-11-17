"""List every dirty_update_impl<T> class: vtable, static instance(s), bitset size (vtable[2] get_bits returns this+4 and a dword count)."""
import sys, os, re as rx, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from re_helpers import *

d = e.d
def va_of(fileoff):
    for nm, va, vs, ra, rs in e.S:
        if ra <= fileoff < ra + rs: return 0x400000 + va + (fileoff - ra)
def find_all(b):
    j = -1; r = []
    while True:
        j = d.find(b, j + 1)
        if j < 0: return r
        r.append(j)

out = []
for m in rx.finditer(rb'\.\?AV\?\$dirty_update_impl@V(\w+)@@', d):
    tdva = va_of(m.start()) - 8
    for j in find_all(struct.pack('<I', tdva)):
        colva = va_of(j - 12)
        for k in find_all(struct.pack('<I', colva)):
            vt = va_of(k) + 4
            f = e.u32(vt + 8)
            if not (0x401000 <= f < 0x101F000): continue
            ins = list(e.md.disasm(d[e.off(f):e.off(f) + 20], f))
            cnt = next((int(i.op_str[5:], 0) for i in ins if (i.mnemonic == 'mov' and i.op_str.startswith('eax, 0x')) or (i.mnemonic == 'mov' and rx.match(r'eax, d+$', i.op_str))), None)
            insts = [a for a in occ(vt)]
            out.append((m.group(1).decode(), vt, cnt, insts))
for nm, vt, cnt, insts in sorted(out, key=lambda x: x[0]):
    print(f'{nm:24s} vt {vt:08x}  bitset {cnt} dwords = {cnt*32 if cnt else "?"} slots   vtable stored by {[hex(x) for x in insts][:4]}')
