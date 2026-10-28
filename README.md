# Medal of Honor: Vanguard — PS2 Static Recompilation

[![CI](https://github.com/LeandroSSB/moh-vanguard-recomp/actions/workflows/ci.yml/badge.svg)](https://github.com/LeandroSSB/moh-vanguard-recomp/actions/workflows/ci.yml)

Native PC port of **Medal of Honor: Vanguard (PS2, NTSC-U SLUS-215.97)** via static recompilation with [PS2Recomp](https://github.com/ran-j/PS2Recomp). Mouse + keyboard FPS controls, faithful 59.94 NTSC clock, and the full bring-up research diary.

[![status](https://img.shields.io/badge/status-not_playable_end--to--end_yet-orange)](docs/research-frontier.md)
[![bring-up](https://img.shields.io/badge/bring--up-7_of_9_stages_measured-green)](#where-it-stands-measured)
[![clock](https://img.shields.io/badge/clock-59.94_NTSC_faithful-green)](#where-it-stands-measured)
[![soak](https://img.shields.io/badge/soak-0_deaths_in_5%2B_h_runs-green)](#where-it-stands-measured)

> ⚠️ **Status: in-progress.** The game boots, plays the full EA logo movie with audio, and runs stable post-movie (main alive past every former wall — the first stage-machine advance landed 2026-10-26). The front-end/menu does **not** open yet: the game enters a post-movie circular idle decoded to instruction level (see [docs/research-frontier.md](docs/research-frontier.md)). Not playable end-to-end yet.

**[→ Live progress map](https://leandrossb.github.io/moh-vanguard-recomp/progress.html)**

## Where it stands (measured)

| stage | state | measured |
|---|---|---|
| Boot | ✅ | retail ELF boots through the runtime; SIF/IOP alive |
| ELF → recompiled code | ✅ | 18.8k+ functions, 0 undecoded instructions |
| CDVD streaming (files + sectors) | ✅ | 199+ reads served, cdvdfsv/IOP |
| EA logo movie (EAGMLOGO.MPC) | ✅ | full playback with audio (FFmpeg decode); 128 uploads @ 639×448 |
| Post-movie stability | ✅ | main alive, 0 deaths in 5+ hour-long runs |
| Clock | ✅ | 59.94 NTSC faithful — terminal vsync 60.03/s ±0.15% |
| Input (MKB layer) | ✅ | mouse-look + scripted-input channel landed ([PR #405](https://github.com/ran-j/PS2Recomp/pull/405)); menu interaction pending on the gate |
| Front-end / menu | ❌ | research frontier — post-movie circular idle, decoded link-by-link ([docs/research-frontier.md](docs/research-frontier.md)) |
| Gameplay @ 60 fps | ⏳ | chained to the front-end gate |

<sub>✅ = measured in this repo's runs (each measurement in [docs/bring-up.md](docs/bring-up.md)). ❌ = the open frontier, decoded. ⏳ = gated on the row above. "7 of 9" counts table rows, not playability: the port stops at the front-end gate.</sub>

## The fixes (12+, root-caused to the instruction)

Every wall in the table above was decoded to the instruction level, fixed with TDD in the runtime, and verified in long runs. Eight upstream PRs:

| PR | fix |
|---|---|
| [#398](https://github.com/ran-j/PS2Recomp/pull/398) | SetupThread's automatic main stack anchored at the top of RDRAM |
| [#399](https://github.com/ran-j/PS2Recomp/pull/399) | analyzer: data-, instruction- and table-referenced function pointers as callable entry candidates |
| [#400](https://github.com/ran-j/PS2Recomp/pull/400) | EE LQ/SQ force 16-byte alignment, as the hardware does |
| [#401](https://github.com/ran-j/PS2Recomp/pull/401) | GS: GIF A+D 0x58 PMODE writes routed to the privileged register file |
| [#402](https://github.com/ran-j/PS2Recomp/pull/402) | DMAC: GIF chains drain through IRQ tags to the END-tag CHCR readback |
| [#403](https://github.com/ran-j/PS2Recomp/pull/403) | IOP: the cdvdfsv RPC family served from a ROM-resident HLE service |
| [#404](https://github.com/ran-j/PS2Recomp/pull/404) | IOP: temporary sids (bit31) accepted; the loader queue's cid-0 SifSendCmd gets its completion reply |
| [#405](https://github.com/ran-j/PS2Recomp/pull/405) | input: opt-in scripted channel (PS2X_INPUT_SCRIPT) + MKB mouse-look layer |

Plus the fixes that live on the `vanguard-bringup` branch: wall-clock clock delivery with bounded replay, EE hardware timers advancing on the wall clock, the async-callback invocation carve, the MPEG need-data wake, timer/alarm delivery shaping. Each with root cause → fix → test in [docs/bring-up.md](docs/bring-up.md).

## What's next — the front-end gate

The one wall left: after the EA movie, main parks in a healthy kernel message-wait and the game falls into a **circular idle** — the worker holds zero tokens ← zero outstanding CD reads ← a worker-only demand issuer; the armer no-ops at gate ≠ 6; the stage machine has zero entries ([docs/research-frontier.md](docs/research-frontier.md), decoded link by link). Inventing game behaviour is off the table.

**The named instrument: A/B against PCSX2.** PCSX2 clears this frontier naturally, so the protocol is: pin both sides at the same logical point (post-movie), capture RAM on both, and let `abdiff` name the first divergent value — a runtime contract served wrong, or earlier game state diverging by timing. Already exonerated by measurement (don't re-spend): the runtime's alarm/vsync machinery, invocation delivery, the EE→IOP bridge, the disc path.

## Repo layout

```
├── ps2xRecomp/ ps2xRuntime/ ps2xAnalyzer/ ps2xIOP/ ps2xTest/   # PS2Recomp toolchain + our fixes
├── config/recompiler.toml      # the recompiler config (user paths)
├── config/mkb.ini              # mouse-look tuning (sens/deadzone/invert_y)
├── scripts/                    # setup / build / play pipeline (PowerShell)
├── docs/                       # bring-up story + research frontier
└── AGENTS.md CLAUDE.md         # agent-driven development guide
```

**No game data lives in this repo.** Bring your own legal copy: your own disc dumped to ISO, and (for PCSX2 A/B comparisons) your own console's BIOS.

## Requirements

- Windows 10/11, CMake 3.20+, MSVC (C++20), ~10 GB free
- Your own Medal of Honor: Vanguard (USA) disc → ISO

## Build & play

```powershell
# 1. build the toolchain (analyzer + recompiler + runtime)
.\scripts\build.ps1

# 2. from your ISO: extract the ELF, analyze, recompile (generates the engine code)
.\scripts\setup.ps1 -IsoPath "D:\path\to\your.iso"

# 3. build the game runner (links the generated code)
.\scripts\build.ps1 -Runner

# 4. play (mouse-look on, MKB config from config\mkb.ini)
.\scripts\play.ps1
```

Full pipeline details: [docs/bring-up.md](docs/bring-up.md).

## The bring-up story

This port exists because the game's own loader machinery is a chain of handshakes — CD streaming callbacks, IOP RPC round-trips, a timer-driven demand loop, and a message table that the loader populates one record at a time. Emulating the instructions wasn't enough: **each handshake needed the runtime to serve the same contract the PS2 hardware served**, or the game's own logic wedged. Every one of those contracts was decoded to the instruction level, fixed with tests, and documented:

- [docs/bring-up.md](docs/bring-up.md) — the 12+ fixes, each with root cause → fix → test
- [docs/research-frontier.md](docs/research-frontier.md) — the open frontier, decoded to the instruction

## Contributing

See [AGENTS.md](AGENTS.md) (also the human contributor guide): where each change goes, the probe discipline, the bring-up loop, and the known landmines.

## Legal

GPL-3.0 (inherited from PS2Recomp). Unaffiliated with Sony or EA. **Bring your own legal copy** — no game data, ISOs, or BIOS files live in this repo, and none should ever be committed.
