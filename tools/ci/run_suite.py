#!/usr/bin/env python3
"""Run ps2x_tests and apply the known-failures policy (tests/KNOWN-FAILURES.md).

Usage: python tools/ci/run_suite.py <path-to-ps2x_tests[.exe]> <known-failures.txt>

Policy:
  1. Run the full suite once from the repo root; output is teed to
     ps2x_tests.log.
  2. All green -> pass. If the MiniTest summary never prints (hang, crash, or
     timeout) -> fail with no retry: a hang is not a flake pass.
  3. If the failing set is exactly a subset of the known-failures list, retry
     the affected suites ONCE (PS2X_TEST_SUITE=<registration substring>;
     full-suite re-run when the failures span several suites). Green retry ->
     pass with a loud notice (documented flake, not hidden). Red retry -> fail.
  4. ANY failure not listed in the known-failures file fails the job
     immediately, with no retry: a known-failure must never mask a NEW
     regression. The tolerated set is exactly the file's list - nothing more.
  5. A listed entry that PASSES triggers a warning so the line gets retired.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path

ANSI = re.compile(r"\x1b\[[0-9;]*m")
STATUS = re.compile(r"\[(Passed|Failed|Error)\]")
SUMMARY = "Total Tests:"

# Suite name (MiniTest::Case) -> PS2X_TEST_SUITE registration substring
# (ps2xTest/src/main.cpp gates registration on that substring).
SUITE_REGISTRATION = {
    "PS2RuntimeInterrupt": "register_ps2_runtime_interrupt_tests",
    "PS2GS": "register_ps2_gs_tests",
}

SUBPROCESS_TIMEOUT_S = 1800


def parse_results(raw: str) -> dict[str, str]:
    """{"suite :: test": "Passed" | "Failed" | "Error"} from MiniTest output.

    MiniTest prints "\\033[33m  [Run]: \\033[0m<test> " without a newline, then
    whatever the test logs, then "[Passed]"/"[Failed]" (or "[Error]: msg").
    The name ends at the first newline or ANSI code (same scheme as the
    reference repo's comprobar_pruebas.py).
    """
    marked = ANSI.sub("\x00", raw)
    suites = [(m.start(), m.group(1).strip())
              for m in re.finditer(r"^\[Suite\]: (.*)$", marked, re.M)]
    results: dict[str, str] = {}
    chunks = marked.split("  [Run]: ")
    pos = len(chunks[0])
    for chunk in chunks[1:]:
        start = pos
        pos += len("  [Run]: ") + len(chunk)
        suite = next((name for at, name in reversed(suites) if at < start), "?")
        name = re.split(r"[\n\x00]", chunk.lstrip("\x00"), maxsplit=1)[0].strip()
        status = STATUS.search(chunk.replace("\x00", ""))
        results[f"{suite} :: {name}"] = status.group(1) if status else "Error"
    return results


def run_suite(exe: str, log_path: Path, suite_filter: str | None) -> tuple[dict[str, str], bool]:
    """Run the suite once. Returns (results, summary_reached)."""
    env_filter = {**os.environ}
    if suite_filter:
        env_filter["PS2X_TEST_SUITE"] = suite_filter
    else:
        env_filter.pop("PS2X_TEST_SUITE", None)
    print(f"::group::ps2x_tests{'' if not suite_filter else ' (retry, PS2X_TEST_SUITE=' + suite_filter + ')'}")
    try:
        proc = subprocess.run(
            [exe],
            cwd=Path(__file__).resolve().parents[2],  # always the repo root
            env=env_filter,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=SUBPROCESS_TIMEOUT_S,
        )
        raw = proc.stdout.decode("utf-8", errors="replace")
        code = proc.returncode
        timed_out = False
    except subprocess.TimeoutExpired as exc:
        raw = (exc.stdout or b"").decode("utf-8", errors="replace") + "\n[TIMEOUT]"
        code = -1
        timed_out = True
    print(raw)
    print("::endgroup::")
    log_path.write_text(raw, encoding="utf-8", errors="replace")
    print(f"suite exit code: {code}{' (timed out)' if timed_out else ''}; log: {log_path}")
    return parse_results(raw), (SUMMARY in raw) and not timed_out


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    exe = sys.argv[1]
    known_file = Path(sys.argv[2])
    known = {
        line.strip() for line in known_file.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    }

    results, summary_reached = run_suite(exe, Path("ps2x_tests.log"), None)
    if not summary_reached:
        print("::error::the suite never printed its summary (hang/crash/timeout) - no retry for hangs")
        return 1

    failed = {name for name, status in results.items() if status != "Passed"}
    passed = set(results) - failed

    for name in sorted(known & passed):
        print(f"::warning::known-flaky entry PASSED - retire it from {known_file}: {name}")

    if not failed:
        print(f"All {len(results)} tests passed.")
        return 0

    unlisted = sorted(failed - known)
    if unlisted:
        print(f"::error::NEW regression(s) not in {known_file} (no retry): {unlisted}")
        return 1

    print(f"::notice::{len(failed)} failing test(s) are documented known-flakes; retrying once: {sorted(failed)}")
    suites = sorted({name.split(" :: ")[0] for name in failed})
    filters = [SUITE_REGISTRATION[s] for s in suites if s in SUITE_REGISTRATION]
    retry_filter = filters[0] if len(filters) == 1 else None  # several unknown suites -> full re-run
    retry_results, retry_summary = run_suite(exe, Path("ps2x_tests_retry.log"), retry_filter)
    if not retry_summary:
        print("::error::the retry run never printed its summary")
        return 1
    still_failing = sorted(name for name, status in retry_results.items() if status != "Passed")
    if still_failing:
        print(f"::error::known-flake retry STILL failing (escalating to red): {still_failing}")
        return 1
    print(f"::notice::known-flake(s) passed on the retry (run 1 failed: {sorted(failed)}). "
          f"Documented cold-env flake, not hidden: see tests/KNOWN-FAILURES.md.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
