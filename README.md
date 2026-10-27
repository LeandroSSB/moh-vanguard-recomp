# Medal of Honor: Vanguard — PS2 Static Recompilation

Native PC port of **Medal of Honor: Vanguard (PS2, NTSC-U SLUS-215.97)** via static recompilation with [PS2Recomp](https://github.com/ran-j/PS2Recomp). Mouse + keyboard FPS controls, faithful 59.94 NTSC clock, and the full bring-up research diary.

> ⚠️ **Status: in-progress.** The game boots, plays the full EA logo movie with audio, and runs stable post-movie (main alive past every former wall — the first stage-machine advance landed 2026-10-26). The front-end/menu does **not** open yet: the game enters a post-movie circular idle decoded to instruction level (see [docs/research-frontier.md](docs/research-frontier.md)). Not playable end-to-end yet.

## What works (measured)

| stage | state |
|---|---|
| Boot → ELF → recompiled code | ✅ 18.8k+ functions, 0 undecoded instructions |
| CDVD streaming (files + sectors) | ✅ 199+ reads, cdvdfsv/IOP served |
| EA logo movie (EAGMLOGO.MPC) | ✅ full playback with audio (FFmpeg decode) |
| Post-movie stability | ✅ main alive, 0 deaths in 5+ hour-long runs |
| Clock | ✅ 59.94 NTSC faithful (terminal 60.03/s ±0.15%) |
| Front-end / menu | ❌ research frontier (post-movie circular idle, decoded) |

**12+ runtime fixes upstream:** [PRs #398–#405](https://github.com/ran-j/PS2Recomp/pulls?q=is%3Apr+author%3ALeandroSSB) — SetupThread stack anchoring, EE→IOP bridge wake + IOP endTimeslice, SetupHeap staging, LQ/SQ 16-byte alignment, analyzer discovery tiers, timer/alarm delivery shaping, GIF DMA drain-to-END, MPEG need-data callback, PMODE A+D, and more. Each: root-caused to the instruction, TDD, reviewed.

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
