#!/usr/bin/env python3
# Copyright (c) Dmitry Sapozhnikov
# Licensed under the MIT license.

"""
Regression test for vtm-tile shutdown orphan-process bug.

Repro path (matches the user's manual reproduction exactly):
  1. Run vtm-tile.
  2. Open the command bar from the [CMD] menu button, run
     'layout: Split Horizontal'  -> 2 horizontal panes, each spawning a
     bash child.
  3. Open the command bar again, run 'pane: Select All Panes'.
  4. Open the command bar again, run 'terminal: Toggle Find Bar'.
  5. Esc to close the find-bar.
  6. Open the command bar again, run 'manager: Shutdown'.
  7. After vtm-tile exits, every shell child must have terminated. None
     should be reparented to PID 1 as an orphan.

All steps go through the command bar (mouse-click [CMD] -> type query ->
Enter) instead of direct keybindings. The user's report identifies this
path specifically, presumably because the command-bar dispatch routes
events differently than direct keybind dispatch and exposes a missing
SIGHUP path on shutdown.

Detection:
  Each spawned shell tags itself with VTM_TILE_ORPHAN_TAG=<unique-uuid>
  in its environment. Before shutdown we record every tagged pid; after
  vtm-tile exits we wait briefly and re-scan /proc for survivors.
  Any survivor is an orphan -> bug.
"""

import os
import re
import sys
import pty
import time
import uuid
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
SETTLE_DELAY = 1.0


# Minimal tile config:
#   * <menu> stripped down to a single [CMD] launcher (OnLeftClick ->
#     TileOpenCommandBar). All other menu items are absent so the [CMD]
#     button is unambiguous on screen.
#   * <app> uses a custom item id ('orphan_term') to dodge vtm.xml's
#     default `<item id="term" cmd="$0 -r term"/>` which would otherwise
#     win the merge and silently truncate our long cmd.
#   * Each pane runs `/usr/bin/env VTM_TILE_ORPHAN_TAG=<tag> bash -i`.
#     vtm's execvpe whitespace-tokenizes cmd and execvp()s directly with
#     no shell interpretation, so VAR=val prefix syntax doesn't work --
#     we use /usr/bin/env to set the variable for the bash it execs.
def make_tile_config(tag):
    item_cmd = f"$0 -r term /usr/bin/env VTM_TILE_ORPHAN_TAG={tag} bash -i"
    return (
        "<config>"
            "<tile>"
                "<confirm_close=0/>"
                '<app selected="orphan_term">'
                f'<item id="orphan_term" label="orphan_term" type="dtvt" cmd=\'{item_cmd}\'/>'
                "</app>"
                "<menu item*>"
                    '<item label="  [CMD]  " tooltip=" Open the command bar. " script=OnLeftClick|TileOpenCommandBar/>'
                "</menu>"
            "</tile>"
        "</config>"
    )


def kill_all_vtm():
    subprocess.run(["pkill", "-9", "-x", "vtm-tile"], capture_output=True)
    deadline = time.time() + 3.0
    while time.time() < deadline:
        result = subprocess.run(["pgrep", "-x", "vtm-tile"], capture_output=True)
        if result.returncode != 0:
            break
        time.sleep(0.1)
    subprocess.run(["pkill", "-9", "-x", "vtm-tile"], capture_output=True)
    time.sleep(0.2)


def set_winsize(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def read_drain(fd, timeout=0.5):
    deadline = time.time() + timeout
    while time.time() < deadline:
        ready, _, _ = select.select([fd], [], [], 0.05)
        if not ready:
            return
        try:
            data = os.read(fd, 65536)
            if not data:
                return
        except OSError:
            return


# ANSI strippers (re-used from test_command_bar_terminal.py) — sufficient
# for locating ASCII markers in the cumulative paint stream.
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


def find_cmd_button(raw_buf):
    """Return (row, col) of the most recent '[CMD]' paint, or None."""
    last = raw_buf.rfind(b"[CMD]")
    if last < 0:
        return None
    prefix = raw_buf[:last]
    cups = list(re.finditer(rb"\x1b\[(\d+);(\d+)H", prefix))
    if not cups:
        return None
    cup = cups[-1]
    row = int(cup.group(1))
    col = int(cup.group(2))
    between = strip_ansi(raw_buf[cup.end():last]).decode("utf-8", errors="replace")
    start_col = col + len(between)
    return (row, start_col + 2)  # centre of '[CMD]'


def pid_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def proc_has_env_tag(pid, tag):
    try:
        with open(f"/proc/{pid}/environ", "rb") as f:
            data = f.read()
    except OSError:
        return False
    return f"VTM_TILE_ORPHAN_TAG={tag}".encode() in data


def find_tagged_pids(tag):
    found = []
    try:
        for entry in os.listdir("/proc"):
            if not entry.isdigit():
                continue
            pid = int(entry)
            if proc_has_env_tag(pid, tag):
                found.append(pid)
    except OSError:
        pass
    return found


class VtmTileSession:
    def __init__(self, args=None, settle_delay=SETTLE_DELAY):
        self.args = args or []
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
            os.execvp(VTM_TILE_BINARY, [VTM_TILE_BINARY] + self.args)
            os._exit(127)
        os.close(slave_fd)
        time.sleep(self.settle_delay)
        self.snapshot(timeout=1.0)
        return self

    def __exit__(self, *args):
        self.cleanup()

    def cleanup(self):
        if self.pid:
            try: os.kill(self.pid, signal.SIGKILL)
            except ProcessLookupError: pass
            try: os.waitpid(self.pid, os.WNOHANG)
            except ChildProcessError: pass
            self.pid = None
        if self.master_fd is not None:
            try: os.close(self.master_fd)
            except OSError: pass
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
            if pid == 0: return True
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

    def wait_for_exit(self, timeout=10.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if not self.is_alive():
                return True
            time.sleep(0.1)
        return False


def cmdbar_run(s, query, expect_substr, settle=0.6, snapshot_timeout=1.5):
    """Drive the [CMD] command bar: locate button, click, type query, Enter.

    `expect_substr` is checked (after stripping spaces) in the rendered
    overlay to ensure the filter actually surfaced a matching item before
    Enter is pressed. Returns (ok, message).
    """
    s.snapshot(timeout=snapshot_timeout)
    coords = find_cmd_button(s._screen_buf)
    if coords is None:
        return False, "[CMD] button not rendered"
    row, col = coords
    s.reset_buffer()
    s.click(col, row)
    rendered = s.snapshot(timeout=snapshot_timeout)
    if "pane: Focus" not in rendered:
        return False, "command bar overlay did not appear"
    s.reset_buffer()
    s.write(query.encode() if isinstance(query, str) else query)
    rendered = s.snapshot(timeout=snapshot_timeout)
    if expect_substr.replace(" ", "") not in rendered.replace(" ", ""):
        return False, f"filter '{query}' did not surface '{expect_substr}'"
    s.write(b"\r")
    time.sleep(settle)
    s.snapshot(timeout=snapshot_timeout)
    return True, ""


def run_orphan_test():
    print("TEST: vtm-tile shutdown leaves no orphan processes (cmdbar path) ... ",
          end="", flush=True)
    tag = f"vtm-orphan-{uuid.uuid4().hex}"
    cfg = make_tile_config(tag)

    # Pre-flight: ensure no stragglers from previous runs.
    leaked_before = find_tagged_pids(tag)
    if leaked_before:
        print(f"FAIL - tag collision (preexisting tagged pids): {leaked_before}")
        return False

    with VtmTileSession(["-c", cfg]) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False

        # Wait for the initial pane's shell to spawn and tag itself.
        deadline = time.time() + 6.0
        initial = []
        while time.time() < deadline:
            initial = find_tagged_pids(tag)
            if initial:
                break
            time.sleep(0.2)
        if not initial:
            print("FAIL - first pane shell never spawned with tag")
            return False

        # Step 2: Split Horizontal via command bar.
        ok, why = cmdbar_run(s, "lay sp hor", "SplitHorizontal")
        if not ok:
            print(f"FAIL - split via cmdbar: {why}")
            return False

        # Wait for the second tagged shell to spawn.
        deadline = time.time() + 6.0
        while time.time() < deadline:
            shells = find_tagged_pids(tag)
            if len(shells) >= 2:
                break
            time.sleep(0.2)
        shells = find_tagged_pids(tag)
        if len(shells) < 2:
            print(f"FAIL - expected >=2 tagged shells after split, got {len(shells)}")
            return False
        print(f"[panes={len(shells)}] ", end="", flush=True)

        # Step 3: Select All Panes via command bar.
        ok, why = cmdbar_run(s, "sel all pan", "SelectAllPanes")
        if not ok:
            print(f"FAIL - select all panes via cmdbar: {why}")
            return False

        # Step 4: Toggle Find Bar via command bar.
        ok, why = cmdbar_run(s, "term tog find", "ToggleFind")
        if not ok:
            print(f"FAIL - toggle find bar via cmdbar: {why}")
            return False

        # Step 5: Esc to close the find bar.
        s.write(b"\x1b")
        time.sleep(0.6)
        s.snapshot(timeout=1.0)

        # Step 6: Shutdown via command bar.
        ok, why = cmdbar_run(s, "manager shut", "Shutdown", settle=0.2)
        if not ok:
            print(f"FAIL - shutdown via cmdbar: {why}")
            return False

        if not s.wait_for_exit(timeout=10.0):
            print("FAIL - vtm-tile did not exit after Shutdown")
            return False

        # CRITICAL: probe orphans inside the `with` block so the session's
        # __exit__ (which runs kill_all_vtm) doesn't mask survivors. The
        # tile's own Shutdown path is responsible for tearing down every
        # vtm-tile child process and SIGHUP'ing each shell. If the shells
        # are still alive here, the bug has reproduced.
        time.sleep(1.0)
        survivors = find_tagged_pids(tag)
        if survivors:
            # Capture diagnostic context before kill_all_vtm wipes it.
            ps_out = subprocess.run(
                ["ps", "-o", "pid,ppid,pgid,sid,stat,comm,args", "-p",
                 ",".join(str(p) for p in survivors)],
                capture_output=True, text=True
            ).stdout
            print(f"FAIL - {len(survivors)} tagged shell(s) survived Shutdown: {survivors}")
            print(ps_out)
            return False

    print("PASS")
    return True


def main():
    if not os.path.isfile(VTM_TILE_BINARY):
        print(f"ERROR: vtm-tile binary not found at {VTM_TILE_BINARY}")
        sys.exit(2)
    kill_all_vtm()
    try:
        ok = run_orphan_test()
    finally:
        print("test")
        kill_all_vtm()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
