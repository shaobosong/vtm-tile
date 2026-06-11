#!/usr/bin/env python3
# Copyright (c) Shaobo Song
# Licensed under the MIT license.

"""
End-to-end TUI regression test for the optional default-action argument
to vtm.tile.PickApplication().

Verifies that passing a mode name to vtm.tile.PickApplication() opens
the app picker with the corresponding Enter-mode button already armed,
so that pressing Enter immediately fires the matching follow-up action:

  PickApplication("+") -> [+] armed; Enter triggers ReRunApplication
  PickApplication("⬒") -> [⬒] armed; Enter triggers CreateWorkspace
  PickApplication("|") -> [|] armed; Enter spawns a new pane via SplitPane(0)
  PickApplication("-") -> [-] armed; Enter spawns a new pane via SplitPane(1)
  PickApplication()    -> no mode; Enter does a plain pick

Also covers:
  - Unknown mode names fall back to the no-mode default.
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


# Each test rebinds Alt+Shift+P (encoded as Esc-P) to a specific
# vtm.tile.PickApplication(<mode>) script. The test then sends Esc-P
# and asserts the resulting behaviour.
def make_config(mode_arg):
    # mode_arg is a Lua expression for the arguments to PickApplication.
    return (
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
            f'<TilePick="vtm.tile.PickApplication({mode_arg});"/>'
        "</Scripting>"
    )


class VtmTileSession:
    def __init__(self, args, settle_delay=SETTLE_DELAY, cols=COLS, rows=ROWS, vtm_config=None):
        self.args = args
        self.settle_delay = settle_delay
        self.cols = cols
        self.rows = rows
        self.vtm_config = vtm_config
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


def open_picker_via_hotkey(s, timeout=2.0):
    s.reset_buffer()
    s.write(b"\x1bP")  # Alt+Shift+P -> vtm.tile.PickApplication(<mode>)
    return s.snapshot(timeout=timeout)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_no_argument_keeps_default_mode():
    """PickApplication() with no argument: Enter is a plain pick."""
    print("TEST: PickApplication() defaults to no mode (plain Enter) ... ",
          end="", flush=True)
    cfg = make_config("")  # vtm.tile.PickApplication();
    with VtmTileSession([], vtm_config=cfg) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        old_shell = find_any_shell(timeout=6.0)
        if old_shell is None:
            return fail("no initial shell")

        rendered = open_picker_via_hotkey(s)
        if "alpha" not in rendered:
            return fail("picker did not open:\n" + rendered[-1500:])

        # Plain Enter (no preset mode) must not split or replace.
        s.write(b"\r")
        time.sleep(1.5)

        if not os.path.exists(f"/proc/{old_shell}"):
            return fail(f"old shell {old_shell} died — default Enter must "
                        "not close the pane")
        shells = shell_pids_under(all_term_applet_pids())
        if len(shells) != 1:
            return fail(f"expected exactly 1 shell after default Enter; "
                        f"got {len(shells)}: {shells}")
    print("OK")
    return True


def test_splith_arg_enter_splits_pane():
    """PickApplication("|") + Enter -> SplitPane(0) immediately."""
    print("TEST: PickApplication(\"|\") + Enter splits the pane ... ",
          end="", flush=True)
    cfg = make_config("'|'")
    with VtmTileSession([], vtm_config=cfg) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        old_shell = find_any_shell(timeout=6.0)
        if old_shell is None:
            return fail("no initial shell")

        rendered = open_picker_via_hotkey(s)
        if "alpha" not in rendered:
            return fail("picker did not open:\n" + rendered[-1500:])

        # Enter immediately — preset mode 3 (SplitPane(0)) should fire.
        s.reset_buffer()
        s.write(b"\r")
        shells = wait_shell_count(2, timeout=6.0)
        if shells is None or old_shell not in shells:
            return fail(f"expected >=2 shells incl. old={old_shell}; got {shells}")
    print("OK")
    return True


def test_splitv_arg_enter_splits_pane():
    """PickApplication("-") + Enter -> SplitPane(1) immediately."""
    print("TEST: PickApplication(\"-\") + Enter splits the pane ... ",
          end="", flush=True)
    cfg = make_config("'-'")
    with VtmTileSession([], vtm_config=cfg) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        old_shell = find_any_shell(timeout=6.0)
        if old_shell is None:
            return fail("no initial shell")

        rendered = open_picker_via_hotkey(s)
        if "alpha" not in rendered:
            return fail("picker did not open:\n" + rendered[-1500:])

        s.reset_buffer()
        s.write(b"\r")
        shells = wait_shell_count(2, timeout=6.0)
        if shells is None or old_shell not in shells:
            return fail(f"expected >=2 shells incl. old={old_shell}; got {shells}")
    print("OK")
    return True


def test_rerun_arg_enter_reruns_pane():
    """PickApplication("+") + filter + Enter -> ReRunApplication."""
    print("TEST: PickApplication(\"+\") + Enter reruns app ... ",
          end="", flush=True)
    cfg = make_config("'+'")
    with VtmTileSession([], vtm_config=cfg) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        old_shell = find_any_shell(timeout=6.0)
        if old_shell is None:
            return fail("no initial shell")

        rendered = open_picker_via_hotkey(s)
        if "alpha" not in rendered:
            return fail("picker did not open:\n" + rendered[-1500:])

        # Filter to alpha so ReRunApplication picks a clearly-different app.
        s.write(b"alph")
        time.sleep(0.3)
        s.write(b"\r")

        if not wait_pid_gone(old_shell, timeout=6.0):
            return fail(f"old shell pid {old_shell} did not exit — "
                        "Rerun preset Enter should have replaced the pane")
        new_shell = find_any_shell(timeout=6.0, exclude=frozenset({old_shell}))
        if new_shell is None:
            return fail("no new shell spawned after Rerun preset Enter")
    print("OK")
    return True


def test_workspace_arg_enter_creates_workspace():
    """PickApplication("⬒") + Enter -> CreateWorkspace."""
    print("TEST: PickApplication(\"⬒\") + Enter creates workspace ... ",
          end="", flush=True)
    cfg = make_config("'⬒'")
    with VtmTileSession([], vtm_config=cfg) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        old_shell = find_any_shell(timeout=6.0)
        if old_shell is None:
            return fail("no initial shell")

        rendered = open_picker_via_hotkey(s)
        if "alpha" not in rendered:
            return fail("picker did not open:\n" + rendered[-1500:])

        s.reset_buffer()
        s.write(b"\r")
        # CreateWorkspace spawns a new applet in the new workspace.
        shells = wait_shell_count(2, timeout=6.0)
        if shells is None or old_shell not in shells:
            return fail(f"expected >=2 shells incl. old={old_shell}; got {shells}")
    print("OK")
    return True


def test_unknown_mode_falls_back_to_default():
    """An unrecognised mode name must behave like no argument at all."""
    print("TEST: PickApplication(\"bogus\") falls back to plain Enter ... ",
          end="", flush=True)
    cfg = make_config("'bogus'")
    with VtmTileSession([], vtm_config=cfg) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        old_shell = find_any_shell(timeout=6.0)
        if old_shell is None:
            return fail("no initial shell")

        rendered = open_picker_via_hotkey(s)
        if "alpha" not in rendered:
            return fail("picker did not open:\n" + rendered[-1500:])

        s.write(b"\r")
        time.sleep(1.5)

        if not os.path.exists(f"/proc/{old_shell}"):
            return fail(f"old shell {old_shell} died — unknown mode should "
                        "fall back to no-mode behaviour")
        shells = shell_pids_under(all_term_applet_pids())
        if len(shells) != 1:
            return fail(f"expected exactly 1 shell with unknown mode; "
                        f"got {len(shells)}: {shells}")
    print("OK")
    return True


TESTS = [
    test_no_argument_keeps_default_mode,
    test_splith_arg_enter_splits_pane,
    test_splitv_arg_enter_splits_pane,
    test_rerun_arg_enter_reruns_pane,
    test_workspace_arg_enter_creates_workspace,
    test_unknown_mode_falls_back_to_default,
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
