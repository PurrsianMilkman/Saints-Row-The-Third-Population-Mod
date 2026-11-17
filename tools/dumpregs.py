import struct,sys
d=open(sys.argv[1],"rb").read()
sig,ver,n,drva=struct.unpack_from("<IIII",d,0)
st={}
for i in range(n):
    t,sz,rva=struct.unpack_from("<III",d,drva+i*12); st[t]=(sz,rva)
sz,rva=st[6]
tid=struct.unpack_from("<I",d,rva)[0]
ctxsz,ctxrva=struct.unpack_from("<II",d,rva+8+152)
c=ctxrva
g=lambda o: struct.unpack_from("<I",d,c+o)[0]
regs=dict(edi=g(156),esi=g(160),ebx=g(164),edx=g(168),ecx=g(172),eax=g(176),ebp=g(180),eip=g(184),esp=g(196))
print({k:hex(v) for k,v in regs.items()})
# memory
def readmem(addr,ln):
    if 5 in st:
        sz,r=st[5]; cnt=struct.unpack_from("<I",d,r)[0]
        for i in range(cnt):
            a,ds,rv=struct.unpack_from("<QII",d,r+4+i*16)
            if a<=addr and addr+ln<=a+ds: return d[rv+(addr-a):rv+(addr-a)+ln]
    if 9 in st:
        sz,r=st[9]; cnt,base=struct.unpack_from("<QQ",d,r); off=base
        for i in range(cnt):
            a,ds=struct.unpack_from("<QQ",d,r+16+i*16)
            if a<=addr and addr+ln<=a+ds: return d[off+(addr-a):off+(addr-a)+ln]
            off+=ds
    return None
sp=regs['esp']
m=readmem(sp,0x80)
print("stack:",[hex(x) for x in struct.unpack("<32I",m)] if m else None)
for nm in ("ecx","esi","edx"):
    v=regs[nm]; mm=readmem(v,0x60) if v else None
    print(nm,hex(v),"->",[hex(x) for x in struct.unpack("<24I",mm)] if mm else None)
