"""Equivalence test for SR3Population.asi builds, without launching the game.

For each plugin variant (old monolith / new modules):
  1. copy SaintsRowTheThird_DX11.exe into run/exe/ and redirect its entry point to a tiny stub that does LoadLibraryA(<plugin>) then sleeps forever
     (the Windows loader maps the real image at 0x400000, so every patch address is valid; the game itself never starts),
  2. start it, wait for the plugin's init() to finish, read the patched image + every code cave it created out of the process memory, kill it.
Then compare: plugin log lines, every image byte, and the caves (disassembled, with cave-relative addresses normalised).

usage: python run_harness.py <old.asi> <new.asi>      (each plugin must have its SR3Population.ini next to it)
"""
import ctypes, ctypes.wintypes as W, os, re, shutil, struct, sys, time
import capstone

GAME = r"C:\Games\Saints Row 3"
EXE = os.path.join(GAME, "SaintsRowTheThird_DX11.exe")
HERE = os.path.dirname(os.path.abspath(__file__))
RUN = os.path.join(HERE, "run")
NL = chr(10)
HEX = re.compile(r"0x[0-9a-f]+")
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.OpenProcess.restype = W.HANDLE
k32.ReadProcessMemory.argtypes = [W.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.CreateProcessW.argtypes = [W.LPCWSTR, W.LPWSTR, ctypes.c_void_p, ctypes.c_void_p, W.BOOL, W.DWORD, ctypes.c_void_p, W.LPCWSTR, ctypes.c_void_p, ctypes.c_void_p]
k32.TerminateProcess.argtypes = [W.HANDLE, W.UINT]


def parse_pe(d):
    pe = struct.unpack_from("<I", d, 0x3c)[0]
    n = struct.unpack_from("<H", d, pe + 6)[0]
    so = struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    info = dict(pe=pe, opt=opt, base=struct.unpack_from("<I", d, opt + 28)[0], size=struct.unpack_from("<I", d, opt + 56)[0], secs=[])
    o = opt + so
    for i in range(n):
        nm = d[o:o + 8].rstrip(b"\0").decode()
        vs, va, rs, ra = struct.unpack_from("<IIII", d, o + 8)
        info["secs"].append((nm, va, vs, ra, rs))
        o += 40
    return info


def r2o(info, rva):
    for nm, va, vs, ra, rs in info["secs"]:
        if va <= rva < va + max(vs, rs):
            return ra + rva - va
    return None


def iat(d, info, name):
    """VA of the IAT slot for an imported function (by name)."""
    imp_rva = struct.unpack_from("<I", d, info["opt"] + 104)[0]
    p = r2o(info, imp_rva)
    while True:
        ot, td, fc, nm, ft = struct.unpack_from("<IIIII", d, p)
        if nm == 0:
            return None
        t = r2o(info, ot or ft)
        for k in range(5000):
            v = struct.unpack_from("<I", d, t + 4 * k)[0]
            if v == 0:
                break
            if not v & 0x80000000:
                a = r2o(info, v) + 2
                if d[a:d.index(b"\0", a)].decode() == name:
                    return info["base"] + ft + 4 * k
        p += 20


def make_test_exe(src_bytes, info, plugin_path, out_path):
    d = bytearray(src_bytes)
    ll, sl = iat(d, info, "LoadLibraryA"), iat(d, info, "Sleep")
    nm, va, vs, ra, rs = info["secs"][0]  # .text
    pos = 0   # the game never runs in this test, so the first bytes of .text are free to hold the stub (excluded from the comparison)
    cave_va = info["base"] + va + pos + 16
    path = plugin_path.encode() + b"\0"
    code = bytearray()
    code += b"\x68" + struct.pack("<I", cave_va + 32)                  # push path
    code += b"\xFF\x15" + struct.pack("<I", ll)                         # call [LoadLibraryA]
    spin = len(code)
    code += b"\x68" + struct.pack("<I", 1000)                           # push 1000
    code += b"\xFF\x15" + struct.pack("<I", sl)                         # call [Sleep]
    code += b"\xEB" + struct.pack("<b", spin - (len(code) + 2))         # jmp spin
    assert len(code) <= 32
    stub = bytes(code).ljust(32, b"\xCC") + path
    off = ra + pos + 16
    d[off:off + len(stub)] = stub
    struct.pack_into("<I", d, info["opt"] + 16, cave_va - info["base"])  # AddressOfEntryPoint
    open(out_path, "wb").write(d)


class SI(ctypes.Structure):
    _fields_ = [("cb", W.DWORD), ("r", ctypes.c_void_p), ("d", ctypes.c_void_p), ("t", ctypes.c_void_p), ("x", W.DWORD * 7), ("fl", W.DWORD),
                ("sw", W.WORD), ("cb2", W.WORD), ("r2", ctypes.c_void_p), ("hi", W.HANDLE), ("ho", W.HANDLE), ("he", W.HANDLE)]


class PI(ctypes.Structure):
    _fields_ = [("hProcess", W.HANDLE), ("hThread", W.HANDLE), ("pid", W.DWORD), ("tid", W.DWORD)]


def run_variant(tag, plugin, info, exe_bytes):
    exe_dir = os.path.join(RUN, "exe")
    os.makedirs(exe_dir, exist_ok=True)
    for dll in ("steam_api.dll", "binkw32.dll"):
        if not os.path.exists(os.path.join(exe_dir, dll)):
            shutil.copy(os.path.join(GAME, dll), exe_dir)
    test = os.path.join(exe_dir, "test_%s_DX11.exe" % tag)
    make_test_exe(exe_bytes, info, os.path.abspath(plugin), test)
    log = os.path.join(os.path.dirname(os.path.abspath(plugin)), "SR3Population.log")
    if os.path.exists(log):
        os.remove(log)
    si = SI()
    si.cb = ctypes.sizeof(si)
    pi = PI()
    ok = k32.CreateProcessW(test, None, None, None, False, 0, None, exe_dir, ctypes.byref(si), ctypes.byref(pi))
    assert ok, ctypes.get_last_error()
    time.sleep(3.0)   # plugin init is synchronous in DllMain; the monitor thread first writes at +4 s, so read before that
    h = pi.hProcess
    base, size = info["base"], info["size"]
    img = bytearray(size)
    bad = 0
    CH = 1 << 20
    for off in range(0, size, CH):
        n = min(CH, size - off)
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t()
        if k32.ReadProcessMemory(h, ctypes.c_void_p(base + off), buf, n, ctypes.byref(got)):
            img[off:off + n] = buf.raw
        else:
            bad += 1

    def rd(addr, n):
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t()
        return buf.raw if k32.ReadProcessMemory(h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)) else None
    return dict(tag=tag, img=bytes(img), bad=bad, rd=rd, h=h, log=log)


def logic_lines(path):
    return [l.rstrip(NL) for l in open(path, errors="replace")] if os.path.exists(path) else []


def describe_cave(rd, target, info, memref):
    """Disassemble a cave (until its final jump out of it) with cave-relative / counter-relative addresses normalised."""
    data = rd(target, 200)
    if not data:
        return ["<unreadable cave>"]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    lo, hi = info["base"], info["base"] + info["size"]
    out = []
    for ins in md.disasm(data, target):
        ops = ins.op_str
        def norm(m):
            v = int(m.group(0), 16)
            if lo <= v < hi:
                return "G:%x" % v
            if target <= v < target + 0x400:
                return "CAVE+%x" % (v - target)
            if "[" in ops and memref is not None and 0 <= v - memref < 0x200:
                return "CNT+%x" % (v - memref)
            return "EXT"
        out.append((ins.mnemonic + " " + HEX.sub(norm, ops)).strip())
        if ins.mnemonic == "jmp":
            h = HEX.findall(ops)
            inside = bool(h) and target <= int(h[0], 16) < target + 0x100
            if not inside:
                break
        if ins.mnemonic == "ret" or len(out) > 40:
            break
    return out


def external_mem_base(rd, targets, info):
    """smallest external memory operand used by the caves (= start of the plugin's counter block)"""
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    lo, hi = info["base"], info["base"] + info["size"]
    vals = []
    for t in targets:
        data = rd(t, 200)
        if not data:
            continue
        for ins in md.disasm(data, t):
            if "[" in ins.op_str:
                for m in HEX.findall(ins.op_str):
                    v = int(m, 16)
                    if not (lo <= v < hi) and not (t <= v < t + 0x400) and v > 0x10000:
                        vals.append(v)
            if ins.mnemonic == "ret":
                break
    return min(vals) if vals else None


def main():
    old, new = sys.argv[1], sys.argv[2]
    exe_bytes = open(EXE, "rb").read()
    info = parse_pe(exe_bytes)
    pristine = bytearray(info["size"])
    for nm, va, vs, ra, rs in info["secs"]:
        pristine[va:va + min(rs, vs)] = exe_bytes[ra:ra + min(rs, vs)]
    pr = bytes(pristine)
    res = {}
    for tag, plug in (("old", old), ("new", new)):
        r = run_variant(tag, plug, info, exe_bytes)
        res[tag] = r
        print("[%s] image read ok, unreadable chunks: %d" % (tag, r["bad"]))

    # 1) plugin logs
    lo_, ln_ = logic_lines(res["old"]["log"]), logic_lines(res["new"]["log"])
    print(NL + "== LOG: old %d lines, new %d lines" % (len(lo_), len(ln_)))
    so, sn = sorted(lo_), sorted(ln_)
    oo = [l for l in so if l not in sn]
    nn = [l for l in sn if l not in so]
    print("only in OLD:", oo if oo else "none")
    print("only in NEW:", nn if nn else "none")
    skip = ("DIAG", "GATES", "REMOVE", "POOLS", "BUDGET", "SR3 population:")   # timing-dependent monitor lines + the line that prints the ini path
    setup_o = [l for l in lo_ if not l.startswith(skip)]
    setup_n = [l for l in ln_ if not l.startswith(skip)]
    print("setup/patch lines: same set of lines:", sorted(setup_o) == sorted(setup_n), "| same order:", setup_o == setup_n, "(%d lines)" % len(setup_o))

    # 2) image bytes
    io, inw = res["old"]["img"], res["new"]["img"]
    stub_lo = info["secs"][0][1] + 16   # RVA of the stub + path (differs between variants by design)
    cookie = 0x474C9CC - info["base"]   # /GS stack-protector cookie: random per process, not a plugin patch
    def changed(img):
        return [i for i in range(len(img)) if img[i] != pr[i] and not (stub_lo <= i < stub_lo + 0x100) and not (cookie <= i < cookie + 4)]
    co, cn = changed(io), changed(inw)
    print(NL + "== IMAGE: bytes changed vs pristine exe: old %d  new %d" % (len(co), len(cn)))
    union = sorted(set(co) | set(cn))
    groups = []
    for i in union:
        if groups and i - groups[-1][1] <= 1:
            groups[-1][1] = i
        else:
            groups.append([i, i])
    print("patch regions:", len(groups))

    tg = {"old": {}, "new": {}}
    for tag, img in (("old", io), ("new", inw)):
        for s, e in groups:
            b = img[s:e + 1]
            if len(b) >= 5 and b[0] == 0xE9 and pr[s] != 0xE9:
                tg[tag][info["base"] + s] = info["base"] + s + 5 + struct.unpack("<i", b[1:5])[0]
    memref = {t: external_mem_base(res[t]["rd"], list(tg[t].values()), info) for t in tg}
    print("counter-block base (old/new):", {k: (hex(v) if v else None) for k, v in memref.items()})

    n_same = n_diff = 0
    for s, e in groups:
        va = info["base"] + s
        a, b = io[s:e + 1], inw[s:e + 1]
        if a[:1] == b"\xE9" and va in tg["old"] and va in tg["new"] and pr[s] != 0xE9:
            da = describe_cave(res["old"]["rd"], tg["old"][va], info, memref["old"])
            db = describe_cave(res["new"]["rd"], tg["new"][va], info, memref["new"])
            ok = (da == db) and a[5:] == b[5:]
            print("  cave %s @%08x  tail old %s new %s  (%d instrs)" % ("OK  " if ok else "DIFF", va, a[5:].hex(), b[5:].hex(), len(da)))
            if not ok:
                print("     old:", da)
                print("     new:", db)
            n_same += ok
            n_diff += (not ok)
        elif a == b:
            n_same += 1
        else:
            pa = struct.unpack("<I", a[:4])[0] if len(a) == 4 else None
            pb = struct.unpack("<I", b[:4])[0] if len(b) == 4 else None
            if len(a) == 4 and s >= 1 and io[s - 1] == 0xE8 and inw[s - 1] == 0xE8:
                # displacement of a `call rel32` redirected to a wrapper cave: compare the wrappers, not the raw bytes
                t_old = info["base"] + s + 4 + struct.unpack("<i", a)[0]
                t_new = info["base"] + s + 4 + struct.unpack("<i", b)[0]
                da = describe_cave(res["old"]["rd"], t_old, info, memref["old"])
                db = describe_cave(res["new"]["rd"], t_new, info, memref["new"])
                ok = da == db
                print("  call-wrapper %s @%08x  %d vs %d instrs" % ("OK  " if ok else "DIFF", va - 1, len(da), len(db)))
                if not ok:
                    print("     old:", da)
                    print("     new:", db)
                n_same += ok
                n_diff += (not ok)
            elif pa and pb and pa > 0x10000 and pb > 0x10000 and not (info["base"] <= pa < info["base"] + info["size"]):
                da, db = res["old"]["rd"](pa, 16), res["new"]["rd"](pb, 16)
                ok = da == db
                print("  ptr  %s @%08x  -> private data old %s new %s" % ("OK  " if ok else "DIFF", va, da.hex() if da else None, db.hex() if db else None))
                n_same += ok
                n_diff += (not ok)
            else:
                # a jmp rel32 whose displacement bytes split the region (displacement bytes that equal the pristine bytes): follow it from its E9
                done = False
                for k in (1, 2, 3, 4):
                    j = s - k
                    if j >= 0 and io[j] == 0xE9 and inw[j] == 0xE9 and pr[j] != 0xE9:
                        t_old = info["base"] + j + 5 + struct.unpack("<i", io[j + 1:j + 5])[0]
                        t_new = info["base"] + j + 5 + struct.unpack("<i", inw[j + 1:j + 5])[0]
                        da = describe_cave(res["old"]["rd"], t_old, info, memref["old"])
                        db = describe_cave(res["new"]["rd"], t_new, info, memref["new"])
                        ok = da == db
                        print("  jmp-displacement %s @%08x (jmp at %08x) caves: %d vs %d instrs" % ("OK  " if ok else "DIFF", va, info["base"] + j, len(da), len(db)))
                        if not ok:
                            print("     old:", da)
                            print("     new:", db)
                        n_same += ok
                        n_diff += (not ok)
                        done = True
                        break
                if not done:
                    print("  DIFF @%08x  old %s  new %s" % (va, a.hex(), b.hex()))
                    print("     context  pristine %s | old %s | new %s" % (pr[s - 4:e + 5].hex(), io[s - 4:e + 5].hex(), inw[s - 4:e + 5].hex()))
                    n_diff += 1
    print(NL + "== RESULT: %d regions equivalent, %d different" % (n_same, n_diff))
    for t in res:
        k32.TerminateProcess(res[t]["h"], 0)


if __name__ == "__main__":
    main()
