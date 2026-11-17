# SR3 DX11 exe - hardcoded per-vehicle limits (static RE audit)

Target: `C:\Games\Saints Row 3\SaintsRowTheThird_DX11.exe`, image base 0x400000, all addresses are DX11 VAs.
Method: full linear capstone sweep of .text (4.1 M instructions) for compares/loop bounds against 0x2F/0x30/0x31 and other
small constants, a static-ctor-array census (call 0xEA8578), the free-index-stack idiom (`cmp al,0x30 / jae`), Team A specs for
names, and live cross-checks against the crash minidump (`readmem`, ~33 vehicles, ~70 NPCs alive, old build with pool cap 300).
Everything is read-only; nothing was run. Mempool-class limits (RL_skin bone pools, shader_constant_cache, Animlib pools ...) are
already handled by `population\audit.cpp` and are not repeated here, except where a hardcoded count inside a mempool defeats that scaling.

## 0. Bottom line

* The vehicle pool (cap 500) is the only vehicle structure the engine sizes dynamically. At least four more 48-entry structures are tied
  1:1 to live vehicles, there is a 48-record seat registry, and three animation registries (400/400/375) are shared with humans.
* **Hard crash at the 49th live wheeled vehicle**: the wheel-list pool (P4) returns NULL and the caller dereferences it without a check,
  fault at `0x00AD70F7` (`mov ecx,[eax+0x20]`, eax = 0).
* **The seat registry caps AMBIENT TRAFFIC DRIVERS at 48 vehicles**: the spawn code seats drivers through the submitter `0xAF1240` and
  ignores its return value (call at 0x92EB0A), so vehicle #49 with an occupant simply gets no driver (silent).
* **Best candidate for "car model frozen while the car keeps simulating" = the animation registries** (instance registry 400,
  controller registry 400, controller-state ring 375). They are shared with humans and their counts are hardcoded constants that
  `audit.cpp` mempool scaling does not change (section 2.1). Live dump: 219/400, 71/400, 146/375 used with only 33 cars + ~70 NPCs.
  With 400 cars + 120+ peds they cannot hold. When the vehicle-side create fails the vehicle gets no anim controller/instance, so its
  skeleton pose is never updated again while the physics body keeps moving. Confidence: medium (mechanism read from code; the link to
  the user's symptom is not proven live, and cars without an anim set may not consume these at all).
* The 48-pools do not explain the freeze at <=40 vehicles (not exhausted there; dump: P1 = P4 = vcust slots = 33 used, exactly one per
  vehicle, no leak). They are what breaks the moment more than 48 vehicles are really alive.

## 1. Summary table

| # | Structure | DX11 addr | Cap | Scales with vehicles | Effect when exceeded | Sev |
|---|---|---|---|---|---|---|
| A1 | wheel-list pool P4 (V+0x7FC), 0x100 x 48 | recs 0x5C31070, count/ids 0x5C34070/71, alloc 0xAC2580, free 0xAC25C0 | 48 | 1 per wheeled vehicle | alloc NULL, unchecked deref, **crash 0xAD70F7** | CRASH |
| A2 | tyre/instance record P1 (V+0x1544), 0x1D8 x 48 | recs 0x5BCA478, count/ids 0x5BCFCF8/9, alloc 0xAC2170, free 0xAC21B0 | 48 | 1 per wheeled vehicle | field stays 0; part-matrix composition (0xA81FD0) and axle parts skipped | crash/visual |
| A3 | vehicle customisation (paint) slots | arrays 0x5BB8BC0 and 0x5BB9AC0 (48 x 0x50), in-use table 0x5B940A0, cb buffers 0x5B94160 (48 x 0x800) and 0x5BAC160 (48 x 0x400); alloc 0xA966E0.., free 0xA98750 | 48 | 1 per vehicle | creator 0xA976E0 returns 0 -> no V+0xA88 record -> no paint/cust state | visual |
| A3b | vcust record pools | cap immediates 0x100C9C6 (0x30), 0x100CA76 (0x690), 0x100CB26 (0xD20) | 48 / 1680 / 3360 | per vehicle | pool refuses | visual |
| A4 | seat/occupancy registry | base 0x5C4D250, 48 x 0x1640, end 0x5C8FE50 | 48 | 1 per vehicle with occupants | submitter 0xAF1240 returns 0; ambient spawn ignores it -> driverless car | traffic cap |
| B1 | anim instance registry R1 (0x3A0 x 400) | hdr 0x66E9EF8, base ptr [0x66E9F04], alloc 0x4B4120 | 400 | 1 per animated vehicle + ~2-3 per human | returns -1 -> no pose | FREEZE suspect |
| B2 | anim controller registry R2 (0x480 x 400) | hdr 0x6744FB8, base ptr [0x6744FC4], alloc 0x4B3F20 | 400 | lazy per instance | returns 0 -> instance has no controller | FREEZE suspect |
| B3 | anim controller-state ring R3 (5 x 0x1B0 per 0x870 block, 75 blocks = 375 nodes) | 0x66C24F0..0x66E9DC0, head [0x620F324], pop 0x49D080 | 375 | vehicle with anim set (0xA84D4A) + humans | NULL -> [V+0x3F0] not set, vehicle anim set-up skipped (0xA84DBA jz) | FREEZE suspect |
| C1 | link record P3 (V+0x1A3C), 0x18 x 48 | recs 0x5C30960, count 0x5C30DE0, alloc 0xAC2480, free 0xAC24C0 | 48 | tow/attach/last-driver links | alloc NULL, caller checks -> link not made | silent |
| C2 | kneecapper block P2 (V+0x1550), 0x80 x 48 | recs 0x5C2EB88, count 0x5C30388, alloc 0xAC23A0, free 0xAC23E0 | 48 | only with kneecapper upgrade | NULL stored; V+0x16CC \|= 8 | silent |
| C3 | def-state slots (V+0xA7C), 0x5C0 x 40 | recs 0x5CD3BA0, count 0x5CE21A0, alloc 0xB732A0 | 40 | armed vehicles only | NULL, feature unset | silent, low |
| C4 | vehicle light-component ring | head [0x5BBAC8C], 16 x 0xB0 at 0x5BBAC98.., builder 0xAAB2E0 | 16 | per light part (kind 0xA) | no light/glare component | visual, low |
| C5 | small pools 2/2/8/8 | counts 0x5C2EB80, 0x5C30540, 0x5C30950, 0x5C31058 | 2,2,8,8 | rare features | silent | none |
| D1 | per-frame vehicle damage/detach queue | count [0x5B224E0], 0x40 x 128 at 0x5B224F0, push 0xA8F513 | 128/frame | collisions | event dropped, flag set | low |
| D2 | handle-keyed table (mission code callers) | 0x5AF1A38.., 32 x 0x18 | 32 | mission vehicles | silent | none (ambient) |
| D3 | distant-vehicle list | object+0x88, 64 x 0x20 (ctor 0xB90BF0) | 64 | far-field fakes (disabled by loose xtbl) | n/a | none now |
| E1 | engine pools (pool array 0x61EE7E0) | audio_emitter 305 (211 used in dump), parking_spot 125 (98 used), roadblock 32, script_vehicle 110, object_rig 90 | - | partly | engine-side handling unknown | see 3 |

## 2. Details and fixes

### 2.1 Animation registries (B1, B2, B3) - top priority for the visual freeze

Evidence chain
* `0x4B3C90` builds R1 through a temporary linear mempool: `call 0xDB1400(buf=0x66E9F10, size=0x5B040, name "object pool")`, then
  `0x4B3E60` carves two blocks from it with HARDCODED sizes (`push 0x5AA00` = 400*0x3A0, `push 0x640` = 400*4) and sets the u16
  capacity with `mov eax,0x190` -> [0x66E9EFA]. R2 is the same pattern at `0x4B3BB0` / `0x4B3D70` (sizes 0x70E40, 0x70800 = 400*0x480, 0x640, `0x190`).
  The mempool name is the generic "object pool" (stack-resident temporary), so `audit.cpp`'s scale table never touches it, and even
  if it did, the carved counts are constants, so the capacity would stay 400.
* R1 alloc `0x4B4120`: `cmp word [0x66E9EFE],0 / ja`, else returns 0. Its only caller chain is `0x4AD420` (reached by thunk `0x4A8340`),
  which returns -1 on failure. Callers of the thunk: 0x722885, 0x73E471, 0x8CA32A (object_rig), 0x950E18 and 0x9F335C (humans),
  **0xA84DC8 (vehicle)**, 0xB8F3F0. Vehicle path `0xA84D4A` (vehicle model/anim set-up): `call 0x49D080` (R3 pop) -> `[ebx+0x3F0]`,
  then `call 0x4A8340` (R1) with the anim-set handle from `[ctx+0x4C/0x50]`, then `0x49CD20` binds it. If R3 returns 0 the whole block
  is skipped (`0xA84DBA je 0xA84E16`). Block only runs when `[vehicleDef+0xAC4]` (anim set) is non-null.
* R2 is allocated lazily from R1 instances: `0x4AAB30` (`cmp [esi+0x348],0` -> `call 0x4B3F20`, stored at instance+0x348).
* Dump (33 vehicles + ~70 NPCs): R1 free word [0x66E9EFE] = 181 -> **219/400 used**; R2 free [0x6744FBE] = 329 -> **71/400**; R3 free ring
  walked from [0x620F324] = 229 -> **146/375 used**. Humans take roughly 2-3 R1 instances each.
* Consequence on exhaustion: the vehicle's anim controller/instance is absent, so the per-part/skeleton pose loop (spec `0xA76690`
  family) has nothing to sample: the model keeps its last pose while the Havok body keeps moving.

Fix (immediate patches; every original byte string verified with e.d). Choose N (e.g. 2048; R1 array 0x1D0000, R2 array 0x240000).
Allocate the backing blocks in the ASI with VirtualAlloc (zeroed, 16-aligned) BEFORE the engine reaches 0x4B3C90 (plugin load time).

| Site | Original bytes | New |
|---|---|---|
| 0x4B3CB2 (R1 mempool size) | `68 40 B0 05 00` | `68 00 20 1D 00` (0x1D2000 = N*0x3A0 + N*4) |
| 0x4B3CB7 (R1 mempool buf) | `68 10 9F 6E 06` | `68 <imm32 R1_BLOCK>` |
| 0x4B3E6E (R1 array bytes) | `68 00 AA 05 00` | `68 00 00 1D 00` (N*0x3A0) |
| 0x4B3E87 (R1 free list) | `68 40 06 00 00` | `68 00 20 00 00` (N*4) |
| 0x4B3E95 (R1 count) | `B8 90 01 00 00` | `B8 00 08 00 00` (N) |
| 0x4B3BD2 (R2 mempool size) | `68 40 0E 07 00` | `68 00 20 24 00` (0x242000) |
| 0x4B3BD7 (R2 mempool buf) | `68 D0 4F 74 06` | `68 <imm32 R2_BLOCK>` |
| 0x4B3D7E (R2 array bytes) | `68 00 08 07 00` | `68 00 00 24 00` (N*0x480) |
| 0x4B3D97 (R2 free list) | `68 40 06 00 00` | `68 00 20 00 00` |
| 0x4B3DA5 (R2 count) | `B8 90 01 00 00` | `B8 00 08 00 00` |

Original sizes are exactly array+freelist (0x5AA00+0x640 = 0x5B040, 0x70800+0x640 = 0x70E40), so the new mempool size must be the same
sum. No other code embeds 400/0x190 for these (the other `0x190` immediates in 0x49xxxx-0x4Cxxxx are stack offsets). Indices are u16
(`movzx`, `mov [..],ax`), so N up to 65535 is safe.

R3 is a static ring and only two sites reference it (every other 0x66E9DC0 operand is a different, adjacent object). Allocate K x 0x870
bytes (K = N/5), then patch:

| Site | Original bytes | New |
|---|---|---|
| 0x49D285 (mov ecx, 0x66C24F0) | `B9 F0 24 6C 06` | `B9 <NEWBUF>` |
| 0x49D367 (cmp ecx, 0x66E9DC0) | `81 F9 C0 9D 6E 06` | `81 F9 <NEWBUF + K*0x870>` |

The init loop 0x49D290..0x49D361 links five 0x1B0 nodes per 0x870 block (+0, +0x1B0, +0x360, +0x510, +0x6C0); nothing else references the ring.

Cheap proof before patching: log once a second u16 [0x66E9EFE] (R1 free), u16 [0x6744FBE] (R2 free) and the ring length at [0x620F324].
If R1 free reaches 0 the freeze is explained.

### 2.2 A1/A2: wheel-list and tyre-record pools (crash at vehicle 49)

Each pool is contiguous: records, then a count byte, then 48 id bytes. Init in `0xAC1140` (loops `cmp al,0x30`, bytes `3C 30`).
* P4: records `0x5C31070` (stride 0x100, `shl esi,8`), count 0x5C34070, ids 0x5C34071. Alloc `0xAC2580` (ctor `0xA76CB0`), free `0xAC25C0`
  (caller `0xA784A8`, vehicle teardown). Allocated from the wheel-geometry rebuild `0xAD7050` at `0xAD70D2`, stored to V+0x7FC.
  **`0xAD70F1: mov eax,[esi+0x7FC]; mov ecx,[eax+0x20]` has no null check -> crash at 0xAD70F7.**
* P1: records `0x5BCA478` (stride 0x1D8), count 0x5BCFCF8, ids 0x5BCFCF9. Alloc `0xAC2170` (record init `0xA782E0`), free `0xAC21B0`
  (releases 8 effect-handle pairs first; caller `0xA831D3` in vehicle destroy), stored at V+0x1544 (`0xAD70E6`). Spec: every wheeled
  vehicle has one; `0xA81FD0` (part matrix composition) runs only when V+0x1544 != 0.
Decoded-operand scan shows only alloc/free/init reference these arrays (plus static ctor 0x100D540 for P1).

Fix: ids are bytes, so the structure cannot exceed 255 entries as is. For N >= 256 replace the free-index stack with hooks:
* P4: new array NEW4 (N x 0x100, zeroed) + u16 stack in the ASI. Redirect 0xAC2580 (bytes `A0 70 40 C3 05 56 3C 30`) and 0xAC25C0
  (`A0 70 40 C3 05 8B 4C 24`) with `jmp` to ASI versions: alloc = pop, `ecx=rec; call 0xA76CB0`, return rec (NULL if empty); free(rec) = push (rec-NEW4)>>8.
* P1: alloc 0xAC2170 (`A0 F8 FC BC 05 56 3C 30`) same way (ctor `call 0xA782E0` with ecx = rec); for the free keep the function body and
  replace only the tail 0xAC2216..0xAC2248 (the `mov cl,[0x5BCFCF8]` ... `ret` index push-back) with a call to the ASI release taking ebp = record.
* If N <= 255 is enough, relocation alone works: operands P4 `0xAC25A6` (`add esi,0x5C31070`), `0xAC25CB`; P1 `0xAC2199`, `0xAC221C`, `0x100D540`, and `3C 30` -> `3C N` at 0xAC2176 / 0xAC2586 (and loop bounds in 0xAC1140).
* Belt and braces regardless: add a null check after `call 0xAC2580` at 0xAD70D2 (test eax,eax / je 0xAD71D8) so a full pool skips the vehicle instead of crashing.

### 2.3 A3: vehicle customisation slots (48 constant buffers + record pools)

* Slot arrays 48 x 0x50 at `0x5BB8BC0` and `0x5BB9AC0`; in-use dword table `0x5B940A0`; per-slot cb data carved in init
  (`0xA94C77` `mov [esp+10h],0x5B94160` ext cb 0x800 x 48; `0xA94C7F` `mov ebp,0x5BAC160` cb 0x400 x 48; names "ext vcust cb %d" / "vcust cb %d").
* Alloc `0xA966E0..0xA96757`: scan `39 1C 85 A0 40 B9 05` (cmp [eax*4+0x5B940A0],ebx), bound `83 F8 30 7C F1` at 0xA96731, bases
  `0xA96740`/`0xA96746`; returns 0 when none free (0xA96757..0xA96765). Free `0xA98750..0xA98777` (bound `83 F8 2F 77 07` at 0xA9876B,
  table store at 0xA98770).
  Init loop end `81 FE 00 0F 00 00` (cmp esi,0xF00) at 0xA94D71; field stores use displacements `[esi+0x5BB8BC4..0x5BB8C0C]` and
  `[esi+0x5BB9AC4..0x5BB9B0C]` (0xA94C9F..0xA94D34); table zero at 0xA94D58. Static ctor/dtor counts `push 0x30` at 0x100CBA3, 0x100CBD3, 0x101CCF5, 0x101CD15.
* Dump: 33 of 48 used for 33 vehicles (no leak).
* Fix (relocation): allocate NEWA/NEWB (N x 0x50), NEWT (N x 4), NEWC1 (N x 0x800), NEWC2 (N x 0x400); add the base delta to every displacement/immediate above,
  loop bound 0xA94D71 imm32 -> N*0x50, cb carve bases (imm32 at 0xA94C7B and 0xA94C80), table refs 0xA94D5B, 0xA9672A, 0xA96753, 0xA98773, and the bounds
  0xA96731 (`83 F8 30` -> imm32 form needs a cave) / 0xA9876B. Simplest: hook alloc/free in the ASI and re-run the init loop there.
* Record pools (descriptors; memory source is allocator object 0x489CA20, headroom NOT verified): immediates `0x100C9C6` (`30 00 00 00`, instance 48),
  `0x100CA76` (`90 06 00 00`, component 1680), `0x100CB26` (`20 0D 00 00`, colour 3360). Scale all three by N/48 (they were sized as 48 x 35 and 48 x 70).
* Not verified: what the renderer shows when V+0xA88 is NULL (expected: default/unpainted car).

### 2.4 A4: seat / occupancy registry (48 records)

Base `0x5C4D250`, stride 0x1640 (8 occupancy slots x 0x100 per record), end 0x5C8FE50; the network receive queue follows at 0x5C8FE88 (do not move).
Find-free `0xB07310` (`83 F8 30 7C ED` at 0xB07323), find-by-handle `0xB07340` (`83 F8 30 7C E7` at 0xB07364), static ctor loop `mov edi,0x2F` (`BF 2F 00 00 00`) at 0x100D8F7.
Submitter `0xAF1240` (30 callers incl. ambient spawn 0x92EB0A with the instant flag; its result is ignored there): no free record -> returns 0.
Dump: 25 records in use with 33 cars alive.

Relocation plan, every decoded reference:
* base imm `0x5C4D250` (`B9 50 D2 C4 05` / `BE 50 D2 C4 05` / `BD`/`BB` forms): 0x607E8E, 0x608D41, 0xAF06B1, 0xAF06F1, 0xAF079C, 0xAF080C, 0xAF088B, 0xAF091B, 0xAF098B,
  0xAF0A2B, 0xAF0A8B, 0xAF0AEB, 0xAF0B4B, 0xAF0BAB, 0xAF0C0B, 0xAF0C6B, 0xAF0CCB, 0xAF0D1B, 0xAF0D6B, 0xAF0DBB, 0xAF0E7B, 0xAF1326, 0xAF1336, 0xAF1414, 0xAF147B,
  0xAF14FC, 0xAF159B, 0xAF24AF, 0xAF2CC8, 0xAF2D72, 0xAF2E8A, 0xAF301B, 0xAF36BA, 0xAF3D86, 0xAF5176, 0xAF7B57, 0xAF7DDC, 0xAF7EC2, 0xAF7F80, 0xAF81AD, 0xB0A1D6, 0xB0A33D,
  0xB0A3AD, 0xB0A40D, 0xB0A4F2, 0xB0A55D, 0xB0A61C, 0xB0A715, 0xB0A81D, 0xB0BA8B, 0xB0CE09, 0xB0D8EC, 0xB0DF27, 0x100D8F2 (the address printed is the instruction start; the imm32 is at +1).
* end imm `0x5C8FE50` (`81 FE/FD/FB 50 FE C8 05`): 0xAF06C3, 0xAF0703, 0xAF254A, 0xAF7F50, 0xAF8300.
* counts: `83 F8 30` at 0xB07323 and 0xB07364 (grow to imm32 form via cave, or hook both functions), `BF 2F 00 00 00` at 0x100D8F7; exit dtor array (static, near 0x101D...).
* The operands 0x5C8FE88 at 0xAF1611, 0xAF25B0, 0xAF8046, 0xAF8151 belong to the receive queue, not the registry.
The limit is "vehicles that currently have occupants/transitions"; it must be >= ambient + player + mission vehicles.

### 2.5 Lower-severity items

* C1 P3: records 0x5C30960 (stride 0x18), count/ids 0x5C30DE0/E1, alloc 0xAC2480 (ctor 0xA76BF0), free 0xAC24C0 (callers 0xAD1C76/0xAD1C9A); alloc sites 0xAD198E/0xAD19AA are null-checked.
* C2 P2: alloc 0xAC23A0 from 0xAB7A37 (kneecapper upgrade only), free from 0xAB5EB7.
* C3: alloc `0xB732A0` (`3C 28` at 0xB732A6), records 0x5CD3BA0 + count 0x5CE21A0, init 0xB72132, free tail 0xB73741..0xB7376B.
* C4: builder 0xAAB2E0..0xAAB48F, head `[0x5BBAC8C]` (`8B 35 8C AC BB 05` at 0xAAB33E), 16 records x 0xB0 built by 0xAAB530.
* D1: `cmp [0x5B224E0],0x80 / jb` at 0xA8F519; records at 0x5B224F0 stride 0x40; reset by 0xA87910.

### 2.6 Checked and cleared (no per-vehicle fixed limit)

* Traffic/spawn code 0x8F0000-0x930000: stack candidate arrays are bounded by their own loop test (100 at 0x908E40, 64 at 0x904A64, 32 at 0x90D8C0, 20 at 0x91F930, 32-entry list + global < 0x352 at 0x92AB44). The remaining 0x30 immediates there are already-patched 0x90DE02 or stack frame sizes.
* `0x77A548/0x77C099/0x782584/0x7849FE/0xAA93A7`: `cmp r,0x30` with 4-byte stride = 12-entry loops. `0xA6DD15/0xA6DE95`: 48-bit player garage bitmask loops (32-bit mask global 0x5B0D584).
* Static array 0x5F0D380 (48 x 0x50, ctor 0xDC57C0): generic thread-keyed helper, not vehicle.
* Radio (`Simultaneous_NPC_Radios`, retail 1) affects audio only. Vehicle type table (132 x 0xB80) is per type, not instance.

## 3. Other engine pools that scale with vehicles (dump values)

`audio_emitter` 305 (211 used), `parking_spot` 125 (98 used; parked max 60 will saturate it), `script_vehicle` 110, `roadblock` 32, `object_rig` 90 (42 used; attached-part
controller, vehicle use unproven). These are normal engine pools (cap u16 at +0x3A) and are raised like the vehicle/NPC pools in `pools.cpp`. `audio_emitter` is the one worth raising.

## 4. What I could not determine

* Whether every car consumes R1/R3 (0xA84D4A needs `[vehicleDef+0xAC4]` != 0). Humans certainly do. B1-B3 are "likely", not proven, for the freeze. Instrument the free counters first.
* What the renderer does with a NULL vcust record (A3) or V+0x1544 == 0 beyond the cited spec statements.
* Light node pool (spec 0x300 x 0x48), physics registration stack (spec 0x0163A8F0, 5000) and the 1024-node AI path ring (`cmp [0x5CB5E50],0x400` at 0xB42FBD): located by spec/compare only, per-vehicle usage not measured.
* Memory headroom of the vcust descriptor pools (allocator 0x489CA20).
* Skid/tyre marks and engine voices: no per-vehicle static table found (only the `skid_mark` effect id at 0xAA17F6); engine emitters come from the audio_emitter pool.
* Nothing was executed; the only dynamic evidence is the old minidump.

## 5. Suggested order

1. B1/B2 (10 immediates) + B3 (2 immediates) and add free-counter logging: cheapest, most likely to cure the frozen visuals.
2. A1/A2 hooks + the null check at 0xAD70D2 (prevents the crash while the rest is built).
3. A4 registry relocation (needed for more than 48 driven cars) and A3 vcust slots/record pools.
4. Raise audio_emitter and parking_spot; leave the C items.
