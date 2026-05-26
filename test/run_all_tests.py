#!/usr/bin/env python3
# Copyright (c) Dmitry Sapozhnikov
# Licensed under the MIT license.

"""
Unified runner for all test_*.py modules under this directory.

Each test module is expected to:
  * expose a module-level ``TESTS`` list of zero-argument callables that
    each return ``True`` (pass), ``False`` (fail), or ``None`` (skip);
  * print its own ``TEST: ... PASS/FAIL`` lines per case;
  * expose a ``kill_all_vtm`` callable for between-test cleanup
    (optional — if absent, the runner falls back to ``pkill``).

The runner imports each module in its own subprocess so that:
  * sys.exit() from module top-level (e.g. Windows-only files on Linux)
    does not abort the whole run;
  * stdout from each test is streamed live to the parent terminal;
  * any leaked state from one file cannot interfere with the next.

Final output: a combined summary of total passed / failed across every
module, in the same format as test_confirm_close.py's main():

    ==========================================================
    Results: P/T passed, F failed
    ==========================================================
"""

import os
import re
import sys
import glob
import subprocess
import time


HERE = os.path.dirname(os.path.abspath(__file__))
RUNNER_SELF = os.path.abspath(__file__)


def _discover_test_files():
    files = sorted(glob.glob(os.path.join(HERE, "test_*.py")))
    return [f for f in files if os.path.abspath(f) != RUNNER_SELF]


_RESULTS_RE = re.compile(
    r"Results:\s+(\d+)/(\d+)\s+passed,\s+(\d+)\s+failed"
)


def _run_one(path):
    """Run a single test file as a subprocess; stream output live and
    return (passed, failed) parsed from its 'Results:' line.

    If the file exits without emitting a 'Results:' line, fall back to:
        exit 0  -> 1 pass, 0 fail
        exit !0 -> 0 pass, 1 fail
    """
    label = os.path.basename(path)
    header = "=" * 78
    print(f"\n{header}\n>>> {label}\n{header}", flush=True)

    proc = subprocess.Popen(
        [sys.executable, path],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=1,
        text=True,
    )
    captured_lines = []
    try:
        assert proc.stdout is not None
        for line in proc.stdout:
            sys.stdout.write(line)
            sys.stdout.flush()
            captured_lines.append(line)
    finally:
        proc.wait()

    output = "".join(captured_lines)
    # Pick the LAST 'Results:' line in case the file printed several.
    matches = list(_RESULTS_RE.finditer(output))
    if matches:
        m = matches[-1]
        passed = int(m.group(1))
        failed = int(m.group(3))
        return passed, failed

    # Fallback when the module bailed out before printing a summary
    # (e.g. Windows-only files on Linux, or missing-binary error).
    if proc.returncode == 0:
        return 1, 0
    return 0, 1


def main():
    files = _discover_test_files()
    if not files:
        print("ERROR: no test files found in", HERE)
        return 1

    total_passed = 0
    total_failed = 0
    per_file = []

    started = time.time()
    for path in files:
        p, f = _run_one(path)
        total_passed += p
        total_failed += f
        per_file.append((os.path.basename(path), p, f))
    elapsed = time.time() - started

    print()
    header = "=" * 78
    print(header)
    print("Per-file summary:")
    print(header)
    name_w = max(len(name) for name, _, _ in per_file)
    for name, p, f in per_file:
        status = "OK" if f == 0 else "FAIL"
        print(f"  {name:<{name_w}}  {p:>3} passed  {f:>3} failed   [{status}]")

    total = total_passed + total_failed
    print()
    print(header)
    print(f"Results: {total_passed}/{total} passed, {total_failed} failed"
          f"  ({elapsed:.1f}s)")
    print(header)

    return 0 if total_failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
