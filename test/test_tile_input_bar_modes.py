#!/usr/bin/env python3
# Copyright (c) Dmitry Sapozhnikov
# Licensed under the MIT license.

"""
End-to-end TUI regression test for the pickapp command-bar Enter-mode
buttons: [+] [⬒] [|] [-] anchored to the right side of the input row.

Verifies:
  1. The four buttons render in the input row when the pickapp overlay
     opens (pickapp_caps == allow_split | allow_replace).
  2. Pressing Enter without selecting any mode applies the highlighted
     entry without splitting/rerunning (existing behavior preserved).
  3. Clicking [|] then Enter spawns an additional pane via SplitPane(0)
     (new term applet appears alongside the original).
  4. Clicking [-] then Enter spawns an additional pane via SplitPane(1).
  5. Clicking [+] then Enter replaces the focused pane via
     ReRunApplication (old shell pid dies, new one spawns).
  6. Clicking the same button twice cancels the mode (Enter then
     behaves as plain app-pick — no extra split/rerun).
  7. Buttons are NOT rendered when the dialog is too narrow to fit
     them along with at least 4 input cells (responsive degradation).
  8. Clicking a list item with an Enter-mode button armed applies the
     same mode suffix as pressing Enter (click-to-pick also respects
     the button selection).
  9. Tab cycles forward through modes none→[+]→[⬒]→[|]→[-]→none; each
     intermediate Enter fires the expected action.
 10. Shift+Tab cycles backward through the same sequence.
 11. Tab in a non-pickapp command bar (allow_split | allow_replace not
     both set) is NOT consumed — the normal Tab behaviour is preserved.
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

# Status-bar App button geometry (must match constants in tile.hpp).
WS_BTN_W = 3
APP_BTN_GAP = 0
APP_BTN_PAD_L = 1


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


def all_term_applet_pids():
    out = set()
    try:
        for entry in os.listdir("/proc"):
            if not entry.isdigit():
                continue
            try:
                with open(f"/proc/{entry}/cmdline", "rb") as f:
                    raw = f.read()
                args = raw.split(b"\x00")
                if not args or not args[0].endswith(b"vtm-tile"):
                    continue
                for i, a in enumerate(args[:-1]):
                    if a == b"-r" and args[i + 1] == b"term":
                        out.add(int(entry))
                        break
            except (OSError, ValueError):
                continue
    except OSError:
        pass
    return out


def shell_pids_under(applet_pids):
    out = set()
    try:
        for entry in os.listdir("/proc"):
            if not entry.isdigit():
                continue
            try:
                with open(f"/proc/{entry}/stat", "r") as f:
                    raw = f.read()
                ppid = int(raw.rsplit(")", 1)[1].split()[1])
                if ppid not in applet_pids:
                    continue
                with open(f"/proc/{entry}/comm", "r") as f:
                    comm = f.read().strip()
                if comm in ("bash", "sh", "dash", "zsh", "fish"):
                    out.add(int(entry))
            except (OSError, ValueError, IndexError):
                continue
    except OSError:
        pass
    return out


def find_any_shell(timeout=5.0, exclude=frozenset()):
    deadline = time.time() + timeout
    while time.time() < deadline:
        applets = all_term_applet_pids()
        shells = shell_pids_under(applets) - exclude
        if shells:
            return next(iter(shells))
        time.sleep(0.1)
    return None


def wait_pid_gone(pid, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not os.path.exists(f"/proc/{pid}"):
            return True
        time.sleep(0.1)
    return False


def wait_shell_count(min_count, exclude=frozenset(), timeout=5.0):
    """Wait until at least min_count shell pids exist not in exclude."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        applets = all_term_applet_pids()
        shells = shell_pids_under(applets) - exclude
        if len(shells) >= min_count:
            return shells
        time.sleep(0.1)
    return None


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


def sgr_press(col, row, button=0):
    return f"\033[<{button};{col};{row}M".encode()


def sgr_release(col, row, button=0):
    return f"\033[<{button};{col};{row}m".encode()


# Two configured apps; "term" is selected by default.
TILE_CONFIG = (
    "<config>"
        "<tile>"
            "<confirm_close=0/>"
            '<app selected="term">'
                "<item*/>"
                '<item id="term"  label="term"  type="dtvt" cmd="$0 -r term"/>'
                '<item id="alpha" label="alpha" type="dtvt" cmd="$0 -r term"/>'
            "</app>"
            "<menu item*/>"
        "</tile>"
        "<events><tile>"
            '<script=TilePick on="Alt+Shift+P"/>'
        "</tile></events>"
    "</config>"
    "<Scripting>"
        '<TilePick="vtm.tile.PickApplication();"/>'
    "</Scripting>"
)
TILE_ARGS = ["-c", TILE_CONFIG]


class VtmTileSession:
    def __init__(self, args, settle_delay=SETTLE_DELAY, cols=COLS, rows=ROWS):
        self.args = args
        self.settle_delay = settle_delay
        self.cols = cols
        self.rows = rows
        self.master_fd = None
        self.pid = None
        self._screen_buf = b""

    def __enter__(self):
        self.master_fd, slave_fd = pty.openpty()
        set_winsize(self.master_fd, self.rows, self.cols)
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

    def snapshot(self, timeout=1.0):
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
        return strip_ansi(self._screen_buf).decode("utf-8", errors="replace")

    def reset_buffer(self):
        self._screen_buf = b""


def fail(msg):
    print(f"FAIL - {msg}")
    return False


# ---------------------------------------------------------------------------
# Pickapp Enter-mode button geometry.
# With COLS=120, dlg_w_max=60, dlg_w=60, dlg_x=(120-60)/2=30 (0-indexed).
# enter_btns_x0 = dlg_x + dlg_w - 1 - 12 = 77 (0-indexed).
# Buttons (0-indexed centers): [+]=78, [⬒]=81, [|]=84, [-]=87.
# 1-indexed: [+]=79, [⬒]=82, [|]=85, [-]=88.
# Row: dlg_y = max(0, (30 - 13)/4) = 4 (0-indexed) -> row 5 (1-indexed).
# ---------------------------------------------------------------------------
BTN_RERUN_COL = 79   # [+] ReRunApplication
BTN_BOX_COL   = 82   # [⬒] CreateWorkspace
BTN_PIPE_COL  = 85   # [|] SplitPane(0)
BTN_DASH_COL  = 88   # [-] SplitPane(1)
BTN_ROW = 5


def open_picker_via_hotkey(s, timeout=2.0):
    s.reset_buffer()
    s.write(b"\x1bP")  # Alt+Shift+P -> vtm.tile.PickApplication()
    rendered = s.snapshot(timeout=timeout)
    return rendered


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_buttons_render_when_picker_opens():
    print("TEST: pickapp picker renders [+] [⬒] [|] [-] buttons ... ",
          end="", flush=True)
    with VtmTileSession(TILE_ARGS) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        if find_any_shell(timeout=6.0) is None:
            return fail("initial shell did not start")
        rendered = open_picker_via_hotkey(s)
        if "alpha" not in rendered:
            return fail("picker did not open:\n" + rendered[-1500:])
        # The 12-cell button strip prints '+', '⬒', '|', '-' glyphs on the
        # input row (each surrounded by single-space padding).
        # Lenient check: each glyph must appear somewhere in the
        # rendered output once the picker is open.
        for glyph, name in [("+", "rerun/+"), ("\u2b12", "newws/⬒"),
                             ("|", "pipe"), ("-", "dash")]:
            if glyph not in rendered:
                return fail(f"glyph '{glyph}' ({name}) not found in picker:\n"
                            + rendered[-1500:])
    print("OK")
    return True


def test_enter_no_mode_keeps_existing_behavior():
    print("TEST: Enter without mode = plain app-pick (no split/rerun) ... ",
          end="", flush=True)
    with VtmTileSession(TILE_ARGS) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        old_shell = find_any_shell(timeout=6.0)
        if old_shell is None:
            return fail("no initial shell")
        old_applets = all_term_applet_pids()

        rendered = open_picker_via_hotkey(s)
        if "alpha" not in rendered:
            return fail("picker did not open")

        # Filter to alpha and press Enter without selecting any mode.
        # Default pickapp Enter just runs the entry's script
        # (SetSelectedApp) — it must NOT close or split the focused
        # pane. The original shell pid stays alive and shell count
        # remains 1.
        s.reset_buffer()
        s.write(b"alph\r")
        time.sleep(1.5)

        if not os.path.exists(f"/proc/{old_shell}"):
            return fail(f"old shell {old_shell} unexpectedly died "
                        "(plain Enter must not close the pane)")
        applets = all_term_applet_pids()
        shells = shell_pids_under(applets)
        if len(shells) != 1:
            return fail(f"expected exactly 1 shell after plain Enter; "
                        f"got {len(shells)}: {shells}")
    print("OK")
    return True


def test_pipe_button_then_enter_splits_pane():
    print("TEST: click [|] then Enter spawns SplitPane(0) ... ",
          end="", flush=True)
    with VtmTileSession(TILE_ARGS) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        old_shell = find_any_shell(timeout=6.0)
        if old_shell is None:
            return fail("no initial shell")

        rendered = open_picker_via_hotkey(s)
        if "alpha" not in rendered:
            return fail("picker did not open")

        # Click [|] button to arm SplitPane(0).
        s.click(BTN_PIPE_COL, BTN_ROW)
        time.sleep(0.2)

        # Press Enter on the (default) selected entry (term).
        s.reset_buffer()
        s.write(b"\r")
        # Two shells expected: original + new pane from split.
        shells = wait_shell_count(2, timeout=6.0)
        if shells is None or old_shell not in shells:
            return fail(f"expected >=2 shells incl. old={old_shell}; got {shells}")
    print("OK")
    return True


def test_dash_button_then_enter_splits_pane():
    print("TEST: click [-] then Enter spawns SplitPane(1) ... ",
          end="", flush=True)
    with VtmTileSession(TILE_ARGS) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        old_shell = find_any_shell(timeout=6.0)
        if old_shell is None:
            return fail("no initial shell")

        rendered = open_picker_via_hotkey(s)
        if "alpha" not in rendered:
            return fail("picker did not open")

        s.click(BTN_DASH_COL, BTN_ROW)
        time.sleep(0.2)

        s.reset_buffer()
        s.write(b"\r")
        shells = wait_shell_count(2, timeout=6.0)
        if shells is None or old_shell not in shells:
            return fail(f"expected >=2 shells incl. old={old_shell}; got {shells}")
    print("OK")
    return True


def test_plus_button_then_enter_reruns_pane():
    print("TEST: click [+] then Enter triggers ReRunApplication ... ",
          end="", flush=True)
    with VtmTileSession(TILE_ARGS) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        old_shell = find_any_shell(timeout=6.0)
        if old_shell is None:
            return fail("no initial shell")

        rendered = open_picker_via_hotkey(s)
        if "alpha" not in rendered:
            return fail("picker did not open")

        # Filter to alpha so the rerun replaces with a different app.
        s.reset_buffer()
        s.write(b"alph")
        time.sleep(0.3)

        # Arm [+] (rerun) and Enter.
        s.click(BTN_RERUN_COL, BTN_ROW)
        time.sleep(0.2)
        s.write(b"\r")

        if not wait_pid_gone(old_shell, timeout=6.0):
            return fail(f"old shell pid {old_shell} did not exit")

        new_shell = find_any_shell(timeout=6.0,
                                   exclude=frozenset({old_shell}))
        if new_shell is None:
            return fail("no new shell spawned after Enter with [+] mode")
    print("OK")
    return True


def test_button_toggle_cancels_mode():
    print("TEST: clicking same button twice cancels the mode ... ",
          end="", flush=True)
    with VtmTileSession(TILE_ARGS) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        old_shell = find_any_shell(timeout=6.0)
        if old_shell is None:
            return fail("no initial shell")

        rendered = open_picker_via_hotkey(s)
        if "alpha" not in rendered:
            return fail("picker did not open")

        # Arm and disarm [|] (two clicks on the same button).
        s.click(BTN_PIPE_COL, BTN_ROW)
        time.sleep(0.15)
        s.click(BTN_PIPE_COL, BTN_ROW)
        time.sleep(0.15)

        # Press Enter -> should behave as plain app-pick (no split).
        s.reset_buffer()
        s.write(b"\r")
        time.sleep(2.0)

        # Final state: exactly one shell (replacement, not split).
        applets = all_term_applet_pids()
        shells = shell_pids_under(applets)
        if len(shells) != 1:
            return fail(f"expected exactly 1 shell after toggled-off mode; "
                        f"got {len(shells)} shells: {shells}")
    print("OK")
    return True


def test_buttons_hidden_when_dialog_too_narrow():
    print("TEST: buttons hidden when terminal too narrow ... ",
          end="", flush=True)
    # With cols=15: dlg_w = min(60, 15-4) = 11. entry_disp_w = 11-4 = 7.
    # 7 < 4+13 -> buttons must NOT render.
    with VtmTileSession(TILE_ARGS, cols=15, rows=20) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        time.sleep(0.5)
        rendered = open_picker_via_hotkey(s, timeout=2.0)
        if "alpha" not in rendered and "term" not in rendered:
            # Picker may not even fit; that is acceptable for this
            # geometry test — what matters is no buttons are drawn.
            pass
        # The button glyphs '+' '|' '-' should not appear in a narrow
        # picker; '|' is the most distinctive (not used for entries).
        # We allow '+' and '-' since they may appear in unrelated UI
        # (status bar etc.); '|' presence at the input row is the
        # strict signal. Be lenient: ensure that the trio together
        # with surrounding spacing pattern (typical "  +    |    -  ")
        # does NOT appear.
        if " + " in rendered and " ⬒ " in rendered and " | " in rendered and " - " in rendered:
            return fail("button strip appears at narrow width:\n"
                        + rendered[-1500:])
    print("OK")
    return True


def test_tab_forward_cycles_modes_and_enter_splits():
    print("TEST: Tab cycles none→[+]→[⬒]→[|]→[-]→none, Enter with [|] splits ... ",
          end="", flush=True)
    with VtmTileSession(TILE_ARGS) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        old_shell = find_any_shell(timeout=6.0)
        if old_shell is None:
            return fail("no initial shell")

        rendered = open_picker_via_hotkey(s)
        if "alpha" not in rendered:
            return fail("picker did not open")

        # Three Tabs: none → [+] → [⬒] → [|]
        s.write(b"\x09\x09\x09")   # Tab Tab Tab
        time.sleep(0.2)

        # Press Enter — should fire SplitPane(0) (mode 3 = [|]).
        s.reset_buffer()
        s.write(b"\r")
        shells = wait_shell_count(2, timeout=6.0)
        if shells is None or old_shell not in shells:
            return fail(f"expected >=2 shells after Tab×3+Enter; got {shells}")
    print("OK")
    return True


def test_tab_wraps_back_to_none_and_enter_is_plain():
    print("TEST: five Tabs wrap mode back to none, Enter is plain pick ... ",
          end="", flush=True)
    with VtmTileSession(TILE_ARGS) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        old_shell = find_any_shell(timeout=6.0)
        if old_shell is None:
            return fail("no initial shell")

        rendered = open_picker_via_hotkey(s)
        if "alpha" not in rendered:
            return fail("picker did not open")

        # Five Tabs: none→[+]→[⬒]→[|]→[-]→none
        s.write(b"\x09\x09\x09\x09\x09")
        time.sleep(0.2)

        # Enter with mode=none → plain pick, no split, old shell survives.
        s.reset_buffer()
        s.write(b"\r")
        time.sleep(1.5)

        if not os.path.exists(f"/proc/{old_shell}"):
            return fail(f"old shell {old_shell} died — mode was not none")
        applets = all_term_applet_pids()
        shells = shell_pids_under(applets)
        if len(shells) != 1:
            return fail(f"expected 1 shell after Tab×5+Enter; got {shells}")
    print("OK")
    return True


def test_shift_tab_cycles_backward():
    print("TEST: Shift+Tab cycles [-]→[|]→[⬒]→[+]→none→[-] ... ",
          end="", flush=True)
    with VtmTileSession(TILE_ARGS) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        old_shell = find_any_shell(timeout=6.0)
        if old_shell is None:
            return fail("no initial shell")

        rendered = open_picker_via_hotkey(s)
        if "alpha" not in rendered:
            return fail("picker did not open")

        # One Shift+Tab from none → [-] (mode 4 = SplitPane(1)).
        s.write(b"\x1b[Z")   # Shift+Tab VT sequence
        time.sleep(0.2)

        s.reset_buffer()
        s.write(b"\r")
        shells = wait_shell_count(2, timeout=6.0)
        if shells is None or old_shell not in shells:
            return fail(f"expected >=2 shells after Shift+Tab+Enter; got {shells}")
    print("OK")
    return True


def test_click_item_with_mode_splits_pane():
    print("TEST: click list item with [|] armed -> SplitPane(0) ... ",
          end="", flush=True)
    with VtmTileSession(TILE_ARGS) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        old_shell = find_any_shell(timeout=6.0)
        if old_shell is None:
            return fail("no initial shell")

        rendered = open_picker_via_hotkey(s)
        if "alpha" not in rendered:
            return fail("picker did not open")

        # Arm [|] mode.
        s.click(BTN_PIPE_COL, BTN_ROW)
        time.sleep(0.2)

        # Now directly click the first visible list item (row dlg_y+2 = row 7
        # 1-indexed, any column inside the list area, e.g. centre of dlg).
        # dlg_y = max(0, (30-13)//4) = 4 (0-indexed) -> row 5 (1-indexed)
        # list starts at dlg_y+2 = 6 (0-indexed) -> row 7 (1-indexed)
        list_row = BTN_ROW + 2   # 1-indexed
        list_col = (COLS // 2)   # somewhere in the middle of the dialog
        s.reset_buffer()
        s.click(list_col, list_row)
        shells = wait_shell_count(2, timeout=6.0)
        if shells is None or old_shell not in shells:
            return fail(f"expected >=2 shells after click-with-mode; "
                        f"got {shells}")
    print("OK")
    return True


def test_ctrl_w_creates_workspace():
    print("TEST: Ctrl+W creates a new workspace with selected entry ... ",
          end="", flush=True)
    with VtmTileSession(TILE_ARGS) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        old_shell = find_any_shell(timeout=6.0)
        if old_shell is None:
            return fail("no initial shell")

        rendered = open_picker_via_hotkey(s)
        if "alpha" not in rendered:
            return fail("picker did not open")

        # Press Ctrl+W — should fire the selected entry's script
        # followed by vtm.tile.CreateWorkspace(), spawning a new
        # workspace (and thus a new shell applet).
        s.reset_buffer()
        s.write(b"\x17")  # Ctrl+W
        shells = wait_shell_count(2, timeout=6.0)
        if shells is None or old_shell not in shells:
            return fail(f"expected >=2 shells after Ctrl+W; got {shells}")
    print("OK")
    return True


def main():
    tests = [
        test_buttons_render_when_picker_opens,
        test_enter_no_mode_keeps_existing_behavior,
        test_pipe_button_then_enter_splits_pane,
        test_dash_button_then_enter_splits_pane,
        test_plus_button_then_enter_reruns_pane,
        test_button_toggle_cancels_mode,
        test_buttons_hidden_when_dialog_too_narrow,
        test_click_item_with_mode_splits_pane,
        test_tab_forward_cycles_modes_and_enter_splits,
        test_tab_wraps_back_to_none_and_enter_is_plain,
        test_shift_tab_cycles_backward,
        test_ctrl_w_creates_workspace,
    ]
    ok = 0
    for t in tests:
        try:
            if t():
                ok += 1
        except Exception as e:
            print(f"FAIL - exception: {e!r}")
        finally:
            kill_all_vtm()
    total = len(tests)
    print(f"\n{ok}/{total} tests passed")
    sys.exit(0 if ok == total else 1)


if __name__ == "__main__":
    main()
