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
  (`_dbg_grid.replay`), then assert which screen rows acquired the
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
import select
import signal
import struct
import subprocess
import sys
import termios
import time

ROOT = os.path.dirname(__file__)
sys.path.insert(0, ROOT)

from _dbg_grid import replay, ROWS, COLS, FOCUSED, INACTIVE  # type: ignore

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


def run_focus_zorder_test():
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


def main():
    if not os.path.isfile(VTM_TILE_BINARY):
        print(f"ERROR: vtm-tile binary not found at {VTM_TILE_BINARY}")
        sys.exit(2)
    kill_all_vtm()
    try:
        ok = run_focus_zorder_test()
    finally:
        kill_all_vtm()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
