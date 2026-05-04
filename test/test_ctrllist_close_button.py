#!/usr/bin/env python3
# Copyright (c) Dmitry Sapozhnikov
# Licensed under the MIT license.

"""
Regression test: ctrllist right-alignment — close button stays at the right
edge of the menu bar at every supported terminal width.

Background
----------
The application menu bar uses a `ui::fork` to split horizontal space between
the scrollable menu text (menuslot) and the window-control buttons — minimize
(—), maximize (□), close (×) — that live in `ctrllist` (ctrlslot).

Bugs (history):
  * No alignment on ctrllist  → buttons placed at the *left* edge of ctrlslot
    (fork's default snap::none).  On a narrow window the menu text crowded
    the × button out of sight.
  * atgrow=snap::both only    → × landed at COLS//2 - 2 (fork's default 50/50
    split) instead of the true right edge.
  * Hidden floor min=12 in    → terminal app's content rail (term.hpp) had
    a hard min_size of 12 cols (originally to "avoid mc crashes when window
    is too small").  This propagated up the layout tree and prevented the
    menubar from shrinking below 12, so for COLS<12 the × glyph could not
    track the right edge.
  * scrlarea snap::both       → in fork's two-pass deform, the scrlarea
    (cake) reported its clamped size of 0 back as the required size,
    forcing split_x = 0 and pinning ctrllist to the left edge.

Fix (combined):
  1. ctrllist  : `.alignment({snap::both, snap::both})` — propagate natural
                 width (15 cells) up regardless of slot size.
  2. scrlarea  : `.alignment({snap::head, snap::both})` — atcrop.x != both
                 suppresses the upward "I need more" report in base::recalc,
                 so fork sees the actual negative split_x and shifts ctrllist
                 left to track the right edge.
  3. term.hpp  : lower the rail min_size from {12,1} to {1,1} so the layout
                 can shrink to allow the menubar to fit any terminal width.

Layout analysis (Windows/default style, macstyle=false):
  menufork       : spans the full inner window width (= COLS)
  slot_1 (menu)  : COLS - 15 cols wide (menu items, scrollable; can be
                   negative — fork shifts ctrllist left to compensate)
  slot_2 (ctrl)  : 15 cols wide; right edge always at COLS

  ctrllist (15 cols: "  —  "  "  □  "  "  ×  ")
  ctrllist.start (0-indexed)  = COLS - 15  (may be negative for narrow COLS)
  × char offset within ctrllist               = 12  (button "  ×  ", char 3)
  × position (0-indexed) = COLS - 15 + 12     = COLS - 3
  × position (1-indexed)                      = COLS - 2

The test captures screen row 1 at terminal widths spanning the supported
range and asserts × lands at column COLS - 2 (1-indexed) for every width.
A secondary click test verifies the button is functional (vtm exits without
confirm_close).
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

VTM_DESK_BINARY = os.environ.get(
    "VTM_DESK_BINARY",
    os.path.join(os.path.dirname(__file__), "..", "build", "vtm-desk"),
)

ROWS = 6          # only need a few rows to see the menu bar
READ_TIMEOUT = 5.0
SETTLE_DELAY = 1.5

# × glyph (MULTIPLICATION SIGN U+00D7) as UTF-8 bytes.
CLOSE_GLYPH = "\u00d7".encode("utf-8")

# Terminal widths to test: × must be at COLS - 2 (1-indexed) for each.
# Narrow widths (5..16) exercise the fork-shifting branch; wider widths
# exercise the normal positive-split branch.
TEST_WIDTHS = [5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 20, 24, 40, 80]


# ---------------------------------------------------------------------------
# Utilities
# ---------------------------------------------------------------------------

def kill_all_vtm():
    """Kill all vtm-desk processes and wait for them to exit."""
    for name in ("vtm-desk",):
        subprocess.run(["pkill", "-9", "-x", name], capture_output=True)
    for _ in range(20):
        time.sleep(0.2)
        if subprocess.run(["pgrep", "-x", "vtm-desk"],
                          capture_output=True).returncode != 0:
            return
    subprocess.run(["pkill", "-9", "-x", "vtm-desk"], capture_output=True)
    time.sleep(0.5)


def set_winsize(fd, rows, cols):
    """Set terminal window size via ioctl."""
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def read_all(fd, timeout=READ_TIMEOUT):
    """Drain all available data from fd within timeout."""
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


def replay_screen(stream, rows, cols):
    """
    Minimal ANSI escape replay that reconstructs a 2-D character grid.

    Handles CUP (ESC[H / ESC[f) cursor positioning, OSC strings, DCS/SOS/PM/APC
    strings, and basic cursor movement.  Returns a list-of-lists where each
    cell contains the raw UTF-8 bytes of the character at that position.
    """
    grid = [[b" "] * cols for _ in range(rows)]
    r = c = 0
    i = 0
    n = len(stream)
    while i < n:
        b = stream[i]
        if b == 0x1b and i + 1 < n:
            nxt = stream[i + 1]
            if nxt == ord('['):
                j = i + 2
                while j < n and 0x30 <= stream[j] <= 0x3f:
                    j += 1
                while j < n and 0x20 <= stream[j] <= 0x2f:
                    j += 1
                if j >= n:
                    break
                final = stream[j]
                params = stream[i + 2:j]
                if final in (ord('H'), ord('f')):
                    parts = params.split(b';') if params else []
                    try:
                        nr = int(parts[0]) if len(parts) > 0 and parts[0] else 1
                        nc = int(parts[1]) if len(parts) > 1 and parts[1] else 1
                    except ValueError:
                        nr, nc = 1, 1
                    r = max(0, min(rows - 1, nr - 1))
                    c = max(0, min(cols - 1, nc - 1))
                i = j + 1
                continue
            elif nxt == ord(']'):
                j = i + 2
                while j < n and stream[j] != 0x07:
                    if stream[j] == 0x1b and j + 1 < n and stream[j + 1] == ord('\\'):
                        j += 2
                        break
                    j += 1
                else:
                    j += 1
                i = j
                continue
            elif nxt in (ord('P'), ord('X'), ord('^'), ord('_')):
                j = i + 2
                while j < n:
                    if stream[j] == 0x1b and j + 1 < n and stream[j + 1] == ord('\\'):
                        j += 2
                        break
                    j += 1
                i = j
                continue
            else:
                i += 2
                continue
        if b == 0x0d:
            c = 0
            i += 1
            continue
        if b == 0x0a:
            if r + 1 < rows:
                r += 1
            i += 1
            continue
        if b == 0x08:
            if c > 0:
                c -= 1
            i += 1
            continue
        if b < 0x20 or b == 0x7f:
            i += 1
            continue
        clen = (1 if b < 0x80 else
                1 if b < 0xc0 else
                2 if b < 0xe0 else
                3 if b < 0xf0 else 4)
        glyph = bytes(stream[i:i + clen])
        if 0 <= r < rows and 0 <= c < cols:
            grid[r][c] = glyph
        c = min(c + 1, cols - 1)
        i += clen
    return grid


def find_close_col(grid):
    """
    Return the 1-indexed column of the first × glyph on screen row 1 (grid[0]),
    or None if not found.
    """
    for c_idx, cell in enumerate(grid[0]):
        if cell == CLOSE_GLYPH:
            return c_idx + 1  # convert 0-indexed to 1-indexed
    return None


def row1_text(grid):
    """Return printable ASCII representation of row 1 (for diagnostic output)."""
    parts = []
    for cell in grid[0]:
        try:
            ch = cell.decode("utf-8")
            parts.append(ch if ch.isprintable() else ".")
        except Exception:
            parts.append("?")
    return "".join(parts)


# ---------------------------------------------------------------------------
# Session helpers
# ---------------------------------------------------------------------------

def launch_vtm(cols, rows=ROWS, extra_args=None):
    """
    Fork a vtm-desk -r term process in a pty of size (rows, cols).
    Returns (master_fd, pid).  Caller is responsible for cleanup.
    """
    args = [VTM_DESK_BINARY, "-r", "term"]
    if extra_args:
        args = [VTM_DESK_BINARY] + extra_args + ["-r", "term"]
    master_fd, slave_fd = pty.openpty()
    set_winsize(master_fd, rows, cols)
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
        os.execvp(args[0], args)
        sys.exit(1)
    os.close(slave_fd)
    return master_fd, pid


def cleanup(master_fd, pid):
    """Kill vtm-desk and close the pty."""
    if pid:
        try:
            os.kill(pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            os.waitpid(pid, os.WNOHANG)
        except ChildProcessError:
            pass
    if master_fd is not None:
        try:
            os.close(master_fd)
        except OSError:
            pass
    kill_all_vtm()


def is_alive(pid):
    """Return True if pid is still running."""
    try:
        result, _ = os.waitpid(pid, os.WNOHANG)
        return result == 0
    except ChildProcessError:
        return False


def wait_for_exit(pid, timeout=5.0):
    """Wait for pid to exit.  Return True if it exited within timeout."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not is_alive(pid):
            return True
        time.sleep(0.1)
    return False


def sgr_mouse_press(col, row, button=0):
    return f"\033[<{button};{col};{row}M".encode()


def sgr_mouse_release(col, row, button=0):
    return f"\033[<{button};{col};{row}m".encode()


# ---------------------------------------------------------------------------
# Individual tests
# ---------------------------------------------------------------------------

def test_close_button_position_at_width(cols):
    """
    At terminal width `cols`, capture screen row 1 and assert the × glyph
    is at 1-indexed column COLS - 2.

    With the full fix (ctrllist snap::both + scrlarea snap::head + lowered
    terminal rail floor):
      ctrllist reports its natural width (15) back to fork.  Fork shifts
      ctrllist left by (15 - COLS) when COLS < 15, so its right edge always
      tracks the terminal's right edge, and × sits at COLS - 2 (1-indexed).
    """
    label = f"COLS={cols}"
    print(f"TEST: ctrllist × position at {label} ... ", end="", flush=True)

    expected = cols - 2

    master_fd, pid = launch_vtm(cols)
    try:
        time.sleep(SETTLE_DELAY)
        stream = read_all(master_fd, timeout=2.0)
        grid = replay_screen(stream, ROWS, cols)
        actual = find_close_col(grid)
        preview = row1_text(grid)[:cols]

        if actual is None:
            print(f"FAIL - × glyph not found on row 1 (row1: {preview!r})")
            return False
        if actual != expected:
            print(
                f"FAIL - × at col {actual}, expected {expected}  "
                f"(row1: {preview!r})"
            )
            return False

        print(f"PASS (× at col {actual})")
        return True
    finally:
        cleanup(master_fd, pid)


def test_close_button_functional():
    """
    With COLS=80, click at the expected × position (col COLS - 2, row 1)
    and verify vtm-desk exits.  This confirms the button is not only in the
    right place but is also interactive.

    confirm_close is disabled so vtm exits immediately on click.
    """
    print("TEST: ctrllist × button functional at COLS=80 ... ", end="", flush=True)
    cols = 80
    close_col = cols - 2  # expected 1-indexed column of ×

    # Use confirm_close=0 so vtm exits immediately without a dialog.
    extra_args = ["-c", "<config><terminal><confirm_close=0/></terminal></config>"]
    master_fd, pid = launch_vtm(cols, extra_args=extra_args)
    try:
        time.sleep(SETTLE_DELAY)
        read_all(master_fd, timeout=1.0)  # drain initial output

        # Click the × button.
        os.write(master_fd, sgr_mouse_press(close_col, 1))
        time.sleep(0.05)
        os.write(master_fd, sgr_mouse_release(close_col, 1))

        if wait_for_exit(pid, timeout=5.0):
            print("PASS")
            return True

        print(
            f"FAIL - vtm did not exit after clicking col {close_col}, row 1"
            f"  (is the × at that column?)"
        )
        return False
    finally:
        cleanup(master_fd, pid)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    if not os.path.isfile(VTM_DESK_BINARY):
        print(f"ERROR: vtm-desk binary not found at {VTM_DESK_BINARY}")
        print("Set VTM_DESK_BINARY env var or build vtm-desk first.")
        return 1

    kill_all_vtm()

    tests = []

    # Position tests at each supported width: × at 1-indexed COLS - 2.
    for w in TEST_WIDTHS:
        tests.append(lambda w=w: test_close_button_position_at_width(w))

    # Functional click test.
    tests.append(test_close_button_functional)

    passed = 0
    failed = 0
    for test in tests:
        try:
            result = test()
            if result:
                passed += 1
            else:
                failed += 1
        except Exception as e:
            import traceback
            print(f"ERROR: {e}")
            traceback.print_exc()
            failed += 1
        finally:
            kill_all_vtm()
            time.sleep(0.3)

    total = passed + failed
    print(f"\n{'='*60}")
    print(f"Results: {passed}/{total} passed, {failed} failed")
    print(f"{'='*60}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
