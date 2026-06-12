#!/usr/bin/env python3
# Copyright (c) Shaobo Song
# Licensed under the MIT license.

"""
End-to-end TUI tests for the Parvion local file-preview pane operations:
  - clicking the blank area (right of / below the columns) never selects an item;
  - right-click context menus (blank: Refresh / Create Directory; item: Upload / Delete / Rename);
  - Create Directory, Delete and Rename act on the real local filesystem.

The app is launched as `vtm-tile -r parvion` with the child's cwd set to a fresh temp directory, so the
local pane lists a known, controlled set of files and the filesystem effects can be asserted on disk.
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
import shutil
import tempfile
import subprocess

VTM_TILE_BINARY = os.path.abspath(os.environ.get(
    "VTM_TILE_BINARY",
    os.path.join(os.path.dirname(__file__), "..", "build", "vtm-tile"),
))  # Absolute: the child chdir()s into the temp dir before exec.

COLS, ROWS = 120, 44
SETTLE = 1.5
_CSI = re.compile(rb"\x1b\[([\x30-\x3f]*)([\x20-\x2f]*)([\x40-\x7e])")


def kill_all_vtm():
    subprocess.run(["pkill", "-9", "-x", "vtm-tile"], capture_output=True)
    deadline = time.time() + 3.0
    while time.time() < deadline:
        if subprocess.run(["pgrep", "-x", "vtm-tile"], capture_output=True).returncode != 0:
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
            if parts[i + 1] == "2" and i + 4 < len(parts):
                try:
                    cur_bg = (int(parts[i + 2]), int(parts[i + 3]), int(parts[i + 4]))
                except ValueError:
                    pass
                i += 4
        i += 1
    return cur_bg


def replay(buf):
    chars = [[""] * COLS for _ in range(ROWS)]
    bg = [[None] * COLS for _ in range(ROWS)]
    cr = cc = 0
    cbg = None
    i, n = 0, len(buf)
    while i < n:
        b = buf[i]
        if b == 0x1b and i + 1 < n:
            c1 = buf[i + 1]
            if c1 == 0x5b:
                m = _CSI.match(buf, i)
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
                    cr = max(0, min(ROWS - 1, r - 1))
                    cc = max(0, min(COLS - 1, c - 1))
                elif final == b"m":
                    cbg = _parse_sgr(params, cbg)
                continue
            elif c1 == 0x5d:
                j = i + 2
                while j < n and buf[j] != 0x07:
                    if buf[j] == 0x1b and j + 1 < n and buf[j + 1] == 0x5c:
                        j += 1
                        break
                    j += 1
                i = j + 1
                continue
            else:
                i += 2
                continue
        if b == 0x0d:
            cc = 0
            i += 1
            continue
        if b == 0x0a:
            cr = min(ROWS - 1, cr + 1)
            i += 1
            continue
        if b < 0x20:
            i += 1
            continue
        if b >= 0xc0:
            j = i + 1
            while j < n and 0x80 <= buf[j] < 0xc0:
                j += 1
            g = buf[i:j].decode("utf-8", "replace")
            i = j
        else:
            g = chr(b)
            i += 1
        if 0 <= cr < ROWS and 0 <= cc < COLS:
            chars[cr][cc] = g
            bg[cr][cc] = cbg
            if cc < COLS - 1:
                cc += 1
    return chars, bg


class ParvionSession:
    def __init__(self, cwd, env=None):
        self.cwd = cwd
        self.env = env or {"PARVION_DEMO_QUEUE": "1"}
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
            os.chdir(self.cwd)
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

    def write(self, data, settle=0.5):
        os.write(self.master_fd, data.encode() if isinstance(data, str) else data)
        self.feed(settle)

    def click(self, col, row, button=0, settle=0.6):
        os.write(self.master_fd, f"\x1b[<{button};{col};{row}M".encode())
        time.sleep(0.05)
        os.write(self.master_fd, f"\x1b[<{button};{col};{row}m".encode())
        self.feed(settle)

    def drag_path(self, points, button=0, settle=0.6):
        """Press at points[0], motion (button|32) through the rest, release at points[-1].
        button=16 sends a Ctrl-modified left drag (SGR: left=0 | ctrl=16)."""
        c0, r0 = points[0]
        os.write(self.master_fd, f"\x1b[<{button};{c0};{r0}M".encode())
        time.sleep(0.04)
        for c, r in points[1:]:
            os.write(self.master_fd, f"\x1b[<{button + 32};{c};{r}M".encode())
            time.sleep(0.04)
        cl, rl = points[-1]
        os.write(self.master_fd, f"\x1b[<{button};{cl};{rl}m".encode())
        self.feed(settle)


def row_text(chars, r):
    return "".join(c if c else " " for c in chars[r])


def find_text(chars, needle):
    for r in range(ROWS):
        c = row_text(chars, r).find(needle)
        if c >= 0:
            return (r, c)
    return None


def grid_contains(chars, needle):
    return any(needle in row_text(chars, r) for r in range(ROWS))


def make_tree():
    d = tempfile.mkdtemp(prefix="parvionpane_")
    open(os.path.join(d, "alpha.txt"), "w").close()
    open(os.path.join(d, "beta.txt"), "w").close()
    os.mkdir(os.path.join(d, "gamma"))
    return d


# ----------------------------------- tests -----------------------------------

def test_blank_click_does_not_select():
    """Clicking the blank area to the right of the columns does not select a file."""
    print("TEST: parvion pane - blank-area click does not select ... ", end="", flush=True)
    d = make_tree()
    try:
        with ParvionSession(d) as s:
            pos = find_text(s.screen()[0], "alpha.txt")
            if pos is None:
                print("FAIL - alpha.txt not listed")
                return False
            r, c = pos
            before = s.screen()[1][r][5]
            s.click(c + 1, r + 1, button=0)        # Click within the columns -> selects.
            sel = s.screen()[1][r][5]
            s.click(54, r + 1, button=0)           # Click the blank area right of the columns.
            after = s.screen()[1][r][5]
            if sel == before:
                print("FAIL - in-column click did not select")
                return False
            if after != before:
                print(f"FAIL - blank-area click left a selection (bg {before} -> {after})")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_right_click_blank_clears_selection():
    """Right-clicking the blank area clears the current file selection (like left-click does)."""
    print("TEST: parvion pane - right-click blank clears selection ... ", end="", flush=True)
    d = make_tree()
    try:
        with ParvionSession(d) as s:
            pos = find_text(s.screen()[0], "alpha.txt")
            if pos is None:
                print("FAIL - alpha.txt not listed")
                return False
            r, c = pos
            before = s.screen()[1][r][5]             # alpha.txt row, unselected.
            s.click(c + 1, r + 1, button=0)          # Left-click selects alpha.
            if s.screen()[1][r][5] == before:
                print("FAIL - in-column click did not select")
                return False
            dd = find_text(s.screen()[0], "/..")
            s.click(5, dd[0] + 7, button=2)          # Right-click a blank row below the list.
            s.write("\x1b")                          # Dismiss the blank menu.
            after = s.screen()[1][r][5]
            if after != before:
                print(f"FAIL - right-click blank left a selection (bg {before} -> {after})")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_blank_context_menu():
    """Right-click on the blank area opens Refresh / Create Directory."""
    print("TEST: parvion pane - blank context menu ... ", end="", flush=True)
    d = make_tree()
    try:
        with ParvionSession(d) as s:
            dd = find_text(s.screen()[0], "/..")
            if dd is None:
                print("FAIL - '..' row not found")
                return False
            s.click(5, dd[0] + 7, button=2)  # Right-click a blank row below the list.
            chars = s.screen()[0]
            missing = [w for w in ("Refresh", "Create Directory") if not grid_contains(chars, w)]
            if missing:
                print(f"FAIL - blank menu missing {missing}")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_item_context_menu():
    """Right-click on a file opens Upload / Delete / Rename (local pane)."""
    print("TEST: parvion pane - item context menu ... ", end="", flush=True)
    d = make_tree()
    try:
        with ParvionSession(d) as s:
            pos = find_text(s.screen()[0], "alpha.txt")
            if pos is None:
                print("FAIL - alpha.txt not listed")
                return False
            s.click(pos[1] + 1, pos[0] + 1, button=2)
            chars = s.screen()[0]
            missing = [w for w in ("Upload", "Delete", "Rename") if not grid_contains(chars, w)]
            if missing:
                print(f"FAIL - item menu missing {missing}")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_create_directory():
    """Blank menu -> Create Directory -> type a name -> Enter creates the directory on disk."""
    print("TEST: parvion pane - Create Directory ... ", end="", flush=True)
    d = make_tree()
    try:
        with ParvionSession(d) as s:
            dd = find_text(s.screen()[0], "/..")
            s.click(5, dd[0] + 7, button=2)         # Right-click blank -> blank menu.
            cd = find_text(s.screen()[0], "Create Directory")
            if cd is None:
                print("FAIL - 'Create Directory' not in menu")
                return False
            s.click(cd[1] + 1, cd[0] + 1, button=0)  # Click it -> input mode.
            s.write("newfolder")
            s.write("\r")
            if not os.path.isdir(os.path.join(d, "newfolder")):
                print("FAIL - directory not created on disk")
                return False
            if not grid_contains(s.screen()[0], "newfolder"):
                print("FAIL - new directory not shown in the listing")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_delete_item():
    """Item menu -> Delete removes the file from disk and the listing."""
    print("TEST: parvion pane - Delete ... ", end="", flush=True)
    d = make_tree()
    try:
        with ParvionSession(d) as s:
            pos = find_text(s.screen()[0], "beta.txt")
            if pos is None:
                print("FAIL - beta.txt not listed")
                return False
            s.click(pos[1] + 1, pos[0] + 1, button=2)   # Right-click -> item menu (selects beta).
            de = find_text(s.screen()[0], "Delete")
            if de is None:
                print("FAIL - 'Delete' not in menu")
                return False
            s.click(de[1] + 1, de[0] + 1, button=0)
            # Delete now asks for confirmation; the message names the single victim.
            if not grid_contains(s.screen()[0], "Delete 'beta.txt'?"):
                print("FAIL - confirmation dialog not shown")
                return False
            s.write("\r")  # Enter -> Confirm (the default selection).
            s.feed(0.6)
            if os.path.exists(os.path.join(d, "beta.txt")):
                print("FAIL - file still on disk after Delete")
                return False
            if grid_contains(s.screen()[0], "beta.txt"):
                print("FAIL - file still shown after Delete")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_delete_cancel():
    """Esc in the Delete confirmation dialog cancels: nothing is removed."""
    print("TEST: parvion pane - Delete cancelled by Esc ... ", end="", flush=True)
    d = make_tree()
    try:
        with ParvionSession(d) as s:
            pos = find_text(s.screen()[0], "beta.txt")
            if pos is None:
                print("FAIL - beta.txt not listed")
                return False
            s.click(pos[1] + 1, pos[0] + 1, button=2)   # Right-click -> item menu (selects beta).
            de = find_text(s.screen()[0], "Delete")
            if de is None:
                print("FAIL - 'Delete' not in menu")
                return False
            s.click(de[1] + 1, de[0] + 1, button=0)
            if not grid_contains(s.screen()[0], "Delete 'beta.txt'?"):
                print("FAIL - confirmation dialog not shown")
                return False
            s.write("\x1b")  # Esc -> Cancel.
            s.feed(0.6)
            if not os.path.exists(os.path.join(d, "beta.txt")):
                print("FAIL - file deleted despite cancelling")
                return False
            if not grid_contains(s.screen()[0], "beta.txt"):
                print("FAIL - file no longer listed after cancel")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_rename_item():
    """Item menu -> Rename -> edit -> Enter renames the file on disk."""
    print("TEST: parvion pane - Rename ... ", end="", flush=True)
    d = make_tree()
    try:
        with ParvionSession(d) as s:
            pos = find_text(s.screen()[0], "alpha.txt")
            if pos is None:
                print("FAIL - alpha.txt not listed")
                return False
            s.click(pos[1] + 1, pos[0] + 1, button=2)
            rn = find_text(s.screen()[0], "Rename")
            if rn is None:
                print("FAIL - 'Rename' not in menu")
                return False
            s.click(rn[1] + 1, rn[0] + 1, button=0)  # Input mode, seeded with "alpha.txt".
            # Clear the seeded name (9 chars) and type a new one.
            s.write("\x7f" * 12)
            s.write("renamed.txt")
            s.write("\r")
            if os.path.exists(os.path.join(d, "alpha.txt")) or not os.path.exists(os.path.join(d, "renamed.txt")):
                print("FAIL - file not renamed on disk")
                return False
            if not grid_contains(s.screen()[0], "renamed.txt"):
                print("FAIL - renamed file not shown")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


SEL_ACCENT = (137, 180, 250)  # theme::sel_bg_act — the focused-selection left accent (x=0).


def test_ctrl_click_keeps_focus():
    """Ctrl+Left-click to (de)select a row keeps the pane focused (the selected row keeps the accent)."""
    print("TEST: parvion pane - Ctrl-click keeps focus ... ", end="", flush=True)
    d = make_tree()
    try:
        with ParvionSession(d) as s:
            pos = find_text(s.screen()[0], "alpha.txt")
            if pos is None:
                print("FAIL - alpha.txt not listed")
                return False
            r = pos[0]
            s.click(pos[1] + 1, r + 1, button=16)  # Ctrl+left-click selects alpha.
            accent = s.screen()[1][r][0]
            if accent != SEL_ACCENT:
                print(f"FAIL - pane lost focus after Ctrl-click (x0 bg {accent})")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_right_click_activates_pane():
    """Right-clicking activates the pane even when another panel held focus."""
    print("TEST: parvion pane - right-click activates the pane ... ", end="", flush=True)
    d = make_tree()
    try:
        with ParvionSession(d) as s:
            tab = find_text(s.screen()[0], "Transferring (")  # Move focus to the queue panel.
            if tab:
                s.click(tab[1] + 1, tab[0] + 1, button=0)
            pos = find_text(s.screen()[0], "alpha.txt")
            if pos is None:
                print("FAIL - alpha.txt not listed")
                return False
            r, c = pos
            s.click(c + 1, r + 1, button=2)  # Right-click the pane item: selects + activates.
            s.write("\x1b")                  # Dismiss the menu.
            accent = s.screen()[1][r][0]
            if accent != SEL_ACCENT:
                print(f"FAIL - pane not focused after right-click (x0 bg {accent})")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_ctrl_drag_adds_to_selection():
    """Ctrl+left-drag over rows adds the swept rows to the existing selection (additive select) when
    the gesture starts on an unselected row, instead of replacing the selection like a plain drag."""
    print("TEST: parvion pane - Ctrl+drag adds to selection ... ", end="", flush=True)
    d = make_tree()  # display rows: 0="/..", 1="/gamma", 2="alpha.txt", 3="beta.txt".
    try:
        with ParvionSession(d) as s:
            pos = find_text(s.screen()[0], "/..")
            if pos is None:
                print("FAIL - '..' row not found")
                return False
            r = pos[0]
            unsel = s.screen()[1][r + 3][5]                 # beta.txt, unselected at start.
            s.click(6, r + 2, button=0)                     # Plain-click display row 1 (/gamma).
            sel = s.screen()[1][r + 1][5]
            if sel == unsel:
                print("FAIL - plain click did not select the row")
                return False
            # Ctrl+drag display rows 2 -> 3 (alpha.txt -> beta.txt); anchor unselected -> select mode.
            s.drag_path([(6, r + 3), (6, r + 4)], button=16)
            bg = s.screen()[1]
            if not (bg[r + 1][5] == sel and bg[r + 2][5] == sel and bg[r + 3][5] == sel):
                print(f"FAIL - Ctrl+drag did not add rows (1={bg[r+1][5]}, 2={bg[r+2][5]}, 3={bg[r+3][5]}, sel={sel})")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_ctrl_drag_deselects():
    """Ctrl+left-drag over already-selected rows removes them (deselect) when the gesture starts on a
    selected row, while keeping the rest of the selection."""
    print("TEST: parvion pane - Ctrl+drag deselects swept rows ... ", end="", flush=True)
    d = make_tree()
    try:
        with ParvionSession(d) as s:
            pos = find_text(s.screen()[0], "/..")
            if pos is None:
                print("FAIL - '..' row not found")
                return False
            r = pos[0]
            unsel = s.screen()[1][r + 3][5]                 # beta.txt, unselected at start.
            s.drag_path([(6, r + 2), (6, r + 4)], button=0) # Plain rubber-band selects rows 1..3.
            sel = s.screen()[1][r + 1][5]
            bg = s.screen()[1]
            if not (sel != unsel and bg[r + 2][5] == sel and bg[r + 3][5] == sel):
                print("FAIL - precondition: rubber-band did not select rows 1..3")
                return False
            # Ctrl+drag rows 2 -> 3 (alpha -> beta); anchor selected -> deselect (row 1 must survive).
            s.drag_path([(6, r + 3), (6, r + 4)], button=16)
            bg = s.screen()[1]
            if not (bg[r + 1][5] == sel and bg[r + 2][5] == unsel and bg[r + 3][5] == unsel):
                print(f"FAIL - Ctrl+drag did not deselect rows (1={bg[r+1][5]}, 2={bg[r+2][5]}, 3={bg[r+3][5]})")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


TESTS = [
    test_ctrl_click_keeps_focus,
    test_right_click_activates_pane,
    test_blank_click_does_not_select,
    test_right_click_blank_clears_selection,
    test_ctrl_drag_adds_to_selection,
    test_ctrl_drag_deselects,
    test_blank_context_menu,
    test_item_context_menu,
    test_create_directory,
    test_delete_item,
    test_delete_cancel,
    test_rename_item,
]


def main():
    if not os.path.isfile(VTM_TILE_BINARY):
        print(f"ERROR: vtm-tile binary not found at {VTM_TILE_BINARY}")
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
