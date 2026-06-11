#!/usr/bin/env python3
# Copyright (c) Shaobo Song
# Licensed under the MIT license.

"""
Regression test: ctrl+LeftDown on the tile gate menubar must defocus the
whole gate, and a second ctrl+LeftDown must refocus it while restoring
the prior inner-pane focus state.

Setup
-----
TILE_CONFIG clears default menubar items (<menu><item*/></menu>) so that
row 1 is an empty gate menubar (cover '▀' band) with no button targets;
ctrl+press on row 1 thus addresses the gate itself.

Scenario
--------
1. split horizontally → right pane solo-focused (FOCUSED_BG),
                       left  pane unfocused      (UNFOCUSED_BG)
2. ctrl+press menubar → gate defocused → both panes UNFOCUSED_BG
3. ctrl+press menubar → gate refocused → prior state restored
                                          (left=UNFOCUSED, right=FOCUSED)
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


VTM_TILE_BINARY = os.environ.get(
    "VTM_TILE_BINARY",
    os.path.join(os.path.dirname(__file__), "..", "build", "vtm-tile"),
)

COLS = 80
ROWS = 24
TILE_SETTLE_DELAY = 1.2

# Empty menubar (<item*/>) makes row 1 a button-free gate menubar.
TILE_CONFIG = (
    "<config>"
        "<tile>"
            "<confirm_close=0/>"
            "<menu><item*/></menu>"
        "</tile>"
        "<terminal><confirm_close=0/></terminal>"
        "<events><tile>"
            """<script=TileSplitHorizontally on="Alt+Shift+'|'"/>"""
        "</tile></events>"
    "</config>"
    "<Scripting>"
        '<TileSplitHorizontally="vtm.tile.SplitPane(0);"/>'
    "</Scripting>"
)
TILE_ARGS    = []  # TILE_CONFIG is shipped via $VTM_CONFIG (vtm_config kwarg).
SPLIT_HZ_KEY = b"\x1b|"

MOUSE_CTRL   = 0x10
FOCUSED_BG   = (58, 82, 142)
UNFOCUSED_BG = (64, 64, 64)
X_HOVER_BG   = (255, 0, 0)
X_PRESS_BG   = (127, 0, 0)

TITLEBAR_ROW = 3
LEFT_HI_COL  = COLS // 2 - 2
RIGHT_LO_COL = COLS // 2 + 2
LEFT_CTR     = COLS // 4
RIGHT_CTR    = 3 * COLS // 4
CONTENT_ROW  = ROWS // 2
MENUBAR_ROW  = 1
MENUBAR_COL  = COLS // 2     # empty menubar → any column is safe


def kill_all_vtm():
    for name in ("vtm-desk", "vtm-tile"):
        subprocess.run(["pkill", "-9", "-x", name], capture_output=True)
    deadline = time.time() + 3.0
    while time.time() < deadline:
        time.sleep(0.1)
        if all(subprocess.run(["pgrep", "-x", n], capture_output=True).returncode != 0
               for n in ("vtm-desk", "vtm-tile")):
            return


def set_winsize(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def read_until_quiet(fd, timeout=5.0, quiet=0.3):
    data = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        remaining = min(deadline - time.time(), quiet)
        ready, _, _ = select.select([fd], [], [], remaining)
        if ready:
            try:
                chunk = os.read(fd, 65536)
                if not chunk:
                    break
                data += chunk
                deadline = max(deadline, time.time() + quiet)
            except OSError:
                break
        elif data:
            break
    return data


def sgr_press(col, row, button=0):
    return f"\033[<{button};{col};{row}M".encode()


def sgr_release(col, row, button=0):
    return f"\033[<{button};{col};{row}m".encode()


def sgr_move(col, row):
    return f"\033[<35;{col};{row}M".encode()


class VtmSession:
    def __init__(self, args, settle=TILE_SETTLE_DELAY, vtm_config=None):
        self.args   = args
        self.settle = settle
        self.vtm_config = vtm_config
        self.master_fd = self.pid = None

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
            if self.vtm_config is not None:
                os.environ["VTM_CONFIG"] = self.vtm_config
            os.execvp(VTM_TILE_BINARY, [VTM_TILE_BINARY] + self.args)
            sys.exit(1)
        os.close(slave_fd)
        time.sleep(self.settle)
        read_until_quiet(self.master_fd, timeout=1.5, quiet=0.4)
        return self

    def __exit__(self, *exc):
        if self.pid:
            try:    os.kill(self.pid, signal.SIGKILL)
            except ProcessLookupError: pass
            try:    os.waitpid(self.pid, 0)
            except ChildProcessError:  pass
            self.pid = None
        if self.master_fd is not None:
            try:    os.close(self.master_fd)
            except OSError: pass
            self.master_fd = None
        kill_all_vtm()

    def write(self, data):
        if isinstance(data, str):
            data = data.encode()
        os.write(self.master_fd, data)

    def read(self, timeout=5.0):
        return read_until_quiet(self.master_fd, timeout=timeout, quiet=0.3)

    def is_alive(self):
        if self.pid is None:
            return False
        try:
            pid, _ = os.waitpid(self.pid, os.WNOHANG)
            if pid == 0:
                return True
            self.pid = None
            return False
        except ChildProcessError:
            self.pid = None
            return False


def parse_sgr_grid(data):
    bg_grid = {}
    cur_bg  = None
    row, col = 1, 1
    i = 0
    while i < len(data):
        c = data[i]
        if c == 0x1b and i + 1 < len(data) and data[i + 1] == ord('['):
            j = i + 2
            while j < len(data) and not (0x40 <= data[j] <= 0x7e):
                j += 1
            if j >= len(data):
                break
            seq   = data[i + 2:j].decode('ascii', errors='replace')
            final = chr(data[j])
            if final == 'm':
                parts = seq.split(';') if seq else ['0']
                k = 0
                while k < len(parts):
                    p = parts[k]
                    try:    n = int(p) if p else 0
                    except ValueError: n = 0
                    if n == 0:
                        cur_bg = None
                    elif n == 48 and k + 4 < len(parts) and parts[k + 1] == '2':
                        cur_bg = (int(parts[k + 2]), int(parts[k + 3]), int(parts[k + 4]))
                        k += 4
                    k += 1
            elif final == 'H':
                p   = seq.split(';')
                row = int(p[0]) if p[0] else 1
                col = int(p[1]) if len(p) > 1 and p[1] else 1
            i = j + 1
            continue
        if c == ord('\n'):
            row += 1; col = 1
        elif c == ord('\r'):
            col = 1
        elif c >= 0x20:
            bg_grid[(row, col)] = cur_bg
            col += 1
        i += 1
    return bg_grid


def dominant_bg(data, row, col_lo, col_hi, exclude=()):
    bg_grid = parse_sgr_grid(data)
    counts  = {}
    for col in range(col_lo, col_hi + 1):
        bg = bg_grid.get((row, col))
        if bg is None or bg in exclude:
            continue
        counts[bg] = counts.get(bg, 0) + 1
    return max(counts, key=counts.get) if counts else None


def ctrl_click(s, col, row):
    s.write(sgr_press(col, row, MOUSE_CTRL))
    time.sleep(0.05)
    s.write(sgr_release(col, row, MOUSE_CTRL))
    time.sleep(0.5)
    # Move mouse off menubar so hover doesn't repaint it.
    s.write(sgr_move(1, ROWS))
    time.sleep(0.3)


def panel_bgs(accum):
    left_bg  = dominant_bg(accum, TITLEBAR_ROW, 1,            LEFT_HI_COL,
                            exclude=(X_HOVER_BG, X_PRESS_BG))
    right_bg = dominant_bg(accum, TITLEBAR_ROW, RIGHT_LO_COL, COLS - 1,
                            exclude=(X_HOVER_BG, X_PRESS_BG))
    return left_bg, right_bg


def test_ctrl_press_menubar_defocus_refocus():
    """ctrl+press on gate menubar defocuses gate; second ctrl+press
       refocuses gate and restores prior inner-pane focus state."""
    print("TEST: tile - ctrl+press gate menubar defocus/refocus ... ",
          end="", flush=True)

    with VtmSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        s.write(SPLIT_HZ_KEY)
        time.sleep(2.0)
        accum = s.read(timeout=1.0)
        if not s.is_alive():
            print("FAIL - vtm died during split")
            return False

        # Baseline: right solo-focused, left unfocused.
        l0, r0 = panel_bgs(accum)
        if l0 != UNFOCUSED_BG or r0 != FOCUSED_BG:
            print(f"FAIL - baseline left={l0} (want {UNFOCUSED_BG}), "
                  f"right={r0} (want {FOCUSED_BG})")
            return False

        # ctrl+press on gate menubar → defocus whole gate.
        ctrl_click(s, MENUBAR_COL, MENUBAR_ROW)
        accum += s.read(timeout=0.8)

        l1, r1 = panel_bgs(accum)
        if l1 != UNFOCUSED_BG or r1 != UNFOCUSED_BG:
            print(f"FAIL - after defocus left={l1} right={r1} "
                  f"(both want {UNFOCUSED_BG}). gate did not defocus children.")
            return False

        # ctrl+press menubar again → refocus gate, restore prior state.
        ctrl_click(s, MENUBAR_COL, MENUBAR_ROW)
        accum += s.read(timeout=0.8)

        l2, r2 = panel_bgs(accum)
        if l2 != UNFOCUSED_BG or r2 != FOCUSED_BG:
            print(f"FAIL - after refocus left={l2} (want {UNFOCUSED_BG}), "
                  f"right={r2} (want {FOCUSED_BG}). prior pane state not restored.")
            return False

        if not s.is_alive():
            print("FAIL - vtm died")
            return False

    print("PASS")
    return True


TESTS = [
    test_ctrl_press_menubar_defocus_refocus,
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
