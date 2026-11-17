# NPC / human count limits audit (SaintsRowTheThird_DX11.exe, static RE)

Date 2026-11-17. All addresses DX11 VA (image base 0x400000). Original bytes were read from the exe with `e.d[e.off(va):...]`.
"imm @X" means the 4 immediate bytes start at X. The crash minidump (70 NPCs, 33 cars) was used to confirm live state.

## Verdict table

| # | Site | What it is | Cap | Scales with humans? | Past cap | Severity |
|---|------|-----------|-----|---------------------|----------|----------|
| 1 | 0x8EFF50 / 0x8F0180 / 0x8EF880 / 0x8F03D0, node array 0x59F9558 | **Spawn-reservation tracker: one 0x28-byte node per live human** (kind 0; vehicles = kind 1, 48 nodes @0x59F8DD8) | 150 (humans) / 48 (cars); handle index is 8 bit (max 256) | YES, 1:1 (dump: 70 humans = 70 nodes used, 80 free) | reserve fails (-1) -> human not created (0x9D7EF2 returns 0), or an existing lower-priority node is evicted | **HIGH: hard ceiling of 150 humans regardless of pool/budget patches** |
| 2 | 0x79A160, table 0x56421D0 | Havok `hk_custom_character` per-frame deferred job queue (character-proxy step) | 150 per frame x 64 B | YES (one job per active character proxy) | job silently not queued: that proxy is not stepped that frame (stuck / no collision response) | **HIGH once >150 proxies are active in one frame** (dump showed 28 queued with 70 NPCs) |
| 3 | 0x8BE050 (called 0x8C6A0B) | Action-node system per-frame pass; stack array of 150 de-duplicated pointers, loops over ALL humans | 150 distinct | Partly (distinct targets, <= humans) | **stack buffer overflow**: slot 150 is the saved return address, more writes smash the caller frame | MEDIUM (crash if >150 distinct qualifying targets) |
| 4 | 0x8EFEED `push 0x96` | Co-op budget interpolation 0x9052A0(f,150): total budget 75..150 when a 2nd player exists | 150 | only in co-op | budget total stays <=150 in co-op | LOW (co-op only) |
| 5 | 0x6515F0 | Snatch activity (pimps/thugs/hos) participant list | 150 (8-byte pairs @+0x30, count @+0x674) | No (only Snatch spawns) | participant silently not recorded | LOW |
| 6 | 0x97D280 | `anim_flinches.xtbl` loader table | 150 rows x 12 B | No (data rows) | extra xtbl rows skipped | NONE |
| 7 | 0xACE220/0xACE2F0/0xACE500/0xACE562/0xACF020 | Vehicle definition hash table (`vehicle_table_par*`, `*_veh.xtbl`) | 150 buckets/entries | No (vehicle TYPES) | extra defs dropped | NONE (vehicle audit) |
| 8 | 0x8C8682 / 0x8C7934 / 0x1003B50 | `action_node` xtbl definition table | 150 defs x 0xCC (@0x58FA140), clamp of 0x58F4CC8 | No (data) | defs beyond 150 clamped | NONE |
| 9 | 0x8833EE | Network co-op `dc_states` budgets (150 = ticks/budget unit, Team A spec-network-coop) | n/a | No | n/a | NONE |

Details, fixes and confidence follow. Sections 1 to 3 are the ones worth patching.

---

## 1. Spawn-reservation tracker (HIGH) - the real ceiling on live humans

### What it is
`0x8EFF50(kind, class, priority)` (cdecl, returns a handle or -1) reserves a tracking record before an entity is created.
- kind 0 = humans, kind 1 = vehicles (confirmed: `0x9D7EAA` call in the human create/bind routine passes kind 0; `0x933E69`, `0xA7DA76` pass kind 1).
- Per-kind control block at `0x59FACC8 + kind*0x90`: +0 generation counter, **+4 free-list head, +8 free count**, then 11 per-class (head,count) pairs from +0xC.
- Node = 0x28 bytes: +0/+4 list links, +8 handle, +0x10/+0x14 owner entity handle, +0x18 class.
- Node arrays are static: humans `0x59F9558` (150 x 0x28 = 0x1770, ends exactly at 0x59FACC8), vehicles `0x59F8DD8` (48 x 0x28 = 0x780, ends at 0x59F9558).
- Init `0x8F0180` (called 0x91E740) links all nodes into the free list; count immediates: `mov edx,0x30` (vehicles) at 0x8F01DA, `mov edx,0x96` (humans) at 0x8F01E6.
- Reserve (0x8EFF50): (a) per-class live count < class limit (table 0x130C300/0x130C304, already x20 by the mod); (b) free budget via 0x8EFEC0 (already patched); (c) take a free node; if the free list is empty it tries to evict a lower-priority node (0x8EF9A0), and if that fails returns -1.
- Caller 0x9D7EAA (human create/bind, tail at 0x9D7EEF): on -1 (except class 7/8 retry) it returns 0 = the human is not created.

### Evidence from the minidump
Kind 0 block: free count (+8) = 0x50 = 80, free head 0x59F9D00, class0 list 40 nodes, class1 list 30 nodes -> 150 - 80 = 70 used = the 70 NPCs alive. Kind 1: free 15 of 48 = 33 used = the 33 cars. So the node count IS the live count.

### Scaling
Exactly 1 node per live human, so with HumanCap/pool 800 the 151st human is refused (or evicts a lower-priority one -> constant churn). This will cap ambient + scripted humans at 150 even with every other patch in place. The 150-limit in the budget table (kind0/class2 = 150, now 3000) is a *different* number.

### Fix (relocate + widen the handle)
Handle layout today (0x8EF880 builds, 0x8F03D0 decodes): `handle = ((gen*2 | kind) << 8) | index`, index decoded with `movzx edx,al` -> **max 256 nodes** per kind. To go beyond 255 the index must be widened. 16-bit index keeps every instruction the same length:

| Address | Original | New | Meaning |
|---|---|---|---|
| imm @0x8EF898 | 58 95 9F 05 | `<NEWH>` (VirtualAlloc, N*0x28 bytes, zeroed) | `mov eax,0x59F9558` in 0x8EF880 (index base, humans) |
| imm @0x8F01E2 | 58 95 9F 05 | `<NEWH>` | `mov eax,0x59F9558` in init 0x8F0180 |
| imm @0x8F03F8 | 58 95 9F 05 | `<NEWH>` | `mov eax,0x59F9558` in lookup 0x8F03D0 |
| imm @0x8F01E7 | 96 00 00 00 | `<N>` (e.g. 20 03 00 00 = 800) | node count, humans |
| 0x8EF8B6 | C1 E3 08 | C1 E3 10 | `shl ebx,8` -> 16 |
| 0x8F03D6 | 0F B6 D0 | 0F B7 D0 | `movzx edx,al` -> `movzx edx,ax` |
| 0x8F03D9 | C1 E8 08 | C1 E8 10 | `shr eax,8` -> 16 (kind bit, followed by `and eax,1`) |
| 0x8F03DF | C1 E9 09 | C1 E9 11 | `shr ecx,9` -> 17 (generation) |
| 0x8F0405 | C1 EA 09 | C1 EA 11 | `shr edx,9` -> 17 (compare with node+8) |

The 6 handle-format patches must be applied together with the relocation (else index>255 corrupts kind/generation). Generation then has 15 effective bits (stale-handle false match chance 1/32768, acceptable; the counter wrap at 0x7FFFFF in 0x8EF8C2 still only affects the truncated value consistently on both sides).
Allocate and fill the new array BEFORE `0x8F0180` runs (ASI init is fine, level init calls it at 0x91E740). Only the three base immediates above reference the array (scan of all .text 32-bit values in 0x59F8DD8..0x59FACC8 found only 0x59F8DD8 x3 and 0x59F9558 x3; no interior references).
Vehicles (kind 1) have the same structure: imm @0x8EF891 / @0x8F01D6 / @0x8F03F1 (`D8 8D 9F 05`) and count imm @0x8F01DB (`30 00 00 00`). Relocate the same way if the vehicle audit wants >48 tracked cars (the vehicle node cap of 48 also limits cars!). Do the handle widening once for both kinds.

Residual risk: I found only one handle decoder (0x8F03D0). Other code that stores the raw handle (e.g. 0x96E0F0 receives it from 0x9D7EAF) just passes it back to 0x8F03D0 as far as I traced. MEDIUM-HIGH confidence for the cap and the patch set; MEDIUM that no second decoder exists (not exhaustively proven; test by spawning >255 humans and watching for NPC rejection).
Minimal alternative with zero handle risk: relocate only and use N = 255 (idx fits in 8 bits). That lifts 150 -> 255.

---

## 2. Havok `hk_custom_character` deferred job queue (HIGH)

- Function 0x79A160 (source path string `...\src\game\havok\hk_custom_character`). Called from 0x79A349 inside 0x79A2A0, which is the character-proxy step (`hk_custom_character_proxy`, Team A spec-physics-runtime.md row 22), itself called from 0x79A84B and 0x79CA7B per proxy per frame.
- Static table `0x56421D0`: entry = 64 bytes (`shl esi,6`); counter `0x56421C4`; job-group pointer `0x56421C0`; adjacent global `0x56421C8` (set by 0x799BD0).
- Counter reset: implicit. When `[0x56421C0]==0` the function creates a job group (0xDC6400) and starts at index 0 (xor eax,eax). `0x799BB0` releases the group and zeroes the pointer once per frame, so the count restarts every frame. Dump: counter = 0x1C (28 queued in the last frame) with 70 NPCs.
- At 150 (`cmp eax,0x96 / jae 0x79A247`) the function returns with no job and no error: that character proxy is **not stepped that frame** (no collision/move result), so humans past the 150th active proxy freeze or ignore world collision. Silent.
- Scales with humans: yes, per active (near/physics-enabled) human proxy; not all 800.
- Table is not extendable in place (0x5644750 follows immediately and is used by 0x79D47F.. / 0x7A5DC1..).

Fix: relocate (everything references the table only via the one `add esi,imm32`; the job body receives the entry pointer in the job record; scan of all .text values 0x56421C0..0x5644800 confirms the only table-base reference is 0x79A1AD; the other hits in that range are false positives (e.g. `[esp+0x564]` displacements)).

| Address | Original | New |
|---|---|---|
| imm @0x79A19C (`3D 96 00 00 00 cmp eax,0x96`, opcode at 0x79A19B) | 96 00 00 00 | `<N>` (e.g. 800 -> 20 03 00 00) |
| imm @0x79A1AD (`81 C6 D0 21 64 05 add esi,0x56421D0`, opcode at 0x79A1AB) | D0 21 64 05 | `<NEWJ>` = VirtualAlloc(N*64) (page aligned = 16-byte aligned, required by the `movaps` stores at +0x10) |

Not determined: the capacity of the job-record pool at 0x47323B8 (vtable 0x117B5BC, slot 2 is the abstract stub; allocation at 0xDC64A0 does not check for NULL, it would crash on exhaustion). Whether jobs > 150 per frame are accepted by that pool must be tested in game (watch for a crash in 0xDC6490). Confidence: HIGH for the table/cap behaviour, MEDIUM for "just relocating is enough".

---

## 3. 0x8BE050 stack array of 150 pointers (MEDIUM, crash risk)

- Function 0x8BE050 (only caller 0x8C6A0B, inside the per-frame action-node pass `0x8C694B`, strings `action_node`, `action_node_restricted`).
- It zeroes a local 150-dword array (memset 0x258 bytes at [esp+0xC]; frame `sub esp,0x264`) then iterates **the whole human pool** (`[0x65382B4]`, active list `+0x1FC`, count `+0x204`; the dump shows capacity 150 at +0x200, count 70, i.e. 0x65382B4 is the NPC pool). For humans whose byte +0x500 is 0x1A or 0x2B and +0x501 == 0 it resolves the object behind the handle at +0x9B8/+0x9BC, takes `[obj+0x9C]` (edx) and inserts it into the array if not already present (linear search, loop 0x8BE188..0x8BE19B, `cmp eax,0x96`).
- When 150 distinct pointers are already present the search exits with eax=150 and **writes the 151st entry at [S+0x264] = the saved return address** (array is exactly at the frame end), i.e. return-address smash -> crash.
- Also bumps a saturating (0x7F) byte at `[[edx+0xD8]+0x2D]` per insert.
- Also the first loop (0x8BE071..0x8BE085) clears bytes of the 0x6C-stride action-node instance table `0x58F4CD0..0x58FA16F` (200 entries) - unrelated data table.
- Scales: number of distinct targets <= number of qualifying humans (and bounded by objects reachable via +0x9B8; with many NPCs each in their own car this exceeds 150).

Fix (all four are existing imm32, array stays at [S+0xC]; N=800 -> buffer 0xC80, frame 0xC8C < 4 KB so no stack probe is needed):

| Address (imm) | Original | New |
|---|---|---|
| @0x8BE052 (`81 EC 64 02 00 00 sub esp`) | 64 02 00 00 | 8C 0C 00 00 |
| @0x8BE057 (`68 58 02 00 00 push 0x258`) | 58 02 00 00 | 80 0C 00 00 |
| @0x8BE197 (`3D 96 00 00 00 cmp eax,0x96`) | 96 00 00 00 | 20 03 00 00 |
| @0x8BE1EE (`81 C4 64 02 00 00 add esp`) | 64 02 00 00 | 8C 0C 00 00 |

Other [esp+x] locals in the function are at S+0..S+8 (below the buffer) and are unaffected. HIGH confidence on the overflow mechanics, MEDIUM on how often it can trigger (depends on distinct targets).

---

## 4. Co-op budget (LOW)

`0x8EFEC0`: with a second player (0x9DE7E0 non-zero) ebx = `0x9052A0(float, 150)` - an interpolation between 75 and 150 (`N/2 + (N - N/2)*t`). The mod patched only the single-player `mov ebx,0x4B` (0x8EFEDA) path. `push 0x96` at 0x8EFEED, imm @0x8EFEEE (`96 00 00 00`), can be set to the same BudgetTotal. Only matters in co-op.

---

## 5. Snatch activity list at 0x6515F0 (LOW)

- Appends (handle lo, handle hi) to an array at object+0x30 (8-byte pairs) with count at +0x674 and silently ignores appends when count >= 150 (`cmp eax,0x96 / jae`). Object = the **Snatch activity**: callers 0x65BB65, 0x65BDF5, 0x65BEF0, 0x65C1E6 lie in the same code as the strings `snatch_pimp`, `snatch_thug`, `snatch_ho`, `snatch_vehicle`, `Pimps`, `Thugs` (xtbl activity setup, Team A spec-ai-runtime.md 46.49/26.2 describe the same activity). The pairs are the spawned pimp/thug/ho handles.
- Past 150: the participant is not recorded (no cleanup tracking). Population is a handful per Snatch; independent of ambient population. No patch needed (object layout is fixed; array ends at +0x4E0, +0x674 is the count, other fields in between). Constructor clear at 0xA96A5A (call 0xDA37F0 on +0x30).

## 6-9. Data-driven 150s (no action)

- 0x97D280: `anim_flinches.xtbl` (strings Flinch, Hit_Location, Hit_Direction). Ring of 12-byte rows at 0x5A32868 .. 0x5A32F70 (150 x 12 = 0x708, counter 0x5A32F70 follows). Row 151+ is skipped (`jge 0x97D47B`). Not human-count related.
- 0xACE220 etc.: object size 0x38C: 150-byte bucket map at +5, 150-byte chain array at +0x9B, 150 dword keys at +0x134, virtual key compare; constructed in the vehicle table (0xACF6DE, strings `vehicle_defaults.xtbl`, `%s_veh.xtbl`, `Vehicle_Weapon`). It maps vehicle TYPE names -> definitions. 0xACF020 compares the serialised size (0x38C) and the 150-entry sub-block. Not human related.
- 0x8C8682 clamps `0x58F4CC8` (number of loaded action_node definitions, 129 in the dump) to 150; definitions live at 0x58FA140 + i*0xCC (ctor loop `mov ecx,0x95 ... add eax,0xCC` at 0x1003B50). A second table of 200 x 0x6C (0x58F4CE0, count 0x58F4CC4 = 163 in the dump) holds action-node instances. Both are xtbl/level data.
- 0x8833EE: `dc_states` network block, 150 is a budget unit (Team A spec-network-coop.md).

## Ruled out (looked like limits, are not)

- `push 0x96` followed by `call 0xDA2320` (0x67064B, 0x6708BE, 0x670A3C, 0x9BB524, 0x9BDF1E, 0x9C069C, 0xB4C0C1, 0xB55ED1) = `timer_ms_arm(150 ms)`; 0xB520DC/0xB5DCBE use the sibling 0xDA2360.
- `push 0x96/0x95/0x97 ; call 0xCA4AB0 ...` in 0xC9xxxx-0xD0xxxx = OpenSSL error-code arguments (library code), `cmp ebx,0x96` at 0xCB6935/0xCB6DE4 = ASN1 year test.
- 0x9267F0 (`mov eax,0x96`) and 0x9268D4 (`push 0x96`) = the projectile pool (string `projectile`), already handled as a shared capacity getter by the mod; not humans.
- 0x9DA7AE `cmp eax,0x96` = grenade ammo count test; 0x962BB9 = damage value; 0x8F01E6 is item 1 above.
- `cmp ...,0x4B/0x4C/0x4A` (75) elsewhere are enum compares (0x7C724E, 0x9D29E5 `Pistol-Gang` counter == 76, 0x96E402 loop bound, 0xA9AC69) or `push 0x4B` constants unrelated to humans. Only the two known 75s (0x8EFED9, 0x8EFF2D) are budget.
- Sites 0x6EE2C1.., 0xF47xxx: byte offsets +0x95/+0x96 of structures, not counts.

## What I could not determine

1. Capacity of the job-record pool behind 0xDC6490 (item 2); it decides whether lifting the 150 table is sufficient.
2. Whether a second consumer decodes the reservation handle (item 1); a runtime test with >255 humans is the proof.
3. Limits that are not expressed as a literal 150/149/151/75: e.g. arrays sized by a runtime value read from a pool, or limits like 16-bit/8-bit indices over the human slot. A brute-force search for "base + 150*stride" address pairs and for 150-multiple byte sizes produced only noise (coincidental multiples), so it is NOT a clean negative result for such forms.
4. The Snatch object's class name (RTTI lookup of vtable 0x1184B2C returned an unrelated name; identification above is from the strings in the caller code).
5. Job-queue frequency of 0x799BB0 (per frame is inferred from the zeroed group pointer and the counter in the dump).
6. Mempool-type limits (char_info, RL_skin, Animlib parent aim, synced_resource...) are covered by audit.cpp (MEMPOOLS) and not re-audited here.

## Suggested patch order

1. Item 1 (reservation nodes, with handle widening) - without it nothing above 150 humans exists.
2. Item 2 (Havok job table).
3. Item 3 (stack buffer) - cheap, removes a crash.
4. Item 4 only if co-op is used.
