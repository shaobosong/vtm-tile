#!/usr/bin/env python3
# Copyright (c) Shaobo Song
# Licensed under the MIT license.

"""
Regression test for vtm-tile left-click-on-press focus behaviour.

Two scenarios are verified:

  1. Focus on press: clicking a pane content area with LeftDown (mouse
     press, before release) must immediately shift focus to that pane.
     Sending only the press event (no release) must be sufficient.

  2. Close button on release: a LeftDown press on the × close button
     must NOT close the pane.  Only the completed LeftClick (press +
     release) must trigger the close.

Controls.hpp change: pro::focus registers its focus-on-click handler on
`input::key::LeftDown` instead of `input::key::LeftClick`.  The close
button handler in application.hpp remains on `LeftClick` (unchanged).
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


# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

VTM_TILE_BINARY = os.environ.get(
    "VTM_TILE_BINARY",
    os.path.join(os.path.dirname(__file__), "..", "build", "vtm-tile"),
)

COLS = 80
ROWS = 24

TILE_SETTLE_DELAY = 1.2

# Config:
#   <tile/confirm_close=0>     — tile pane × closes immediately (no tile-level
#                                confirm dialog); the test relies on this so
#                                LeftUp triggers an observable close action.
#   <terminal/confirm_close=1> — the inner terminal widget *does* prompt; this
#                                produces the dimmed-title shadow the release
#                                assertion looks for. We must set this
#                                explicitly because $VTM_CONFIG now propagates
#                                to the dtvt pane-child (see doc/vtm-tile-config.md);
#                                relying on the user's ~/.config/vtm/settings.xml
#                                default would make the test environment-dependent.
TILE_CONFIG = (
    "<config>"
        "<tile><confirm_close=0/></tile>"
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
SPLIT_HZ_KEY = b"\x1b|"   # Alt+Shift+|

# Reference colours from the default vtm.xml palette.
FOCUSED_BG   = (58, 82, 142)   # #3A528E — winfocus
UNFOCUSED_BG = (64, 64, 64)    # #404040 — window_clr
X_HOVER_BG   = (255, 0, 0)     # × button hover
X_PRESS_BG   = (127, 0, 0)     # × button press-down

# Approximate column of the × button on the left pane menubar.
# Matches the position used in test_tile_pane_close_focus.py.
_LEFT_RIGHT_COL = COLS // 2 - 2   # = 38
X_BTN_COL = _LEFT_RIGHT_COL - 2   # = 36
X_BTN_ROW = 3


# ---------------------------------------------------------------------------
# Process / pty helpers
# ---------------------------------------------------------------------------

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
    """Read fd until it is silent for `quiet` seconds (or `timeout` expires)."""
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
        self.args = args
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
            try:
                os.kill(self.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                os.waitpid(self.pid, 0)
            except ChildProcessError:
                pass
            self.pid = None
        if self.master_fd is not None:
            try:
                os.close(self.master_fd)
            except OSError:
                pass
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


# ---------------------------------------------------------------------------
# SGR parser
# ---------------------------------------------------------------------------

def parse_sgr_grid(data):
    """Parse ANSI SGR stream into a bg-colour map keyed by 1-indexed (row, col)."""
    bg_grid = {}
    cur_bg = None
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
            seq = data[i + 2:j].decode('ascii', errors='replace')
            final = chr(data[j])
            if final == 'm':
                parts = seq.split(';') if seq else ['0']
                k = 0
                while k < len(parts):
                    p = parts[k]
                    try:
                        n = int(p) if p else 0
                    except ValueError:
                        n = 0
                    if n == 0:
                        cur_bg = None
                    elif n == 48 and k + 4 < len(parts) and parts[k + 1] == '2':
                        cur_bg = (int(parts[k + 2]), int(parts[k + 3]), int(parts[k + 4]))
                        k += 4
                    k += 1
            elif final == 'H':
                p = seq.split(';')
                row = int(p[0]) if p[0] else 1
                col = int(p[1]) if len(p) > 1 and p[1] else 1
            i = j + 1
            continue
        if c == ord('\n'):
            row += 1
            col = 1
        elif c == ord('\r'):
            col = 1
        elif c >= 0x20:
            bg_grid[(row, col)] = cur_bg
            col += 1
        i += 1
    return bg_grid


def dominant_bg(data, row, col_lo, col_hi, exclude=()):
    """Return the most common bg colour on `row` in [col_lo, col_hi], skipping `exclude`."""
    bg_grid = parse_sgr_grid(data)
    counts = {}
    for col in range(col_lo, col_hi + 1):
        bg = bg_grid.get((row, col))
        if bg is None or bg in exclude:
            continue
        counts[bg] = counts.get(bg, 0) + 1
    return max(counts, key=counts.get) if counts else None


def is_darker_variant(color, reference, factor=0.5, tolerance=15):
    """Return True if `color` ≈ `reference` * `factor` (element-wise).

    Used to detect vtm's confirm-dialog shadow: FOCUSED_BG * 0.5 = (29,41,71).
    """
    if color is None or reference is None:
        return False
    return all(
        abs(color[i] - round(reference[i] * factor)) <= tolerance
        for i in range(3)
    )


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_focus_on_press():
    """LeftDown (press only, no release) to a non-focused pane must shift focus."""
    print("TEST: tile - pane focus fires on LeftDown (press), not LeftClick ... ",
          end="", flush=True)

    left_right_col = COLS // 2 - 2     # ≈ right edge of left pane
    right_left_col = COLS // 2 + 2     # ≈ left edge of right pane

    with VtmSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        # Horizontal split.
        s.write(SPLIT_HZ_KEY)
        time.sleep(2.0)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - vtm died during split")
            return False

        # Full click on left pane center to guarantee left pane is focused.
        s.write(sgr_press(COLS // 4, ROWS // 2))
        time.sleep(0.05)
        s.write(sgr_release(COLS // 4, ROWS // 2))
        time.sleep(0.7)
        s.write(sgr_move(1, ROWS))   # move mouse away from pane content
        time.sleep(0.4)
        s.read(timeout=0.5)          # drain — now: left=FOCUSED, right=UNFOCUSED

        # Send ONLY LeftDown to the right pane center — no release.
        s.write(sgr_press(3 * COLS // 4, ROWS // 2))
        time.sleep(0.6)
        after_press = s.read(timeout=0.5)

        # Send LeftUp to leave vtm in a clean state.
        s.write(sgr_release(3 * COLS // 4, ROWS // 2))
        time.sleep(0.2)

    after_left  = dominant_bg(after_press, 3, 1,              left_right_col,
                               exclude=(X_HOVER_BG, X_PRESS_BG))
    after_right = dominant_bg(after_press, 3, right_left_col, COLS - 1,
                               exclude=(X_HOVER_BG, X_PRESS_BG))

    if after_left != UNFOCUSED_BG or after_right != FOCUSED_BG:
        print(f"FAIL - after LeftDown (no release): "
              f"left={after_left} (want {UNFOCUSED_BG}), "
              f"right={after_right} (want {FOCUSED_BG}). "
              "Focus did not shift on press; check controls.hpp pro::focus handler.")
        return False

    print("PASS")
    return True


def test_close_button_on_release():
    """LeftDown on × must NOT close the pane; only the full LeftClick must close it."""
    print("TEST: tile - × close button fires on LeftClick (release), not press ... ",
          end="", flush=True)

    left_right_col = COLS // 2 - 2
    right_left_col = COLS // 2 + 2

    with VtmSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        # Horizontal split.
        s.write(SPLIT_HZ_KEY)
        time.sleep(2.0)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - vtm died during split")
            return False

        # Click right pane to focus it (left pane becomes unfocused).
        s.write(sgr_press(3 * COLS // 4, ROWS // 2))
        time.sleep(0.05)
        s.write(sgr_release(3 * COLS // 4, ROWS // 2))
        time.sleep(0.7)
        s.write(sgr_move(1, ROWS))
        time.sleep(0.4)
        s.read(timeout=0.5)   # drain — now: left=UNFOCUSED, right=FOCUSED

        # LeftDown ONLY on × of left pane.  No mouse-move between press and
        # release — moving the mouse cancels vtm's clean-click state and
        # prevents LeftClick from firing on the subsequent LeftUp.
        s.write(sgr_press(X_BTN_COL, X_BTN_ROW))
        time.sleep(0.6)
        after_press = s.read(timeout=0.5)

        if not s.is_alive():
            print("FAIL - vtm exited on LeftDown (close must need full LeftClick)")
            return False

        # After LeftDown: focus moved to left pane; no confirm dialog yet.
        # Left title bar must be FOCUSED_BG.  A dialog would dim it darker.
        press_left = dominant_bg(after_press, 3, 1, left_right_col,
                                  exclude=(X_HOVER_BG, X_PRESS_BG))
        if press_left != FOCUSED_BG:
            print(f"FAIL - after LeftDown on ×: left pane titlebar={press_left} "
                  f"(want {FOCUSED_BG}). "
                  "Either focus did not shift or close fired on press.")
            return False

        # LeftUp completes LeftClick.  No mouse-move before this — preserves
        # clean-click state so LeftClick fires and triggers close.
        s.write(sgr_release(X_BTN_COL, X_BTN_ROW))
        time.sleep(1.0)
        s.write(sgr_move(1, ROWS))   # safe to move now that release is sent
        time.sleep(0.4)
        after_release = s.read(timeout=0.5)

        if not s.is_alive():
            # vtm exited cleanly — both panes gone, acceptable outcome.
            print("PASS (vtm exited after LeftUp)")
            return True

        # With <terminal/confirm_close=1> in TILE_CONFIG, the inner terminal
        # shows a confirm dialog when the tile asks it to close.  The dialog
        # shadows the left pane title bar to approximately
        # FOCUSED_BG * 0.5 = (29, 41, 71).
        release_left = dominant_bg(after_release, 3, 1, left_right_col,
                                    exclude=(X_HOVER_BG, X_PRESS_BG))
        dimmed = tuple(round(v * 0.5) for v in FOCUSED_BG)
        if not is_darker_variant(release_left, FOCUSED_BG):
            print(f"FAIL - after LeftUp on ×: left pane titlebar={release_left} "
                  f"(want ~{dimmed} — confirm dialog shadow). "
                  "Close may not have been triggered on release.")
            return False

    print("PASS")
    return True


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

TESTS = [
    test_focus_on_press,
    test_close_button_on_release,
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
