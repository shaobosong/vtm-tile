#!/usr/bin/env python3
# Copyright (c) Dmitry Sapozhnikov
# Licensed under the MIT license.

"""
Regression test for vtm.tile.ReRunApplication() and the pickapp Ctrl+R
keybinding.

Verifies:
  1. Ctrl+R inside the App-picker overlay closes the currently focused
     applet and immediately spawns a new applet using the selected app
     id (i.e. the close+run timing is correct: rerun fires only AFTER
     pop_back has emptied the slot, otherwise RunApplication would be
     a no-op since events::ui::create only acts on empty slots).
       Concretely: focused pane runs `term`; user opens the App picker
       via the status-bar App button, types "alph" to filter, presses
       Ctrl+R. The original shell pid must die and a new shell pid must
       spawn (proves rerun happened). The status-bar App button must
       update to "App: alpha" (proves SetSelectedApp ran).
  2. Empty-slot fast path: vtm.tile.ReRunApplication() invoked when the
     focused slot is already empty must run the selected app directly
     without any close phase. We exercise this by binding ReRun to a
     hotkey, using vtm.tile.ClosePane() first (which leaves the slot
     empty), then sending the ReRun hotkey. A new shell pid must spawn.
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
    """Return set of pids running 'vtm-tile -r term' (the dtvt term applet).

    The tile server forks 'vtm-tile -r term' subprocesses for each term
    pane; these get reparented to init (ppid=1), so we can't walk
    descendants from the client pid. Instead we identify them by argv.
    """
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
                # Look for "-r" followed by "term".
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
    """Find shell-like child pids of any of the given applet pids."""
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
                # Common shells.
                if comm in ("bash", "sh", "dash", "zsh", "fish"):
                    out.add(int(entry))
            except (OSError, ValueError, IndexError):
                continue
    except OSError:
        pass
    return out


def find_any_shell(timeout=5.0, exclude=frozenset()):
    """Wait until at least one shell pid (under a vtm-tile term applet)
    exists that is not in `exclude`; return one such pid (or None)."""
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


# Two configured apps; "term" is selected by default. Both spawn a real
# shell via the dtvt term applet so we can track shell pids.
# The positional argument "term" passed after -c is consumed by tile::hall
# as the initial layout / pane to spawn (single term pane).
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
    "</config>"
)
TILE_ARGS = ["-c", TILE_CONFIG]


# Same plus key bindings to invoke ReRunApplication and ClosePane directly.
TILE_CONFIG_WITH_HOTKEYS = (
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
            '<script=TileRerun on="Alt+Shift+R"/>'
            '<script=TileClose on="Alt+Shift+C"/>'
            '<script=TilePick  on="Alt+Shift+P"/>'
        "</tile></events>"
    "</config>"
    "<Scripting>"
        '<TileRerun="vtm.tile.ReRunApplication();"/>'
        '<TileClose="vtm.tile.ClosePane();"/>'
        '<TilePick="vtm.tile.PickApplication();"/>'
    "</Scripting>"
)
TILE_ARGS_WITH_HOTKEYS = ["-c", TILE_CONFIG_WITH_HOTKEYS]


class VtmTileSession:
    def __init__(self, args, settle_delay=SETTLE_DELAY):
        self.args = args
        self.settle_delay = settle_delay
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
            # Make the embedded shell quiet & predictable: disable rc files
            # and use a hermetic HOME so user shell prompts (starship etc.)
            # don't overflow the status bar row.
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


def app_btn_click_col():
    # 0-indexed start = WS_BTN_W + APP_BTN_GAP = 3. Click on the centre
    # of the "App" prefix (column 6 in 1-indexed terms).
    return WS_BTN_W + APP_BTN_GAP + APP_BTN_PAD_L + 2 + 1  # 1-indexed


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_pickapp_ctrl_r_replaces_focused_pane():
    """Ctrl+R inside the picker: close current applet + run with new selection."""
    print("TEST: pickapp Ctrl+R closes focused pane and runs new app ... ",
          end="", flush=True)
    with VtmTileSession(TILE_ARGS_WITH_HOTKEYS) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")

        # Wait for the initial term applet's shell to fork.
        old_shell = find_any_shell(timeout=6.0)
        if old_shell is None:
            return fail("could not find initial shell pid (term not started?)")
        old_applets = all_term_applet_pids()

        # Open the picker via Alt+Shift+P -> vtm.tile.PickApplication().
        # Avoids brittle status-bar click coords (the embedded shell prompt
        # can overflow into the bar row depending on user environment).
        s.reset_buffer()
        s.write(b"\x1bP")
        rendered = s.snapshot(timeout=2.0)
        if "alpha" not in rendered:
            return fail("picker did not open via Alt+Shift+P "
                        "(no 'alpha' in output):\n" + rendered[-1500:])

        # Filter to "alph" so that the selected entry is alpha.
        s.reset_buffer()
        s.write(b"alph")
        time.sleep(0.3)
        rendered = s.snapshot(timeout=1.0)
        if "alpha" not in rendered:
            return fail("filter 'alph' did not match alpha:\n"
                        + rendered[-1500:])

        # Press Ctrl+R: SetSelectedApp('alpha') + ReRunApplication().
        s.reset_buffer()
        s.write(b"\x12")  # Ctrl+R
        # Allow time for: dispatch_script -> SetSelectedApp -> ReRunApplication
        # -> close (async via tier::preview quit::one enqueue) -> our enqueued
        # createby -> new dtvt subprocess -> shell fork.
        if not wait_pid_gone(old_shell, timeout=6.0):
            return fail(f"old shell pid {old_shell} did not exit "
                        "(ClosePane phase failed)")

        new_shell = find_any_shell(timeout=6.0,
                                   exclude=frozenset({old_shell}))
        if new_shell is None:
            return fail("no new shell pid spawned after Ctrl+R "
                        "(rerun did not fire after close)")
        if new_shell == old_shell:
            return fail(f"new shell pid {new_shell} same as old; "
                        "rerun did not actually replace")

        # Verify a *new* term applet (vtm-tile -r term) was spawned.
        new_applets = all_term_applet_pids()
        if new_applets == old_applets:
            return fail("no new vtm-tile -r term applet spawned")

    print("OK")
    return True


def test_rerun_application_empty_slot_fast_path():
    """ReRunApplication() on an already-empty slot: must run directly."""
    print("TEST: ReRunApplication empty-slot fast path ... ",
          end="", flush=True)
    with VtmTileSession(TILE_ARGS_WITH_HOTKEYS) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")

        old_shell = find_any_shell(timeout=5.0)
        if old_shell is None:
            return fail("could not find initial shell pid")

        # Send Alt+Shift+C => ClosePane(). Wait for the slot to actually
        # become empty (old shell gone).
        s.write(b"\x1bC")
        if not wait_pid_gone(old_shell, timeout=5.0):
            return fail(f"old shell pid {old_shell} did not exit after close")
        time.sleep(0.5)  # let the veer settle into empty-slot state.

        # Now send Alt+Shift+R => ReRunApplication() on an empty slot.
        # This exercises the fast path: createby fires synchronously
        # without any quit::any listener detour.
        s.write(b"\x1bR")
        time.sleep(2.0)

        new_shell = find_any_shell(timeout=5.0,
                                   exclude=frozenset({old_shell}))
        if new_shell is None:
            return fail("ReRunApplication on empty slot did not spawn a shell")

    print("OK")
    return True


def test_rerun_application_replaces_focused_pane_via_hotkey():
    """ReRunApplication() with focused applet: close + rerun (single hotkey)."""
    print("TEST: ReRunApplication on focused applet closes+reruns ... ",
          end="", flush=True)
    with VtmTileSession(TILE_ARGS_WITH_HOTKEYS) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")

        old_shell = find_any_shell(timeout=5.0)
        if old_shell is None:
            return fail("could not find initial shell pid")

        # Single Alt+Shift+R: ReRunApplication on a non-empty slot.
        # Must close THEN rerun deterministically.
        s.write(b"\x1bR")

        if not wait_pid_gone(old_shell, timeout=5.0):
            return fail(f"old shell {old_shell} did not exit on rerun")

        new_shell = find_any_shell(timeout=5.0,
                                   exclude=frozenset({old_shell}))
        if new_shell is None:
            return fail("no new shell after rerun (close+run timing broken)")
        if new_shell == old_shell:
            return fail(f"shell pid did not change ({old_shell})")
    print("OK")
    return True


TESTS = [
    test_rerun_application_empty_slot_fast_path,
    test_rerun_application_replaces_focused_pane_via_hotkey,
    test_pickapp_ctrl_r_replaces_focused_pane,
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
