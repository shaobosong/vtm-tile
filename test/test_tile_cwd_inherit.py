#!/usr/bin/env python3
# Copyright (c) Dmitry Sapozhnikov
# Licensed under the MIT license.

"""
Regression test for /config/terminal/cwd controlling tile cwd inheritance.

Behavior under test:
  * When /config/terminal/cwd is true, the tile actions SplitPane,
    ReRunApplication, and CreateWorkspace launch the next instance
    using the focused pane's tracked cwd (sourced from the cwdsync
    stream, populated when a shell emits OSC 9;9;<path> BEL).
  * When /config/terminal/cwd is false (the default), the launch
    falls back to the inherited cwd (the test process's startup
    directory).

Strategy:
  1. Launch `vtm-tile` in a pty with an overlay config that
       (a) sets <terminal><cwd=true|false/></terminal>,
       (b) binds hotkeys for SplitHorizontally / ReRunApplication /
           CreateWorkspace so the test can trigger them deterministically.
  2. Wait for vtm-tile to fork the initial shell. Locate the shell pid by
     scanning /proc for shells whose grand-ancestor is a `vtm-tile -r term`
     dtvt subprocess.
  3. Emit OSC 9;9;/tmp BEL from the shell so the inner term applet
     publishes /tmp via e2::form::prop::cwd. The outer tile's
     app_window listener stashes the path into the focused pane's
     `pane.tracked_cwd` property.
  4. Trigger one of the actions. Wait for a NEW shell to spawn under
     a different `vtm-tile -r term` subprocess and inspect its
     /proc/<pid>/cwd:
       * cwd=true  scenario => new shell must land in /tmp.
       * cwd=false scenario => new shell must NOT land in /tmp.
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
SETTLE_DELAY = 2.0
POST_ACTION_DELAY = 3.0


# ---------------------------------------------------------------------------
# Process helpers (mirror the patterns in test_tile_rerun_application.py and
# test_terminal_restart_cwd.py).
# ---------------------------------------------------------------------------

def kill_all_vtm():
    subprocess.run(["pkill", "-9", "-x", "vtm-tile"], capture_output=True)
    deadline = time.time() + 3.0
    while time.time() < deadline:
        if subprocess.run(["pgrep", "-x", "vtm-tile"], capture_output=True).returncode != 0:
            return
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
    """Return set of pids running 'vtm-tile -r term' (the dtvt term applet)."""
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
                if comm in ("bash", "sh", "dash", "zsh", "fish"):
                    out.add(int(entry))
            except (OSError, ValueError, IndexError):
                continue
    except OSError:
        pass
    return out


def cwd_of_pid(pid):
    try:
        return os.readlink(f"/proc/{pid}/cwd")
    except OSError:
        return None


def wait_for_new_shell(prev_shells, timeout=POST_ACTION_DELAY):
    """Return a shell pid that is NOT in prev_shells, once one stabilises."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        applets = all_term_applet_pids()
        current = shell_pids_under(applets)
        added = current - prev_shells
        for pid in added:
            # Tolerate transient init period: let the shell finish its
            # chdir before we sample /proc/<pid>/cwd.
            time.sleep(0.3)
            if os.path.exists(f"/proc/{pid}"):
                return pid
        time.sleep(0.1)
    return None


# ---------------------------------------------------------------------------
# Overlay config
# ---------------------------------------------------------------------------

def make_overlay_config(cwd_enabled):
    """Build the -c overlay XML.

    Binds:
      * Alt+Shift+S  -> SplitHorizontally
      * Alt+Shift+R  -> ReRunApplication
      * Alt+Shift+C  -> CreateWorkspace
    """
    flag = "true" if cwd_enabled else "false"
    return (
        "<config>"
            f"<terminal><cwd={flag}/><confirm_close=false/></terminal>"
            "<tile>"
                "<confirm_close=0/>"
                '<app selected="term">'
                    "<item*/>"
                    '<item id="term" label="term" type="dtvt" cmd="$0 -r term"/>'
                "</app>"
            "</tile>"
            "<events><tile>"
                '<script=TileSplitHorizontally on="Alt+Shift+S"/>'
                '<script=TileReRunApplication on="Alt+Shift+R"/>'
                '<script=TileCreateWorkspace  on="Alt+Shift+C"/>'
            "</tile></events>"
        "</config>"
        "<Scripting>"
            '<TileSplitHorizontally="vtm.tile.SplitPane(0);"/>'
            '<TileReRunApplication="vtm.tile.ReRunApplication();"/>'
            '<TileCreateWorkspace="vtm.tile.CreateWorkspace();"/>'
        "</Scripting>"
    )


# ---------------------------------------------------------------------------
# Session harness
# ---------------------------------------------------------------------------

class VtmTileSession:
    def __init__(self, cwd_enabled, start_cwd):
        self.cwd_enabled = cwd_enabled
        self.start_cwd = start_cwd
        self.master_fd = None
        self.pid = None

    def __enter__(self):
        self.master_fd, slave_fd = pty.openpty()
        set_winsize(self.master_fd, ROWS, COLS)
        cfg = make_overlay_config(self.cwd_enabled)
        # vtm-tile (client mode) launches the tile server + client; the overlay
        # config drives the tile applet's initial layout from /config/tile/app.
        argv = [VTM_TILE_BINARY, "-c", cfg]
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
            try:
                os.chdir(self.start_cwd)
            except OSError:
                pass
            os.execvp(argv[0], argv)
            sys.exit(1)
        os.close(slave_fd)
        time.sleep(SETTLE_DELAY)
        read_all(self.master_fd, timeout=1.0)
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

    def drain(self, timeout=0.5):
        return read_all(self.master_fd, timeout=timeout)

    def emit_osc_cwd(self, path):
        """Have the bash shell echo OSC 9;9;<path> BEL, then drain output.

        The inner term applet receives the OSC, riseups e2::form::prop::cwd,
        and the outer tile's app_window listener stashes it into the
        focused pane's pane.tracked_cwd property.
        """
        # Use printf so bash itself doesn't try to interpret the path.
        # %b interprets backslash escapes for us.
        self.write(f"printf '\\x1b]9;9;{path}\\x07'\n")
        # Give the inner term + dtvt + outer tile chain time to forward
        # the path up to the app_window listener.
        time.sleep(0.6)
        self.drain(timeout=0.3)

    def cd_shell(self, path):
        """Run `cd <path>` in the shell -- exercises the term_body poller
        path on platforms whose shells update the process CWD without
        emitting OSC (cmd.exe on Windows; PowerShell with [Environment]::
        CurrentDirectory synced in the prompt; plain bash without OSC 9;9
        configured)."""
        self.write(f"cd {path}\n")
        # The poller runs at ~250ms; wait long enough for at least one cycle
        # to observe the new cwd and propagate it to the outer tile.
        time.sleep(1.2)
        self.drain(timeout=0.3)


# ---------------------------------------------------------------------------
# Per-action drivers
# ---------------------------------------------------------------------------

def trigger_split(s):
    """Alt+Shift+S bound to TileSplitHorizontally."""
    s.write(b"\x1bS")


def trigger_rerun(s):
    """Alt+Shift+R bound to TileReRunApplication."""
    s.write(b"\x1bR")


def trigger_create_workspace(s):
    """Alt+Shift+C bound to TileCreateWorkspace."""
    s.write(b"\x1bC")


# ---------------------------------------------------------------------------
# Scenario runner
# ---------------------------------------------------------------------------

def _run_action_scenario(action_name, trigger, cwd_enabled,
                          start_cwd, target_cwd, advance):
    """Drive one scenario.

    `advance` is a callable taking the session that moves the focused
    pane's tracked cwd to `target_cwd` -- either by emitting OSC 9;9
    (shell-side cwdsync) or by `cd`-ing the shell (poller path).

    Returns (old_pid, new_pid, new_cwd, msg).
    """
    with VtmTileSession(cwd_enabled, start_cwd) as s:
        applets = all_term_applet_pids()
        deadline = time.time() + 4.0
        old_shells = set()
        while time.time() < deadline and not old_shells:
            old_shells = shell_pids_under(applets)
            if not old_shells:
                applets = all_term_applet_pids()
                time.sleep(0.2)
        if not old_shells:
            return (None, None, None, "could not locate initial shell pid")
        old_pid = next(iter(old_shells))

        advance(s, target_cwd)

        # Trigger the action and wait for a new shell to spawn.
        trigger(s)
        new_pid = wait_for_new_shell(old_shells, timeout=POST_ACTION_DELAY)
        if new_pid is None:
            return (old_pid, None, None,
                    f"no new shell spawned after {action_name}")
        new_cwd = cwd_of_pid(new_pid)
        return (old_pid, new_pid, new_cwd, "ok")


def _advance_via_osc(s, target_cwd):
    """Source: shell emits OSC 9;9;<path> via printf. Exercises the cwdsync
    propagation chain (inner term OSC parser -> dtvt -> outer)."""
    s.emit_osc_cwd(target_cwd)


def _advance_via_cd(s, target_cwd):
    """Source: shell `cd`s without emitting OSC. Exercises the term_body
    poller chain (cwd_of(child_pid) -> riseup -> dtvt -> outer). This is
    the path Windows PowerShell uses when the user's prompt syncs
    [Environment]::CurrentDirectory = $PWD."""
    s.cd_shell(target_cwd)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def _scenario_positive(action_name, trigger, source_label, advance):
    """cwd=true: the new shell starts in /tmp (the focused pane's cwd)."""
    label = f"TEST: cwd=true {action_name} via {source_label} inherits focused pane cwd ... "
    print(label, end="", flush=True)
    start_cwd = os.path.realpath(os.path.dirname(__file__))
    target_cwd = "/tmp"
    old_pid, new_pid, new_cwd, msg = _run_action_scenario(
        action_name, trigger, True, start_cwd, target_cwd, advance
    )
    if msg != "ok":
        print(f"FAIL ({msg})")
        return False
    if not new_cwd:
        print(f"FAIL (could not read /proc/{new_pid}/cwd)")
        return False
    if os.path.realpath(new_cwd) != os.path.realpath(target_cwd):
        print(f"FAIL (new shell cwd is {new_cwd!r}; expected {target_cwd!r})")
        return False
    print(f"PASS (old_pid={old_pid}, new_pid={new_pid}, new_cwd={new_cwd})")
    return True


def _scenario_negative(action_name, trigger, source_label, advance):
    """cwd=false: the new shell ignores the focused pane's cwd."""
    label = f"TEST: cwd=false {action_name} via {source_label} ignores focused pane cwd ... "
    print(label, end="", flush=True)
    start_cwd = os.path.realpath(os.path.dirname(__file__))
    target_cwd = "/tmp"
    old_pid, new_pid, new_cwd, msg = _run_action_scenario(
        action_name, trigger, False, start_cwd, target_cwd, advance
    )
    if msg != "ok":
        print(f"FAIL ({msg})")
        return False
    if not new_cwd:
        print(f"FAIL (could not read /proc/{new_pid}/cwd)")
        return False
    if os.path.realpath(new_cwd) == os.path.realpath(target_cwd):
        print(f"FAIL (new shell cwd is {target_cwd!r}; should NOT inherit)")
        return False
    print(f"PASS (old_pid={old_pid}, new_pid={new_pid}, new_cwd={new_cwd})")
    return True


_ACTIONS = [
    ("SplitPane", "trigger_split"),
    ("ReRunApplication", "trigger_rerun"),
    ("CreateWorkspace", "trigger_create_workspace"),
]
_SOURCES = [
    ("OSC9;9", "_advance_via_osc"),
    ("cd-poll", "_advance_via_cd"),
]


def _make_scenario(scenario_fn, action_name, trigger_name, source_label, advance_name):
    def _test():
        return scenario_fn(action_name,
                           globals()[trigger_name],
                           source_label,
                           globals()[advance_name])
    _test.__name__ = (
        f"test_{scenario_fn.__name__.lstrip('_')}_"
        f"{action_name}_{source_label}".replace(";", "").replace("-", "_")
    )
    _test.__doc__ = f"{scenario_fn.__name__}: {action_name} via {source_label}"
    return _test


TESTS = (
    [_make_scenario(_scenario_positive, a, fn, sl, adv)
     for a, fn in _ACTIONS for sl, adv in _SOURCES]
    + [_make_scenario(_scenario_negative, a, fn, sl, adv)
       for a, fn in _ACTIONS for sl, adv in _SOURCES]
)


def main():
    if not os.path.exists(VTM_TILE_BINARY):
        print(f"vtm-tile binary not found: {VTM_TILE_BINARY}")
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
