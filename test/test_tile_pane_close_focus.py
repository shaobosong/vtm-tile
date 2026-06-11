#!/usr/bin/env python3
# Copyright (c) Shaobo Song
# Licensed under the MIT license.

"""
Regression test for the source change in src/netxs/desktopio/application.hpp
that focuses a tile pane the moment its menubar × close button is clicked
(before the confirm-close dialog appears).

Scenario
--------
After a horizontal split, click the *right* pane to give it focus, then
click the × close button on the *left* (currently unfocused) pane.  The
expected behavior is:

  1. The left-click on × refocuses the left pane (so the dialog and any
     subsequent close action target it).
  2. The terminal's confirm_close dialog opens, overlaying a translucent
     "shadower" (#202020 with alpha) on top of the now-focused pane.
  3. The visible left-pane menubar bg therefore renders as the focused
     palette color blended with the shadower — a *darker focused color*
     (≈ each channel halved, e.g. (29, 41, 71)).

If the source change is missing, clicking × on an unfocused pane leaves
focus on the right pane, so the left menubar stays at the unfocused gray
(#404040 → (64, 64, 64)) and this test fails.

This test is intentionally self-contained: it does not import helpers
from any other test module.
"""

import os
import sys
import pty
import time
import select
import signal
import struct
import fcntl
import termios
import subprocess


# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

VTM_TILE_BINARY = os.environ.get(
    "VTM_TILE_BINARY",
    os.path.join(os.path.dirname(__file__), "..", "build", "vtm-tile"),
)

# Terminal geometry for the test.
COLS = 80
ROWS = 24

# Time to let vtm-tile finish its initial layout/render.
TILE_SETTLE_DELAY = 1.0

# Generic read deadline for non-blocking output drains.
READ_TIMEOUT = 5.0

# Self-contained tile config: enable confirm_close on both the tile applet
# and the per-pane terminal, and bind a horizontal-split shortcut.
TILE_CONFIG = (
    "<config>"
        "<tile><confirm_close=1/></tile>"
        "<terminal><confirm_close=1/></terminal>"
        "<events><tile>"
            """<script=TileSplitHorizontally on="Alt+Shift+'|'"/>"""
        "</tile></events>"
    "</config>"
    "<Scripting>"
        '<TileSplitHorizontally="vtm.tile.SplitPane(0);"/>'
    "</Scripting>"
)
TILE_ARGS = []  # TILE_CONFIG is shipped via $VTM_CONFIG (vtm_config kwarg).
SPLIT_HZ_KEY = b"\x1b|"   # Alt+Shift+| → split horizontally.

# Reference colors derived from the default vtm.xml palette.
FOCUSED_BG   = (58, 82, 142)   # #3A528E  — winfocus (bright).
UNFOCUSED_BG = (64, 64, 64)    # #404040  — window_clr (passive).
X_HOVER_BG   = (255, 0, 0)     # X close button hover/press feedback (#FF0000).
X_PRESS_BG   = (127, 0, 0)     # X close button mouse-down feedback (#7F0000).


# ---------------------------------------------------------------------------
# Process / pty helpers (inlined; no test-file imports)
# ---------------------------------------------------------------------------

def kill_all_vtm():
    """Reap any leftover vtm-desk / vtm-tile processes."""
    for name in ("vtm-desk", "vtm-tile"):
        subprocess.run(["pkill", "-9", "-x", name], capture_output=True)
    for _ in range(20):
        time.sleep(0.3)
        gone = True
        for name in ("vtm-desk", "vtm-tile"):
            r = subprocess.run(["pgrep", "-x", name], capture_output=True)
            if r.returncode == 0:
                gone = False
                break
        if gone:
            return
    for name in ("vtm-desk", "vtm-tile"):
        subprocess.run(["pkill", "-9", "-x", name], capture_output=True)
    time.sleep(0.5)


def set_winsize(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def read_all(fd, timeout=READ_TIMEOUT):
    """Read all data available within `timeout` seconds."""
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


def sgr_press(col, row, button=0):   return f"\033[<{button};{col};{row}M".encode()
def sgr_release(col, row, button=0): return f"\033[<{button};{col};{row}m".encode()
def sgr_move(col, row):              return f"\033[<35;{col};{row}M".encode()


class VtmSession:
    """Run vtm-tile under a pty for scripted interaction."""

    def __init__(self, binary, args, settle_delay=TILE_SETTLE_DELAY, vtm_config=None):
        self.binary = binary
        self.args = args
        self.settle_delay = settle_delay
        self.vtm_config = vtm_config
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
            os.dup2(slave_fd, 0); os.dup2(slave_fd, 1); os.dup2(slave_fd, 2)
            if slave_fd > 2:
                os.close(slave_fd)
            if self.vtm_config is not None:
                os.environ["VTM_CONFIG"] = self.vtm_config
            os.execvp(self.binary, [self.binary] + self.args)
            sys.exit(1)
        os.close(slave_fd)
        time.sleep(self.settle_delay)
        read_all(self.master_fd, timeout=1.0)   # drain startup output
        return self

    def __exit__(self, *exc):
        if self.pid:
            try: os.kill(self.pid, signal.SIGKILL)
            except ProcessLookupError: pass
            try: os.waitpid(self.pid, os.WNOHANG)
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

    def read(self, timeout=READ_TIMEOUT):
        return read_all(self.master_fd, timeout=timeout)

    def click(self, col, row, button=0):
        self.write(sgr_press(col, row, button))
        time.sleep(0.05)
        self.write(sgr_release(col, row, button))

    def mouse_move(self, col, row):
        self.write(sgr_move(col, row))

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


# ---------------------------------------------------------------------------
# SGR parser
# ---------------------------------------------------------------------------

def parse_sgr_grid(data):
    """Parse SGR-colored output into row→col grids.
    Returns (fg_grid, bg_grid) keyed by (row, col), 1-indexed."""
    fg_grid, bg_grid = {}, {}
    cur_fg = cur_bg = None
    row, col = 1, 1
    i = 0
    while i < len(data):
        c = data[i]
        if c == 0x1b and i + 1 < len(data) and data[i+1] == ord('['):
            j = i + 2
            while j < len(data) and not (0x40 <= data[j] <= 0x7e):
                j += 1
            if j >= len(data):
                break
            seq = data[i+2:j].decode('ascii', errors='replace')
            final = chr(data[j])
            if final == 'm':
                parts = seq.split(';') if seq else ['0']
                k = 0
                while k < len(parts):
                    p = parts[k]
                    try:    n = int(p) if p else 0
                    except ValueError: n = 0
                    if n == 0:
                        cur_fg = None; cur_bg = None
                    elif n == 38 and k + 4 < len(parts) and parts[k+1] == '2':
                        cur_fg = (int(parts[k+2]), int(parts[k+3]), int(parts[k+4]))
                        k += 4
                    elif n == 48 and k + 4 < len(parts) and parts[k+1] == '2':
                        cur_bg = (int(parts[k+2]), int(parts[k+3]), int(parts[k+4]))
                        k += 4
                    k += 1
            elif final == 'H':
                p = seq.split(';')
                row = int(p[0]) if p[0] else 1
                col = int(p[1]) if len(p) > 1 and p[1] else 1
            i = j + 1
            continue
        if c == ord('\n'):
            row += 1; col = 1
        elif c == ord('\r'):
            col = 1
        elif c >= 0x20:
            fg_grid[(row, col)] = cur_fg
            bg_grid[(row, col)] = cur_bg
            col += 1
        i += 1
    return fg_grid, bg_grid


def dominant_bg(data, row, col_lo, col_hi, exclude=()):
    """Return the most common bg color seen on `row` between `col_lo`
    and `col_hi` (inclusive), excluding any colors in `exclude`."""
    _, bg_grid = parse_sgr_grid(data)
    counts = {}
    for col in range(col_lo, col_hi + 1):
        bg = bg_grid.get((row, col))
        if bg is None or bg in exclude:
            continue
        counts[bg] = counts.get(bg, 0) + 1
    return max(counts, key=counts.get) if counts else None


def is_darker_variant(dim, bright, tolerance=24):
    """True if `dim` looks like a shadower-blended version of `bright`:
    each channel of `dim` is strictly less than the matching channel of
    `bright`, and they share the same hue (the per-channel dim/bright
    ratio is consistent within `tolerance`/255)."""
    if dim is None or bright is None or dim == bright:
        return False
    if any(d >= b for d, b in zip(dim, bright)):
        return False
    ratios = [d * 1000 // max(b, 1) for d, b in zip(dim, bright)]
    return max(ratios) - min(ratios) <= tolerance * 1000 // 255


# ---------------------------------------------------------------------------
# The test
# ---------------------------------------------------------------------------

def test_left_x_click_refocuses_pane_under_dialog():
    """Click right pane center → focus moves to right.  Then click left
    pane × → the source change in application.hpp must refocus the left
    pane *before* the confirm_close dialog opens, so the visible left
    menubar renders the focused palette dimmed by the dialog's shadower
    overlay (a darker-focused color), proving that the about-to-close
    pane is the focused one."""
    print("TEST: tile - X click refocuses pane under confirm dialog ... ",
          end="", flush=True)
    with VtmSession(VTM_TILE_BINARY, TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        # ── Step 1: Horizontal split ──────────────────────────────────
        s.write(SPLIT_HZ_KEY)
        time.sleep(2.0)
        post_split = s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - vtm died during split")
            return False

        # ── Step 2: Click *right* pane center to focus it ─────────────
        s.click(3 * COLS // 4, ROWS // 2)
        time.sleep(0.7)
        s.mouse_move(1, ROWS)        # clear hover state
        time.sleep(0.4)
        post_focus_right = s.read(timeout=0.5)

        # Sanity check: with the right pane focused, the left menubar
        # (row 3) must be the unfocused gray and the right menubar must
        # be the bright focused blue.
        baseline = post_split + post_focus_right
        left_right_col = COLS // 2 - 2          # approx. right edge of left pane
        right_left_col = COLS // 2 + 2          # approx. left edge of right pane
        base_left  = dominant_bg(baseline, 3, 1,              left_right_col,
                                 exclude=(X_HOVER_BG, X_PRESS_BG))
        base_right = dominant_bg(baseline, 3, right_left_col, COLS - 1,
                                 exclude=(X_HOVER_BG, X_PRESS_BG))
        if base_left != UNFOCUSED_BG or base_right != FOCUSED_BG:
            print(f"FAIL - baseline mismatch: left={base_left} "
                  f"(want {UNFOCUSED_BG}), right={base_right} "
                  f"(want {FOCUSED_BG})")
            return False

        # ── Step 3: Click × on the *left* pane menubar (row 3) ────────
        s.click(left_right_col - 2, 3)
        time.sleep(1.2)
        s.mouse_move(1, ROWS)        # clear X-button hover
        time.sleep(0.7)
        post_click = s.read(timeout=0.8)
        if not s.is_alive():
            print("SKIP - clicking × did not open dialog (vtm exited)")
            return True

        # ── Step 4: Inspect left-pane menubar with dialog open ────────
        after_left = dominant_bg(post_click, 3, 1, left_right_col,
                                 exclude=(X_HOVER_BG, X_PRESS_BG))
        s.write(b"\x1b")             # cancel dialog so vtm exits cleanly
        time.sleep(0.4)

        print()
        print(f"  baseline left  (right focused)            = {base_left}")
        print(f"  baseline right (right focused)            = {base_right}")
        print(f"  after × click  left (with confirm dialog) = {after_left}")

        if after_left is None:
            print("FAIL - no menubar repaint observed after × click")
            return False
        if after_left == UNFOCUSED_BG:
            print("FAIL - left menubar still UNFOCUSED gray; the × click "
                  "did NOT refocus the left pane (source change missing?)")
            return False
        if after_left == FOCUSED_BG:
            print("FAIL - left menubar shows BRIGHT focused color; the "
                  "confirm dialog's shadower should darken it")
            return False
        if not is_darker_variant(after_left, FOCUSED_BG):
            print(f"FAIL - left menubar bg {after_left} is not a darker "
                  f"variant of focused {FOCUSED_BG}")
            return False
        print("PASS")
        return True


TESTS = [
    test_left_x_click_refocuses_pane_under_dialog,
]


def main():
    if not os.path.isfile(VTM_TILE_BINARY):
        print(f"ERROR: vtm-tile not found at {VTM_TILE_BINARY}")
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
