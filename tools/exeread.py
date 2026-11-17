import struct,capstone,re,sys
d=open("C:/Games/Saints Row 3/SaintsRowTheThird.exe","rb").read()
pe=struct.unpack_from("<I",d,0x3c)[0]; n=struct.unpack_from("<H",d,pe+6)[0]; so=struct.unpack_from("<H",d,pe+20)[0]
o=pe+24+so; S=[]
for i in range(n):
    nm=d[o:o+8].rstrip(b"\0");vs,va,rs,ra=struct.unpack_from("<IIII",d,o+8);S.append((nm,va,vs,ra,rs));o+=40
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_32)
def off(a):
    for nm,va,vs,ra,rs in S:
        if va<=a-0x400000<va+max(vs,rs): return ra+a-0x400000-va
def dis(a,cnt):
    f=off(a)
    for i in list(md.disasm(d[f:f+cnt*8],a))[:cnt]: print(f"{i.address:08x} {i.mnemonic} {i.op_str}")
def xrefs(a):
    t=d[off(0x401000):off(0x401000)+S[0][4]]
    r=[]
    for m in re.finditer(re.escape(struct.pack("<I",a)),d[S[0][3]:S[0][3]+S[0][4]]):
        r.append(0x400000+S[0][1]+m.start())
    return r
def u32(a): return struct.unpack_from("<I",d,off(a))[0]
