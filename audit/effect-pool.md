# effect_object pool (vanilla 1230) - audit and patch plan

Binary: SaintsRowTheThird_DX11.exe, base 0x400000, static analysis only (dump used only to read live globals).
Confidence tags: [H] high (code read end to end and/or confirmed against the crash dump), [M] medium, [L] low.

## 0. Short answer

Raising the pool to N is FEASIBLE. The crash was NOT the pool allocator, it was one hidden static array that is indexed by effect slot:

* "Group array" G at 0x48781B0, 308 groups x 96 bytes = 0x7380 bytes (ends exactly at 0x487F530). One group holds 4 effect slots as structure-of-arrays (x[4], y[4], z[4], 2 x float[4], flags[4]). 308 groups = ceil(1232/4).
* Any slot index >= 1232 writes past G. With N=5000, G would have to be 1250 groups = 0x1D4C0 bytes, i.e. it spills 0x1D4C0-0x7380 = 0x16140 bytes over the node arena (0x487F580..0x4891580), the effect instantiator at 0x4891580 (vtable, pool struct, pool base ptr [0x48915C4]) and the type tables behind it (0x4891718, 0x4891810, 0x4891908 ...).
* The 0x487F540 vtable overwrite is exactly slot 1232 lane 0 (see section 2).

Required patches (all verified byte for byte below):
1. The 3 pool constants (get_capacity, size, alloc push) - already known.
2. Relocate G to a VirtualAlloc'd block of (N/4)*96 bytes: 17 absolute-immediate sites.
3. Fix the one function with stack arrays indexed by slot (0x5C6EE0): 8 sites.
4. Leave every 0x4CE / 0x4CD / 0x1338 site in the 0x5C4xxx-0x5C8xxx range ALONE: they belong to an unrelated name-hash-table template, not to the pool.
5. Optional: a soft limit, the 3072-node shared arena at 0x487F540 (section 4), which fails gracefully but starves at roughly 3000 live effects.

## 1. Every dependency on pool capacity / effect index (classified)

### 1.1 Real dependencies

| What | Where | Nature | Scales with N automatically? |
|---|---|---|---|
| get_capacity | vtable 0x112682C slot [2] = 0x5CA5B0 `b8 ce 04 00 00` | feeds the object-system list/handle sizing (0x47E030/0x47E560 sum capacities, limit 0xFFFF) | no, patch (plugin's capGetter redirect is fine) |
| size method | slot [4] = 0x5CA5C0 `b8 2c 63 0c 00` (imm at 0x5CA5C1) | objects-heap budget, 20 + N*660 | no, patch |
| alloc wrapper | slot [6] = 0x5CA640, `68 ce 04 00 00` (imm at 0x5CA645) | cap passed to allocator 0x5CA5D0 | no, patch |
| allocator 0x5CA5D0 | uses the pushed cap for: object array N*0x290 (16-aligned), handle/free-list table N*4 (u16 pair per slot), `word [pool+0xa] = N`, then constructs N objects (ctor 0x5C5AE0 via 0x84A2D0) | all dynamic | yes (N must be <= 0xFFFF; u16 count/head/tail) |
| free wrapper 0x5CA660, free 0x5C5770 | bounds via `word [pool+0xa]`, index via `/656` (imul 0x63E7063F) | dynamic | yes |
| pool count word | 0x48915BA (= pool 0x48915B0 + 0xA), base ptr 0x48915C4 (= pool+0x14), handle table 0x48915C8 | dynamic | yes |
| **G, the group array** | 0x48781B0..0x487F530, 96-byte group = 4 slots | STATIC, sized 308 groups | **NO - must be relocated** |
| **per-frame effect update 0x5C6EE0** | stack frame 0x2724 with two 1232-entry arrays + loop counts 0x134 / 0x4D0 | STATIC frame | **NO - must be patched** |
| shared "param node" arena | global object 0x487F540, 3072 nodes x 20 B inside a 0x12000 B embedded buffer | STATIC | soft limit, optional |

[H] on all of the above.

### 1.2 What G contains and who touches it ([H])

Slot index is always derived as `idx = (obj - [0x48915C4]) / 656` (the `mov eax,0x63E7063F / imul / sar 8 / shr 31 / add` idiom, 20 sites, all in 0x5C5460-0x5CB270), then
`group = (idx>>2)*96`, `lane = idx&3`, `G + group + lane*4 + field`.

Fields: +0x00 x, +0x10 y, +0x20 z (written from effect+0x40/+0x44/+0x48), +0x30 float (LOD dist A), +0x40 float (LOD dist B), +0x50 u32 flag bits (|=1 visible, 2 alive, 4 active, 8 pending, ...). Written by 0x5C5A20 (+0x30/+0x40), 0x5C5A60 (flag or), 0x5C5AA0 (flag and-not), ctor 0x5C5AE0 (flag=1), 0x5C5FE0, 0x5C6010, 0x5C6390, 0x5C6960 (xyz), 0x5C69E0, 0x5C6AD0, 0x5C6BB0, 0x5C7560, dtor 0x5C8510, 0x5C907C (init/alloc), 0x5CB227 (xyz). Read in bulk by the SIMD update 0x5C6EE0 (loop over all 308 groups).

Complete list of absolute references to the G range found by scanning every 4-byte value of .text (whole exe) in 0x48781B0..0x487F540: exactly the 17 sites in section 3.2; nothing outside 0x5C5A00-0x5CB300. [H]

### 1.3 The 1230 / 1229 / 0x1338 sites in the effect module: ALL belong to `str_data_hash_table<int,1230,32,unsigned short>` (name registry), NOT to the pool ([H])

Evidence: vtable 0x112680C has RTTI `?$str_data_hash_table@H$0EMO@$0CA@G@@` (template args int, 0x4CE, 0x20, ushort). Layout: buckets word[1230] at +6 (0x99C B), next word[1230] at +0x9A2, keys dword[1230] at +0x1340 (0x1338 B), values dword[1230] at +0x2678 (0x1338 B), string storage 1230*32 at +0x39B0 (total 0xD370). The only instantiation is two stack locals in 0x5C8160 (frame 0x1A6EC, stack objects at esp+0x1C and esp+0xD38C, exactly 0xD370 apart), which walks the effect objects and de-duplicates their NAME strings (`[obj+0x20C]` through 0x5C2F00). It is a debug/statistics routine with NO callers: no E8/E9, no vtable or data pointer to 0x5C8160 anywhere in the file. The table is keyed on effect-definition names (675 effect infos at 0x4844148 stride 0x98, count 0x2A3), so it never needs more than the number of DISTINCT names, independent of the pool size. 1230 here is a coincidence of the designers choosing the same number.

| Address | What | Verdict |
|---|---|---|
| 0x5C4513 imm (0x5C4514), 0x5C4558 (0x5C4559), 0x5C4573 (0x5C4574) | `mov edx,0x4CE; cmp ax,dx` bucket scan loops in hash-table iterate/first/next (0x5C4500, 0x5C4550) | name table, leave |
| 0x5C45BE (0x5C45BF) `mov edx,0x4CD`; 0x5C4593 `push 0x99C`; 0x5C45D3 `0x133C` | hash-table clear (0x5C4590) | name table, leave |
| `push 0x4CE` at 0x5C5666, 0x5C56CC, 0x5C5837, 0x5C58C7, 0x5C6E27, 0x5C823E, 0x5C82EE, 0x5C835D, 0x5C8442 (imm at +1 each) | modulus argument of hash 0xDAF520(key,1230) in find/insert/erase/iterate of the table | name table, leave |
| 0x5C82D0, 0x5C83B3, 0x5C83D4 | loops / hash in the dead function 0x5C8160 | dead, leave |
| `0x1338` at 0x5C6EAC/0x5C6EC2 (0x5C6EAD / 0x5C6EC3 imm) | stream load 0x5C6E60 (vtable 0x1126818): reads 0x2678 + 0x1338 bytes of the table | name table, leave |
| 0x5C4416 `0x2678` etc. | table save/load, `+0x2678` value-array offsets | name table, leave |
| `0x1340`, `0x2678`, `0x39B0` offsets in 0x5C44xx-0x5C58xx, 0x5C8030/0x5C8039/0x5C8048 | same class | leave |
| 0x5CA5B1, 0x5CA645 | get_capacity / alloc push | REAL, patch |

The other listed immediates are not effect-pool code at all (all verified as bytes inside other instructions or unrelated constants):
* 0x4A4F95, 0x57AFB3, 0x6F1F89, 0x73D55A, 0x99D729, 0xA0FC7A, 0xCE935B: the 0x4CE is the rel32 of a `jcc/jmp` (`0f 8x ce 04 00 00`, `e9 ce 04..`) or a rel displacement, not an immediate operand.
* 0x9C292D: `add eax,0x4CE` after a random range call (an id base, unrelated).
* 0x7EB67F: bytes `70 26 00 00` of `mov [esp+0x70],0x26`, not 0x2670.
* 0x4CD hits at 0x5A8580, 0xB6EEF1, 0xB6EF3B, 0xC3B49F, and 0x4CF/0x4CC/0x4CB hits: unrelated (jumps / other tables); none sit in the effect module apart from 0x5C45BF.
[M-H]: I checked the effect module exhaustively and the 7 jcc sites by byte context; I did not individually disassemble the 0x4CB/0x4CC/0x4CF hits elsewhere (not relevant to the pool).

### 1.4 The two big stack frames ([H])

* 0x5C6EE0 (called per frame from 0x5CA340 via `call 0x5C6EE0` at 0x5CA4CA): `mov eax,0x2724; call _chkstk(0xEA6F40)`; frame layout (offsets from esp after the alloca, before the 3 pushes at 0x5C6F05):
  * locals/temps 0x00..0x9F (SIMD constants at 0x20..0x90, temps 0x10/0x34/0x74/0x78 all < 0xA0),
  * `float dist[1232]` at +0xA0 (0xA0..0x13DF),
  * `int state[1232]` at +0x13E0 (0x13E0..0x271F),
  * stack cookie at +0x2720. Frame = 0x2724.
  Loop 1 (0x5C6FD0..0x5C70B4) runs `edi=0x134` (=308) groups, esi steps 0x10 per group; edx walks G with `add edx,0x60`, starting at `mov edx,0x48781C0` (= G+0x10). The two stores inside use displacement +8 (0xA8 / 0x13E8) because 2 args (8 bytes) are pushed at that moment. Loop 2 (0x5C70C0..0x5C71F0) is `esi` over `cmp esi,0x4D0` (=1232 = 308*4) slots, `ebx += 0x290` walking the pool, guarded by `cmp esi, word [0x48915BA]` (so lanes past the pool count take the null-object branch only if state==-1, which padding lanes never have, same as vanilla).
* 0x5C8160: dead, see 1.3.
No other function in 0x5C2000-0x5CB300 has a frame > 0x100 (searched every `call 0xEA6F40` and `sub esp,imm>=0x1000`).

### 1.5 Things that look related but are not limited by N

* 0x5C3EC0 (`mov esi,0x48781E4`, loop 600 x 0xB8): no callers; it is a loop over a 600-entry table that ends at 0x4878150; the guard `cmp edi,[0x484413C]` keeps it from touching element 600 (0x4878150, which overlaps G+0x34). Uncalled; ignore.
* Counters 0x4878150..0x4878158 and type table 0x487815C..0x48781A0, list head 0x48781A0, 0x48781A4/A8: indexed by effect TYPE, not slot; do not move.
* Object-handle ranges: slot lists per type come from get_capacity (0x47E030 sums them, rejects total > 0xFFFF). N=5000 raises the vanilla 27894 sum by 3770, far under 65535.
* Per-type bitset iteration 0x5C8E00 / 0x5C8EB0 and generic pool iteration use dynamic sizes.

## 2. Root cause of the 0x487F540 corruption (5000-slot attempt) [H]

The dump confirms it value by value. After the pool grew, effect slot 1232 (group 308, lane 0) was written with `G = 0x48781B0 + 308*96 = 0x487F530`:

| address | meaning if it is G+308*96 | dump value |
|---|---|---|
| 0x487F530 (+0x00 x lane 0) | x | 12.7 |
| 0x487F540 (+0x10 y lane 0) | **y** -> overwrote the vtable ptr of the object at 0x487F540 | **0x410A761F = 8.65f** |
| 0x487F550 (+0x20 z lane 0) | z | 0x448943CF = 1098.0 |
| 0x487F560 (+0x30 lane 0) | LOD dist A (0x5C5A4C) | 0x41C80000 = 25.0 |
| 0x487F570 (+0x40 lane 0) | LOD dist B (0x5C5A58) | 0x42200000 = 40.0 (it is the pointer `[0x487F570]` that 0x5C7CE9 uses as the node index table) |

So (12.7, 8.65, 1098) is the position of effect 1232. The crash at 0x5C7CC4: `mov eax,[0x487F540]; call [eax+0xC]` executes the float as a vtable pointer. The object at 0x487F540 is the lock-protected node pool (ctor 0x5C8CB0, vtable slots 3/4 = lock/unlock). Further slots (1233..) hit 0x487F580+ (its embedded node arena, whose live data in the dump is intact only because the game died first) and, for N=5000, everything up to 0x4895670 including the effect instantiator itself ([0x48915C4] pool base). The "~1 minute" delay is simply the time for the fill level to reach the first slot >= 1232 (free-list ordering hands out low indices first). The previous attempt patched allocator + size, so nothing stopped writes past G; the 0x4CE sites are irrelevant to it.

## 3. Patch plan to raise capacity to N (N a multiple of 4, <= 65532; numbers below for N=5000)

N=5000: groups Gn = N/4 = 1250 (0x4E2), G bytes = 120000 (0x1D4C0), heap size = 20 + N*660 = 3300020 (0x325AB4), extra over vanilla = 0x325AB4 - 0xC632C = 0x25F788 (about 2.5 MB) -> check against the objects-heap slack as pools.cpp already does (size + 0xC800 rounded to 64 KB).

### 3.1 Pool constants (as the plugin already does) [H]
| Address | Original | Replacement (N=5000) |
|---|---|---|
| 0x5CA645 (imm of `68 ce 04 00 00`) | `ce 04 00 00` | `88 13 00 00` |
| 0x5CA5C1 (imm of `b8 2c 63 0c 00`) | `2c 63 0c 00` | `b4 5a 32 00` |
| vtable 0x112682C + 8 (get_capacity ptr) | 0x5CA5B0 | pointer to `b8 88 13 00 00 c3` cave (or patch `ce 04 00 00` at 0x5CA5B1 -> `88 13 00 00`; this getter is private to effect_object, `0x5CA5B0` has no other caller) |

### 3.2 Relocate G: 17 immediates [H]
Allocate `Gbuf = VirtualAlloc(NULL, Gn*96, MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE)` once, BEFORE the pool is built (plugin init; the object array is built by the ctor 0x5C5AE0 from the allocator, which writes G flags). Zero-filled page-aligned memory is correct (G is BSS, nothing initialises it statically); 16-byte alignment holds (movaps in the SIMD loop). Then write the 32-bit value at each address, all currently `b0 81 87 04` (0x048781B0) unless stated:

| Site (imm32 address) | Instruction | New value |
|---|---|---|
| 0x5C5A48 | `add eax,imm` | Gbuf |
| 0x5C5A8C | `add eax,imm` | Gbuf |
| 0x5C5ACD | `add eax,imm` | Gbuf |
| 0x5C6006 | `add eax,imm` | Gbuf |
| 0x5C6082 | `add eax,imm` | Gbuf |
| 0x5C63FB | `add eax,imm` | Gbuf |
| 0x5C69A4 | `add eax,imm` | Gbuf |
| 0x5C6A1F | `add eax,imm` | Gbuf |
| 0x5C6B1C | `add eax,imm` | Gbuf |
| 0x5C6B88 | `add eax,imm` | Gbuf |
| 0x5C6C23 | `add eax,imm` | Gbuf |
| 0x5C75D1 | `add eax,imm` | Gbuf |
| 0x5C8561 | `add eax,imm` | Gbuf |
| 0x5C90DC | `add ecx,imm` (`81 c1`) | Gbuf |
| 0x5CB283 | `add eax,imm` | Gbuf |
| 0x5C5B75 (orig `00 82 87 04` = 0x4878200) | `mov dword [edx*4+imm],1` | Gbuf + 0x50 |
| 0x5C6FA7 (orig `c0 81 87 04` = 0x48781C0) | `mov edx,imm` (SIMD loop start) | Gbuf + 0x10 |

Replacement bytes are the little-endian 32-bit value; the plugin should check the original 4 bytes before writing (as `patch()` does). All the `[eax+ecx*4+0x30/0x40/0x50]` displacements are relative to the group and stay as is. The original static G (0x48781B0..0x487F530) becomes unused; do not reuse.

### 3.3 Fix the stack frame of 0x5C6EE0 [H]
Let `D = 4*(N-1232)` (a multiple of 16 because N is a multiple of 4). New layout: dist[N] at +0xA0, state[N] at +0xA0+4N, cookie at +0xA0+8N, frame = 0xA4+8N. The frame stays 4 mod 16 (required for the aligned movaps/movdqa after the 3 pushes). For N=5000: state base 0x4EC0, store disp 0x4EC8, cookie 0x9CE0, frame 0x9CE4 (about 40 KB; `_chkstk` probes it, fine on the main thread).

| Address | Original bytes | Meaning | Replacement (N=5000) |
|---|---|---|---|
| 0x5C6EE7 (imm of `b8 24 27 00 00`) | `24 27 00 00` | alloca size | `e4 9c 00 00` |
| 0x5C6EFA (disp of `89 84 24 20 27 00 00`) | `20 27 00 00` | cookie store | `e0 9c 00 00` |
| 0x5C6FC0 (imm of `bf 34 01 00 00`) | `34 01 00 00` | group count 308 | `e2 04 00 00` |
| 0x5C709C (disp of `66 0f 7f b4 34 e8 13 00 00`) | `e8 13 00 00` | state[] store (arg-pushed +8) | `c8 4e 00 00` |
| 0x5C70C3 (disp of `8b 84 b4 e0 13 00 00`) | `e0 13 00 00` | state[] read | `c0 4e 00 00` |
| 0x5C71EC (imm of `81 fe d0 04 00 00`) | `d0 04 00 00` | slot loop bound 1232 | `88 13 00 00` |
| 0x5C71F9 (disp of `8b 8c 24 2c 27 00 00`) | `2c 27 00 00` | cookie check (+0xC) | `ec 9c 00 00` |

Untouched on purpose: the float array displacements 0xA0 / 0xA8 (dist[] start does not move), all temps below 0xA0, and everything else in the function. Because the SIMD loop reads G from `[edx-0x10 .. edx+0x4F]` per group, the loop count 0x4E2 and Gbuf from 3.2 are what keep it in bounds. General formulas: state base = 0xA0+4N, store disp = +8, cookie = 0xA0+8N, frame = cookie+4, cookie check = cookie+0xC, groups = N/4, slot bound = N.

### 3.4 Do NOT patch
All sites of section 1.3 (0x4CE / 0x4CD / 0x1338 / 0x2678 / 0x1340 / 0x39B0 / 0x99C / 0x133C in the str_data_hash_table code), nor the dead function 0x5C8160. Patching the hash modulus would also have to change the string table layout and stream sizes and gains nothing.

### 3.5 Optional: soft limit of the node arena (global object 0x487F540) [M]
The object at 0x487F540 (ctor 0x5C8CB0) hands out 20-byte nodes for per-effect parameter lists (`[effect+0x16C]` list, set by 0x5C7C90). Capacity 0xC00 (3072): ctor `push 0x12000` (0x5C8D18) / `push 0xC00` (0x5C8D39); the allocator 0xE21600 takes `cap*20 + cap*4` = 0x12000 from a linear allocator constructed over the INLINE buffer `[this+0x40]` = 0x487F580..0x4891580 (immediately followed by the effect instantiator at 0x4891580, so it cannot grow in place). Dump: 962 nodes in use with ~1000 live effects (about 1 node per effect), so with N=5000 and more than ~3000 live effects the arena empties. Failure is graceful: 0x5C7C90 tests `esi` for null (`je 0x5C7DB5`) and just skips storing the parameter, so the effect keeps running but loses that parameter (likely fade/scale). To lift it, replace `lea edx,[esi+0x40]` (0x5C8D1D, `8d 56 40`, followed by `push edx`) with a jump to a cave that pushes a VirtualAlloc'd buffer, and change the two immediates to cap*24 and cap. Not required for the crash fix; test first without it.
  (For cap=0x3000: arena size = 0x48000 -> imm bytes `00 80 04 00` at 0x5C8D19, count -> `00 30 00 00` at 0x5C8D3A.)

### 3.6 Order and checks
1. At plugin init: VirtualAlloc Gbuf, write 3.2, 3.3, 3.1 (3.1 may stay as is if the plugin already does it). All before the first object is constructed (the pool is built during game init, after ASI load). 
2. Log counters: expect `effect_object` capacity 5000 in the POOLS line, Gbuf != 0.
3. Cheap sanity at runtime: read back a few G flags: slot 0 flag must be 1 after init in `Gbuf`, and `*(u32*)0x48781B0` must stay 0 (old region untouched).
4. Stress: spawn >1232 effects (the old failure point); watch 0x487F540's vtable (`*(u32*)0x487F540 == 0x1120xxx` stays a code-segment-ish value).

## 4. Risks / unknowns
* [H] G is the only static array indexed by slot; the exhaustive absolute-address scan of .text finds nothing else in 0x48781B0..0x487F540 and the stack-frame search finds only 0x5C6EE0 (and the dead 0x5C8160). Other code that could be affected only through the engine's generic object lists (get_capacity) is covered by the plugin's existing getter redirect; that was not re-derived here beyond reading 0x47E030.
* [M] Heap budget: size patch must still fit the "objects" heap slack (pools.cpp comment says it was 99.97% full in vanilla; the plugin raises other pool sizes too, but the budget is computed from the sum of size methods, so it grows with it).
* [M] The `0x5C8D18/0x5C8D39` node arena patch is optional and unverified in game.
* [L] Whether the frame growth of 0x5C6EE0 matters for the render/update thread stack size: 40 KB is small for a default 1 MB stack; I did not look at the thread's stack reserve.
* Savegames: no pool-sized data is serialised by this module (the 0x2678/0x1338 stream belongs to the name hash table, which is unchanged), so existing saves are unaffected by this change.
