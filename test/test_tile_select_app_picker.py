#!/usr/bin/env python3
# Copyright (c) Dmitry Sapozhnikov
# Licensed under the MIT license.

"""
End-to-end TUI regression test for the tile "select-app picker" feature.

Verifies that:
  * The tile menu renders an "App: <label>" button reflecting the
    currently selected default app.
  * Clicking the App button opens a command-bar-style picker overlay
    populated from the configured <tile><app> items.
  * The picker prefixes the currently selected entry with "* " and
    other entries with "  " so the active default app is visible.
  * Typing a fuzzy query filters the entries; pressing Enter applies
    the selection by invoking vtm.tile.SetSelectedApp and broadcasting
    a label change.
  * After selection, the menu's App button updates to the new label,
    proving the e2::form::prop::any broadcast reaches the menu.
  * Pressing Esc dismisses the picker without changing the selection.
  * The Lua method vtm.tile.PickApplication() also opens the picker
    (parity with the menu-click trigger), bound to Alt+Shift+P.

The test inlines a minimal <menu> that exposes a single "App: <label>"
button at a predictable column so the click coordinates are stable
regardless of the user's settings.xml.
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
import subprocess

VTM_TILE_BINARY = os.environ.get(
    "VTM_TILE_BINARY",
    os.path.join(os.path.dirname(__file__), "..", "build", "vtm-tile"),
)

COLS = 120
ROWS = 30
READ_TIMEOUT = 5.0
SETTLE_DELAY = 1.5


# Tile config:
#   - Two configured apps: "term" and "alpha". term is selected by default.
#   - confirm_close=0 so we can exit cleanly via the close button.
#   - An explicit <menu> with a single "App: <label>" item: this isolates
#     the App button to a known column (the menu's left padding=2 means
#     the label starts at column 3), independent of whatever menu items
#     the user's settings.xml otherwise installs.
def make_tile_config(extra_events="", extra_scripting=""):
    return (
        "<config>"
            "<tile>"
                "<confirm_close=0/>"
                '<app selected="term">'
                    "<item*/>"
                    '<item id="term"  label="term"  type="dtvt" cmd="$0 -r term"/>'
                    '<item id="alpha" label="alpha" type="dtvt" cmd="$0 -r term"/>'
                "</app>"
                "<menu item*>"
                    "<padding=2/>"
                    "<slim=1/>"
                    "<item tooltip=' Pick app '>"
                        "<script=OnLeftClick|TilePickApplication/>"
                        "<script>"
                            '<on="release: e2::form::upon::started" source="tile"/>'
                            '<on="release: e2::form::prop::any"/>'
                            "local app_label = vtm.tile.SelectedApp() "
                            'vtm.item.Label(app_label == "" and "App" or "App: " .. app_label) '
                            "vtm.item.Deface()"
                        "</script>"
                    "</item>"
                "</menu>"
            "</tile>"
            + extra_events +
        "</config>"
        + extra_scripting
    )


TILE_CONFIG = make_tile_config()
TILE_ARGS = []  # TILE_CONFIG is shipped via $VTM_CONFIG (vtm_config kwarg).

# Menu row and column of the App button.
#   - The menu is attached to slot::_1 of the outer fork: the top row of the
#     pty, i.e. row 1 (1-indexed).
#   - padding=2 puts two spaces of left padding before the first item label,
#     so "App: term" starts at column 3. Clicking column 5 lands squarely
#     inside the label.
APP_BTN_ROW = 1
APP_BTN_CLICK_COL = 5


def kill_all_vtm():
    subprocess.run(["pkill", "-9", "-x", "vtm-tile"], capture_output=True)
    deadline = time.time() + 3.0
    while time.time() < deadline:
        result = subprocess.run(["pgrep", "-x", "vtm-tile"], capture_output=True)
        if result.returncode != 0:
            break
        time.sleep(0.1)
    subprocess.run(["pkill", "-9", "-x", "vtm-tile"], capture_output=True)


def set_winsize(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def read_all(fd, timeout=READ_TIMEOUT):
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


def sgr_press(col, row, button=0):
    return f"\033[<{button};{col};{row}M".encode()


def sgr_release(col, row, button=0):
    return f"\033[<{button};{col};{row}m".encode()


_ANSI_CSI = re.compile(rb"\x1b\[[\x30-\x3f]*[\x20-\x2f]*[\x40-\x7e]")
_ANSI_OSC = re.compile(rb"\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)")
_ANSI_DCS = re.compile(rb"\x1bP[^\x1b]*\x1b\\")
_ANSI_OTHER = re.compile(rb"\x1b[()][\x30-\x7e]|\x1b[=>78NOMcDEHM]")


def strip_ansi(buf):
    out = _ANSI_DCS.sub(b"", buf)
    out = _ANSI_OSC.sub(b"", out)
    out = _ANSI_CSI.sub(b"", out)
    out = _ANSI_OTHER.sub(b"", out)
    return out


def render_grid(buf, rows=ROWS, cols=COLS):
    """Render the byte stream to a [rows][cols] character grid by
    tracking the cursor through CUP/CR/LF and skipping other escape
    sequences. Sufficient to read button labels at known coordinates."""
    grid = [[" "] * cols for _ in range(rows)]
    cur_row, cur_col = 1, 1
    i = 0
    n = len(buf)
    while i < n:
        b = buf[i]
        if b == 0x1b:
            if i + 1 < n and buf[i + 1] == 0x5b:  # CSI
                j = i + 2
                params = b""
                while j < n and (0x30 <= buf[j] <= 0x3f or 0x20 <= buf[j] <= 0x2f):
                    params += bytes([buf[j]])
                    j += 1
                if j < n:
                    final = buf[j]
                    if final == 0x48 or final == 0x66:  # CUP / HVP
                        p = params.decode("ascii", errors="replace").split(";")
                        try:
                            r = int(p[0]) if p and p[0] else 1
                            c = int(p[1]) if len(p) > 1 and p[1] else 1
                            cur_row, cur_col = r, c
                        except ValueError:
                            pass
                    i = j + 1
                    continue
                break
            if i + 1 < n and buf[i + 1] in (0x5d, 0x50, 0x5f):
                terminator = buf[i + 1]
                j = i + 2
                while j < n:
                    if buf[j] == 0x07 and terminator == 0x5d:
                        j += 1
                        break
                    if buf[j] == 0x1b and j + 1 < n and buf[j + 1] == 0x5c:
                        j += 2
                        break
                    j += 1
                i = j
                continue
            i += 2
            continue
        if b == 0x0d:
            cur_col = 1
            i += 1
            continue
        if b == 0x0a:
            cur_row += 1
            i += 1
            continue
        if b < 0x20:
            i += 1
            continue
        if b < 0x80:
            ch = chr(b); width = 1
        elif b < 0xc0:
            i += 1; continue
        elif b < 0xe0:
            ch = buf[i:i + 2].decode("utf-8", errors="replace"); width = 2
        elif b < 0xf0:
            ch = buf[i:i + 3].decode("utf-8", errors="replace"); width = 3
        else:
            ch = buf[i:i + 4].decode("utf-8", errors="replace"); width = 4
        if 1 <= cur_row <= rows and 1 <= cur_col <= cols:
            grid[cur_row - 1][cur_col - 1] = ch
        cur_col += 1
        i += width
    return grid


def grid_to_text(grid):
    return "\n".join("".join(row) for row in grid)


class VtmTileSession:
    def __init__(self, args, settle_delay=SETTLE_DELAY, vtm_config=None):
        self.args = args
        self.settle_delay = settle_delay
        self.vtm_config = vtm_config
        self.master_fd = None
        self.pid = None
        self._screen_buf = b""

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
            os.environ["SHELL"] = "/bin/bash"
            os.environ["BASH_ENV"] = "/dev/null"
            os.environ["ENV"] = "/dev/null"
            os.environ["HOME"] = "/tmp"
            os.environ["PS1"] = "$ "
            os.environ.pop("STARSHIP_SHELL", None)
            os.environ.pop("STARSHIP_SESSION_KEY", None)
            if self.vtm_config is not None:
                os.environ["VTM_CONFIG"] = self.vtm_config
            os.execvp(VTM_TILE_BINARY, [VTM_TILE_BINARY] + self.args)
            sys.exit(1)
        os.close(slave_fd)
        time.sleep(self.settle_delay)
        self._screen_buf += read_all(self.master_fd, timeout=1.0)
        return self

    def __exit__(self, *_):
        if self.pid:
            try:
                os.kill(self.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                os.waitpid(self.pid, os.WNOHANG)
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

    def click(self, col, row, button=0):
        self.write(sgr_press(col, row, button))
        time.sleep(0.05)
        self.write(sgr_release(col, row, button))

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

    def drain(self, timeout=1.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            ready, _, _ = select.select([self.master_fd], [], [], 0.1)
            if ready:
                try:
                    chunk = os.read(self.master_fd, 65536)
                    if chunk:
                        self._screen_buf += chunk
                        continue
                except OSError:
                    pass
            else:
                break

    def snapshot_text(self, timeout=1.0):
        self.drain(timeout)
        return strip_ansi(self._screen_buf).decode("utf-8", errors="replace")

    def snapshot_grid(self, timeout=1.0):
        self.drain(timeout)
        return render_grid(self._screen_buf)

    def reset_buffer(self):
        self._screen_buf = b""


def fail(msg):
    print(f"FAIL - {msg}")
    return False


def row_text(grid, row_1indexed):
    return "".join(grid[row_1indexed - 1])


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_menu_renders_app_button():
    print("TEST: tile menu renders 'App: term' ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        grid = s.snapshot_grid(timeout=2.0)
        menu_row = row_text(grid, APP_BTN_ROW)
        if "App: term" not in menu_row:
            return fail(
                f"'App: term' not on menu row {APP_BTN_ROW}; got {menu_row!r}"
            )
    print("OK")
    return True


def test_menu_app_button_click_opens_picker():
    print("TEST: clicking 'App: term' opens the picker ... ",
          end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.drain(timeout=2.0)
        grid = s.snapshot_grid(timeout=0.3)
        if "App: term" not in row_text(grid, APP_BTN_ROW):
            return fail("App button not rendered before click")

        s.reset_buffer()
        s.click(APP_BTN_CLICK_COL, APP_BTN_ROW)
        rendered = s.snapshot_text(timeout=1.5)

        if "* term" not in rendered:
            return fail("Expected '* term' (selected entry) in picker:\n"
                        + rendered[-1500:])
        if "alpha" not in rendered:
            return fail("Expected 'alpha' entry in picker:\n"
                        + rendered[-1500:])
    print("OK")
    return True


def test_picker_filters_and_selects_via_enter():
    print("TEST: picker filters by query and Enter applies selection ... ",
          end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.drain(timeout=2.0)

        s.reset_buffer()
        s.click(APP_BTN_CLICK_COL, APP_BTN_ROW)
        rendered = s.snapshot_text(timeout=1.5)
        if "alpha" not in rendered:
            return fail("Picker did not open:\n" + rendered[-1500:])

        # Filter down to "alpha" and confirm.
        s.write(b"alph")
        time.sleep(0.3)
        s.reset_buffer()
        s.write(b"\r")
        time.sleep(0.5)
        grid = s.snapshot_grid(timeout=1.5)
        menu_row = row_text(grid, APP_BTN_ROW)
        if "App: alpha" not in menu_row:
            return fail(
                f"Menu did not update to 'App: alpha' after Enter; "
                f"row={menu_row!r}\nfull:\n{grid_to_text(grid)[-1500:]}"
            )
    print("OK")
    return True


def test_picker_escape_cancels():
    print("TEST: Esc dismisses the picker without changing the selection ... ",
          end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.drain(timeout=2.0)
        s.click(APP_BTN_CLICK_COL, APP_BTN_ROW)
        rendered = s.snapshot_text(timeout=1.5)
        if "* term" not in rendered:
            return fail("Picker did not open before Esc:\n" + rendered[-1500:])

        s.reset_buffer()
        s.write(b"\x1b")
        time.sleep(0.4)
        grid = s.snapshot_grid(timeout=1.0)
        menu_row = row_text(grid, APP_BTN_ROW)
        if "App: term" not in menu_row:
            return fail(
                f"After Esc, menu should still show 'App: term'; row={menu_row!r}"
            )
    print("OK")
    return True


def test_lua_method_pickapplication_opens_picker():
    print("TEST: vtm.tile.PickApplication() Lua method opens the picker ... ",
          end="", flush=True)
    # Bind Alt+Shift+P (Esc-P over the pty) to PickApplication() so we
    # can trigger the Lua method without clicking.
    cfg = make_tile_config(
        extra_events=(
            "<events><tile>"
                '<script=TilePick on="Alt+Shift+P"/>'
            "</tile></events>"
        ),
        extra_scripting=(
            "<Scripting>"
                '<TilePick="vtm.tile.PickApplication();"/>'
            "</Scripting>"
        ),
    )
    with VtmTileSession([], vtm_config=cfg) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.drain(timeout=2.0)
        s.reset_buffer()
        s.write(b"\x1bP")  # Alt+Shift+P
        rendered = s.snapshot_text(timeout=1.5)
        if "* term" not in rendered or "alpha" not in rendered:
            return fail("Picker did not open via PickApplication():\n"
                        + rendered[-1500:])
    print("OK")
    return True


TESTS = [
    test_menu_renders_app_button,
    test_menu_app_button_click_opens_picker,
    test_picker_filters_and_selects_via_enter,
    test_picker_escape_cancels,
    test_lua_method_pickapplication_opens_picker,
]


def main():
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
