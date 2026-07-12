#!/usr/bin/env python3
# Copyright (c) Shaobo Song
# Licensed under the MIT license.

"""
End-to-end TUI tests for the Parvion local file-preview pane operations:
  - clicking the blank area (right of / below the columns) never selects an item;
  - right-click context menus (blank: Refresh / Create Directory; item: Upload / Copy full path / Delete / Rename);
  - Create Directory, Delete and Rename act on the real local filesystem.

The app is launched as `vtm-tile -r parvion` with the child's cwd set to a fresh temp directory, so the
local pane lists a known, controlled set of files and the filesystem effects can be asserted on disk.
"""

import os
import re
import sys
import pty
import time
import base64
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
_OSC52 = re.compile(rb"\x1b\]52;[^;]*;([A-Za-z0-9+/=]+)(?:\x07|\x1b\\)")


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

    def double_click(self, col, row, button=0, settle=0.6):
        for _ in range(2):
            os.write(self.master_fd, f"\x1b[<{button};{col};{row}M".encode())
            time.sleep(0.02)
            os.write(self.master_fd, f"\x1b[<{button};{col};{row}m".encode())
            time.sleep(0.02)
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


SORT_GLYPHS = ("↕", "↑", "↓")


def table_header_field(chars, title, row=None):
    """Return a table header's bounds and sort marker without fixed coordinates."""
    if row is None:
        pos = find_text(chars, title)
        if pos is None:
            return None
        row, col = pos
    else:
        col = row_text(chars, row).find(title)
        if col < 0:
            return None
    dividers = [c for c in range(COLS) if chars[row][c] == "│"]
    left = max((c + 1 for c in dividers if c < col), default=0)
    right = min((c for c in dividers if c >= col + len(title)), default=COLS)
    markers = [(c, chars[row][c]) for c in range(left, right)
               if chars[row][c] in SORT_GLYPHS]
    marker_col, marker = markers[0] if markers else (-1, "")
    return row, col, left, right, marker_col, marker


def click_table_header(session, title, row=None):
    field = table_header_field(session.screen()[0], title, row)
    if field is None:
        return False
    hr, col, *_ = field
    session.click(col + 1, hr + 1)
    return True


def named_row_order(chars, names):
    found = []
    for name in names:
        pos = find_text(chars, name)
        if pos is None:
            return None
        found.append((pos[0], name))
    return [name for _, name in sorted(found)]


def make_tree():
    d = tempfile.mkdtemp(prefix="parvionpane_")
    open(os.path.join(d, "alpha.txt"), "w").close()
    open(os.path.join(d, "beta.txt"), "w").close()
    os.mkdir(os.path.join(d, "gamma"))
    return d


# ----------------------------------- tests -----------------------------------


def test_shared_table_file_sort_configuration():
    """File headers sort by name/bytes/mtime while keeping parent and directories first."""
    print("TEST: parvion pane - shared table file sort configuration ... ", end="", flush=True)
    d = tempfile.mkdtemp(prefix="parvionsort_")
    try:
        os.mkdir(os.path.join(d, "adir"))
        os.mkdir(os.path.join(d, "zdir"))
        with open(os.path.join(d, "alpha.bin"), "wb") as f:
            f.write(b"a" * 9)
        with open(os.path.join(d, "zeta.bin"), "wb") as f:
            f.write(b"z")
        os.utime(os.path.join(d, "alpha.bin"), (1_000_000_000, 1_000_000_000))
        os.utime(os.path.join(d, "zeta.bin"),  (1_100_000_000, 1_100_000_000))
        names = ("adir", "zdir", "alpha.bin", "zeta.bin")
        with ParvionSession(d) as s:
            chars = s.screen()[0]
            modified = table_header_field(chars, "Modified")
            if modified is None:
                print("FAIL - file headers not found"); return False
            hr = modified[0]
            bad = [(title, table_header_field(chars, title, hr))
                   for title in ("Name", "Size", "Modified")
                   if not table_header_field(chars, title, hr)
                   or table_header_field(chars, title, hr)[5] != "↕"]
            if bad:
                print(f"FAIL - headers are not sortable: {bad}"); return False

            # Name descending reverses values within the directory/file groups, never the groups.
            click_table_header(s, "Name", hr)  # ascending
            click_table_header(s, "Name", hr)  # descending
            chars = s.screen()[0]
            if table_header_field(chars, "Name", hr)[5] != "↓":
                print("FAIL - Name did not enter descending mode"); return False
            if named_row_order(chars, names) != ["zdir", "adir", "zeta.bin", "alpha.bin"]:
                print(f"FAIL - descending grouped Name order: {named_row_order(chars, names)}"); return False
            parent = find_text(chars, "/..")
            first = find_text(chars, "zdir")
            if not parent or not first or parent[0] >= first[0]:
                print("FAIL - parent row was not pinned above descending sort"); return False
            click_table_header(s, "Name", hr)  # source order
            if named_row_order(s.screen()[0], names) != ["adir", "zdir", "alpha.bin", "zeta.bin"]:
                print("FAIL - third Name click did not restore source order"); return False

            # Size is numeric: the one-byte zeta file precedes the nine-byte alpha file.
            click_table_header(s, "Size", hr)
            if named_row_order(s.screen()[0], names) != ["adir", "zdir", "zeta.bin", "alpha.bin"]:
                print(f"FAIL - ascending numeric Size order: {named_row_order(s.screen()[0], names)}"); return False

            # Modified descending puts the newer zeta file first, still below directories.
            click_table_header(s, "Modified", hr)
            click_table_header(s, "Modified", hr)
            if named_row_order(s.screen()[0], names) != ["adir", "zdir", "zeta.bin", "alpha.bin"]:
                print(f"FAIL - descending Modified order: {named_row_order(s.screen()[0], names)}"); return False
        print("PASS")
        return True
    finally:
        shutil.rmtree(d, ignore_errors=True)

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
    """Right-click on a file opens Upload / Copy full path / Delete / Rename (local pane)."""
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
            missing = [w for w in ("Upload", "Copy full path", "Delete", "Rename") if not grid_contains(chars, w)]
            if missing:
                print(f"FAIL - item menu missing {missing}")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_copy_full_path():
    """Item menu -> Copy full path writes the selected item's absolute path to the clipboard."""
    print("TEST: parvion pane - Copy full path ... ", end="", flush=True)
    d = make_tree()
    try:
        with ParvionSession(d) as s:
            pos = find_text(s.screen()[0], "alpha.txt")
            if pos is None:
                print("FAIL - alpha.txt not listed")
                return False
            s.click(pos[1] + 1, pos[0] + 1, button=2)
            cp = find_text(s.screen()[0], "Copy full path")
            if cp is None:
                print("FAIL - 'Copy full path' not in menu")
                return False
            before = len(s._buf)
            s.click(cp[1] + 1, cp[0] + 1, button=0)
            s.feed(0.6)
            hits = _OSC52.findall(s._buf[before:])
            if not hits:
                print("FAIL - no clipboard sequence emitted")
                return False
            got = base64.b64decode(hits[-1]).decode("utf-8", "replace")
            want = os.path.join(d, "alpha.txt")
            if got != want:
                print(f"FAIL - clipboard {got!r} != {want!r}")
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


def test_delete_key_item():
    """Delete key on a selected pane item uses the same confirmation/delete path as the item menu."""
    print("TEST: parvion pane - Delete key ... ", end="", flush=True)
    d = make_tree()
    try:
        with ParvionSession(d) as s:
            pos = find_text(s.screen()[0], "beta.txt")
            if pos is None:
                print("FAIL - beta.txt not listed")
                return False
            s.click(pos[1] + 1, pos[0] + 1, button=0)
            s.write("\x1b[3~", settle=0.4)
            if not grid_contains(s.screen()[0], "Delete 'beta.txt'?"):
                print("FAIL - confirmation dialog not shown")
                return False
            s.write("\r", settle=0.8)
            if os.path.exists(os.path.join(d, "beta.txt")):
                print("FAIL - file still on disk after Delete key")
                return False
            if grid_contains(s.screen()[0], "beta.txt"):
                print("FAIL - file still shown after Delete key")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_delete_preserves_viewport_and_navigation_selection():
    """A deletion refresh keeps the viewport and selects the adjacent surviving row coherently."""
    print("TEST: parvion pane - Delete preserves viewport/navigation selection ... ", end="", flush=True)
    d = tempfile.mkdtemp(prefix="parvionpane_delete_view_")
    try:
        for i in range(48):
            open(os.path.join(d, f"row_{i:02d}.txt"), "w").close()
        with ParvionSession(d) as s:
            parent = find_text(s.screen()[0], "/..")
            if parent is None:
                print("FAIL - parent row not found")
                return False
            s.click(parent[1] + 1, parent[0] + 1)
            s.write("\x1b[B" * 41, settle=1.0)  # Select row_40 and scroll it into view.
            victim = find_text(s.screen()[0], "row_40.txt")
            if victim is None:
                print("FAIL - deletion target not visible")
                return False
            selected_bg = s.screen()[1][victim[0]][victim[1]]
            s.write("\x1b[3~", settle=0.4)
            if not grid_contains(s.screen()[0], "Delete 'row_40.txt'?"):
                print("FAIL - delete confirmation not shown")
                return False
            s.write("\r", settle=1.2)
            chars, bg = s.screen()
            if find_text(chars, "row_40.txt") is not None:
                print("FAIL - deleted item remains visible")
                return False
            adjacent = find_text(chars, "row_41.txt")
            if adjacent is None or bg[adjacent[0]][adjacent[1]] != selected_bg:
                print("FAIL - refresh did not select the adjacent surviving row")
                return False
            if find_text(chars, "/..") is not None:
                print("FAIL - deletion refresh reset the viewport to the top")
                return False
            s.write("\x1b[B")
            following = find_text(s.screen()[0], "row_42.txt")
            if following is None or s.screen()[1][following[0]][following[1]] != selected_bg:
                print("FAIL - arrow navigation did not continue from the visible selection")
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


def test_shared_table_keyboard_activation_and_parent():
    """The table owns focus after a row click, but pane-level Enter/Backspace actions still fire."""
    print("TEST: parvion pane - shared table routes Enter/Backspace activation ... ", end="", flush=True)
    d = make_tree()
    nested = os.path.join(d, "gamma", "inside_gamma.txt")
    open(nested, "w").close()
    try:
        with ParvionSession(d) as s:
            folder = find_text(s.screen()[0], "/gamma")
            if folder is None:
                print("FAIL - gamma directory not listed")
                return False
            s.click(folder[1] + 1, folder[0] + 1)
            s.write("\r", settle=0.8)
            if not grid_contains(s.screen()[0], "inside_gamma.txt"):
                print("FAIL - Enter did not activate the selected directory")
                return False
            if grid_contains(s.screen()[0], "alpha.txt"):
                print("FAIL - listing did not change after entering gamma")
                return False
            s.write("\x7f", settle=0.8)  # Backspace returns to the parent directory.
            if not grid_contains(s.screen()[0], "alpha.txt"):
                print("FAIL - Backspace did not return to the parent directory")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_enter_after_directory_change_activates_visible_parent():
    """Entering a directory replaces the selection with '..'; immediate Enter must activate that
    visible row, not the stale table cursor at the directory's former numeric offset."""
    print("TEST: parvion pane - Enter after directory change activates visible '..' ... ", end="", flush=True)
    d = make_tree()
    nested = os.path.join(d, "gamma", "inside_gamma.txt")
    open(nested, "w").close()
    try:
        with ParvionSession(d) as s:
            folder = find_text(s.screen()[0], "/gamma")
            if folder is None:
                print("FAIL - gamma directory not listed")
                return False
            s.click(folder[1] + 1, folder[0] + 1)
            s.write("\r", settle=0.8)
            parent = find_text(s.screen()[0], "/..")
            if parent is None or not grid_contains(s.screen()[0], "inside_gamma.txt"):
                print("FAIL - did not enter gamma")
                return False
            selected_bg = s.screen()[1][parent[0]][parent[1]]
            inside = find_text(s.screen()[0], "inside_gamma.txt")
            if inside is None or s.screen()[1][inside[0]][inside[1]] == selected_bg:
                print("FAIL - '..' is not the sole visible selection after navigation")
                return False
            s.write("\r", settle=0.8)
            if not grid_contains(s.screen()[0], "alpha.txt"):
                print("FAIL - immediate Enter did not return through '..'")
                return False
            if grid_contains(s.screen()[0], "inside_gamma.txt"):
                print("FAIL - remained in gamma after activating visible '..'")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_shared_table_keyboard_navigation_scrolls_to_last_row():
    """Repeated Down keeps the keyboard cursor selected and scrolls a long pane to its last row."""
    print("TEST: parvion pane - shared table keyboard navigation scrolls selection ... ", end="", flush=True)
    d = tempfile.mkdtemp(prefix="parvionpane_many_")
    try:
        for i in range(48):
            open(os.path.join(d, f"row_{i:02d}.txt"), "w").close()
        with ParvionSession(d) as s:
            first = find_text(s.screen()[0], "/..")
            if first is None:
                print("FAIL - '..' row not found")
                return False
            s.click(first[1] + 1, first[0] + 1)
            selected_bg = s.screen()[1][first[0]][first[1]]
            if selected_bg is None:
                print("FAIL - first row did not become selected")
                return False
            s.write("\x1b[B" * 60, settle=1.0)  # Down Arrow; clamping should stop on the last row.
            chars, bg = s.screen()
            last = find_text(chars, "row_47.txt")
            if last is None:
                print("FAIL - keyboard navigation did not reveal the final row")
                return False
            if bg[last[0]][last[1]] != selected_bg:
                print(f"FAIL - final row is visible but not selected (bg {bg[last[0]][last[1]]})")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_typeahead_and_navigation_share_selection_cursor():
    """Arrow navigation continues from the item selected by ASCII type-ahead."""
    print("TEST: parvion pane - type-ahead and arrows share selection cursor ... ", end="", flush=True)
    d = tempfile.mkdtemp(prefix="parvionpane_keycursor_")
    try:
        for name in ("alpha.txt", "beta.txt", "charlie.txt", "delta.txt"):
            open(os.path.join(d, name), "w").close()
        with ParvionSession(d) as s:
            alpha = find_text(s.screen()[0], "alpha.txt")
            if alpha is None:
                print("FAIL - alpha row not found")
                return False
            s.click(alpha[1] + 1, alpha[0] + 1)
            selected_bg = s.screen()[1][alpha[0]][alpha[1]]
            s.write("c")
            charlie = find_text(s.screen()[0], "charlie.txt")
            if charlie is None or s.screen()[1][charlie[0]][charlie[1]] != selected_bg:
                print("FAIL - type-ahead did not select charlie")
                return False
            s.write("\x1b[B")
            delta = find_text(s.screen()[0], "delta.txt")
            if delta is None or s.screen()[1][delta[0]][delta[1]] != selected_bg:
                print("FAIL - Down continued from the pre-type-ahead cursor")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_page_home_end_keys_navigate_and_select():
    """Paging visits visible edges before scrolling; Home/End select boundaries."""
    print("TEST: parvion pane - Page/Home/End navigate and select ... ", end="", flush=True)
    d = tempfile.mkdtemp(prefix="parvionpane_pagekeys_")
    try:
        for i in range(48):
            open(os.path.join(d, f"row_{i:02d}.txt"), "w").close()
        with ParvionSession(d) as s:
            start = find_text(s.screen()[0], "row_02.txt")
            if start is None:
                print("FAIL - starting row not found")
                return False
            s.click(start[1] + 1, start[0] + 1)
            selected_bg = s.screen()[1][start[0]][start[1]]
            visible = [(find_text(s.screen()[0], f"row_{i:02d}.txt"), i) for i in range(48)]
            bottom = max((item for item in visible if item[0] is not None), key=lambda item: item[0][0])
            s.write("\x1b[6~")  # First PageDown selects the current visible bottom.
            pos = find_text(s.screen()[0], f"row_{bottom[1]:02d}.txt")
            if (find_text(s.screen()[0], "row_02.txt") is None or pos is None
             or s.screen()[1][pos[0]][pos[1]] != selected_bg):
                print("FAIL - PageDown did not select the current visible bottom")
                return False
            s.write("\x1b[5~")  # First PageUp selects the current visible top.
            parent = find_text(s.screen()[0], "/..")
            if parent is None or s.screen()[1][parent[0]][parent[1]] != selected_bg:
                print("FAIL - PageUp did not select the current visible top")
                return False
            s.write("\x1b[6~" * 2)  # Edge first, then advance one page.
            if find_text(s.screen()[0], "row_02.txt") is not None:
                print("FAIL - second PageDown did not advance the viewport")
                return False
            s.write("\x1b[5~" * 2)  # Edge first, then return to the previous page.
            parent = find_text(s.screen()[0], "/..")
            if parent is None or s.screen()[1][parent[0]][parent[1]] != selected_bg:
                print("FAIL - second PageUp did not return to the previous page")
                return False
            s.write("\x1b[F")   # End selects and reveals the final item.
            last = find_text(s.screen()[0], "row_47.txt")
            if last is None or s.screen()[1][last[0]][last[1]] != selected_bg:
                print("FAIL - End did not select the final item")
                return False
            s.write("\x1b[H")   # Home selects and reveals the first item.
            parent = find_text(s.screen()[0], "/..")
            if parent is None or s.screen()[1][parent[0]][parent[1]] != selected_bg:
                print("FAIL - Home did not select the first item")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_paging_prioritizes_visible_page_after_scroll():
    """Paging uses visible edges after independent scrollbar movement."""
    print("TEST: parvion pane - paging prioritizes visible page ... ", end="", flush=True)
    d = tempfile.mkdtemp(prefix="parvionpane_page_offscreen_")
    try:
        for i in range(48):
            open(os.path.join(d, f"row_{i:02d}.txt"), "w").close()
        with ParvionSession(d) as s:
            start = find_text(s.screen()[0], "row_02.txt")
            if start is None:
                print("FAIL - starting row not found")
                return False
            s.click(start[1] + 1, start[0] + 1)
            selected_bg = s.screen()[1][start[0]][start[1]]
            chars = s.screen()[0]
            tracks = {}
            for r, row in enumerate(chars):
                for c, ch in enumerate(row[:COLS // 2]):
                    if ch in ("▐", "█"):
                        tracks.setdefault(c, []).append(r)
            if not tracks:
                print("FAIL - local pane vertical scrollbar not found")
                return False
            col, rows = max(tracks.items(), key=lambda item: len(item[1]))
            s.drag_path([(col + 1, min(rows) + 1), (col + 1, max(rows) + 1)])
            chars = s.screen()[0]
            visible = [(find_text(chars, f"row_{i:02d}.txt"), i) for i in range(48)]
            top = min((item for item in visible if item[0] is not None), key=lambda item: item[0][0])
            if find_text(chars, "row_47.txt") is None:
                print("FAIL - scrollbar did not move selection off-screen")
                return False
            s.write("\x1b[6~")
            chars, bg = s.screen()
            last = find_text(chars, "row_47.txt")
            if last is None or bg[last[0]][last[1]] != selected_bg:
                print("FAIL - PageDown did not prioritize the visible bottom")
                return False
            s.write("\x1b[5~")
            pos = find_text(s.screen()[0], f"row_{top[1]:02d}.txt")
            if pos is None or s.screen()[1][pos[0]][pos[1]] != selected_bg:
                print("FAIL - PageUp did not prioritize the visible top")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_shift_navigation_extends_and_shrinks_selection():
    """Shift+arrows/pages/Home/End extend from one fixed keyboard anchor."""
    print("TEST: parvion pane - Shift navigation multi-select ... ", end="", flush=True)
    d = tempfile.mkdtemp(prefix="parvionpane_shiftkeys_")
    try:
        for i in range(48):
            open(os.path.join(d, f"row_{i:02d}.txt"), "w").close()
        with ParvionSession(d) as s:
            start = find_text(s.screen()[0], "row_02.txt")
            plain = find_text(s.screen()[0], "row_01.txt")
            if start is None or plain is None:
                print("FAIL - starting rows not found")
                return False
            unselected_bg = s.screen()[1][plain[0]][plain[1]]
            s.click(start[1] + 1, start[0] + 1)
            selected_bg = s.screen()[1][start[0]][start[1]]

            s.write("\x1b[1;2B" * 2)  # Shift+Down twice: row_02 through row_04.
            chars, bg = s.screen()
            selected = [find_text(chars, f"row_{i:02d}.txt") for i in (2, 3, 4)]
            if any(pos is None or bg[pos[0]][pos[1]] != selected_bg for pos in selected):
                print("FAIL - Shift+Down did not extend the selection")
                return False
            s.write("\x1b[1;2A")  # Shift+Up shrinks the movable endpoint to row_03.
            chars, bg = s.screen()
            row04 = find_text(chars, "row_04.txt")
            if row04 is None or bg[row04[0]][row04[1]] != unselected_bg:
                print("FAIL - Shift+Up did not shrink the selection")
                return False

            visible = [(find_text(s.screen()[0], f"row_{i:02d}.txt"), i) for i in range(48)]
            bottom = max((item for item in visible if item[0] is not None), key=lambda item: item[0][0])
            s.write("\x1b[6;2~")  # First Shift+PageDown extends to the visible bottom.
            pos = find_text(s.screen()[0], f"row_{bottom[1]:02d}.txt")
            if (find_text(s.screen()[0], "row_02.txt") is None or pos is None
             or s.screen()[1][pos[0]][pos[1]] != selected_bg):
                print("FAIL - Shift+PageDown did not extend to the visible bottom")
                return False
            s.write("\x1b[6;2~")  # From the edge, advance to the next page.
            if find_text(s.screen()[0], "row_02.txt") is not None:
                print("FAIL - second Shift+PageDown did not advance the viewport")
                return False
            s.write("\x1b[5;2~" * 2)  # Visit the visible top, then the previous page.
            chars, bg = s.screen()
            parent = find_text(chars, "/..")
            row02 = find_text(chars, "row_02.txt")
            row03 = find_text(chars, "row_03.txt")
            row04 = find_text(chars, "row_04.txt")
            if (parent is None or row02 is None or row03 is None or row04 is None
             or bg[parent[0]][parent[1]] != selected_bg
             or bg[row02[0]][row02[1]] != selected_bg
             or bg[row03[0]][row03[1]] != unselected_bg
             or bg[row04[0]][row04[1]] != unselected_bg):
                print("FAIL - Shift+PageUp did not shrink through visible page tops")
                return False

            s.write("\x1b[1;2H")  # Shift+Home: parent through the row_02 anchor.
            parent = find_text(s.screen()[0], "/..")
            if parent is None or s.screen()[1][parent[0]][parent[1]] != selected_bg:
                print("FAIL - Shift+Home did not extend to the first item")
                return False
            s.click(row02[1] + 1, row02[0] + 1)  # Reset the anchor for Shift+End.
            s.write("\x1b[1;2F")
            last = find_text(s.screen()[0], "row_47.txt")
            if last is None or s.screen()[1][last[0]][last[1]] != selected_bg:
                print("FAIL - Shift+End did not extend to the final item")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_typeahead_scrolls_selection_into_view_and_resets_horizontal_scroll():
    """An ASCII name jump reveals its row and returns the table to the Name column."""
    print("TEST: parvion pane - type-ahead follows selection and resets horizontal scroll ... ", end="", flush=True)
    d = tempfile.mkdtemp(prefix="parvionpane_typeahead_")
    try:
        long_stem = "a" * 90
        for i in range(48):
            open(os.path.join(d, f"{long_stem}_{i:02d}.txt"), "w").close()
        target = "z_typeahead_target.txt"
        open(os.path.join(d, target), "w").close()
        with ParvionSession(d) as s:
            chars = s.screen()[0]
            name = table_header_field(chars, "Name")
            if name is None:
                print("FAIL - Name header not found")
                return False
            header_row, _, _, name_right, _, _ = name
            if chars[header_row][name_right] != "│":
                print("FAIL - Name divider not found")
                return False

            # Auto-fit the long Name column, then page its horizontal rail to the right.
            s.double_click(name_right + 1, header_row + 1)
            chars = s.screen()[0]
            hbars = []
            for r in range(header_row + 1, ROWS):
                cols = [c for c, ch in enumerate(chars[r]) if ch in ("▂", "▄")]
                if cols and min(cols) < COLS // 2:
                    hbars.append((len(cols), r, min(cols), max(cols)))
            if not hbars:
                print("FAIL - auto-fit did not create the local pane horizontal scrollbar")
                return False
            _, hrow, _, hright = max(hbars)
            s.click(hright + 1, hrow + 1)
            if "Name" in row_text(s.screen()[0], header_row)[:COLS // 2]:
                print("FAIL - horizontal scrollbar did not move away from the Name column")
                return False

            s.write("z", settle=0.8)
            chars, bg = s.screen()
            pos = find_text(chars, target)
            if pos is None:
                print("FAIL - type-ahead did not scroll the target row into view")
                return False
            if "Name" not in row_text(chars, header_row)[:COLS // 2]:
                print("FAIL - type-ahead did not reset horizontal scrolling")
                return False
            if bg[pos[0]][pos[1]] is None:
                print("FAIL - visible type-ahead target is not selected")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_vertical_scrollbar_stays_at_last_item():
    """Dragging the file-list scrollbar to its end must not snap back to the selection."""
    print("TEST: parvion pane - vertical scrollbar stays at last item ... ", end="", flush=True)
    d = tempfile.mkdtemp(prefix="parvionpane_vscroll_")
    try:
        for i in range(48):
            open(os.path.join(d, f"row_{i:02d}.txt"), "w").close()
        with ParvionSession(d) as s:
            chars = s.screen()[0]
            tracks = {}
            for r, row in enumerate(chars):
                for c, ch in enumerate(row[:COLS // 2]):
                    if ch in ("▐", "█"):
                        tracks.setdefault(c, []).append(r)
            if not tracks:
                print("FAIL - local pane vertical scrollbar not found")
                return False
            col, rows = max(tracks.items(), key=lambda item: len(item[1]))
            top, bottom = min(rows), max(rows)
            if bottom <= top:
                print("FAIL - local pane vertical scrollbar track is too short")
                return False
            s.drag_path([(col + 1, top + 1), (col + 1, bottom + 1)], settle=0.8)
            if not grid_contains(s.screen()[0], "row_47.txt"):
                print("FAIL - scrollbar snapped away from the last list item")
                return False
            s.feed(0.8)
            if not grid_contains(s.screen()[0], "row_47.txt"):
                print("FAIL - scrollbar reset to the top after reaching the last list item")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_file_activation_preserves_scrolled_viewport():
    """Enter / double-click file activation must not reset the file pane's vertical scroll."""
    print("TEST: parvion pane - file activation preserves scrolled viewport ... ", end="", flush=True)
    d = tempfile.mkdtemp(prefix="parvionpane_activate_")
    try:
        for i in range(48):
            open(os.path.join(d, f"row_{i:02d}.txt"), "w").close()
        with ParvionSession(d) as s:
            first = find_text(s.screen()[0], "/..")
            if first is None:
                print("FAIL - '..' row not found")
                return False
            s.click(first[1] + 1, first[0] + 1)
            s.write("\x1b[B" * 60, settle=1.0)
            chars, bg = s.screen()
            last = find_text(chars, "row_47.txt")
            if last is None:
                print("FAIL - keyboard navigation did not reveal the final row")
                return False
            selected_bg = bg[last[0]][last[1]]
            s.write("\r", settle=0.8)
            chars, bg = s.screen()
            last = find_text(chars, "row_47.txt")
            if last is None:
                print("FAIL - Enter activation reset the viewport to the top")
                return False
            if bg[last[0]][last[1]] != selected_bg:
                print(f"FAIL - final row lost selection after Enter (bg {bg[last[0]][last[1]]})")
                return False

            s.double_click(last[1] + 1, last[0] + 1, settle=0.8)
            chars, bg = s.screen()
            last = find_text(chars, "row_47.txt")
            if last is None:
                print("FAIL - double-click activation reset the viewport to the top")
                return False
            if bg[last[0]][last[1]] != selected_bg:
                print(f"FAIL - final row lost selection after double-click (bg {bg[last[0]][last[1]]})")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


TESTS = [
    test_shared_table_file_sort_configuration,
    test_ctrl_click_keeps_focus,
    test_right_click_activates_pane,
    test_shared_table_keyboard_activation_and_parent,
    test_enter_after_directory_change_activates_visible_parent,
    test_shared_table_keyboard_navigation_scrolls_to_last_row,
    test_typeahead_and_navigation_share_selection_cursor,
    test_page_home_end_keys_navigate_and_select,
    test_paging_prioritizes_visible_page_after_scroll,
    test_shift_navigation_extends_and_shrinks_selection,
    test_typeahead_scrolls_selection_into_view_and_resets_horizontal_scroll,
    test_vertical_scrollbar_stays_at_last_item,
    test_file_activation_preserves_scrolled_viewport,
    test_blank_click_does_not_select,
    test_right_click_blank_clears_selection,
    test_ctrl_drag_adds_to_selection,
    test_ctrl_drag_deselects,
    test_blank_context_menu,
    test_item_context_menu,
    test_copy_full_path,
    test_create_directory,
    test_delete_item,
    test_delete_cancel,
    test_delete_key_item,
    test_delete_preserves_viewport_and_navigation_selection,
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
