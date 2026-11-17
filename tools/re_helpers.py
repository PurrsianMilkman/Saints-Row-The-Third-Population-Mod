r"""Static RE helpers for SaintsRowTheThird_DX11.exe (the exe the user runs). Addresses are DX11 virtual addresses.
Usage:  import sys; sys.path.insert(0, r"D:\SR3-Population-Mod\tools"); from re_helpers import *
  e.dis(addr, n)               linear disassembly from addr (e = exe11 module; e.d raw bytes, e.off(va) file offset, e.u32(va))
  show(addr, before, after)    disassembly around an address, realigned from the enclosing function start
  fstart(addr)                 heuristic function start
  callers(fn) / jmps(fn)       direct E8 / E9 call sites of fn
  occ(value)                   every .text location where the 32-bit value appears (immediates / absolute addresses)
  strx(text)                   (address, [code refs]) of a NUL-terminated .rdata string
  readmem(addr, n)             bytes from the crash minidump (game state with ~33 cars / 70 npcs), or None
"""
import sys, struct, io, contextlib, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sys,struct
sys.path.insert(0,r'D:\SR3-Population-Mod\tools')
import exe11 as e
t=e.S[0]; txt=e.d[t[3]:t[3]+t[4]]; B=0x400000+t[1]
rd=[s for s in e.S if s[0]==b'.rdata'][0]
blob=e.d[rd[3]:rd[3]+rd[4]]
def fstart(a):
    j=a-B
    while j>0 and not (txt[j-1]==0xCC and txt[j]!=0xCC) and not(txt[j-1]==0xC3 and txt[j] in (0x55,0x56,0x57,0x53,0x6A,0x68,0x83,0x8B,0x81,0xA1,0xB9)): j-=1
    return B+j
def show(h,before=8,after=8):
    s=fstart(h)
    if h-s>3000: s=h-0x40
    ins=list(e.md.disasm(txt[s-B:h-B+0x60],s))
    k=[i for i,x in enumerate(ins) if x.address<=h<x.address+x.size]
    k=k[0] if k else 0
    for x in ins[max(0,k-before):k+after]: print(f'   {x.address:08x} {x.mnemonic} {x.op_str}')
def strx(nm):
    i=blob.find(nm.encode()+b'\0')
    while i>0 and blob[i-1]!=0: i=blob.find(nm.encode()+b'\0',i+1)
    if i<0: return None,[]
    a=0x400000+rd[1]+i
    pat=struct.pack('<I',a); j=-1; hits=[]
    while True:
        j=txt.find(pat,j+1)
        if j<0: break
        hits.append(B+j)
    return a,hits
def callers(tg):
    r=[]; i=txt.find(b'\xe8')
    while i>=0:
        if i+5<=len(txt) and B+i+5+struct.unpack_from('<i',txt,i+1)[0]==tg: r.append(B+i)
        i=txt.find(b'\xe8',i+1)
    return r
def jmps(tg):
    r=[]; i=txt.find(b'\xe9')
    while i>=0:
        if i+5<=len(txt) and B+i+5+struct.unpack_from('<i',txt,i+1)[0]==tg: r.append(B+i)
        i=txt.find(b'\xe9',i+1)
    return r

def occ(val):
    pat=struct.pack('<I',val); j=-1; r=[]
    while True:
        j=txt.find(pat,j+1)
        if j<0: break
        r.append(B+j)
    return r
_dump=r'C:\Games\SR3-reshade-backup\dlss5-feed-crash.dmp'
_dd=open(_dump,'rb').read() if os.path.exists(_dump) else None
def readmem(addr,ln):
    if _dd is None: return None
    d=_dd; sig,ver,n,drva=struct.unpack_from("<IIII",d,0); st={}
    for i in range(n):
        t,sz,rva=struct.unpack_from("<III",d,drva+i*12); st[t]=(sz,rva)
    if 9 in st:
        sz,r=st[9]; cnt,base=struct.unpack_from("<QQ",d,r); off=base
        for i in range(cnt):
            a,ds=struct.unpack_from("<QQ",d,r+16+i*16)
            if a<=addr and addr+ln<=a+ds: return d[off+(addr-a):off+(addr-a)+ln]
            off+=ds
    if 5 in st:
        sz,r=st[5]; cnt=struct.unpack_from("<I",d,r)[0]
        for i in range(cnt):
            a,ds,rv=struct.unpack_from("<QII",d,r+4+i*16)
            if a<=addr and addr+ln<=a+ds: return d[rv+(addr-a):rv+(addr-a)+ln]
    return None
