#!/usr/bin/env python3
# Copyright (c) Dmitry Sapozhnikov
# Licensed under the MIT license.

"""
Regression test: type a long ASCII string into a vtm-tile session and
verify every character is visible on screen.

Two modes are exercised:
  1. `vtm-tile` (default startup, whatever app it spawns).
  2. `vtm-tile -r term` (explicit terminal applet).

Goal: catch regressions where keystrokes are dropped, mangled, or
shifted off-screen by the renderer / input pipeline.

Test string: "1234567890abcdefghijklmnopqrstuvwxyz" (36 ASCII chars,
all single-cell glyphs, no shell metachars, no escape sequences).

Strategy:
  * pty-spawn vtm-tile with the requested args.
  * Wait for the shell prompt to settle.
  * Start a fresh bash session by sending "bash\\r", then wait for the
    bash prompt so subsequent input is received by a known shell.
  * Type the test string slowly (one byte at a time) so the terminal
    has time to echo each char.
  * Snapshot the screen, strip ANSI, check the full string appears as
    a contiguous run on some line.
"""

import os
import re
import sys
import pty
import time
import select
import signal
import struct
import fcntl
import termios
import subprocess

VTM_TILE_BINARY = os.environ.get(
    "VTM_TILE_BINARY",
    os.path.join(os.path.dirname(__file__), "..", "build", "vtm-tile"),
)

COLS = 120
ROWS = 30
SETTLE_DELAY = 1.5
TEST_STRING = "1234567890abcdefghijklmnopqrstuvwxyz"

_ANSI_CSI = re.compile(rb"\x1b\[[\x30-\x3f]*[\x20-\x2f]*[\x40-\x7e]")
_ANSI_OSC = re.compile(rb"\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)")
_ANSI_DCS = re.compile(rb"\x1bP[^\x1b]*\x1b\\")
_ANSI_OTHER = re.compile(rb"\x1b[()][\x30-\x7e]|\x1b[=>78NOMcDEHM]")


def strip_ansi(buf):
    out = _ANSI_DCS.sub(b"", buf)
    out = _ANSI_OSC.sub(b"", out)
    out = _ANSI_CSI.sub(b"", out)
    out = _ANSI_OTHER.sub(b"", out)
    return out


def kill_all_vtm():
    subprocess.run(["pkill", "-9", "-x", "vtm-tile"], capture_output=True)
    time.sleep(0.3)


def set_winsize(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def read_all(fd, timeout):
    data = b""
    deadline = time.time() + timeout
    while True:
        remaining = deadline - time.time()
        if remaining <= 0:
            break
        ready, _, _ = select.select([fd], [], [], min(remaining, 0.1))
        if ready:
            try:
                chunk = os.read(fd, 65536)
                if not chunk:
                    break
                data += chunk
            except OSError:
                break
        elif data:
            break
    return data


def reconstruct_screen(raw_buf, rows=ROWS, cols=COLS):
    """Replay CUP positioning + visible glyphs onto a virtual grid.

    Coarse but sufficient for ASCII regression scanning: we only honor
    CSI ?H (CUP), CSI ?H aliases CSI ?f, plus printable bytes. Everything
    else is skipped.
    """
    grid = [[" "] * cols for _ in range(rows)]
    cur_row = 1
    cur_col = 1
    i = 0
    n = len(raw_buf)
    while i < n:
        b = raw_buf[i]
        if b == 0x1b and i + 1 < n and raw_buf[i + 1] == ord('['):
            j = i + 2
            while j < n and not (0x40 <= raw_buf[j] <= 0x7e):
                j += 1
            if j >= n:
                break
            params = raw_buf[i + 2:j]
            final = raw_buf[j]
            if final in (ord('H'), ord('f')):
                m = re.match(rb"^(\d*);?(\d*)$", params)
                if m:
                    cur_row = int(m.group(1) or b'1')
                    cur_col = int(m.group(2) or b'1')
            elif final == ord('C'):  # cursor forward
                m = re.match(rb"^(\d*)$", params)
                if m:
                    cur_col += int(m.group(1) or b'1')
            i = j + 1
            continue
        if b == 0x1b:
            i += 2
            continue
        if b == 0x0d:  # CR
            cur_col = 1
            i += 1
            continue
        if b == 0x0a:  # LF
            cur_row += 1
            i += 1
            continue
        if b == 0x08:  # BS
            cur_col = max(1, cur_col - 1)
            i += 1
            continue
        if b < 0x20:
            i += 1
            continue
        # Visible byte. Handle UTF-8 multibyte: take the leading byte's
        # cell and advance past continuation bytes (we just store the
        # leading byte char which is fine for our ASCII scan).
        if 1 <= cur_row <= rows and 1 <= cur_col <= cols:
            try:
                ch = chr(b)
            except ValueError:
                ch = "?"
            grid[cur_row - 1][cur_col - 1] = ch
        # Skip UTF-8 continuation bytes.
        if b >= 0xc0:
            k = i + 1
            while k < n and 0x80 <= raw_buf[k] < 0xc0:
                k += 1
            i = k
        else:
            i += 1
        cur_col += 1
    return ["".join(row).rstrip() for row in grid]


def _run_test_impl(extra_args, label):
    print(f"TEST [{label}]: type '{TEST_STRING}' ... ", end="", flush=True)
    master_fd, slave_fd = pty.openpty()
    set_winsize(master_fd, ROWS, COLS)
    pid = os.fork()
    if pid == 0:
        os.close(master_fd)
        os.setsid()
        fcntl.ioctl(slave_fd, termios.TIOCSCTTY, 0)
        os.dup2(slave_fd, 0)
        os.dup2(slave_fd, 1)
        os.dup2(slave_fd, 2)
        if slave_fd > 2:
            os.close(slave_fd)
        os.execvp(VTM_TILE_BINARY, [VTM_TILE_BINARY] + extra_args)
        sys.exit(1)
    os.close(slave_fd)

    raw_buf = b""
    try:
        time.sleep(SETTLE_DELAY)
        raw_buf += read_all(master_fd, timeout=1.0)
        # Drain to ensure the outer shell prompt has appeared.
        for _ in range(3):
            chunk = read_all(master_fd, timeout=0.4)
            if not chunk:
                break
            raw_buf += chunk

        # Start a fresh bash session so the test string is typed into a
        # known shell regardless of vtm-tile's default startup app.
        os.write(master_fd, b"bash\r")
        time.sleep(0.8)
        raw_buf += read_all(master_fd, timeout=0.8)

        # Type one byte at a time so the renderer paints each cell.
        for ch in TEST_STRING:
            os.write(master_fd, ch.encode())
            time.sleep(0.03)
            raw_buf += read_all(master_fd, timeout=0.15)

        # Final settle.
        time.sleep(0.5)
        raw_buf += read_all(master_fd, timeout=0.8)

        screen = reconstruct_screen(raw_buf)
        # Search every line for the contiguous test string.
        match_line = None
        for idx, line in enumerate(screen):
            if TEST_STRING in line:
                match_line = (idx + 1, line)
                break

        if match_line is None:
            # Build a diagnostic: which characters of TEST_STRING appear
            # at all somewhere on the screen, in order, and which are
            # missing. This helps distinguish "all chars dropped" from
            # "chars present but split across lines".
            joined = "\n".join(screen)
            present = "".join(ch for ch in TEST_STRING if ch in joined)
            missing = "".join(ch for ch in TEST_STRING if ch not in joined)
            print("FAIL")
            print(f"  expected contiguous: {TEST_STRING!r}")
            print(f"  present chars:       {present!r}")
            print(f"  missing chars:       {missing!r}")
            print("  screen lines containing test chars:")
            for idx, line in enumerate(screen):
                if any(c in line for c in TEST_STRING):
                    print(f"    row {idx+1:2d}: {line!r}")
            return False
        print(f"PASS (row {match_line[0]})")
        return True
    finally:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            os.waitpid(pid, 0)
        except ChildProcessError:
            pass
        try:
            os.close(master_fd)
        except OSError:
            pass
        kill_all_vtm()


def test_shell_input_display_default():
    """Verify ASCII keystrokes echo correctly under default vtm-tile startup."""
    return _run_test_impl([], "vtm-tile")


def test_shell_input_display_dash_r_term():
    """Verify ASCII keystrokes echo correctly under `vtm-tile -r term`."""
    return _run_test_impl(["-r", "term"], "vtm-tile -r term")


TESTS = [
    test_shell_input_display_default,
    test_shell_input_display_dash_r_term,
]


def main():
    if not os.path.isfile(VTM_TILE_BINARY):
        print(f"ERROR: vtm-tile binary not found at {VTM_TILE_BINARY}")
        return 1

    kill_all_vtm()

    passed = 0
    failed = 0

    for test in TESTS:
        try:
            result = test()
            if result:
                passed += 1
            else:
                failed += 1
        except Exception as e:
            print(f"ERROR: {test.__name__}: {e}")
            import traceback
            traceback.print_exc()
            failed += 1
        finally:
            kill_all_vtm()

    total = passed + failed
    print(f"\n{'='*60}")
    print(f"Results: {passed}/{total} passed, {failed} failed")
    print(f"{'='*60}")

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
