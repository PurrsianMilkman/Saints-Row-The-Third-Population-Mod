# Engine-side capacity limits outside the entity pools (static audit, DX11 exe)

Target: `C:\Games\Saints Row 3\SaintsRowTheThird_DX11.exe`, VA base 0x400000, DX11 addresses throughout.
Live numbers come from the in-game minidump (about 33 vehicles / 70 NPCs) via `readmem`. Static analysis only; nothing was run.
Confidence tags: [H] read straight from code and dump, [M] code read but a consequence is inferred, [L] guess.

## 0. Ranked summary (what breaks first as population grows)

| # | Limit | Cap | Live at dump | Breaks at | Failure | Fix |
|---|-------|-----|--------------|-----------|---------|-----|
| 1 | "vehicle custom instance" list + 48 static vehicle-customisation CB slots (vcust cb / ext vcust cb) | **48** | 33 | **49th live vehicle** | NULL render-instance, NULL deref in the creator -> crash [M] | section 1 (list cap one imm32, slot arrays need relocation) |
| 2 | `vehicle_deformable_mesh_instance` renderlib class | **194** | 66 (peak 70) | ~97 vehicles (2 per car) | pool returns NULL, vehicle part silently missing or crash [M] | section 2, static cap dword at 0x1312A70 |
| 3 | `rl_skin_instance` | 512 | 180 (peak 236) | ~1.8 per entity: about 280 entities | NULL, entity not drawn [M] | section 2, `mov esi,0x200` at 0x5DF6D9 |
| 4 | `rl_dynamic_material` | 1536 | 1167 (76%) | already 76% | NULL | section 2 |
| 5 | `rl_corona_instance` (car lights) | 700 | 244 (peak 282) | about 80 cars | missing coronas | section 2 |
| 6 | Havok TOI event queue | **250** | 0 at dump | bursts of CCD contacts, many fast bodies | event dropped/"TOI event queue full"; tunnelling | section 3, one imm32 at 0xC2FD40 |
| 7 | Havok character-proxy job table | **150 per frame** | 28 | 150 proxy updates per frame | extra humans' proxy job is not queued (silent) [M] | section 3 |
| 8 | `rl_pc_clone_mesh_instance` | 1024 | 82 (peak 112) | ~12 per entity-ish | NULL | section 2 |
| 9 | Char morph render instances ("mv cb %d") | 152 | 1 | only characters with morphs | NULL, result stored unchecked [L] | section 6 |
| 10 | `rl_light`, `rl_zbias_decal`, `rl_shape_occluder`, `rl_reflected_light` etc | see table | low | fine at 400 cars only if lights scale less than 1 per car | | section 2 |

Items 1, 2 and 3 are the ones to fix before attempting 400 vehicles. None of this was in audit.cpp's scale table because these are fixed
arrays and descriptor caps, not mempools.

---

## 1. Vehicle customisation slots: hard 48 (the next wall after the entity pools)

[H] code, [M] consequence.

What it is. Every live vehicle owns a "vehicle custom instance" (render-side customisation state: paint, wheels, underglow, etc.).
Pieces, all static:
* The instance list object at **0x1311670** (class vtable 0x1184B2C, name string "vehicle custom instance").
  Static initialiser at **0x100C9B7**: `mov dword ptr [0x1311688], 0x30` (capacity 48), item size 0x60 (`shl eax,5 * 3` at 0xA96C57), backing store from the
  "permanent" mempool (0x489CA20, field [0x131168C]). Items are chained into a free list at init (method 0xA96C10); allocator is 0xA93830
  (pops free list head, returns NULL when empty).
* A parallel slot table, also fixed at 48:
  * flag dwords `0x5B940A0 .. 0x5B94160` (one owner pointer per slot),
  * 48 x 0x50-byte mempool objects "ext vcust cb N" at 0x5BB9AC0 over a 48 x 0x800 buffer at 0x5B94160 (2048 B each),
  * 48 x 0x50-byte mempool objects "vcust cb N" at 0x5BB8BC0 over a 48 x 0x400 buffer at 0x5BAC160 (1024 B each).
  Initialiser 0xA94B95 (loop bound `cmp esi,0xF00` = 48 x 0x50 at 0xA94D71; bytes `81 FE 00 0F 00 00`).
  Static array ctors/dtors: 0x100CB90 / 0x100CBC0 (push 0x30, 0x50, base), 0x101CCF0 / 0x101CD10.
* Allocator = 0xA966C0 (callers 0xA789E4 vehicle creation, 0xA993F9, 0xA99BE9, 0xA99D35 re-creation after customisation):
  1. `call 0xA93830` on list 0x1311670 -> NULL when all 48 are taken -> `xor eax,eax; ret`.
  2. else finds first free slot by scanning `cmp [eax*4+0x5B940A0],ebx` with `cmp eax,0x30` (bytes `83 F8 30 7C F1 EB 1F` at 0xA96731); if none free, slot pointers stay 0 and it
     returns 0 as well (0xA9675F path).
* Free = 0xA986A0: recovers the slot index as `(ptr-0x5BB8BC0)/0x50` (`81 E9 C0 8B BB 05` at 0xA9874E, magic divide 0x66666667) with `cmp eax,0x2F` (`83 F8 2F` at 0xA9876B).

Live fill at the crash dump: slots 0..30, 33, 34 occupied (33 vehicles); one-to-one with live vehicles. Each used slot holds 52..152 bytes of CB data.

What happens past 48. At 0xA789E4 the NULL result is stored (`mov [edi+0x400],eax`) and immediately passed as arg0 to **0xA99640**, whose prologue does
`mov [ebx+0x3c], ebp` with ebx = arg0 -> write to address 0x3C -> access violation. There is no NULL check on this path.
Vanilla could never hit it because the vehicle pool is 48. With the mod's pool of 300 the first vehicle past 48 should crash the game (crash, not a frozen model).
User saw no crash because the spawner has not exceeded ~40 live vehicles so far. Confidence in the crash: [M] (I read the prologue, not the whole callee).

Fix.
1. List capacity: `0x100C9C0: C7 05 88 16 31 01 30 00 00 00` -> imm32 at **0x100C9C6** (`30 00 00 00`) to N (N = 512 -> `00 02 00 00`; 512 x 0x60 = 48 KB from "permanent", which has ~780 KB free; do not exceed ~4000).
2. Slot arrays must move to heap (VirtualAlloc, 16-byte aligned). Sites to rewrite, all verified to be the only code references to these arrays:
   * flag dwords base 0x5B940A0: operand at 0xA94D5B, 0xA9672A, 0xA96753, 0xA98773 (4 disp32; NOTE the plain `occ()` scan also hits many other lines in 0x5B94xxx - those belong to other data, ignore them).
   * "vcust cb" objects base 0x5BB8BC0 and "ext vcust cb" base 0x5BB9AC0: every disp32 in 0xA94B95..0xA94D5B (about 25), 0xA96740, 0xA96746, 0xA98750 (`sub ecx,0x5BB8BC0`), the static array ctor/dtor pushes at 0x100CB9E, 0x100CBCE, 0x101CCFA, 0x101CD1A and their `push 0x30` counts at 0x100CB9B, 0x100CBCB, 0x101CCF6.
   * buffers: 0xA94C7B (`mov [esp+10h],0x5B94160` ext, stride 0x800) and 0xA94C7F (`mov ebp,0x5BAC160` vcust, stride 0x400).
   * counts: `cmp esi,0xF00` (0xA94D71), `cmp eax,0x30` (0xA96731), `cmp eax,0x2F` (0xA9876B), and the `cmp eax,0x5BB8B68` loop in 0xA94C5F belongs to a different 32-entry list at 0x5BB8168 (do NOT touch).
   Simplest implementation: keep the engine's own code and swap the base constants with a cave that VirtualAllocs `N*0x50` for both objects arrays, `N*0x400` and `N*0x800` for the buffers, and N dwords for the flags, then patch the disp32s (same technique as the plugin's existing caves). Order matters: patch before the engine init at 0xA94B90 runs.
   The static constructors at 0x100CB90/0x100CBC0 run `0xEA8578` (vector-ctor) over the old arrays at C-runtime startup; either leave them (harmless, they construct the unused originals) or retarget.
   The "permanent" mempool is 93% used (10.6 of 11.4 MB), so N x 0x60 stays small; the CB buffers (N x 3 KB) must come from VirtualAlloc, not from the engine heap.
3. If relocation is judged too invasive, the alternative is to cap live vehicles at 48 and instead raise render density some other way. I would not: user wants ~400.

Unknown: whether vehicles beyond the player's render distance (the "distant vehicles" tier, disabled via loose xtbl) skip this path. All 4 callers are in the live vehicle creation/recreation path, so I assume every live vehicle counts.

---

## 2. Renderlib per-class instance pools (the biggest finding for the visuals)

[H] data, [M] consequences.

Renderlib keeps a registry of fixed-capacity classes. Each class descriptor (static in .data) is laid out, relative to the name pointer:

```
+0x00 name  +0x04 hash  +0x08 sizeof  +0x0C ctor/init  +0x10 factory=0x5FFF750 ("rl factory" mempool)
+0x14 CAPACITY  +0x18 IN USE  +0x1C PEAK  +0x20 free-list head  +0x24 "exhausted" flag  +0x25 warn-enable  +0x28 base
```
(node form: node = name-8; the registry ring head is [0x5FFF724]; the `rl factory` mempool is allocated by **0xE2AF00** as the sum over all classes of `cap * size`, 16-byte rounded, using `malloc` (0xEA97AC)).
Verified: sum(cap*size) over the 59 descriptors in the dump = 9,745,552 B, and the "rl factory" mempool "used" = 9,745,600 B. So the factory sizes itself from the caps; raising caps before init automatically enlarges it. No audit.cpp mempool entry is needed or useful for "rl factory".

Allocation (shown for rl_skin_instance, 0xE46580; same shape for every class): pop free list `[desc+0x20]`; **if empty returns NULL**, and if `[desc+0x25]` is set marks `[desc+0x24]=1` and increments the global **[0x5FFF72C]** (number of currently exhausted classes - handy for logging). No message, no assert. Callers' NULL handling was not traced per class.

Live table from the dump (entity-linked classes first; cap / in use / peak; bytes per class):

| class | descriptor | cap address | cap | in use | peak | sizeof | likely driver |
|-------|-----------|-------------|-----|--------|------|--------|---------------|
| vehicle_deformable_mesh_instance | 0x1312A5C | **0x1312A70** (static dword `C2 00 00 00`) | 194 | 66 | 70 | 0x3B0 | vehicles, 2 per car |
| rl_skin_instance | 0x4734E14 | 0x4734E28 (set at runtime) | 512 | 180 | 236 | 0x220 | humans + vehicle skins |
| rl_pc_clone_mesh_instance | 0x475A71C | 0x475A730 (static `00 04 00 00`) | 1024 | 82 | 112 | 0x140 | vehicle part clones |
| rl_dynamic_material | 0x4734B90 | 0x4734BA4 (runtime) | 1536 | 1167 | 1167 | 0x2C | per-instance materials (paint, tint, etc.) |
| rl_corona_instance | 0x474A638 | 0x474A64C (static `BC 02 00 00`) | 700 | 244 | 282 | 0x12C | lights/coronas |
| rl_light | 0x4734DCC | 0x4734DE0 (runtime) | 2000 | 141 | 314 | 0x130 | dynamic lights |
| rl_reflected_light | 0x47354E8 | 0x47354FC | 400 | 17 | 27 | 0x120 | |
| rl_zbias_decal | 0x4734378 | 0x473438C (runtime) | 1024 | 158 | 234 | 0x110 | decals (blood, tyre marks?) |
| rl_planar_decal | 0x474AAB0 | 0x474AAC4 | 256 | 27 | 27 | 0xE0 | |
| rl_pc_morph_container | 0x475A6D0 | 0x475A6E4 | 200 | 50 | 50 | 0xC | morph chars |
| rl_pc_morph_instance | 0x4759BC0 | 0x4759BD4 (runtime) | 300 | 33 | 35 | 0x4C | morph chars |
| rl_pc_dynamic_morph_instance | 0x4759BF4 | 0x4759C08 | 100 | 7 | 8 | 0x3C | |
| rl_shape_occluder | 0x4734324 | 0x4734338 (runtime) | 350 | 108 | 116 | 0x300 | occluders, mostly world |
| rl_raycast_occluder | 0x4735458 | 0x473546C | 100 | 7 | 11 | 0x110 | |
| rl_mesh_instance | 0x4734834 | 0x4734848 (runtime) | 4096 | 1303 | 1405 | 0x100 | world + entities |
| rl_multi_mesh_instance | 0x47343C0 | 0x47343D4 (runtime) | 5120 | 1874 | 3351 | 0x120 | world + entities |
| rl_pc_mesh | 0x4759CC4 | 0x4759CD8 | 8192 | 6198 | 7020 | 0x50 | mostly world; 76% |
| rl_pc_shader | 0x475AA6C | 0x475AA80 | 1440 | 847 | 847 | 0x10 | data |
| rl_renderable_particles_instance | 0x4734A08 | 0x4734A1C | 1600 | 58 | 83 | 0xC4 | effects |
| rl_renderable_particles_instance_group | 0x474A5E0 | 0x474A5F4 | 512 | 141 | 155 | 0x120 | effects |
| vfx_mesh_instance | 0x4759960 | 0x4759974 | 512 | 140 | 167 | 0x2C | effects |
| vfx_effect_template | 0x47598C4 | 0x47598D8 | 350 | 281 | 282 | 0x28 | data, 80% |
| rl_static_material | 0x4734C44 | 0x4734C58 | 17408 | 15246 | 15823 | 0x18 | world data, 88% (static, not entity linked) |
| rl_material_map | 0x47342B4 | 0x47342C8 | 11264 | 8395 | 8937 | 8 | world data |
| rl_tree_instance, rl_tree | | | 5000 / 256 | 1912 / 11 | | | world |

(The full 59-row dump is reproducible with the scan in section 9.)

Where the caps come from. One PC set-up function, **0x5DF6C0**, writes most of them as immediates at engine start (before renderlib init, 0xE2AFF0 call at 0x5DF7F0).
Exact immediates (instruction bytes read from the exe; imm32 address in the last column):

| class | instruction | original imm | imm32 address |
|-------|-------------|--------------|---------------|
| rl_skin_instance AND rl_pc_custom_light_volume | `BE 00 02 00 00` mov esi,0x200 (esi later stored to both caps at 0x5DF729 and 0x5DF794) | 0x200 | **0x5DF6DA** |
| rl_dynamic_material | `C7 05 A4 4B 73 04 <imm>` | 0x600 | 0x5DF6F3 |
| rl_light | `C7 05 E0 4D 73 04 <imm>` | 0x7D0 | 0x5DF71B |
| rl_shape_occluder | `C7 05 38 43 73 04 <imm>` | 0x15E | 0x5DF735 |
| rl_zbias_decal | `C7 05 8C 43 73 04 <imm>` | 0x400 | 0x5DF749 |
| rl_mesh_instance (final write) | `C7 05 48 48 73 04 <imm>` at 0x5DF780 | 0x1000 | 0x5DF786 (an earlier write of 0x828 at 0x5DF701 is overwritten) |
| rl_multi_mesh_instance | `C7 05 D4 43 73 04 <imm>` | 0x1400 | 0x5DF711 |
| rl_pc_morph_instance | `C7 05 D4 9B 75 04 <imm>` at 0x5DF79A | 0x12C | 0x5DF7A0 |

Classes with static caps in .data (not touched by 0x5DF6C0): vehicle_deformable_mesh_instance (0x1312A70), rl_pc_clone_mesh_instance (0x475A730), rl_corona_instance (0x474A64C), rl_pc_morph_container (0x475A6E4), rl_pc_dynamic_morph_instance (0x4759C08), rl_reflected_light, rl_planar_decal, vfx_*. A plugin can write these dwords directly at load time (before the engine reaches 0x5DF6C0 / 0xE2AF00); the factory sizes itself.

Recommended caps (vehicle target 400 plus several hundred humans). Memory is size x cap and is cheap:

| class | new cap | cost |
|-------|---------|------|
| vehicle_deformable_mesh_instance | 1024 | 0.94 MB |
| rl_pc_clone_mesh_instance | 4096 | 1.3 MB |
| rl_skin_instance (and light volume, shared esi) | 4096 | 2.2 MB + 0.3 MB |
| rl_dynamic_material | 8192 | 0.36 MB, and "renderlib dynamic materials" mempool is already x8 |
| rl_corona_instance | 4096 | 1.2 MB |
| rl_light | 4000 | 1.2 MB |
| rl_zbias_decal | 4096 | 1.1 MB |
| rl_pc_morph_container / morph_instance / dynamic_morph | 800 / 1200 / 400 | tiny |
| rl_mesh_instance / rl_multi_mesh_instance | 16384 / 20000 | 4 MB / 5.6 MB |
| rl_shape_occluder | leave (world) | |

Sum of additions is about 20 MB of `malloc`, fine in a 32-bit LAA process. Do the writes as 32-bit data stores; order independent.
Caveat [M]: whether anything else reads these caps and has a second fixed array sized to the vanilla cap (as the vcust slots did) was NOT checked for each class. After raising, verify each with the "inuse/peak" scan; and watch [0x5FFF72C].

What exceeding does: allocation returns NULL (the descriptor flag at +0x24 is only raised when the warn flag at +0x25 is set; that flag is cleared for most). I did not trace caller handling, so the visible effect is unknown: either the part is not created (invisible vehicle part / entity) or a NULL write crashes. This is the most likely place for a "model stays behind while the body moves" failure: if a re-created render instance fails to allocate, the old one stays registered at the old transform. Confidence [L] on that story; [H] on the caps.

---

## 3. Havok

### 3.1 TOI event queue (`m_sizeOfToiEventQueue`) = 250 [H]
* The string at 0x11A1D30 is raised by two functions: 0xC27660 (multithreaded simulation, hkpMultithreadedSimulation.cpp line 0x415, test at 0xC2767E..0xC27686: `[edi+0x38]&0x3FFFFFFF` capacity vs `[edi+0x34]` size) and 0xC29AA0 (hkpContinuousSimulation::addToiEvent, line 0x456, `jne` at 0xC29AB9).
* The queue is an hkArray of 0x70-byte ToiEvent entries, reserved in the continuous-simulation constructor 0xC29940 with `reserve(world->+0x344)` (code at 0xC299AA: `mov ecx,[ebx+0x344]`).
* `world+0x344` is copied from `hkpWorldCinfo+0x64`. The cinfo constructor is **0xC2FC80**; it stores the default at **0xC2FD3D: `C7 46 64 FA 00 00 00`** (imm32 at **0xC2FD40**). The game builds its cinfo at 0x75E2D0 (call 0xC2FC80) and overrides only `m_broadPhaseQuerySize` (to 0x1000 via `C7 84 24 84 00 00 00 00 10 00 00` at 0x75E3C3), **not** the TOI queue. Live confirmation: simulation object at 0x4BBFC80 (vtable 0x11A1C44), array at +0x30, size +0x34 = 0, capacity +0x38 = 0xFA.
* Exceeded: Havok drops/merges the TOI event and posts "TOI event queue full ... increase hkpWorldCinfo::m_sizeOfToiEventQueue" (warn once); effect is lost continuous collisions (fast bodies tunnel / penetrate). Not a crash. With several hundred vehicles in motion plus debris this is plausible. [M]
* Fix: `0xC2FD40: FA 00 00 00 -> 00 10 00 00` (4096 events = 458 KB from the Havok heap). Must be applied before the world is built (0x75E2D0 runs at level init; the plugin loads earlier). Raising it also helps the multithreaded path (same member).

### 3.2 Character-proxy job table: 150 per queue lifetime [H], consequence [M]
* Function **0x79A160** (hk_custom_character_proxy.cpp) queues one job per character proxy update. Table of 64-byte records at **0x56421D0**, counter [0x56421C4], cap check **0x79A19B: `3D 96 00 00 00`** (imm32 at 0x79A19C = 0x96 = 150), base imm at `81 C6 D0 21 64 05` (0x79A1AB, imm32 at 0x79A1AD). Over the limit the function falls to 0x79A247 and returns without queuing.
* The counter is reset when the job group is torn down (0x799BB0 clears [0x56421C0]) and recreated next call (0x79A16D). The dump had 28 queued.
* Exceeded: the 151st proxy update in a group is silently dropped; that humanoid's Havok proxy is not stepped (stationary or stale position) while its animation/render continues. With 400 humans near the player this is hit every frame. (Already listed in NOTES.md as "Havok-glue 150x64"; this audit adds the exact bytes and that the reset is per group.)
* Fix: allocate N x 64 B (16-aligned, `movaps` is used) with VirtualAlloc, write the pointer into the imm32 at 0x79A1AD, and change both `cmp eax,0x96` (0x79A19C) and nothing else (only code reference to the table is 0x79A1AD; the job callback 0x79A140 receives the record pointer). N = 2048 is plenty.
* Not determined: whether the job group pool/jobs themselves (generic pools at 0x47323B8 jobs, 0x47323D8 groups; allocator returns NULL then `[esi]` store would crash at 0xDC64B1) have a smaller cap. I could not find their capacity in the time spent. This is worth a dynamic check: log `*(0x47323B8+...)`.

### 3.3 Havok "bmem" pools [H]/[L]
* `havok bmem 0` object 0x5533BA0 over buffer 0x4B33B80, size 0x800000 (8 MB); `havok bmem 1` object 0x5537470 over buffer 0x5333B80, size 0x200000 (2 MB). Buddy allocators with 8 KB leaves (2046 and 510 tree bytes at +0x3D, init in 0x787030 / 0x7873A0, created by 0x787820). Allocation 0x787B60 tries bmem0 then bmem1.
* In the dump both are completely free (every tree byte = 3, used-bytes counter +0x38C4 = 0). So they are not what Havok is consuming at ~100 bodies. Cannot say what gets allocated from them; probably only large transient blocks. Not a measured constraint.
* The 4 KB-ish Havok free-list heap `hkFreeListMem` (string 0x1197C00) draws from the game heap; its fill was not measured.

### 3.4 Other Havok items not found as fixed limits
* `m_broadPhaseQuerySize` = 0x1000 (query buffer; Havok splits overlong queries) - not a hard cap. [L]
* Broadphase handle count, contact point caps, island limits: no game override found; Havok 2010.2 uses growable arrays. No evidence of a fixed number tied to humans/vehicles. [L]
* `raycast_group_pool` (string 0x1156F40, used in the big per-step function at 0x75D4BE): a `0xDBA5F0` temporary pool on parent 0x489B088, size derived from a runtime count ([esi+0x68] call result). Looks data-driven, not a constant. [L]
* Ragdolls: no constant found. Per-ragdoll hkpWorld insertion has no static cap I could find. [L]
* The three `havok.cpp` frame functions use a 92 KB stack frame (0x169D0) with arrays of 0x4B0-byte entries sized at runtime. No fixed count seen.

---

## 4. Mempools: state at the dump and which are/are not entity-linked

From a full scan of the dump (about 450 pools; vtables 0x124DB74 block, 0x1120ACC linear). "used" for block pools is the block counter (+0x44), for linear pools bytes.

Already scaled by audit.cpp and consistent with what I found: RL_skin large (71/180 -> humans, 1 block each, 3072 B = 64 bones x 48 B), RL_skin small (682/2504 blocks with 33 cars -> **about 20.7 blocks per vehicle; unscaled would stop at ~120 vehicles**), Deformation weights (88/1824 blocks, 2.7 per car), shader_constant_cache (29/63), renderlib heap, dynamic materials (15616/32767), material block map, synced_resource (103/116), Animlib pools, v_anim data (429/1600), cloth sim runtime (26/1535), prop block pool.

Allocation check for the skin pools: **0xE5C990**: request `n` matrices (48 B each): `cmp eax,0x28 / jle` -> `n<=40` small pool (0x60-byte blocks, `n*0x30` bytes), else large pool. Pool creation 0xE5C910 uses globals 0x473A3E4 = 180 and 0x473A3E8 = 2504 (see audit.cpp, factor x8).

Not entity-scaled (load-time table data), so scaling them (as audit.cpp does for char_info) is harmless but unnecessary:
* "char_info data pool" 29464/32768 B: filled by a table loop at 0xBE2420 (per char_info row `eax+eax` bytes), load-time. [M]
* "NPC Palette" 4308/6000 B (0xBE8560 creates it, parse at 0xBE6BF0..), "Viseme mempool" 2976/3000 B (0x9795D0): load-time tables. [M]
* "temp traffic pool" (0xA69E91) and "distant vehicles temporary mempool" (0xB9298C) and "morph preload mempool" (0x5D4383): temporary scratch pools on parent 0x489B088 built via 0xDBA5F0, sized from a runtime element count x 4/ x 16; they live on the stack of that function, which is why the registry skips them. No fixed cap. [M]
* `blood_pool` (0x996A5B) is a table lookup, not a pool. [H] `decal mempool` (0x5D5EC1): decals gpu 154256/524288 B used. [H]
* `rl_particle_global_mempool`: two linear pools, 896000 B (0 used) and 576000 B (38912 used), tail pool 12288. Plenty. [H]

Near full but static (not entity related): audio 99.9%, game_heap 8 MB (used = size, committed), objects 99.6% (plugin enlarged), rl factory 100% (by construction), font gpu, interface gpu 93%, coop/cust slots gpu 89%, vehicle_info 91%, city 92%, permanent 93%, persona voice 99.6%, "externalized vehicle gpu" 69% (per vehicle model not per instance), vlib 48%, "stream2 shared prim pp" 80% (5126/6409 blocks; per unique mesh, could grow with more distinct models; if you want a hedge add `"stream2 shared prim pp"` x2 to kScale - the pool is only 256 KB).

Additions suggested for audit.cpp kScale: `{"stream2 shared prim pp", 2.0}` as a hedge only. Nothing else in the list is population driven. The real gaps are the fixed arrays and descriptor caps in sections 1-3.

Other fixed per-entity name families seen in the dump that are NOT mempool-limited by the scale table: "vcust cb N" 48 and "ext vcust cb N" 48 (section 1), "mv cb N" 152 (section 6), "pcust comp N" 72 (player customisation components, 148 B each, only a handful used - player only), audio persona voice pools "0:Player".."40:PhoneCall" (41 pools: Player x2, Homey x12, Ped x20 (indices 12..31), Lawman x8, PhoneCall; section 5).

---

## 5. Audio (Wwise "AK")

* No voice/emitter cap constant exists in the exe: `audio_constants.xtbl` (parsed at 0x55227A) only holds timers and distances (PlayTimers, VehicleImpactDistance, ...), and the Wwise global voice limit lives in the sound banks' Init.bnk, not in code. I could not determine it statically. [L]
* Persona voice dialogue pools: 41 linear pools named "N:Player/Homey/Ped/Lawman/PhoneCall" (4740 B each for Ped, 34200 for Player), created at 0x70A67A..0x70A795 ("%d:%s", "persona type %d", "persona voice" 1 MB pool at 0x491E528, 99.6% full). These are speech slots (20 concurrent ambient Ped lines), not per-human. Exceeding drops lines. Not scaled by population. [M]
* "Audio Emitter rbb tree" (string 0x116846C) is an r-b tree, no fixed size found.
* If cars/people sound wrong at scale, the lever is the Wwise project Init.bnk voice limit and the emitter count, not the exe. Needs a runtime test.

---

## 6. Character render instances "mv cb %d" = 152 (low priority)

[H] structure, [M] meaning. 152 records of 0x1F4 bytes at 0x49D4A00 (init 0x74DDE8, loop bound `cmp esi,0x49E7490`), free list head [0x49CB174], used list [0x49CB170]; allocator 0x74E3D0 returns NULL when empty; callers 0x893FFB and 0x9E36D3 store the result unchecked into `[x+4]`. At the dump the used list holds 1 record and the free list 151, so this is not a per-ped structure (70 NPCs, 1 used): it seems tied to morph/customised characters (the code references ".cmorph_pc"/".gmorph_pc" at 0x74D826..0x74D83F). Risk only if the mod spawns many morph-customised characters (unlikely). No fix proposed.

---

## 7. What I could not determine

* Whether callers of a NULL from each rl class pool crash or just skip (section 2). Needs a runtime test: log `[0x5FFF72C]` plus each class's in-use and peak once a second; the numbers are cheap to read (descriptor ring at [0x5FFF724]).
* Capacity of the job pool (0x47323B8) and job-group pool (0x47323D8).
* Wwise global voice limit and AK command-queue size.
* Whether anything else besides 0xA966C0 depends on 48 for vehicles (e.g. the 32-entry list at 0x5BB8168, `cmp eax,0x5BB8B68`, belongs to a different customisation list; its owner was not traced).
* Per-entity cost of rl_dynamic_material, rl_corona, rl_zbias_decal: only the totals at one moment are known (no baseline without entities), so my "per vehicle" ratios for those are inferred from the entity counts only.
* Havok heap fill and hkFreeListMem limits.

## 8. Suggested order of work

1. Raise the vehicle customisation list and relocate its slot arrays (section 1) - otherwise the first car beyond 48 crashes.
2. Raise vehicle_deformable_mesh_instance, rl_skin_instance, rl_pc_clone_mesh_instance, rl_dynamic_material, rl_corona_instance (+ the rest of the section-2 table). Pure data/imm32 edits, no relocation.
3. TOI queue 250 -> 4096 and the 150-record character-proxy table (section 3).
4. Add logging of every renderlib class descriptor (in-use/peak/cap) and [0x5FFF72C] to audit.cpp; this will show the next wall without guessing.

## 9. Reproducing the numbers

Descriptor scan (Python, using re_helpers): find every dword equal to 0x5FFF750 in the dump, take `va-0x10` as the descriptor (name pointer at +0), read cap/in-use/peak at +0x14/+0x18/+0x1C. Mempool scan: find dwords equal to 0x124DB74 / 0x1120ACC (4-aligned) in the dump regions, validate the name at +0x10, read buffer +0x38, size +0x3C, used +0x44 (block pools: block size +0x4C, count +0x50).
