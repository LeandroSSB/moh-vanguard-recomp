# Known failures (ps2x_tests)

This file is the formal register of known-flaky tests. The machine-readable
list is [`known-failures.txt`](known-failures.txt) (format `suite :: test`,
MiniTest names); CI enforcement lives in
[`tools/ci/run_suite.py`](../tools/ci/run_suite.py), called by
[`.github/workflows/ci.yml`](../.github/workflows/ci.yml).

## The rules

1. **A known-failure must never mask a NEW regression.** `run_suite.py` fails
   the job immediately on any test that is not in `known-failures.txt` - no
   retry, no tolerance. The tolerated set is *exactly* the file's list,
   nothing more.
2. **A known-failure is retried at most once, scoped.** Only when the failing
   set is a subset of the list does CI re-run the affected suites
   (`PS2X_TEST_SUITE=` registration filter) once. Green retry passes the job
   with a loud `::notice::` (documented, not hidden); red retry fails it.
3. **Every entry needs a mechanism and evidence here.** A line in
   `known-failures.txt` without a matching section below is a bug in the
   register.
4. **Retire on cure.** When a listed test passes, `run_suite.py` emits a
   `::warning::` - remove the line (diario e correção, como manda o AGENTS.md:
   nunca esconda regressões em known-failures).

## Entries

### `PS2RuntimeInterrupt :: M6-T3: beats missed under a zero-cycle stall are replayed bounded, not dropped`

- **Mechanism.** The test drives the real EE scheduler for ~3.6 s of wall
  clock (a 600 ms zero-cycle stall between slow phases) and asserts the
  wall-faithful beat schedule: total span > 3.2 s, delivered rate in
  [56.9, 65]/s, and the bounded-replay invariant `maxDelta <= 16` (missed
  beats spread over scheduling passes, never a single-pass flood). Every
  assertion is wall-clock-derived: on a cold or loaded shared runner a
  scheduling pass can stretch so far that beats bunch into fewer passes (or
  the measured span/rate shifts), tripping the bounds without any semantic
  change.
- **Evidence.** CI run
  [40566755659](https://github.com/LeandroSSB/moh-vanguard-recomp/actions/runs/40566755659)
  (2026-10-27): 495/496, this test only, on commit `40f54f8` - which changed
  **zero** lines of runtime code relative to `d2630ff`, whose CI run had the
  Windows suite fully green (496/496). Same code, different outcome:
  environment-caused, not a regression.
- **Class.** Wall-clock-sensitive scheduler test, cold-env flake.

### `PS2GS :: sceGsSyncV resumes through the scheduler with deterministic field parity`

- **Mechanism.** The test asserts deterministic even→odd interlaced field
  parity across the first two VBlank deadline fires scheduled off the wall
  clock (`sceGsSyncV` → scheduler invocation → resume). On the first run in a
  cold build environment the scheduler's first deadline can race the wall
  clock, so the observed first field parity can flip; an immediate re-run in
  the now-warm environment is green.
- **Evidence.** Named in the bring-up diary (M6/M9 bring-up, first runs in
  fresh build environments); not yet observed on CI.
- **Class.** Wall-clock-sensitive scheduler test, first-run cold-env flake.

## Adding an entry

1. Reproduce the flake (CI or local) and root-cause the mechanism to the
   wall-clock/timing race - a *deterministic* failure is never an entry.
2. Add the line to `known-failures.txt` **and** the section here in the same
   commit.
3. State the evidence (run link or local log) and the class.
