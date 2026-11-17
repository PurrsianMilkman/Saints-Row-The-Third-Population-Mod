# Saints Row: The Third – Population Mod (alpha)

> **Status: ALPHA.** It runs, but it is unfinished and only lightly tested. Expect crashes and odd behaviour, and back up your saves.

An ASI plugin that raises Saints Row: The Third's limits on ambient traffic, pedestrians and parked cars, and lifts the engine limits that
break when the city holds far more people and vehicles than it was built for. **The game executable is never modified on disk.** Every
change is made in memory at startup, and each patch site is checked against its expected original bytes first. If the bytes don't match,
that patch is skipped and logged.

Supported build: **`SaintsRowTheThird_DX11.exe`** (PC, 1.0.0.1, tested on the GOG release). Most features are DX11-only.

## What it changes

| Area | Vanilla | Mod (default ini) |
|---|---|---|
| Ambient car target | 12–18 | 400 |
| Pedestrian base | 40 | 120 |
| Parked-car maximum | 18 | 60 |
| Vehicle / NPC entity pools | 48 / 150 | 500 / 800 |
| Effect / mover / debris pools | 1230 / 5280 / 50 | 5000 / 10000 / 127 |
| Spawn / despawn distances | vanilla | ×4 (vehicles and pedestrians only, not LOD) |
| Spawn-reservation tracker (live humans / vehicles) | 150 / 48 | 800 / 500 |
| Per-type render "dirty update" bitsets | sized for the vanilla pools | sized for the new pools |
| Seat registry, vehicle customisation slots | 48 | 512 |
| Wheel / tyre records | 48 | 255 |
| Animation registries | 400 / 400 / 375 | 2048 |
| Render-library instance caps, Havok TOI queue | vanilla | raised |
| "permanent" engine heap | 11.4 MB | 32 MB |

All values can be changed in `SR3Population.ini`. Every engine-limit fix has its own `Fix…=0/1` switch, so you can find which one is
responsible for a problem.

## Known issues (alpha)

- **The traffic and pedestrian counts don't reach their targets yet.** The game rewrites its pedestrian/driver split every update
  (about 40 pedestrians and 30 drivers), which caps ambient pedestrians at 40 and cars at about 28. This is the next fix.
- Parked cars depend on the map's parking spots near the player.
- Only short sessions have been tested: 10 minutes standing in the world, and several 3-minute runs. Long play sessions have not been tested.

## Install

1. Install [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) as `dinput8.dll` in the game folder.
2. Build (below), or take `SR3Population.asi` and `SR3PopCounter.asi` from a release.
3. Copy both `.asi` files and `config\SR3Population.ini` into the game folder, next to `SaintsRowTheThird_DX11.exe`.

`SR3PopCounter.asi` is optional. It shows an on-screen overlay with live vehicle, parked and pedestrian counts (F10 toggles it).

**Uninstall:** delete the two `.asi` files and `SR3Population.ini`. The game files are never changed, so nothing needs restoring.

Your support is greatly appreciated. Nothing is paywalled: https://www.patreon.com/cw/Purrsian

## Logs and crash reports

`SR3Population.log` (in the game folder) contains:
- one OK/SKIP line per patch;
- every 4 seconds: spawn diagnostics, entity pools, engine memory-pool fill levels, and engine warnings;
- on a crash: `FAULT` / `FATAL` blocks. Each faulting address and register is classified (exe section, mod-relocated table, engine
  memory pool, entity pool slot, stack, heap…). A fatal crash also writes `SR3Population.dmp`.

`python tools\crash_report.py <log>` turns the crash blocks into function addresses, disassembly and a verified call chain.

## Build

Visual Studio (x86 toolset). From a PowerShell prompt in the repo:

```
cmd /c ".\build.bat"
```

The output is `out\SR3Population.asi` and `out\SR3PopCounter.asi`.

## Repository layout

| Path | Contents |
|---|---|
| `population/` | the main plugin: counts, distances, spawn throttles, pools, engine-limit fixes, audit and crash reporter |
| `counter/` | the overlay plugin |
| `config/` | default `SR3Population.ini` |
| `audit/` | static reverse-engineering reports on every engine limit found (addresses, bytes, rationale) |
| `tools/` | Python and PowerShell helpers: disassembly helpers, crash analysis, unattended launch test, equivalence harness |
| `NOTES.md` | development log and handoff notes |

## Credits and legal

Saints Row: The Third is © Volition / Deep Silver. **No game assets or game code are distributed here.** The repository holds only
original source code and documentation. The `tools/` scripts read your own installed copy of the game.

Licensed under the MIT License (see `LICENSE`).
