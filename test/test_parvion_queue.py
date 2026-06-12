#!/usr/bin/env python3
# Copyright (c) Shaobo Song
# Licensed under the MIT license.

"""
End-to-end TUI regression tests for the Parvion (parvion) transfer-queue
tables — the behaviors added on top of parvion/queue.hpp:

  1. The Failed tab's "Reason" column has a draggable width handle on its
     right edge (one more column border than the other transfer tabs).
  2. A selected row's highlight ends at the last column; the area to its
     right stays blank (and no longer hit-tests as part of the row).
  3. Right-click on the blank area opens a context menu with
     "Start All" / "Pause All" / "Remove All" (scoped to the active tab).
  4. Right-click on an item opens "Start" / "Pause" / "Remove" / "Pin to Top".
     Pin to Top moves a pending item to the front of the pending group.
  5. Right-click with several rows selected applies the action to all of them.

Driven via a pty using the SGR mouse protocol, mirroring the harness used by
the existing test_dropdown_menu / test_tile_ctrl_click_focus suites. The app
is launched as `vtm-tile -r parvion` with the demo queue seeded (PARVION_DEMO_QUEUE=1)
plus three extra pending items (PARVION_DEMO_QUEUED_N=3); the demo never spawns a
backend, so the queued items stay put for the queue-action assertions.
"""

import os
import re
import sys
import pty
import time
import fcntl
import select
import signal
import struct
import termios
import subprocess

VTM_TILE_BINARY = os.environ.get(
    "VTM_TILE_BINARY",
    os.path.join(os.path.dirname(__file__), "..", "build", "vtm-tile"),
)

COLS = 120
ROWS = 44
SETTLE = 1.5

_CSI_RE = re.compile(rb"\x1b\[([\x30-\x3f]*)([\x20-\x2f]*)([\x40-\x7e])")


def kill_all_vtm():
    subprocess.run(["pkill", "-9", "-x", "vtm-tile"], capture_output=True)
    deadline = time.time() + 3.0
    while time.time() < deadline:
        if subprocess.run(["pgrep", "-x", "vtm-tile"],
                          capture_output=True).returncode != 0:
            break
        time.sleep(0.1)


def set_winsize(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def _parse_sgr(params, cur_bg):
    parts = [p.decode() if p else "0" for p in (params.split(b";") if params else [b"0"])]
    i = 0
    while i < len(parts):
        try:
            n = int(parts[i] or "0")
        except ValueError:
            n = 0
        if n == 0 or n == 49:
            cur_bg = None
        elif n == 48 and i + 1 < len(parts):
            mode = parts[i + 1]
            if mode == "2" and i + 4 < len(parts):
                try:
                    cur_bg = (int(parts[i + 2]), int(parts[i + 3]), int(parts[i + 4]))
                except ValueError:
                    pass
                i += 4
            elif mode == "5" and i + 2 < len(parts):
                i += 2
        i += 1
    return cur_bg


def replay(buf):
    """Replay *buf* into (chars, bg): per-cell last-painted glyph and bg color.

    chars[r][c] : last visible glyph painted at (r, c), or "" if none.
    bg[r][c]    : last bg color (R,G,B) painted at (r, c), or None.
    """
    chars = [[""] * COLS for _ in range(ROWS)]
    bg = [[None] * COLS for _ in range(ROWS)]
    cur_row = cur_col = 0
    cur_bg = None
    i, n = 0, len(buf)
    while i < n:
        b = buf[i]
        if b == 0x1b and i + 1 < n:
            c1 = buf[i + 1]
            if c1 == 0x5b:  # CSI
                m = _CSI_RE.match(buf, i)
                if not m:
                    i += 1
                    continue
                params, final = m.group(1), m.group(3)
                i = m.end()
                if final in (b"H", b"f"):
                    parts = (params.split(b";") if params else [b"", b""])
                    if len(parts) < 2:
                        parts += [b""]
                    try:
                        r = int(parts[0]) if parts[0] else 1
                        c = int(parts[1]) if parts[1] else 1
                    except ValueError:
                        r, c = 1, 1
                    cur_row = max(0, min(ROWS - 1, r - 1))
                    cur_col = max(0, min(COLS - 1, c - 1))
                elif final == b"A":
                    cur_row = max(0, cur_row - (int(params) if params else 1))
                elif final == b"B":
                    cur_row = min(ROWS - 1, cur_row + (int(params) if params else 1))
                elif final == b"C":
                    cur_col = min(COLS - 1, cur_col + (int(params) if params else 1))
                elif final == b"D":
                    cur_col = max(0, cur_col - (int(params) if params else 1))
                elif final == b"m":
                    cur_bg = _parse_sgr(params, cur_bg)
                continue
            elif c1 == 0x5d:  # OSC
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
            elif c1 in (0x50, 0x5f, 0x5e):  # DCS / APC / PM
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
        # Visible glyph (decode a full UTF-8 sequence; all glyphs here are width 1).
        if b >= 0xc0:
            j = i + 1
            while j < n and 0x80 <= buf[j] < 0xc0:
                j += 1
            ch = buf[i:j].decode("utf-8", "replace")
            i = j
        else:
            ch = chr(b)
            i += 1
        if 0 <= cur_row < ROWS and 0 <= cur_col < COLS:
            chars[cur_row][cur_col] = ch
            bg[cur_row][cur_col] = cur_bg
            if cur_col < COLS - 1:
                cur_col += 1
    return chars, bg


class ParvionSession:
    def __init__(self, env=None):
        self.env = env or {}
        self.master_fd = None
        self.pid = None
        self._buf = b""

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
            for k, v in self.env.items():
                os.environ[k] = v
            os.execvp(VTM_TILE_BINARY, [VTM_TILE_BINARY, "-r", "parvion"])
            sys.exit(1)
        os.close(slave_fd)
        time.sleep(SETTLE)
        self.feed(1.0)
        return self

    def __exit__(self, *_):
        if self.pid:
            try:
                os.kill(self.pid, signal.SIGKILL)
                os.waitpid(self.pid, 0)
            except (ProcessLookupError, ChildProcessError):
                pass
            self.pid = None
        if self.master_fd is not None:
            try:
                os.close(self.master_fd)
            except OSError:
                pass
            self.master_fd = None
        kill_all_vtm()

    def feed(self, timeout=0.6):
        deadline = time.time() + timeout
        while time.time() < deadline:
            ready, _, _ = select.select([self.master_fd], [], [], 0.05)
            if not ready:
                continue
            try:
                chunk = os.read(self.master_fd, 65536)
            except OSError:
                break
            if not chunk:
                break
            self._buf += chunk
            deadline = max(deadline, time.time() + 0.12)
        return self

    def screen(self):
        return replay(self._buf)

    def _write(self, data):
        os.write(self.master_fd, data)

    def write(self, data, settle=0.5):
        self._write(data.encode() if isinstance(data, str) else data)
        self.feed(settle)

    def click(self, col, row, button=0, settle=0.6):
        self._write(f"\x1b[<{button};{col};{row}M".encode())
        time.sleep(0.05)
        self._write(f"\x1b[<{button};{col};{row}m".encode())
        self.feed(settle)

    def drag(self, col1, row1, col2, row2, button=0, settle=0.6):
        """Left-drag from (col1,row1) to (col2,row2): press, motion (button|32), release."""
        self._write(f"\x1b[<{button};{col1};{row1}M".encode())
        time.sleep(0.05)
        self._write(f"\x1b[<{button + 32};{col2};{row2}M".encode())
        time.sleep(0.05)
        self._write(f"\x1b[<{button};{col2};{row2}m".encode())
        self.feed(settle)

    def double_click(self, col, row, button=0, settle=0.6):
        """Two clicks at (col,row) well within the 500ms double-click threshold."""
        for _ in range(2):
            self._write(f"\x1b[<{button};{col};{row}M".encode())
            time.sleep(0.02)
            self._write(f"\x1b[<{button};{col};{row}m".encode())
            time.sleep(0.02)
        self.feed(settle)

    def drag_path(self, points, button=0, settle=0.6):
        """Press at points[0], motion (button|32) through the rest, release at points[-1]."""
        c0, r0 = points[0]
        self._write(f"\x1b[<{button};{c0};{r0}M".encode())
        time.sleep(0.04)
        for c, r in points[1:]:
            self._write(f"\x1b[<{button + 32};{c};{r}M".encode())
            time.sleep(0.04)
        cl, rl = points[-1]
        self._write(f"\x1b[<{button};{cl};{rl}m".encode())
        self.feed(settle)


# ----- grid query helpers (1-based mouse coords, 0-based grid indices) -----

def row_text(chars, r):
    return "".join(ch if ch else " " for ch in chars[r])


def find_text(chars, needle):
    """First (row, col) where `needle` appears in the rendered grid, else None."""
    for r in range(ROWS):
        c = row_text(chars, r).find(needle)
        if c >= 0:
            return (r, c)
    return None


def grid_contains(chars, needle):
    return any(needle in row_text(chars, r) for r in range(ROWS))


def find_menu_item(chars, name):
    """Find a column-toggle row '▣ name' / '□ name'; return (row, col-of-name) or None."""
    for r in range(ROWS):
        rt = row_text(chars, r)
        for mark in ("▣ ", "□ "):
            idx = rt.find(mark + name)
            if idx >= 0:
                return (r, idx + len(mark))
    return None


def click_label(s, needle, button=0):
    """Find `needle` on screen and click its first cell (mouse coords are 1-based)."""
    pos = find_text(s.screen()[0], needle)
    if pos is None:
        return False
    r, c = pos
    s.click(c + 1, r + 1, button=button)
    return True


def tab_count(chars, label):
    """Parse the count in a tab label like 'Transferring (5)'. None if absent."""
    for r in range(ROWS):
        m = re.search(re.escape(label) + r"\s*\((\d+)\)", row_text(chars, r))
        if m:
            return int(m.group(1))
    return None


DEMO_ENV = {"PARVION_DEMO_QUEUE": "1", "PARVION_DEMO_QUEUED_N": "3"}


def header_border_count(s, header_word):
    """Count the '│' column-border markers on the table header row, located via
    a header label unique to the active tab ('Speed' or 'Reason')."""
    chars = s.screen()[0]
    pos = find_text(chars, header_word)
    if pos is None:
        return None
    return row_text(chars, pos[0]).count("│")


# ----------------------------------- tests -----------------------------------

def test_reason_column_has_resize_handle():
    """Failed tab's Reason column adds a 5th draggable border vs. the 4 on Transferring."""
    print("TEST: parvion - Reason column has a resize handle ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        xfer = header_border_count(s, "Speed")  # Transferring tab (default).
        if xfer is None:
            print("FAIL - Transferring header not found")
            return False
        if not click_label(s, "Failed ("):
            print("FAIL - Failed tab not found")
            return False
        fail = header_border_count(s, "Reason")
        if fail is None:
            print("FAIL - Reason header not found on Failed tab")
            return False
        if not (xfer == 5 and fail == 6):
            print(f"FAIL - expected 5 borders on Transferring and 6 on Failed, got {xfer} and {fail}")
            return False
        print("PASS")
        return True


def test_selection_highlight_ends_at_last_column():
    """Selecting a row highlights the columns but leaves the area to their right blank."""
    print("TEST: parvion - selection highlight ends at last column ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        pos = find_text(s.screen()[0], "notes.txt")
        if pos is None:
            print("FAIL - item row not found")
            return False
        r, c = pos
        blank_c = 100  # Well past the last column (content ends near col 62).
        bg_before = s.screen()[1]
        name_before = bg_before[r][c]
        blank_before = bg_before[r][blank_c]
        s.click(c + 1, r + 1, button=0)  # Select the row.
        bg_after = s.screen()[1]
        name_after = bg_after[r][c]
        blank_after = bg_after[r][blank_c]
        if name_after is None or name_after == name_before:
            print(f"FAIL - row not highlighted (name bg {name_before} -> {name_after})")
            return False
        if blank_after != blank_before:
            print(f"FAIL - highlight bled past last column (blank bg {blank_before} -> {blank_after})")
            return False
        print("PASS")
        return True


def test_expand_button_press_and_hold_feedback():
    """Holding the left button on a row's +/- button doubles its highlight ("pushed",
    like the connect bar's buttons); releasing drops back to the hover shade and still
    toggles the row's subtasks."""
    print("TEST: parvion - expand button press-and-hold feedback ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        chars = s.screen()[0]
        pos = find_text(chars, "bigfile.iso")
        if pos is None:
            print("FAIL - parallel item row not found")
            return False
        r, _ = pos
        c = row_text(chars, r).find("+")  # The enabled "+" button (item collapsed).
        if c < 0:
            print("FAIL - expand button not found on the parallel row")
            return False
        resting = s.screen()[1][r][c]
        # Hover the button in two motion steps: the first packet only seeds the
        # pointer position (vtm swallows it as the initial sync), the second lands.
        s._write(f"\x1b[<35;{100};{r + 1}M".encode())
        s.feed(0.3)
        s._write(f"\x1b[<35;{c + 1};{r + 1}M".encode())
        s.feed(0.6)
        hover = s.screen()[1][r][c]
        if hover is None or hover == resting:
            print(f"FAIL - no hover highlight (bg {resting} -> {hover})")
            return False
        s._write(f"\x1b[<0;{c + 1};{r + 1}M".encode())   # Press and hold.
        s.feed(0.6)
        pressed = s.screen()[1][r][c]
        if pressed is None or pressed in (hover, resting):
            print(f"FAIL - held press not distinct (bg rest {resting} / hover {hover} / press {pressed})")
            return False
        s._write(f"\x1b[<0;{c + 1};{r + 1}m".encode())   # Release: toggles the subtasks.
        s.feed(0.6)
        released = s.screen()[1][r][c]
        if released != hover:
            print(f"FAIL - press shade did not drop on release (bg {released}, hover {hover})")
            return False
        if not grid_contains(s.screen()[0], "Part 1/4"):
            print("FAIL - release did not expand the subtasks")
            return False
        print("PASS")
        return True


def test_blank_area_menu_lists_all_actions():
    """Right-click on the blank area shows Start All / Pause All / Remove All."""
    print("TEST: parvion - blank-area menu lists *All actions ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        pos = find_text(s.screen()[0], "notes.txt")
        if pos is None:
            print("FAIL - item row not found")
            return False
        r, _ = pos
        s.click(100, r + 1, button=2)  # Right-click blank area to the right of the columns.
        chars = s.screen()[0]
        missing = [w for w in ("Start All", "Pause All", "Remove All") if not grid_contains(chars, w)]
        if missing:
            print(f"FAIL - menu missing {missing}")
            return False
        print("PASS")
        return True


def test_right_click_blank_clears_selection():
    """Right-clicking the blank area clears the queue selection (like left-click does)."""
    print("TEST: parvion - right-click blank clears selection ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        pos = find_text(s.screen()[0], "notes.txt")
        if pos is None:
            print("FAIL - item row not found")
            return False
        r, c = pos
        before = s.screen()[1][r][c]            # notes.txt name cell, unselected.
        s.click(c + 1, r + 1, button=0)         # Left-click selects the row.
        sel = s.screen()[1][r][c]               # The selection highlight color.
        if sel is None or sel == before:
            print(f"FAIL - left-click did not select the row (bg {before} -> {sel})")
            return False
        s.click(100, r + 1, button=2)           # Right-click the blank area right of the columns.
        s._write(b"\x1b")                        # Dismiss the All menu.
        s.feed(0.3)
        after = s.screen()[1][r][c]
        if after == sel:
            print(f"FAIL - right-click blank left the row selected (bg still {after})")
            return False
        print("PASS")
        return True


def test_item_menu_lists_item_actions():
    """Right-click a queued item shows Start / Pause / Remove / Pin to Top."""
    print("TEST: parvion - item menu lists item actions ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        pos = find_text(s.screen()[0], "queued_00")  # A pending item: full menu incl. Pin to Top.
        if pos is None:
            print("FAIL - queued item row not found")
            return False
        r, c = pos
        s.click(c + 1, r + 1, button=2)  # Right-click the item.
        chars = s.screen()[0]
        missing = [w for w in ("Start", "Pause", "Remove", "Pin to Top") if not grid_contains(chars, w)]
        if missing:
            print(f"FAIL - menu missing {missing}")
            return False
        print("PASS")
        return True


def test_transferring_item_menu_omits_pin():
    """Right-clicking an in-flight (transferring) item drops 'Pin to Top' (nothing to pin)."""
    print("TEST: parvion - transferring item menu omits Pin to Top ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        pos = find_text(s.screen()[0], "notes.txt")  # A demo transferring item.
        if pos is None:
            print("FAIL - transferring item row not found")
            return False
        r, c = pos
        s.click(c + 1, r + 1, button=2)  # Right-click the item.
        chars = s.screen()[0]
        missing = [w for w in ("Start", "Pause", "Remove") if not grid_contains(chars, w)]
        if missing:
            print(f"FAIL - menu missing {missing}")
            return False
        if grid_contains(chars, "Pin to Top"):
            print("FAIL - transferring item menu still shows 'Pin to Top'")
            return False
        print("PASS")
        return True


def test_failed_succeeded_item_menu_omits_pause_and_pin():
    """On the Failed and Succeeded tabs the item menu drops 'Pause' and 'Pin to Top' (Start/Remove stay)."""
    print("TEST: parvion - Failed/Succeeded item menu omits Pause + Pin to Top ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        for tab_label, item_name in (("Failed (", "upload.bin"), ("Succeeded (", "archive.tar")):
            if not click_label(s, tab_label):
                print(f"FAIL - {tab_label} tab not found")
                return False
            pos = find_text(s.screen()[0], item_name)
            if pos is None:
                print(f"FAIL - item {item_name} not found")
                return False
            r, c = pos
            s.click(c + 1, r + 1, button=2)  # Right-click the item.
            chars = s.screen()[0]
            missing = [w for w in ("Start", "Remove") if not grid_contains(chars, w)]
            if missing:
                print(f"FAIL - {item_name} menu missing {missing}")
                return False
            present = [w for w in ("Pause", "Pin to Top") if grid_contains(chars, w)]
            if present:
                print(f"FAIL - {item_name} menu still shows {present}")
                return False
            s._write(b"\x1b")  # Dismiss the menu before the next tab.
            s.feed(0.3)
        print("PASS")
        return True


def test_failed_succeeded_all_menu_omits_pause_all():
    """On the Failed and Succeeded tabs the blank-area menu drops 'Pause All' (Start All/Remove All stay)."""
    print("TEST: parvion - Failed/Succeeded blank-area menu omits Pause All ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        for tab_label, item_name in (("Failed (", "upload.bin"), ("Succeeded (", "archive.tar")):
            if not click_label(s, tab_label):
                print(f"FAIL - {tab_label} tab not found")
                return False
            pos = find_text(s.screen()[0], item_name)
            if pos is None:
                print(f"FAIL - item {item_name} not found")
                return False
            # Right-click well right of the columns (Failed content ends at col 107) -> blank-area menu.
            s.click(115, pos[0] + 1, button=2)
            chars = s.screen()[0]
            missing = [w for w in ("Start All", "Remove All") if not grid_contains(chars, w)]
            if missing:
                print(f"FAIL - {tab_label} blank-area menu missing {missing}")
                return False
            if grid_contains(chars, "Pause All"):
                print(f"FAIL - {tab_label} blank-area menu still shows 'Pause All'")
                return False
            s._write(b"\x1b")  # Dismiss the menu before the next tab.
            s.feed(0.3)
        print("PASS")
        return True


def test_header_menu_lists_column_toggles():
    """Right-clicking the table header opens a per-column show/hide menu (▣ = shown by default)."""
    print("TEST: parvion - header menu lists column toggles ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        hdr = find_text(s.screen()[0], "Local Name")
        if hdr is None:
            print("FAIL - header not found")
            return False
        s.click(95, hdr[0] + 1, button=2)  # Right-click the header row (blank right area).
        chars = s.screen()[0]
        # Every column has a toggle row, each marked shown (▣) by default. The "▣ " prefix
        # distinguishes the menu rows from the identically-named column headers.
        missing = [n for n in ("Local Name", "Remote Name", "Size", "Progress", "Speed")
                   if not grid_contains(chars, "▣ " + n)]
        if missing:
            print(f"FAIL - menu missing shown (▣) toggles for {missing}")
            return False
        print("PASS")
        return True


def test_header_menu_toggles_column_visibility():
    """Clicking a column toggle hides that column from the table."""
    print("TEST: parvion - header menu hides a column ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        hdr = find_text(s.screen()[0], "Local Name")
        if hdr is None:
            print("FAIL - header not found")
            return False
        hr = hdr[0]
        if "Speed" not in row_text(s.screen()[0], hr):
            print("FAIL - Speed column missing initially")
            return False
        s.click(95, hr + 1, button=2)  # Open the header menu.
        item = find_menu_item(s.screen()[0], "Speed")
        if item is None:
            print("FAIL - Speed toggle not found in menu")
            return False
        s.click(item[1] + 1, item[0] + 1, button=0)  # Click the Speed toggle.
        chars = s.screen()[0]
        hr2 = find_text(chars, "Local Name")[0]
        if "Speed" in row_text(chars, hr2):
            print("FAIL - Speed column still shown after toggle")
            return False
        borders = row_text(chars, hr2).count("│")
        if borders != 4:
            print(f"FAIL - expected 4 column borders after hiding Speed, got {borders}")
            return False
        print("PASS")
        return True


def test_remove_item_via_menu():
    """Right-click an item -> Remove drops it and decrements the tab count."""
    print("TEST: parvion - Remove deletes the item ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        before = tab_count(s.screen()[0], "Transferring")
        pos = find_text(s.screen()[0], "notes.txt")
        if pos is None or before is None:
            print("FAIL - item row / tab count not found")
            return False
        r, c = pos
        s.click(c + 1, r + 1, button=2)         # Open the item menu.
        if not click_label(s, "Remove"):        # Click "Remove".
            print("FAIL - Remove entry not found")
            return False
        # Remove now asks for confirmation; a single selection gets the singular wording.
        if not grid_contains(s.screen()[0], "Remove this transfer from the queue?"):
            print("FAIL - confirmation dialog not shown")
            return False
        s.write("\r")  # Enter -> Confirm (the default selection).
        chars = s.screen()[0]
        after = tab_count(chars, "Transferring")
        if grid_contains(chars, "notes.txt"):
            print("FAIL - item still present after Remove")
            return False
        if after != before - 1:
            print(f"FAIL - tab count {before} -> {after} (expected {before - 1})")
            return False
        print("PASS")
        return True


def test_multiselect_remove_all_selected():
    """Ctrl-click builds a multi-selection; Remove drops every selected row."""
    print("TEST: parvion - multi-select Remove drops all selected ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        before = tab_count(s.screen()[0], "Transferring")
        p0 = find_text(s.screen()[0], "queued_00")
        p2 = find_text(s.screen()[0], "queued_02")
        if before is None or p0 is None or p2 is None:
            print("FAIL - queued rows / tab count not found")
            return False
        s.click(p0[1] + 1, p0[0] + 1, button=0)   # Select queued_00.
        s.click(p2[1] + 1, p2[0] + 1, button=16)  # Ctrl-click queued_02 -> two selected.
        s.click(p2[1] + 1, p2[0] + 1, button=2)   # Right-click a selected row.
        if not click_label(s, "Remove"):
            print("FAIL - Remove entry not found")
            return False
        # Remove now asks for confirmation; a multi-selection gets the counted wording.
        if not grid_contains(s.screen()[0], "Remove 2 transfers from the queue?"):
            print("FAIL - confirmation dialog not shown")
            return False
        s.write("\r")  # Enter -> Confirm (the default selection).
        chars = s.screen()[0]
        after = tab_count(chars, "Transferring")
        if grid_contains(chars, "queued_00") or grid_contains(chars, "queued_02"):
            print("FAIL - a selected row survived Remove")
            return False
        if not grid_contains(chars, "queued_01"):
            print("FAIL - an unselected row was removed")
            return False
        if after != before - 2:
            print(f"FAIL - tab count {before} -> {after} (expected {before - 2})")
            return False
        print("PASS")
        return True


def test_keyboard_clear_finished_confirm():
    """The Delete key asks before clearing finished items; Esc preserves them, Enter clears."""
    print("TEST: parvion - keyboard clear-finished asks for confirmation ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        pos = find_text(s.screen()[0], "notes.txt")
        if pos is None:
            print("FAIL - queue row not found")
            return False
        s.click(pos[1] + 1, pos[0] + 1, button=0)  # Focus the queue panel.
        s.write("\x1b[3~")                         # Delete -> confirmation dialog.
        if not grid_contains(s.screen()[0], "Clear all finished transfers?"):
            print("FAIL - confirmation dialog not shown")
            return False
        s.write("\x1b")                            # Esc -> Cancel: nothing cleared.
        chars = s.screen()[0]
        if tab_count(chars, "Failed") != 1 or tab_count(chars, "Succeeded") != 1:
            print("FAIL - finished items cleared despite cancelling")
            return False
        s.write("\x1b[3~")                         # Delete again ...
        s.write("\r")                              # ... Enter -> Confirm: finished items cleared.
        chars = s.screen()[0]
        if tab_count(chars, "Failed") != 0 or tab_count(chars, "Succeeded") != 0:
            print("FAIL - finished items not cleared after confirm")
            return False
        print("PASS")
        return True


def test_pin_to_top_reorders_pending():
    """Pin to Top moves a pending item to the front of the pending group."""
    print("TEST: parvion - Pin to Top reorders pending items ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        chars = s.screen()[0]
        r0 = find_text(chars, "queued_00")
        r2 = find_text(chars, "queued_02")
        if r0 is None or r2 is None:
            print("FAIL - queued rows not found")
            return False
        if not (r2[0] > r0[0]):
            print(f"FAIL - precondition: queued_02 (row {r2[0]}) not below queued_00 (row {r0[0]})")
            return False
        s.click(r2[1] + 1, r2[0] + 1, button=2)  # Right-click queued_02.
        if not click_label(s, "Pin to Top"):
            print("FAIL - Pin to Top entry not found")
            return False
        chars = s.screen()[0]
        n0 = find_text(chars, "queued_00")
        n1 = find_text(chars, "queued_01")
        n2 = find_text(chars, "queued_02")
        if not (n0 and n1 and n2):
            print("FAIL - queued rows missing after pin")
            return False
        if not (n2[0] < n0[0] and n0[0] < n1[0]):
            print(f"FAIL - order after pin: q02={n2[0]} q00={n0[0]} q01={n1[0]} (expected q02 < q00 < q01)")
            return False
        print("PASS")
        return True


def progress_text(chars, name_row):
    """Read the right-aligned Progress column content on a row. With columns Local Name(24) +
    Remote Name(24) + Size(11), Progress content spans cols 66..74; its '│' border at col 75 is excluded."""
    return "".join(chars[name_row][66:75]).strip()


def test_pause_then_start_progress_label():
    """Pause marks a queued item 'paused'; Start returns it to 'queued'."""
    print("TEST: parvion - Pause/Start toggles the progress label ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        pos = find_text(s.screen()[0], "queued_00")
        if pos is None:
            print("FAIL - queued row not found")
            return False
        r, c = pos
        if progress_text(s.screen()[0], r) != "queued":
            print(f"FAIL - precondition: progress is '{progress_text(s.screen()[0], r)}', expected 'queued'")
            return False
        s.click(c + 1, r + 1, button=2)
        if not click_label(s, "Pause"):
            print("FAIL - Pause entry not found")
            return False
        if progress_text(s.screen()[0], r) != "paused":
            print(f"FAIL - after Pause progress is '{progress_text(s.screen()[0], r)}', expected 'paused'")
            return False
        s.click(c + 1, r + 1, button=2)
        if not click_label(s, "Start"):
            print("FAIL - Start entry not found")
            return False
        if progress_text(s.screen()[0], r) != "queued":
            print(f"FAIL - after Start progress is '{progress_text(s.screen()[0], r)}', expected 'queued'")
            return False
        print("PASS")
        return True


def test_local_and_remote_name_columns():
    """The Name column is split into Local Name / Remote Name, each holding that side's full path."""
    print("TEST: parvion - Local Name / Remote Name columns show full paths ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        chars = s.screen()[0]
        if not (grid_contains(chars, "Local Name") and grid_contains(chars, "Remote Name")):
            print("FAIL - Local Name / Remote Name headers not found")
            return False
        # Demo download item: local_path=/local/bigfile.iso, remote_path=bigfile.iso.
        pos = find_text(chars, "bigfile.iso")
        if pos is None:
            print("FAIL - item row not found")
            return False
        r = pos[0]
        local  = "".join(chars[r][7:30]).strip()    # Local Name column (cols 7..29).
        remote = "".join(chars[r][31:54]).strip()   # Remote Name column (cols 31..53).
        if local != "/local/bigfile.iso":
            print(f"FAIL - Local Name column shows {local!r}, expected '/local/bigfile.iso'")
            return False
        if remote != "bigfile.iso":
            print(f"FAIL - Remote Name column shows {remote!r}, expected 'bigfile.iso'")
            return False
        print("PASS")
        return True


def test_column_dividers_extend_to_item_rows():
    """The column control handles run down through the list items, not just the header."""
    print("TEST: parvion - column dividers extend down to item rows ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        chars = s.screen()[0]
        hdr = find_text(chars, "Speed")       # Header row.
        item = find_text(chars, "notes.txt")  # An item row.
        if hdr is None or item is None:
            print("FAIL - header / item row not found")
            return False
        hc = row_text(chars, hdr[0]).count("│")
        ic = row_text(chars, item[0]).count("│")
        if not (hc == 5 and ic == 5):
            print(f"FAIL - expected 5 dividers on header and item rows, got {hc} and {ic}")
            return False
        print("PASS")
        return True


def test_long_content_truncated_with_ellipsis():
    """A name wider than its column is tail-truncated with '…' at the column edge."""
    print("TEST: parvion - overlong content truncated with ellipsis ... ", end="", flush=True)
    with ParvionSession({"PARVION_DEMO_QUEUE": "1", "PARVION_DEMO_QUEUE_N": "1"}) as s:
        chars = s.screen()[0]
        pos = find_text(chars, "a_file_with_a_very")  # Prefix that survives truncation.
        if pos is None:
            print("FAIL - long-named row not found")
            return False
        r = pos[0]
        # Local Name column content spans cols 7..29 (width 24 - 1); the ellipsis lands on the last cell.
        if chars[r][29] != "…":
            print(f"FAIL - expected '…' at the Local Name column edge, got {chars[r][29]!r}")
            return False
        if "hscroll" in row_text(chars, r):
            print("FAIL - long name not truncated (tail still visible)")
            return False
        print("PASS")
        return True


def test_reason_initial_width_is_20():
    """The Failed tab's Reason column starts 20 cells wide (right border at col 82)."""
    print("TEST: parvion - Reason column initial width is 20 ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        if not click_label(s, "Failed ("):
            print("FAIL - Failed tab not found")
            return False
        chars = s.screen()[0]
        pos = find_text(chars, "Reason")
        if pos is None:
            print("FAIL - Reason header not found")
            return False
        borders = [c for c in range(COLS) if chars[pos[0]][c] == "│"]
        # reason_cx (86) + width 20 - 1 = 106 is the rightmost border on the Failed header row.
        if not borders or max(borders) != 106:
            print(f"FAIL - Reason right border at col {max(borders) if borders else None}, expected 106")
            return False
        print("PASS")
        return True


def test_double_click_autofits_column():
    """Double-clicking a column border auto-fits the column to its widest content + header,
    growing a too-narrow column back to fit."""
    print("TEST: parvion - double-click border auto-fits column ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        hdr = find_text(s.screen()[0], "Local Name")
        if hdr is None:
            print("FAIL - header not found")
            return False
        hr = hdr[0]
        # Shrink Local Name to the 2-cell minimum first (border 30 -> 8) so the auto-fit must grow it.
        s.drag(31, hr + 1, 3, hr + 1)
        mid = [c for c in range(7, COLS) if s.screen()[0][hr][c] == "│"]
        if not mid or min(mid) != 8:
            print(f"FAIL - Local Name border at {min(mid) if mid else None} after shrink, expected 8")
            return False
        # Double-click the (now col-8) border -> auto-fit to the widest local path
        # "/local/queued_00.dat" (20) + 1 for the border cell => width 21 => border at col 27.
        s.double_click(9, hr + 1)
        after = [c for c in range(7, COLS) if s.screen()[0][hr][c] == "│"]
        if not after or min(after) != 27:
            print(f"FAIL - Local Name border at {min(after) if after else None} after auto-fit, expected 27")
            return False
        print("PASS")
        return True


def test_min_column_width_is_two():
    """Grabbing a column border *on an item row* and dragging it far left clamps the column
    to a 2-cell minimum (also proving the handle is grabbable down the list, not just the header)."""
    print("TEST: parvion - minimum column width is 2 (drag from item row) ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        item = find_text(s.screen()[0], "notes.txt")
        hdr = find_text(s.screen()[0], "Speed")
        if item is None or hdr is None:
            print("FAIL - header / item row not found")
            return False
        ir = item[0]
        # The Local Name border sits at grid col 30 (q_name_x 7 + width 24 - 1) on every row; grab it
        # on the item row and drag hard left.
        s.drag(31, ir + 1, 5, ir + 1)
        chars = s.screen()[0]
        borders = [c for c in range(7, COLS) if chars[hdr[0]][c] == "│"]
        # Clamped to width 2 -> Name border at q_name_x 7 + 2 - 1 = 8 (would be 12 if the old min 6 held).
        if not borders or min(borders) != 8:
            print(f"FAIL - leftmost border at col {min(borders) if borders else None}, expected 8 (min width 2)")
            return False
        print("PASS")
        return True


def test_resize_handle_no_offset_drift_after_clamp():
    """Drag a column handle left past its minimum (overshoot), then back right in one gesture: the
    handle must end up under the cursor, not drifting right by the absorbed overshoot. Regression for
    the old delta-accumulating resize, where the handle kept a stale offset from the cursor."""
    print("TEST: parvion - queue resize handle has no offset drift after clamp ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        hdr = find_text(s.screen()[0], "Local Name")
        if hdr is None:
            print("FAIL - header not found")
            return False
        hr = hdr[0]
        b0 = 30       # Local Name border: q_name_x 7 + width 24 - 1.
        target = 18   # Final cursor grid column (between the min-clamp at 8 and b0).
        # One gesture: grab the border, overshoot hard left (clamps to the 2-cell min), then move
        # right to `target`. Mouse coords are 1-based, so the cursor's grid column is mouse_col - 1.
        s.drag_path([(b0 + 1, hr + 1), (2, hr + 1), (target + 1, hr + 1)])
        chars = s.screen()[0]
        borders = [c for c in range(7, COLS) if chars[hr][c] == "│"]
        left = min(borders) if borders else None
        # Position-based resize lands the border exactly under the cursor; the old code would leave it
        # to the right (target + the overshoot it had absorbed at the min clamp).
        if left != target:
            print(f"FAIL - handle at col {left}, expected {target} (a larger value = stale-offset drift)")
            return False
        print("PASS")
        return True


def test_pane_resize_handle_no_offset_drift_after_clamp():
    """Same no-drift guarantee for the file-preview pane's column handles (shared resize logic)."""
    print("TEST: parvion - file pane resize handle has no offset drift after clamp ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        hdr = find_text(s.screen()[0], "Modified")  # The file pane's column header.
        if hdr is None:
            print("FAIL - file pane header not found")
            return False
        hr = hdr[0]
        b0 = 24       # Name border: p_name_x 1 + width 24 - 1.
        target = 12
        # Overshoot to the far-left edge (mouse col 1) so the width clamps below the 2-cell minimum,
        # then drag right to `target`.
        s.drag_path([(b0 + 1, hr + 1), (1, hr + 1), (target + 1, hr + 1)])
        chars = s.screen()[0]
        borders = [c for c in range(1, 50) if chars[hr][c] == "│"]  # Local pane only (left half).
        left = min(borders) if borders else None
        if left != target:
            print(f"FAIL - handle at col {left}, expected {target} (a larger value = stale-offset drift)")
            return False
        print("PASS")
        return True


SEL_ACCENT = (137, 180, 250)  # theme::sel_bg_act — the focused-selection left accent (x=0).


def test_right_click_activates_queue():
    """Right-clicking the (initially unfocused) queue panel activates it: the selected row shows
    the focused left accent."""
    print("TEST: parvion - right-click activates the queue panel ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        pos = find_text(s.screen()[0], "notes.txt")  # A transferring item; queue starts unfocused.
        if pos is None:
            print("FAIL - item not found")
            return False
        r, c = pos
        s.click(c + 1, r + 1, button=2)  # Right-click: selects + activates the queue.
        accent = s.screen()[1][r][0]
        if accent != SEL_ACCENT:
            print(f"FAIL - queue not focused after right-click (x0 bg {accent})")
            return False
        print("PASS")
        return True


def test_queue_rubber_band_selects():
    """A rubber-band drag from the blank area up over items selects the swept rows."""
    print("TEST: parvion - queue rubber-band selects swept items ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        chars = s.screen()[0]
        r00 = find_text(chars, "queued_00")
        r02 = find_text(chars, "queued_02")
        if r00 is None or r02 is None:
            print("FAIL - queued rows not found")
            return False
        before = s.screen()[1][r02[0]][10]            # queued_02 name cell, unselected.
        blank = r02[0] + 3                            # a blank row below the last item (mouse 1-based).
        s.drag_path([(10, blank), (10, r00[0] + 1)])  # blank -> up to queued_00.
        after = s.screen()[1][r02[0]][10]
        if after == before:
            print(f"FAIL - bottommost swept item not selected (bg {before} -> {after})")
            return False
        print("PASS")
        return True


def test_queue_rubber_band_blank_deselects_last():
    """Dragging the rubber-band back down to the blank area deselects every item, including the
    bottommost (regression: the last item used to stay selected)."""
    print("TEST: parvion - queue rubber-band deselects bottommost on return ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        chars = s.screen()[0]
        r00 = find_text(chars, "queued_00")
        r02 = find_text(chars, "queued_02")
        if r00 is None or r02 is None:
            print("FAIL - queued rows not found")
            return False
        before = s.screen()[1][r02[0]][10]
        blank = r02[0] + 3
        # blank -> up over the items -> back down to the blank area, all in one gesture.
        s.drag_path([(10, blank), (10, r00[0] + 1), (10, blank)])
        after = s.screen()[1][r02[0]][10]
        if after != before:
            print(f"FAIL - bottommost item still selected after returning to blank (bg {before} -> {after})")
            return False
        print("PASS")
        return True


def test_queue_ctrl_drag_adds_to_selection():
    """Ctrl+left-drag over rows adds them to the existing selection (additive select) instead of
    replacing it, when the gesture starts on an unselected row."""
    print("TEST: parvion - queue Ctrl+drag adds to selection ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        chars = s.screen()[0]
        p0, p1, p2 = (find_text(chars, n) for n in ("queued_00", "queued_01", "queued_02"))
        if not (p0 and p1 and p2):
            print("FAIL - queued rows not found")
            return False
        unsel = s.screen()[1][p2[0]][10]              # queued_02 name cell, nothing selected yet.
        s.click(10, p0[0] + 1, button=0)             # Plain-click queued_00 -> sole selection.
        sel = s.screen()[1][p0[0]][10]
        if sel == unsel:
            print("FAIL - plain click did not select queued_00")
            return False
        # Ctrl+drag queued_01 -> queued_02 (button 16 = left+Ctrl); anchor unselected -> select mode.
        s.drag_path([(10, p1[0] + 1), (10, p2[0] + 1)], button=16)
        bg = s.screen()[1]
        if not (bg[p0[0]][10] == sel and bg[p1[0]][10] == sel and bg[p2[0]][10] == sel):
            print(f"FAIL - Ctrl+drag did not add to selection (00={bg[p0[0]][10]}, 01={bg[p1[0]][10]}, 02={bg[p2[0]][10]}, sel={sel})")
            return False
        print("PASS")
        return True


def test_queue_ctrl_drag_deselects():
    """Ctrl+left-drag over already-selected rows removes them (deselect) while keeping the rest of the
    selection, when the gesture starts on a selected row."""
    print("TEST: parvion - queue Ctrl+drag deselects swept rows ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        chars = s.screen()[0]
        p0, p1, p2 = (find_text(chars, n) for n in ("queued_00", "queued_01", "queued_02"))
        if not (p0 and p1 and p2):
            print("FAIL - queued rows not found")
            return False
        unsel = s.screen()[1][p2[0]][10]
        s.drag_path([(10, p0[0] + 1), (10, p2[0] + 1)], button=0)   # Plain rubber-band selects 00..02.
        sel = s.screen()[1][p0[0]][10]
        bg = s.screen()[1]
        if not (sel != unsel and bg[p1[0]][10] == sel and bg[p2[0]][10] == sel):
            print("FAIL - precondition: plain rubber-band did not select 00..02")
            return False
        # Ctrl+drag queued_01 -> queued_02; anchor selected -> deselect mode (queued_00 must survive).
        s.drag_path([(10, p1[0] + 1), (10, p2[0] + 1)], button=16)
        bg = s.screen()[1]
        if not (bg[p0[0]][10] == sel and bg[p1[0]][10] == unsel and bg[p2[0]][10] == unsel):
            print(f"FAIL - Ctrl+drag did not deselect swept rows (00={bg[p0[0]][10]}, 01={bg[p1[0]][10]}, 02={bg[p2[0]][10]})")
            return False
        print("PASS")
        return True


def test_pane_rubber_band_multi_select():
    """A rubber-band drag in the file pane highlights every swept row (multi-select)."""
    print("TEST: parvion - file pane rubber-band multi-select ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        pos = find_text(s.screen()[0], "/..")
        if pos is None:
            print("FAIL - '..' row not found")
            return False
        r = pos[0]
        before1 = s.screen()[1][r + 1][5]  # display row 1, unselected by default.
        s.drag_path([(6, r + 1), (6, r + 3)])  # display row 0 -> down to row 2.
        bg = s.screen()[1]
        a0, a1, a2, a3 = bg[r][5], bg[r + 1][5], bg[r + 2][5], bg[r + 3][5]
        if not (a1 != before1 and a1 == a0 and a2 == a0 and a3 != a0):
            print(f"FAIL - swept rows not all selected (rows {a0},{a1},{a2} / next {a3})")
            return False
        print("PASS")
        return True


def test_pane_ctrl_click_multi_select():
    """Ctrl-click adds a non-contiguous row to the file pane selection."""
    print("TEST: parvion - file pane ctrl-click multi-select ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        pos = find_text(s.screen()[0], "/..")
        if pos is None:
            print("FAIL - '..' row not found")
            return False
        r = pos[0]
        s.click(6, r + 2, button=0)    # Plain-click display row 1.
        s.click(6, r + 4, button=16)   # Ctrl-click display row 3 -> rows 1 and 3 selected.
        bg = s.screen()[1]
        a1, a2, a3 = bg[r + 1][5], bg[r + 2][5], bg[r + 3][5]
        if not (a1 == a3 and a2 != a1):
            print(f"FAIL - expected rows 1 and 3 selected, row 2 not (got {a1},{a2},{a3})")
            return False
        print("PASS")
        return True


def test_pane_header_has_dividers():
    """The file preview pane uses the transfer-table column dividers (│)."""
    print("TEST: parvion - file pane header has column dividers ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        chars = s.screen()[0]
        pos = find_text(chars, "Modified")  # Only the file pane's column header has "Modified".
        if pos is None:
            print("FAIL - file pane header not found")
            return False
        if "│" not in row_text(chars, pos[0]):
            print("FAIL - no column dividers on the file pane header")
            return False
        print("PASS")
        return True


def test_pane_header_menu_lists_column_toggles():
    """Right-clicking the file pane header opens a per-column show/hide menu (▣ shown by default)."""
    print("TEST: parvion - file pane header menu lists column toggles ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        pos = find_text(s.screen()[0], "Modified")
        if pos is None:
            print("FAIL - file pane header not found")
            return False
        s.click(pos[1] + 1, pos[0] + 1, button=2)  # Right-click the column header.
        chars = s.screen()[0]
        missing = [n for n in ("Name", "Size", "Modified") if not grid_contains(chars, "▣ " + n)]
        if missing:
            print(f"FAIL - menu missing shown (▣) toggles for {missing}")
            return False
        print("PASS")
        return True


def test_pane_selection_ends_at_last_column():
    """A selected file row is highlighted across the columns only, not the whole pane width."""
    print("TEST: parvion - file pane selection ends at last column ... ", end="", flush=True)
    with ParvionSession(DEMO_ENV) as s:
        pos = find_text(s.screen()[0], "/..")  # The ".." row in the (only) local pane.
        if pos is None:
            print("FAIL - '..' row not found")
            return False
        r, c = pos
        s.click(c + 1, r + 1, button=0)  # Focus + select the row.
        bg = s.screen()[1]
        col_bg   = bg[r][10]  # Inside the Name column (content ends at col 52).
        blank_bg = bg[r][54]  # Blank area right of the last column.
        if col_bg is None or col_bg == blank_bg:
            print(f"FAIL - highlight reached the blank area (col={col_bg}, blank={blank_bg})")
            return False
        print("PASS")
        return True


TESTS = [
    test_reason_column_has_resize_handle,
    test_right_click_activates_queue,
    test_selection_highlight_ends_at_last_column,
    test_expand_button_press_and_hold_feedback,
    test_blank_area_menu_lists_all_actions,
    test_right_click_blank_clears_selection,
    test_item_menu_lists_item_actions,
    test_transferring_item_menu_omits_pin,
    test_failed_succeeded_item_menu_omits_pause_and_pin,
    test_failed_succeeded_all_menu_omits_pause_all,
    test_header_menu_lists_column_toggles,
    test_header_menu_toggles_column_visibility,
    test_remove_item_via_menu,
    test_multiselect_remove_all_selected,
    test_keyboard_clear_finished_confirm,
    test_pin_to_top_reorders_pending,
    test_pause_then_start_progress_label,
    test_local_and_remote_name_columns,
    test_column_dividers_extend_to_item_rows,
    test_long_content_truncated_with_ellipsis,
    test_reason_initial_width_is_20,
    test_double_click_autofits_column,
    test_min_column_width_is_two,
    test_resize_handle_no_offset_drift_after_clamp,
    test_pane_resize_handle_no_offset_drift_after_clamp,
    test_queue_rubber_band_selects,
    test_queue_rubber_band_blank_deselects_last,
    test_queue_ctrl_drag_adds_to_selection,
    test_queue_ctrl_drag_deselects,
    test_pane_rubber_band_multi_select,
    test_pane_ctrl_click_multi_select,
    test_pane_header_has_dividers,
    test_pane_header_menu_lists_column_toggles,
    test_pane_selection_ends_at_last_column,
]


def main():
    if not os.path.isfile(VTM_TILE_BINARY):
        print(f"ERROR: vtm-tile binary not found at {VTM_TILE_BINARY}")
        print("Set VTM_TILE_BINARY env var or build vtm-tile first.")
        return 1
    kill_all_vtm()
    passed = failed = 0
    for test in TESTS:
        try:
            if test():
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
            time.sleep(0.4)
    total = passed + failed
    print(f"\n{'=' * 60}")
    print(f"Results: {passed}/{total} passed, {failed} failed")
    print(f"{'=' * 60}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
