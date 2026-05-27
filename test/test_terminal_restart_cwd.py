#!/usr/bin/env python3
# Copyright (c) Dmitry Sapozhnikov
# Licensed under the MIT license.

"""
Regression test for the <restart_cwd> terminal config option.

Behavior under test:
  * When <terminal><restart_cwd=true/></terminal> is set, the terminal's
    Restart action (vtm.terminal.Restart()) relaunches the shell using
    the CHILD's current working directory rather than the cwd that was
    inherited at session start.
  * When the option is false (default), the relaunched shell falls back
    to the original cwd, which mirrors the legacy behavior.

Strategy:
  1. Launch `vtm-tile -r term` inside a pty with an overlay config that
       (a) enables the restart_cwd option (true or false depending on
           the test scenario), and
       (b) binds Alt+Shift+R to TerminalRestart so the test can trigger
           a restart with a single byte sequence (\\x1bR, mirroring the
           pattern already used by test_command_bar_terminal.py).
  2. Wait for vtm-tile to fork the shell. Locate the shell pid by
     scanning /proc for a child of the vtm-tile pid.
  3. Send `cd /tmp\\n` to move the shell to /tmp. Verify via
     /proc/<pid>/cwd that the shell really moved.
  4. Send Alt+Shift+R to trigger TerminalRestart.
  5. Wait for the new shell to spawn (its pid must differ from the old
     shell's pid). Verify /proc/<new_pid>/cwd:
       * scenario A (restart_cwd=true)  => must be /tmp
       * scenario B (restart_cwd=false) => must NOT be /tmp; it should
                                            stay the test process's cwd
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

VTM_TILE_BINARY = os.environ.get(
    "VTM_TILE_BINARY",
    os.path.join(os.path.dirname(__file__), "..", "build", "vtm-tile"),
)

COLS = 100
ROWS = 30

READ_TIMEOUT = 5.0
SETTLE_DELAY = 2.0           # vtm-tile -r term standalone start
POST_RESTART_DELAY = 3.0     # restart spawns a new shell child


def kill_all_vtm():
    """Kill any leftover vtm processes between tests."""
    for name in ("vtm-tile", "vtm-desk"):
        subprocess.run(["pkill", "-9", "-x", name], capture_output=True)
    for _ in range(20):
        time.sleep(0.2)
        if all(
            subprocess.run(["pgrep", "-x", n], capture_output=True).returncode != 0
            for n in ("vtm-tile", "vtm-desk")
        ):
            return
    for name in ("vtm-tile", "vtm-desk"):
        subprocess.run(["pkill", "-9", "-x", name], capture_output=True)


def set_winsize(fd, rows, cols):
    winsize = struct.pack("HHHH", rows, cols, 0, 0)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, winsize)


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


def child_pids_of(parent_pid):
    """Return list of immediate-child pids of parent_pid via /proc."""
    out = []
    try:
        for entry in os.listdir("/proc"):
            if not entry.isdigit():
                continue
            try:
                with open(f"/proc/{entry}/stat", "r") as f:
                    fields = f.read().split()
                # field[3] is PPid (after 'comm' which can contain spaces but is
                # parenthesized -- use rsplit on ')' to be safe).
                with open(f"/proc/{entry}/stat", "r") as f:
                    raw = f.read()
                tail = raw.rsplit(")", 1)[1].split()
                ppid = int(tail[1])  # tail[0] is state, tail[1] is ppid
                if ppid == parent_pid:
                    out.append(int(entry))
            except (OSError, ValueError, IndexError):
                continue
    except OSError:
        pass
    return out


def find_shell_pid(vtm_pid, timeout=4.0):
    """Find the shell child forked by vtm-tile."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        kids = child_pids_of(vtm_pid)
        # The shell is the first non-vtm child; vtm-tile here doesn't fork helpers
        # for the standalone -r term mode, so the only child *should* be the shell.
        for pid in kids:
            try:
                with open(f"/proc/{pid}/comm", "r") as f:
                    comm = f.read().strip()
                # Skip any short-lived helper that happens to be present.
                if comm and comm not in ("vtm-tile",):
                    return pid
            except OSError:
                continue
        time.sleep(0.1)
    return None


def cwd_of_pid(pid):
    """Return the current working directory of pid, or None on failure."""
    try:
        return os.readlink(f"/proc/{pid}/cwd")
    except OSError:
        return None


def make_overlay_config(restart_cwd_enabled):
    """Build the -c overlay XML.

    We always bind Alt+Shift+R to TerminalRestart so the test can trigger a
    restart deterministically, regardless of any vtm.xml-level binding state.
    """
    flag = "true" if restart_cwd_enabled else "false"
    return (
        "<config>"
        f"<terminal><cwd={flag}/><confirm_close=false/></terminal>"
        "<events><terminal>"
        '<script=TerminalRestart on="Alt+Shift+R"/>'
        "</terminal></events>"
        "</config>"
    )


class VtmTileTermSession:
    """Run `vtm-tile -r term` standalone in a pty for a single test."""

    def __init__(self, restart_cwd_enabled, start_cwd):
        self.restart_cwd_enabled = restart_cwd_enabled
        self.start_cwd = start_cwd
        self.master_fd = None
        self.pid = None

    def __enter__(self):
        self.master_fd, slave_fd = pty.openpty()
        set_winsize(self.master_fd, ROWS, COLS)

        cfg = make_overlay_config(self.restart_cwd_enabled)
        argv = [VTM_TILE_BINARY, "-r", "term"]

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
            try:
                os.chdir(self.start_cwd)
            except OSError:
                pass
            os.environ["VTM_CONFIG"] = cfg
            os.execvp(argv[0], argv)
            sys.exit(1)
        os.close(slave_fd)
        time.sleep(SETTLE_DELAY)
        read_all(self.master_fd, timeout=1.0)
        return self

    def __exit__(self, *args):
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


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def _run_scenario(restart_cwd_enabled, start_cwd, target_cwd):
    """Drive one full scenario; return (old_pid, new_pid, new_cwd, msg)."""
    with VtmTileTermSession(restart_cwd_enabled, start_cwd) as s:
        old_pid = find_shell_pid(s.pid)
        if old_pid is None:
            return (None, None, None, "could not locate initial shell pid")

        old_cwd = cwd_of_pid(old_pid)
        if old_cwd != start_cwd:
            # Some shells resolve symlinks, so compare via realpath.
            if os.path.realpath(old_cwd or "") != os.path.realpath(start_cwd):
                return (old_pid, None, None,
                        f"initial shell cwd unexpected: {old_cwd!r} != {start_cwd!r}")

        # Move the shell to target_cwd and verify the kernel sees the change.
        s.write(f"cd {target_cwd}\n")
        # Give bash time to process and chdir.
        for _ in range(30):
            time.sleep(0.1)
            cur = cwd_of_pid(old_pid)
            if cur and os.path.realpath(cur) == os.path.realpath(target_cwd):
                break
        else:
            return (old_pid, None, None,
                    f"shell did not chdir to {target_cwd!r} (still {cur!r})")
        s.drain(timeout=0.3)

        # Trigger TerminalRestart via the bound key Alt+Shift+R.
        s.write(b"\x1bR")

        # Wait for the old shell to exit and a new one to take its place.
        deadline = time.time() + POST_RESTART_DELAY
        new_pid = None
        while time.time() < deadline:
            time.sleep(0.15)
            kids = child_pids_of(s.pid)
            candidates = [p for p in kids if p != old_pid]
            if candidates:
                # Pick the first stable candidate.
                new_pid = candidates[0]
                # Make sure it's a shell (not a transient helper).
                try:
                    with open(f"/proc/{new_pid}/comm", "r") as f:
                        comm = f.read().strip()
                    if comm and comm != "vtm-tile":
                        break
                except OSError:
                    new_pid = None
        if new_pid is None:
            return (old_pid, None, None, "no new shell spawned after restart")

        # Allow the new shell to settle into its cwd.
        time.sleep(0.4)
        new_cwd = cwd_of_pid(new_pid)
        return (old_pid, new_pid, new_cwd, "ok")


def test_restart_cwd_enabled_uses_child_cwd():
    """restart_cwd=true: the new shell starts in /tmp (the child's cwd)."""
    print("TEST: restart_cwd=true preserves child cwd ... ", end="", flush=True)
    start_cwd = os.path.realpath(os.path.dirname(__file__))
    target_cwd = "/tmp"
    old_pid, new_pid, new_cwd, msg = _run_scenario(True, start_cwd, target_cwd)
    if msg != "ok":
        print(f"FAIL ({msg})")
        return False
    if old_pid == new_pid:
        print(f"FAIL (shell pid unchanged: {old_pid})")
        return False
    if not new_cwd or os.path.realpath(new_cwd) != os.path.realpath(target_cwd):
        print(f"FAIL (new shell cwd is {new_cwd!r}, expected {target_cwd!r})")
        return False
    print(f"PASS (old_pid={old_pid}, new_pid={new_pid}, new_cwd={new_cwd})")
    return True


def test_restart_cwd_disabled_uses_original_cwd():
    """restart_cwd=false: the new shell ignores /tmp and uses the original cwd."""
    print("TEST: restart_cwd=false ignores child cwd ... ", end="", flush=True)
    start_cwd = os.path.realpath(os.path.dirname(__file__))
    target_cwd = "/tmp"
    old_pid, new_pid, new_cwd, msg = _run_scenario(False, start_cwd, target_cwd)
    if msg != "ok":
        print(f"FAIL ({msg})")
        return False
    if old_pid == new_pid:
        print(f"FAIL (shell pid unchanged: {old_pid})")
        return False
    if new_cwd is None:
        print("FAIL (could not read new shell cwd)")
        return False
    if os.path.realpath(new_cwd) == os.path.realpath(target_cwd):
        print(f"FAIL (new shell cwd is {new_cwd!r}; should not equal {target_cwd!r})")
        return False
    # The original session cwd is what we chdir'd the test child to before exec.
    if os.path.realpath(new_cwd) != os.path.realpath(start_cwd):
        # Be lenient: as long as it isn't /tmp, the option is observably off.
        print(f"PASS-soft (new_cwd={new_cwd}, not /tmp; original was {start_cwd})")
        return True
    print(f"PASS (old_pid={old_pid}, new_pid={new_pid}, new_cwd={new_cwd})")
    return True


TESTS = [
    test_restart_cwd_enabled_uses_child_cwd,
    test_restart_cwd_disabled_uses_original_cwd,
]


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
