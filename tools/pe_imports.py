import struct,sys
p=sys.argv[1]
d=open(p,"rb").read(); pe=struct.unpack_from("<I",d,0x3c)[0]
n=struct.unpack_from("<H",d,pe+6)[0]; so=struct.unpack_from("<H",d,pe+20)[0]
opt=pe+24
entry=struct.unpack_from("<I",d,opt+16)[0]; base=struct.unpack_from("<I",d,opt+28)[0]
imp_rva,imp_sz=struct.unpack_from("<II",d,opt+104)
secs=[]; o=opt+so
for i in range(n):
    nm=d[o:o+8].rstrip(b"\0").decode(); vs,va,rs,ra=struct.unpack_from("<IIII",d,o+8); secs.append((nm,va,vs,ra,rs)); o+=40
def r2o(rva):
    for nm,va,vs,ra,rs in secs:
        if va<=rva<va+max(vs,rs): return ra+rva-va
    return None
print("entry rva",hex(entry),"base",hex(base),"imports rva",hex(imp_rva),"size",hex(imp_sz),"sections",[(s[0],hex(s[1])) for s in secs])
p0=r2o(imp_rva)
for _ in range(64):
    ot,td,fc,nm,ft=struct.unpack_from("<IIIII",d,p0)
    if nm==0: break
    a0=r2o(nm); name=d[a0:d.index(b"\0",a0)].decode()
    t=r2o(ot or ft); k=0; found=[]
    for k in range(4000):
        v=struct.unpack_from("<I",d,t+4*k)[0]
        if v==0: break
        if not v&0x80000000:
            a1=r2o(v)+2; fn=d[a1:d.index(b"\0",a1)].decode()
            if fn in("LoadLibraryA","GetProcAddress","ExitProcess","LoadLibraryW","Sleep"): found.append((fn,hex(base+ft+4*k)))
    print(name,k,found)
    p0+=20
