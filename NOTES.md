# Session notes 2026-11-02/03 (cdf21aec) - SR3 population mod

## Goal
User's Saints Row 3 copy is **C:\Games\Saints Row 3** (they RUN `SaintsRowTheThird_DX11.exe`, not the DX9 exe the Team A specs describe).
Make ambient traffic, pedestrians and parked cars "insane", **without patching the exe on disk** (ASI plugin + loose files in the game root).
Standing rule from the user: if the game misbehaves, **fix the cause, never lower the numbers**. Also: "traffic" means everything (on foot, driving, parked).

## Code: D:\SR3-Population-Mod  (installed copies live in the game folder)
Split into modules at the END of this session (equivalence to the old monolith PROVEN by the harness; not installed yet):
```
population/  common.h/.cpp (Ctx, L(), patch(), ini helpers)  counts.cpp  distance.cpp  spawn.cpp  parked.cpp  pools.cpp  diag.cpp  main.cpp
counter/     counter.h  build.cpp (DX9/DX11 address tables)  sample.cpp  overlay.cpp  main.cpp
config/SR3Population.ini     build.bat -> out\SR3Population.asi + out\SR3PopCounter.asi     archive/ (old monolith sources + builds, keep)
tools/       exeread.py (DX9 exe disassembly helper), exe11.py (DX11), dumpregs.py (minidump regs), harness/ (equivalence test, see below)
```
- Build: run `cmd /c ".\build.bat"` from **PowerShell** (bash `cmd //c` does not work). `vswhere` noise is harmless.
- Python helper modules must not be named `dis.py` (shadows stdlib; capstone breaks).
- Python heredocs mangle `\\n` into real newlines in C string literals -> use the Edit tool or build the backslash with chr(92). The Write tool refuses to overwrite a file not Read first.
- `SR3Population.ini` keys (current values in config/): VehicleMultiplier 33.4 (->400 cars), PedestrianMultiplier 3.0 (->120), HumanCap 1500, VehiclePoolSize 300,
  NpcPoolSize 800, BudgetMultiplier 40, BudgetTotal 3000, ParkedMax 60, DistanceMultiplier 4.0 (vehicle despawn + ped radii ONLY), PedDistanceExtra 4.0,
  SpawnRingMeters 200, SpawnSpeed 4.0, ModelNearbyCap 40, SegmentCap 12, BypassModelCap 1, ForceFullDensity 1, GlobalDistanceScale 1.0,
  EffectPoolSize 5000, MoverPoolSize 10000, DebrisPoolSize 120.
- Loose files also placed in the game root: `spawn_info_categories.xtbl` (caps x3 - probably NOT loaded: peds stayed at 40), `distant_vehicle_traffic_types.xtbl`
  (Length_Per_Spawn 1000000) and `distant_vehicle_spawn_parameters.xtbl` (densities 0) = far-field fake cars disabled at the user's request,
  `distant_ped_spawn_parameters.xtbl` (Length_Per_spawn 15).

## INSTALLED STATE (important)
Game folder has the **3:58 PM monolith build** of SR3Population.asi and the 3:48 PM ini, SR3PopCounter.asi from 3:29 PM-ish.
It does NOT contain: EffectPoolSize/MoverPoolSize/DebrisPoolSize patches, the "POOLS:" full table (only fullest-10), or anything from the split.
The user's last run (6:03 PM log) was therefore the OLD build, although I had told them the effect-pool fix was ready (I never installed it - the game was running).
Latest good build = `out\SR3Population.asi` / `out\SR3PopCounter.asi` (6:09 PM, built from the split sources, equivalent to the monolith (harness-verified)).
Installing: the game must be closed (the .asi is locked while it runs); compare SHA-256 afterwards.

## Verified engine facts (DX11 addresses; DX9 = about -0x1000 for code, data irregular; every site is byte-checked before writing)
- Pools: vehicle 48 slots, NPC 150, effect_object 1230, general_mover 5280, object_debris 50 ... 55 pools, all from the fixed "objects" heap. Heap size =
  sum of each pool class's size method (vtable[4] `mov eax,imm32`) + 0xC800 rounded to 64 KB, already 99.97% full in vanilla -> raising a capacity WITHOUT patching
  its size method crashes (NULL allocation in pool init, minidump at 0x84A2E6). size = 20 + cap*(stride+4). Capacity is the `push cap` in the allocate wrapper
  (vehicle 0xA86075 imm8 -> rebuilt with imm32 cave at 0xA86070; npc 0x9D8E65; effect 0x5CA645; mover 0xA06C45; debris 0x8F3675 imm8). Pool array at 0x61EE7E0 (idx 7 vehicle, 34 npc, 8 effect_object); capacity u16 +0x3A, free u16 +0x3E.
- The "18 ambient cars" ceiling was the **spawn budget gate** 0x904890 -> 0x8F0080: limit table [0x130C304 + 12*kind + 28*class] (kind1/class1 = 18), plus the free-capacity
  function 0x8EFEC0 with a hardcoded **75 total humans+drivers** (imm at 0x8EFEDA and a second term at 0x8EFF29). Both patched. Also hard ceilings: live ambient cap 0x30 (imm32 at 0x90DE03),
  human headroom 150 (0x9076F0), model nearby cap 4 (0x9164D1), segment cap 3/5 (0x90D023/28), per-model live cap jb (0x915B7F), spawn timer 200/300 ms at 6 inlined sites, parked max 18 (global 0x130CDB8 + 2 resets + setter clamp).
- Pass driver ~0x90E2C0..0x90E523; gates in order: 0x92A4E0, 0x92A4F0, 0xAD2250, 0x90DDD0, 0x904890; ring candidate generator 0x90D1A0; attempts 0x90CD00 (A), 0x90C110 (B).
- Car-base override branch (0x906564, 17 bytes) used imm8 and capped the target at 100/150 while the player was in a car -> rebuilt with imm32. Player enter/exit-vehicle handlers (0x9063DF/0x906441/0x9066F4/0x906746) rewrite the car-base global to 18/12 and clamp parked max to 18 -> patched.
- **The engine-wide distance scale (global 0x130EF8C, store sites 0x9D95C3 / 0x9D94F3) also drives vehicle and human LOD**. Multiplying it x4 made cars use lowest LOD from ~9 m (symptom: cars render wrong). Now left at 1.0;
  the boost is applied only at: ring constant (0x90D2C9 -> private double), vehicle despawn radius read (0x9E9C8F cave), 4 ped radius operands (70 m / 90 m doubles).
- Gate counters showed the pass stops after the initial fill (budget gate returned false); after budget patches ambient cars reached 22-36 (not 400): spawn points per pass are scarce (avg 0.5-7 candidates), and peds plateau at exactly 40 (unexplained - suspect per-group ped DayCap; spawn_info loose xtbl may not load).
- Vehicle releases are rare (~33/3 min) except one burst of 27 by the generic vehicle destroy routine (caller 0xA831BE).
- Pool fill levels (old build): effect_object hit 1230/1230 in one run (992 in another), general_mover 89%, object_debris 64%, parking_spot 125/125 once. Vehicle pool never above ~40/300.

## Crashes (both fixed)
1. Heap too small for enlarged pools (see above). 2. Typo in the jmp opcode of the pool-wrapper cave (`0xE8-0x0F` = 0xD9, illegal instruction at 0xA86070) -> 0xE9.

## OPEN PROBLEM: car visuals freeze mid-air
User report: model of a car (ambient AND the one they drive) stays frozen in the world while the real car keeps moving; appears within ~1-3 minutes. Cause NOT found.
Ruled out/weak: pool exhaustion alone (frozen models also happened with effect_object at 81%), vehicle churn (few releases), slot index >48 (unproven). Untested fix in unreleased build: bigger effect/mover/debris pools.
Leads: (1) 58 code refs to vehicle field +0x1DDC (used for LOD position) - log |pos(+0x40) - pos(+0x1DDC)| per vehicle to detect render/sim desync; (2) the 27-car bulk destroy;
(3) other fixed per-vehicle arrays sized 48; (4) disabled far-field "dummy car" tier (loose xtbl) interacting with handoff. Ask the user how it looks each time (they answer symptoms well).

## Equivalence test of the refactor - DONE, PASSED (2026-11-02 evening)
tools/harness/run_harness.py builds two test copies of the real DX11 exe whose entry point is a stub (LoadLibraryA(plugin) then Sleep forever), starts each,
reads the patched image + the code caves out of the process, and compares old monolith (archive/srpop.monolith.cpp) vs the new modules.
Result, repeated 8+ times: **186/186 patch regions equivalent, 0 different**; 72 OK / 0 SKIP lines; identical set of patch log lines (order differs only because the
new code groups patches by feature); every code cave disassembles identically (jmp caves, call wrappers, private doubles). Counter plugin: identical logged text
(tools/harness/run/counter_check.py). Harness noise that is NOT a plugin difference: the per-process /GS cookie at 0x474C9CC, and cave addresses (VirtualAlloc is random).
Run it: `python tools\harness\run_harness.py <old.asi> <new.asi>` (each with SR3Population.ini next to it; the game must not need to be closed - it never starts the game).
Gotchas: the system maps data into 0x400000.. of any normal process, so a plain harness cannot map the image; using the real exe as the host (entry redirected) works.
Python helpers: never `.split(b"\0")` a 70 MB bytes object (hangs) - use index(); the Windows Python cannot open /d/... paths.

## Next steps
1. Install the split build `out\*.asi` + `config\SR3Population.ini` (game closed, compare SHA-256) and verify the log shows `OK effect_object pool capacity 1230` (split already proven equivalent).
2. Reproduce the freeze with the new pools; if it persists add the +0x1DDC desync logging; read POOLS: lines early vs late.
3. Ped count stuck at 40 and parked cars low: find per-group DayCap compare for peds; check whether the loose spawn_info_categories.xtbl is actually used (log category records).
4. Remaining unknowns: kind-1 free capacity ([0x59FAD60], total 69) source; DX9 coverage of pools/budgets (DX11 only for those).

## 2026-11-17: dependency audit (in progress)
- ROOT-CAUSE CANDIDATE for the frozen car models: every concrete_instantiator has vtable[2] get_capacity = `mov eax,<vanilla cap>; ret`.
  The object system (0x47E030 / 0x47E560) sums these per type to size each type's object list, the objects heap and the 16-bit object
  handle ranges (limit 65535; vanilla sum 27894). Our enlarged pools (vehicle 500, npc 800, mover 10000, debris 127) still reported 48/150/5280/50,
  so objects past the vanilla count fell out of their type list / aliased other types' handles. FIXED in pools.cpp (capGetter: vtable slot ->
  private `mov eax,cap; ret`, because 0xF6BC30 / 0x9267F0 / 0x8F35E0 are shared by other classes). Installed 2026-11-17 06:18, untested in game.
- audit.cpp: hooks the mempool ctors 0xDB9950 (block) / 0xDB1400 (linear) / 0xDB3150 / 0xDB3180 (base) -> registry of every mempool,
  enlarges population-linked mempools at construction (ResourceScale, default 8; buffers verified to be referenced only by their ctor push),
  logs block-pool can-alloc refusals (0xDB99D0), mempool errors (0xDC8EE0 -> buffer 0x5F10050) and the engine's empty warning function
  0x778910 (1386 call sites, e.g. "NPC limit reached"). Log lines: MEMPOOLS / WARN / REFUSED / MEMPOOL ERROR; full table every 60 s.
- Crash-dump mempool table (33 cars/70 npcs): shader_constant_cache 29/63 blocks, synced_resource 103/116, pf 99%, char_info 90%,
  renderlib heap 6.5/10 MB, RL_skin large 71/180 (characters), Animlib parent aim 79/127. Full object-pool names via RTTI (tools: re_helpers.py).
- Effect pool stays 1230: ~15 hardcoded 1230 uses in the effect module incl. a 108 KB stack frame with 1230-entry arrays (5000 crashed).
- Fixed-150 structures found (to fix): 0x6515F0 list of 150 pairs, 0x79A160 Havok-glue table 150x64 @0x56421D0, 0x8BE050 stack array 150,
  0xACE220 150-slot map. Parallel audits writing audit/npc-limits.md, vehicle-limits.md, effect-pool.md, engine-limits.md.
- ReShade + dlss5-feed were uninstalled at the user's request (moved to C:\Games\SR3-reshade-backup\, nothing deleted).

## 2026-11-17 (later): ROOT CAUSES FOUND + crash tooling
- FROZEN CAR MODELS = dirty_update_impl<T> bitsets. Each renderable type has a static container (table 0x5901B20) with an EMBEDDED
  bitset of slots needing a visual/transform update, sized for the vanilla pool: vehicle 2 dwords (64 slots), debris 64, effect 1248
  (4 containers), mover 5280. Slots past it set bits beyond the bitset (corrupting the next static object) and are never walked ->
  rendered model stops while physics continues. Mark 0x8CB5F0 / clear 0x8CB650 / walk 0x5DDCB0 all go through vtable[2] get_bits,
  so pools.cpp dirtyBits() redirects that slot to a private bitset. tools/dirty_sets.py lists every container.
- LOAD CRASH (memset 0xEB4F65, write 0x3840) = "permanent" mempool (0x489CA20, 11.4 MB, built by hand at 0x5D3210 - NOT via a ctor)
  ran out; sub-pools carved from it (1 MB at 0x70A790) got NULL buffers. Now PermanentHeapMB (default 32) patches 0x5D3216/0x5D325A.
- Render-walk crash at 0x5DBF60 = the mover bitset overflow (mover pool 10000 vs bitset 5280). Fixed by dirtyBits.
- Also implemented from the audits: spawn tracker 150/48 -> pools (+16-bit handle incl. a 2nd decoder at 0x8F0266 the audit missed),
  havok character jobs, action-node stack array, anim registries (400/400/375 -> 2048), wheel/tyre pools 48 -> 255 (+null check
  0xAD70D2), seat registry 48 -> 512, renderlib class caps, TOI queue 4096, vehicle customisation slots 48 -> 512 (+ record pools),
  effect pool 5000 (slot table G relocated + per-frame stack frame rescaled). Each has an ini switch Fix*=0/1 for bisecting.
- Crash tooling: crash.cpp (FAULT = first chance, FATAL = unhandled + SR3Population.dmp; every register / the fault address classified:
  exe section, mod table, mempool, entity pool slot, stack, module, heap, unmapped), tools/crash_report.py (function + disassembly +
  verified call chain), tools/launch_test.ps1 (launch, watch N s, kill; -Set key=value overrides). 0x9E7817 null-player read during
  loading is a vanilla FIRST-CHANCE fault the game handles - ignore it.
- Verified 2026-11-17 ~07:30: full build ALIVE 150 s with world running (26 cars, 40 peds), no FATAL, no REFUSED.
- Still open: ambient cars ~26 / peds exactly 40 (spawn-side limits, not resources).

## PAUSED 2026-11-17 (user request) - mod DISABLED
- All mod files moved out of the game folder to C:\Games\SR3-population-mod-disabled\ (both .asi, ini, logs, the 4 loose .xtbl).
  To re-enable: move them back into C:\Games\Saints Row 3\. SRTT.MixFix.x86.asi (not ours) is still installed.
- Next step when resuming: the human class split 0x8F0280 rewrites the budget limits every update - peds (kind0/class0) = 40, drivers
  (kind0/class1) = 30 (61/9 when the player flag 0x1D9A&2 is set; script override 0x130C460 for peds). THAT is the real ped 40 / car ~28
  ceiling (my x40 budget table patch is overwritten). Plan: cave at 0x8F02C1 (29 bytes a1 60 c4 30 01 ... 5f 5e c3) that does
  imul esi,PedMult / imul edi,cars/18 before the stores. Only caller 0x8F1020.
- Unattended testing works: tools/launch_test.ps1 -Continue -MenuAt 60 (plugin presses CONTINUE itself via AutoContinueSeconds,
  autotest.cpp hooks 0x5D24E8). EXITS/LIMITS lines in the log. The game runs windowed; synthetic keys/clicks are ignored by the menu.
- Uncommitted-in-game work since the last verified build: ped density override (0x130CB00 + reset 0x90F894), crowding threshold
  (0x90E3B0), EXITS/LIMITS diagnostics, autotest. Verified ALIVE 160-200 s in-world.

## 2026-11-17 session summary (for the next session)
Goal (user): far more traffic / peds / parked cars, without touching the exe on disk; fix causes, never lower numbers; user does NOT test -
launch and verify yourself (tools/launch_test.ps1).

Done today (all in population/, verified in game unless noted):
- pools.cpp: capGetter (vtable[2] get_capacity per resized pool), dirtyBits (vtable[2] get_bits of dirty_update_impl<T> containers ->
  private bitsets; THE frozen-car root cause), effect pool 5000 (slot table G relocated, 0x5C6EE0 stack frame rescaled), "permanent"
  heap 32 MB (PermanentHeapMB; 0x5D3216/0x5D325A - load crash root cause).
- limits.cpp: spawn tracker 150/48 -> pools (+16-bit handle, decoders 0x8F03D0 and 0x8F0266), Havok character jobs, action-node stack
  array, anim registries 2048, wheel/tyre pools 255 (+null check 0xAD70D2), seat registry 512, co-op budget.
- render.cpp: renderlib class caps, Havok TOI 4096, vehicle customisation slots 512 (+record pools).
- audit.cpp: mempool ctor hooks (registry + ResourceScale scaling), engine warning hook 0x778910 (it is also a shared empty no-op),
  can-alloc refusals, mempool errors.  crash.cpp: FAULT/FATAL + memory classification + dump.  autotest.cpp: AutoContinueSeconds.
- counts.cpp: ped density override 0x130CB00 + reset 0x90F894 -> 1.0.  spawn.cpp: crowding throttle 0x90E3B0 4 -> 127.
- diag.cpp: EXITS (per spawn-pass bail-out), LIMITS (live budget table), DESYNC (BROKEN: counts free slots - fix or remove).
- tools: re_helpers.py, crash_report.py, dirty_sets.py, launch_test.ps1 (-Continue -MenuAt 60 -Set k=v; screenshot tests/last.png), keys.ps1.
- audit/*.md: four static audits (npc, vehicle, effect, engine limits) done by subagents; every patch byte re-verified before use.
- Published: public repo github.com/PurrsianMilkman/Saints-Row-The-Third-Population-Mod, commit 0d16acd, release v0.1.0-alpha
  (prerelease, asi+ini attached). Do not push further without the user's word.

Learned / gotchas:
- Pool resize needs: alloc push + vtable[4] size + vtable[2] get_capacity + the type's dirty_update bitset (+ "permanent" headroom).
- UAL loads plugins before CRT static init (GOG build, no CEG) -> static-initializer immediates can be patched at DllMain.
- 0x9E7817 null-player read during load is a vanilla first-chance fault the game handles; my first reporter mistook it for the crash.
- The main menu ignores synthetic keyboard/mouse (SendInput) -> the plugin calls 0x7ACC20 (Lua main_menu_continue) itself.
- The menu background is a live city with traffic: counts taken at the menu are not in-world numbers.
- Python heredocs in bash turn "\n" into real newlines and backticks break quoting: write edit scripts with the Write tool.
- Agents' audit claims had one miss each time (second handle decoder 0x8F0266; extra G reference 0x5C3ECC) - always re-scan.

State: mod DISABLED (files in C:\Games\SR3-population-mod-disabled\), work PAUSED by the user.
Next: cave at 0x8F02C1 scaling the human split (peds 40 / drivers 30) -> then measure in-world with launch_test -Continue; fix DESYNC.
