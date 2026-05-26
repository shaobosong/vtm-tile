#!/usr/bin/env python3
# Copyright (c) Dmitry Sapozhnikov
# Licensed under the MIT license.

"""
Regression test for vtm-tile Ctrl+LeftClick focus-bubbling bug.

Bug:
  Ctrl+LeftClick inside a focused terminal pane was incorrectly causing
  the tile applet's menubar (top row) to repaint with the inactive
  palette. The pro::focus unfocus riseup originating at the pane was
  bubbling up through the tile applet's `object` fork (mode::hub)
  unimpeded, decrementing object's focus count to 0 and flipping the
  menubar's window_clr from `tone::winfocus` to `tone::window_clr`.

Fix:
  src/netxs/apps/tile.hpp: change the tile `object` fork's pro::focus
  plugin from default (mode::hub) to mode::focusable, so the unfocus
  riseup halts at object (controls.hpp:2479-2491 last_step path) and
  notify_focus_state is NOT called on object. The menubar therefore
  stays painted with the focused palette while the tile applet remains
  hosted by the gate, even when an inner pane is unfocused via
  Ctrl+LeftClick.

Probe (position-aware):
  Replay the captured ANSI stream through a per-cell bg-color grid
  (`replay`, inlined below), then assert which screen rows acquired the
  inactive bg color in the post-click delta:
    - row 0 (menubar): MUST NOT acquire inactive bg.
    - row 2 (per-pane title bar): allowed to acquire inactive bg
      (the clicked pane legitimately lost focus, so its tabbar dims).

The previous version of this test used a coarse byte-search for the
inactive SGR fragment anywhere in the stream, which produced false
positives whenever ANY cell repainted with the inactive palette.
"""

import fcntl
import os
import pty
import re
import select
import signal
import struct
import subprocess
import sys
import termios
import time

ROWS = 30
COLS = 120

# Palette tuples read off the wire from a real vtm-tile session
# (see vtm.xml /config/colors/window and /config/colors/focus).
FOCUSED = (58, 82, 142)    # #3A528E, focused-window background
INACTIVE = (64, 64, 64)    # #404040, unfocused-window background

_CSI_RE = re.compile(rb"\x1b\[([\x30-\x3f]*)([\x20-\x2f]*)([\x40-\x7e])")


def _parse_sgr(params, cur_bg):
    parts = [p.decode() if p else "0" for p in (params.split(b";") if params else [b"0"])]
    i = 0
    while i < len(parts):
        try:
            n = int(parts[i] or "0")
        except ValueError:
            n = 0
        if n == 0:
            cur_bg = None
        elif n == 49:
            cur_bg = None
        elif n == 48 and i + 1 < len(parts):
            mode = parts[i + 1]
            if mode == "2" and i + 4 < len(parts):
                try:
                    r = int(parts[i + 2])
                    g = int(parts[i + 3])
                    b = int(parts[i + 4])
                    cur_bg = (r, g, b)
                except ValueError:
                    pass
                i += 4
            elif mode == "5" and i + 2 < len(parts):
                # 256-color background; ignore (vtm-tile uses true-color on this path).
                i += 2
        i += 1
    return cur_bg


def replay(buf):
    """Replay *buf* and return (grid, touched).

    grid[r][c]    : last bg color (R,G,B) painted at cell (r, c), or None
    touched[r][c] : True iff a visible glyph was placed at (r, c)
    """
    grid = [[None] * COLS for _ in range(ROWS)]
    touched = [[False] * COLS for _ in range(ROWS)]
    cur_row = 0
    cur_col = 0
    cur_bg = None

    i = 0
    n = len(buf)
    while i < n:
        b = buf[i]
        if b == 0x1b and i + 1 < n:
            c1 = buf[i + 1]
            if c1 == 0x5b:  # '[' -> CSI
                m = _CSI_RE.match(buf, i)
                if not m:
                    i += 1
                    continue
                params = m.group(1)
                final = m.group(3)
                i = m.end()
                if final == b"H" or final == b"f":
                    parts = params.split(b";") if params else [b"", b""]
                    if len(parts) < 2:
                        parts = parts + [b""]
                    try:
                        r = int(parts[0]) if parts[0] else 1
                        c = int(parts[1]) if parts[1] else 1
                    except ValueError:
                        r, c = 1, 1
                    cur_row = max(0, min(ROWS - 1, r - 1))
                    cur_col = max(0, min(COLS - 1, c - 1))
                elif final == b"A":
                    try: k = int(params) if params else 1
                    except ValueError: k = 1
                    cur_row = max(0, cur_row - k)
                elif final == b"B":
                    try: k = int(params) if params else 1
                    except ValueError: k = 1
                    cur_row = min(ROWS - 1, cur_row + k)
                elif final == b"C":
                    try: k = int(params) if params else 1
                    except ValueError: k = 1
                    cur_col = min(COLS - 1, cur_col + k)
                elif final == b"D":
                    try: k = int(params) if params else 1
                    except ValueError: k = 1
                    cur_col = max(0, cur_col - k)
                elif final == b"m":
                    cur_bg = _parse_sgr(params, cur_bg)
                continue
            elif c1 == 0x5d:  # ']' -> OSC, terminated by BEL or ESC '\\'
                j = i + 2
                while j < n:
                    if buf[j] == 0x07:
                        j += 1
                        break
                    if buf[j] == 0x1b and j + 1 < n and buf[j + 1] == 0x5c:
                        j += 2
                        break
                    j += 1
                i = j
                continue
            elif c1 in (0x50, 0x5f, 0x5e):  # 'P' DCS, '_' APC, '^' PM
                j = i + 2
                while j < n - 1:
                    if buf[j] == 0x1b and buf[j + 1] == 0x5c:
                        j += 2
                        break
                    j += 1
                else:
                    j = n
                i = j
                continue
            else:
                i += 2
                continue
        if b == 0x0d:
            cur_col = 0
            i += 1
            continue
        if b == 0x0a:
            cur_row = min(ROWS - 1, cur_row + 1)
            i += 1
            continue
        if b == 0x08:
            cur_col = max(0, cur_col - 1)
            i += 1
            continue
        if b < 0x20 or b == 0x7f:
            i += 1
            continue
        if 0 <= cur_row < ROWS and 0 <= cur_col < COLS:
            grid[cur_row][cur_col] = cur_bg
            touched[cur_row][cur_col] = True
        if b >= 0xc0:
            j = i + 1
            while j < n and 0x80 <= buf[j] < 0xc0:
                j += 1
            i = j
        else:
            i += 1
        if cur_col < COLS - 1:
            cur_col += 1
    return grid, touched


VTM_TILE_BINARY = os.environ.get(
    "VTM_TILE_BINARY",
    os.path.join(os.path.dirname(__file__), "..", "build", "vtm-tile"),
)

SETTLE = 1.2


def kill_all_vtm():
    subprocess.run(["pkill", "-9", "-x", "vtm-tile"], capture_output=True)
    deadline = time.time() + 2.0
    while time.time() < deadline:
        if subprocess.run(["pgrep", "-x", "vtm-tile"],
                          capture_output=True).returncode != 0:
            break
        time.sleep(0.1)


def set_winsize(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def drain(fd, timeout=0.5):
    """Read everything available on fd until quiet for `timeout` seconds."""
    out = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.05)
        if not ready:
            continue
        try:
            chunk = os.read(fd, 65536)
        except OSError:
            break
        if not chunk:
            break
        out += chunk
        deadline = max(deadline, time.time() + 0.15)
    return out


def sgr_press(col, row, button=0):
    return f"\x1b[<{button};{col};{row}M".encode()


def sgr_release(col, row, button=0):
    return f"\x1b[<{button};{col};{row}m".encode()


class VtmTile:
    def __init__(self, args=None):
        self.args = args or []
        self.master_fd = None
        self.pid = None

    def __enter__(self):
        self.master_fd, slave_fd = pty.openpty()
        set_winsize(self.master_fd, ROWS, COLS)
        self.pid = os.fork()
        if self.pid == 0:
            os.close(self.master_fd)
            os.setsid()
            fcntl.ioctl(slave_fd, termios.TIOCSCTTY, 0)
            os.dup2(slave_fd, 0)
            os.dup2(slave_fd, 1)
            os.dup2(slave_fd, 2)
            if slave_fd > 2:
                os.close(slave_fd)
            os.execvp(VTM_TILE_BINARY, [VTM_TILE_BINARY] + self.args)
            os._exit(127)
        os.close(slave_fd)
        return self

    def __exit__(self, *exc):
        if self.pid:
            try: os.kill(self.pid, signal.SIGKILL)
            except ProcessLookupError: pass
            try: os.waitpid(self.pid, 0)
            except ChildProcessError: pass
            self.pid = None
        if self.master_fd is not None:
            try: os.close(self.master_fd)
            except OSError: pass
            self.master_fd = None
        kill_all_vtm()

    def write(self, data):
        if isinstance(data, str):
            data = data.encode()
        os.write(self.master_fd, data)

    def click(self, col, row, *, ctrl=False):
        # SGR mouse encoding: bit 16 of the button code marks Ctrl modifier.
        btn = 0 | (16 if ctrl else 0)
        self.write(sgr_press(col, row, btn))
        time.sleep(0.05)
        self.write(sgr_release(col, row, btn))


def rows_painted(grid, touched, target_rgb):
    """Return the set of row indices that have >=1 cell painted with target_rgb."""
    rows = set()
    for r in range(ROWS):
        for c in range(COLS):
            if touched[r][c] and grid[r][c] == target_rgb:
                rows.add(r)
    return rows


def test_ctrl_leftclick_keeps_menubar_focused():
    print("TEST: Ctrl+LeftClick on terminal pane must not flip menubar inactive ... ",
          end="", flush=True)

    with VtmTile() as s:
        time.sleep(SETTLE)
        boot = drain(s.master_fd, 1.5)
        boot_grid, boot_touched = replay(boot)
        boot_focused = rows_painted(boot_grid, boot_touched, FOCUSED)

        if 0 not in boot_focused:
            print(f"FAIL - boot paint did not put focused palette on row 0 "
                  f"(rows with FOCUSED bg = {sorted(boot_focused)})")
            return False

        # Plain LeftClick at terminal-area centre.
        s.click(60, 15, ctrl=False)
        plain = drain(s.master_fd, 1.0)
        plain_grid, plain_touched = replay(plain)
        plain_inactive = rows_painted(plain_grid, plain_touched, INACTIVE)
        if 0 in plain_inactive:
            print(f"FAIL - plain LeftClick already flipped menubar inactive "
                  f"(rows with INACTIVE = {sorted(plain_inactive)}). "
                  "Test environment broken.")
            return False

        time.sleep(0.3)

        # Ctrl+LeftClick at same coordinates.
        s.click(60, 15, ctrl=True)
        ctrl = drain(s.master_fd, 1.0)
        ctrl_grid, ctrl_touched = replay(ctrl)
        ctrl_inactive = rows_painted(ctrl_grid, ctrl_touched, INACTIVE)

        if 0 in ctrl_inactive:
            print(f"FAIL - Ctrl+LeftClick repainted menubar (row 0) inactive "
                  f"(rows with INACTIVE = {sorted(ctrl_inactive)}). "
                  "The unfocus riseup is bubbling out of tile.hpp's `object` "
                  "fork; check that it is constructed with "
                  "pro::focus::mode::focusable.")
            return False

    print("PASS")
    return True


TESTS = [
    test_ctrl_leftclick_keeps_menubar_focused,
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
