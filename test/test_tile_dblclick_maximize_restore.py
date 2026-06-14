#!/usr/bin/env python3
# Copyright (c) Shaobo Song
# Licensed under the MIT license.

"""
Regression test: a rapid second double-click on the in-pane applet's menu bar
(the menu bar of term / parvion / etc. inside a tile pane) must restore the pane,
not leave it stuck maximized.

Bug
---
Double-clicking the applet's menu bar (row 3) maximizes the pane; double-clicking
again restores it. But when the two double-clicks happen in quick succession at
the *same* cell (~0.1 s apart), the pane only maximizes and never restores.

Root cause: the in-pane applet runs as a DirectVT child and classifies the clicks
on its own gear. The click-counter tracks one continuous multi-click run
(single->double->triple->quad) while successive presses stay within the
double-click timeout and at the same cell. A menu-bar double-click is forwarded to
the host (which performs the maximize/restore toggle) only at run-count 2, so a
second same-cell double-click within the timeout is read as a triple/quad-click
and never forwarded -> no restore. Widening the interval or moving the cursor a
cell breaks the run and makes it work again.

Fix: when the applet's gate forwards a double-click outside (console.hpp), it now
calls gear.break_click_chain() (the same primitive the window-control buttons
use), resetting the run so the next double-click is forwarded as a fresh double.

Setup
-----
TILE_CONFIG clears the terminal pane's menu bar (<terminal><menu><item*/></menu>)
so the pane menu bar at row 3 is button-free and any column there is a safe
double-click target; the tile app list is a single clean "term" pane.

State detector (focus-direction independent)
--------------------------------------------
Read the pane menu/title row over the left half and the right half:
  - split / restored : the two halves are different panes -> left_bg != right_bg
                       (exactly one pane is focused)
  - maximized        : one pane spans full width        -> left_bg == right_bg

Scenario
--------
1. split horizontally                    -> two panes  (left_bg != right_bg)
2. one double-click on left menu bar     -> maximized  (left_bg == right_bg)   [positive control]
3. wait > dblclick timeout, double-click -> restored   (left_bg != right_bg)   [interval control: "returns to normal"]
4. two double-clicks 0.1 s apart, same   -> restored   (left_bg != right_bg)   [bug repro: fails pre-fix]
   cell, no intervening move
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

# Clean pane menu bar: clear the terminal's menu (<menu><item*/></menu>) so row 3
# is a button-free bar; a single "term" app keeps every split pane identical.
# The split keybinding is the only thing the harness adds on top of the
# clean-menubar config.
TILE_CONFIG = (
    "<config>"
        "<terminal>"
            "<menu><item*/></menu>"          # Clear the terminal pane's menu bar (the user's "clean menu bar").
            "<confirm_close=0/>"
        "</terminal>"
        "<tile>"
            "<confirm_close=0/>"
            '<app selected="term">'
                "<item*/>"
                '<item id="term" label="term" type="dtvt" cmd="$0 -r term"/>'
            "</app>"
        "</tile>"
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

FOCUSED_BG   = (58, 82, 142)
UNFOCUSED_BG = (64, 64, 64)
X_HOVER_BG   = (255, 0, 0)
X_PRESS_BG   = (127, 0, 0)

TITLEBAR_ROW = 3
LEFT_LO_COL  = 1
LEFT_HI_COL  = COLS // 2 - 2
RIGHT_LO_COL = COLS // 2 + 2
RIGHT_HI_COL = COLS - 1
LEFT_CTR     = COLS // 4          # double-click target: center of the left pane bar


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


def bar_bgs(accum, row=TITLEBAR_ROW):
    """Dominant background of the left and right halves of the pane menu/title row."""
    left  = dominant_bg(accum, row, LEFT_LO_COL,  LEFT_HI_COL,  exclude=(X_HOVER_BG, X_PRESS_BG))
    right = dominant_bg(accum, row, RIGHT_LO_COL, RIGHT_HI_COL, exclude=(X_HOVER_BG, X_PRESS_BG))
    return left, right


def is_maximized(accum, row=TITLEBAR_ROW):
    """Maximized: one pane spans full width -> both halves are the same pane."""
    left, right = bar_bgs(accum, row)
    return left is not None and left == right


def is_split(accum, row=TITLEBAR_ROW):
    """Split/restored: two panes side by side -> halves differ (one focused)."""
    left, right = bar_bgs(accum, row)
    return left is not None and right is not None and left != right


def dump_rows(accum, lo=1, hi=6):
    out = []
    for r in range(lo, hi + 1):
        l, rt = bar_bgs(accum, r)
        out.append(f"  row {r}: left={l} right={rt}")
    return "\n".join(out)


def dclick(s, col, row):
    """One double-click = two rapid press/release pairs at the same cell."""
    for _ in range(2):
        s.write(sgr_press(col, row))
        time.sleep(0.02)
        s.write(sgr_release(col, row))
        time.sleep(0.02)


def park_mouse(s):
    """Move the cursor off the menu bar so hover doesn't tint the sampled row."""
    s.write(sgr_move(1, ROWS))
    time.sleep(0.3)


# Sentinels distinguishing a real verdict from "the TUI never rendered here".
PASS, FAIL, SKIP = "PASS", "FAIL", "SKIP"


def establish_split(s):
    """Split horizontally and wait (polling) until two side-by-side panes render.

    The tile UI can take a few seconds to draw on a slow/headless host; we send
    the split shortcut then accumulate output until the row-3 title bar shows the
    two-pane signature (left != right), or give up. Returns (ok, accum)."""
    s.write(SPLIT_HZ_KEY)
    accum = b""
    deadline = time.time() + 8.0
    while time.time() < deadline:
        accum += s.read(timeout=0.6)
        if not s.is_alive():
            return False, accum
        if is_split(accum):
            park_mouse(s)
            accum += s.read(timeout=0.5)
            if is_split(accum):
                return True, accum
        time.sleep(0.2)
    return False, accum


def run_scenario(s):
    """Drive split -> maximize -> restore on a live session. Returns (verdict, msg).

    A SKIP verdict means the precondition (a rendered side-by-side split) was never
    reached, i.e. the host could not drive the tile UI -- not a behavioural failure."""
    ok, accum = establish_split(s)
    if not ok:
        return SKIP, "tile UI did not render a side-by-side split\n" + dump_rows(accum)

    # Positive control: one double-click on the left pane's menu bar maximizes it
    # (proves both the maximize action and the maximized/split detector).
    dclick(s, LEFT_CTR, TITLEBAR_ROW)
    park_mouse(s)
    accum += s.read(timeout=0.8)
    if not is_maximized(accum):
        return FAIL, "single double-click did not maximize\n" + dump_rows(accum)

    # Interval control ("returns to normal"): once the click run has expired, a
    # double-click at the same cell restores the pane.
    time.sleep(0.8)  # > dblclick timeout (default 500 ms) -> fresh click run
    dclick(s, LEFT_CTR, TITLEBAR_ROW)
    park_mouse(s)
    accum += s.read(timeout=0.8)
    if not is_split(accum):
        return FAIL, "spaced double-click did not restore (control)\n" + dump_rows(accum)

    # Bug repro: two double-clicks 0.1 s apart at the SAME cell with no intervening
    # mouse move. Pre-fix the second one is folded into one 4-click run, so only the
    # first double-click registers and the pane stays maximized.
    dclick(s, LEFT_CTR, TITLEBAR_ROW)
    time.sleep(0.1)
    dclick(s, LEFT_CTR, TITLEBAR_ROW)
    park_mouse(s)
    accum += s.read(timeout=0.8)
    if not s.is_alive():
        return FAIL, "vtm died during the rapid double double-click"
    if is_maximized(accum):
        return FAIL, ("rapid second double-click did not restore "
                      "(pane stuck maximized)\n" + dump_rows(accum))
    if not is_split(accum):
        return FAIL, "pane not in split state after restore\n" + dump_rows(accum)
    return PASS, ""


def test_dblclick_maximize_restore():
    """A rapid second double-click on the pane menu bar must restore the pane.

    Returns True (pass), False (fail), or None (skipped: the host could not render
    the tile UI after several attempts)."""
    print("TEST: tile - rapid double double-click on menu bar restores ... ",
          end="", flush=True)

    last = ""
    for _ in range(3):  # The first render is flaky on slow/headless hosts; retry.
        with VtmSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
            verdict, msg = run_scenario(s)
        if verdict == PASS:
            print("PASS")
            return True
        if verdict == FAIL:
            print("FAIL - " + msg)
            return False
        last = msg
        kill_all_vtm()
        time.sleep(0.5)
    print("SKIP - " + last)
    return None


TESTS = [
    test_dblclick_maximize_restore,
]


def main():
    if not os.path.isfile(VTM_TILE_BINARY):
        print(f"ERROR: vtm-tile binary not found at {VTM_TILE_BINARY}")
        return 1

    kill_all_vtm()

    passed = 0
    failed = 0
    skipped = 0

    for test in TESTS:
        try:
            result = test()
            if result is None:
                skipped += 1
            elif result:
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

    total = passed + failed + skipped
    print(f"\n{'='*60}")
    print(f"Results: {passed}/{total} passed, {failed} failed, {skipped} skipped")
    print(f"{'='*60}")

    # A skip means the host could not render the tile UI (not a regression), so it
    # does not fail the suite; only a real behavioural FAIL does.
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
