#!/usr/bin/env python3
# Copyright (c) Dmitry Sapozhnikov
# Licensed under the MIT license.

"""
Startup output capture test for vtm-tile.

Opens vtm-tile in an 80×24 pty, captures all bytes emitted during startup,
strips ANSI/VT escape sequences, and prints the resulting human-readable text
directly to stdout (in the same style as the failure-mode debug dump in
test_command_bar_dispatches_only_to_focused_pane /
test_two_pane_both_ctrl_click_defocused_cmdbar_no_ops), then exits vtm-tile
cleanly via the title-bar close button.

Intended as a diagnostic / exploration tool: the output shows exactly what
vtm-tile renders on first paint in a standard 80×24 terminal.
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

COLS = 80
ROWS = 24
READ_TIMEOUT = 5.0
SETTLE_DELAY = 1.0

# Minimal tile config: one terminal pane, no menu customisation needed
# because this test only inspects the raw startup paint.
TILE_CONFIG = (
    "<config>"
        "<tile>"
            "<confirm_close=0/>"
            '<app selected="term">'
                '<item id="term" label="term" type="dtvt" cmd="$0 -r term"/>'
            "</app>"
        "</tile>"
    "</config>"
)
TILE_ARGS = ["-c", TILE_CONFIG]

# ── ANSI strip helpers (copied from test_command_bar_terminal.py) ────────────

_ANSI_CSI   = re.compile(rb"\x1b\[[\x30-\x3f]*[\x20-\x2f]*[\x40-\x7e]")
_ANSI_OSC   = re.compile(rb"\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)")
_ANSI_DCS   = re.compile(rb"\x1bP[^\x1b]*\x1b\\")
_ANSI_OTHER = re.compile(rb"\x1b[()][\x30-\x7e]|\x1b[=>78NOMcDEHM]")


def strip_ansi(buf):
    out = _ANSI_DCS.sub(b"", buf)
    out = _ANSI_OSC.sub(b"", out)
    out = _ANSI_CSI.sub(b"", out)
    out = _ANSI_OTHER.sub(b"", out)
    return out


# ── Low-level pty helpers ────────────────────────────────────────────────────

def set_winsize(fd, rows, cols):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def read_all(fd, timeout=READ_TIMEOUT):
    """Drain all available data from *fd* until the stream goes quiet."""
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
            # Brief quiet period after receiving data → assume drained.
            break
    return data


def kill_all_vtm():
    subprocess.run(["pkill", "-9", "-x", "vtm-tile"], capture_output=True)
    deadline = time.time() + 3.0
    while time.time() < deadline:
        result = subprocess.run(["pgrep", "-x", "vtm-tile"], capture_output=True)
        if result.returncode != 0:
            break
        time.sleep(0.1)
    subprocess.run(["pkill", "-9", "-x", "vtm-tile"], capture_output=True)


# ── Session context manager ─────────────────────────────────────────────────

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
            os.execvp(VTM_TILE_BINARY, [VTM_TILE_BINARY] + self.args)
            sys.exit(1)
        os.close(slave_fd)
        time.sleep(self.settle_delay)
        # Capture initial paint into our running buffer.
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

    def read(self, timeout=READ_TIMEOUT):
        chunk = read_all(self.master_fd, timeout=timeout)
        self._screen_buf += chunk
        return chunk

    def snapshot(self, timeout=1.0):
        """Drain pending output and append it to the running buffer."""
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

    def wait_for_exit(self, timeout=5.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if not self.is_alive():
                return True
            time.sleep(0.1)
        return False

    def click(self, col, row, button=0):
        self.write(f"\033[<{button};{col};{row}M".encode())
        time.sleep(0.05)
        self.write(f"\033[<{button};{col};{row}m".encode())

    def click_close_button(self):
        """Click the × close button in the title bar (row 1, near right edge)."""
        self.click(COLS - 2, 1)

    def normal_exit(self, timeout=5.0):
        """Click the vtm-tile top-right close button and wait for exit.

        Mirrors the same helper in test_command_bar_terminal.py: we read
        briefly before clicking to drain any pending paint so the click
        is not swallowed by an in-progress SGR sequence.
        """
        self.read(timeout=0.2)
        time.sleep(0.2)
        self.click_close_button()
        return self.wait_for_exit(timeout=timeout)


# ── Diagnostic printer (matches the failure-mode dump style from
#    test_command_bar_dispatches_only_to_focused_pane and
#    test_two_pane_both_ctrl_click_defocused_cmdbar_no_ops) ─────────────────

def print_visible_capture(raw: bytes, label: str = "startup") -> None:
    """Print the human-readable text extracted from the raw pty stream.

    Strips all ANSI/VT escape sequences and prints the resulting visible
    characters directly — no repr-quoting, no hex dump.  This mirrors the
    visible-text branch of the failure-mode debug dump used in
    test_two_pane_both_ctrl_click_defocused_cmdbar_no_ops:

        visible = strip_ansi(s._screen_buf).decode("utf-8", errors="replace")
        print(f"\\nDEBUG visible (first 600 chars): {repr(visible[:600])}")

    except that here we print the full text unquoted so it is immediately
    readable on a standard terminal.
    """
    visible = strip_ansi(raw).decode("utf-8", errors="replace")
    print(f"\n--- visible capture: {label} ({len(raw)} raw bytes) ---")
    print(visible)
    print(f"--- end of capture: {label} ---\n")


# ── Main test ────────────────────────────────────────────────────────────────

def test_capture_startup_bytes():
    """Start vtm-tile in an 80×24 pty, print the visible startup text, then exit.

    This test always passes as long as vtm-tile starts and exits cleanly.
    Its purpose is to provide a reproducible, human-readable snapshot of
    what vtm-tile renders on first paint so developers can inspect visible
    glyphs and layout without running the full regression suite.

    The output format mirrors the visible-text branch of the failure-mode
    debug dump introduced in test_two_pane_both_ctrl_click_defocused_cmdbar_no_ops
    (lines 1235–1241 of test_command_bar_terminal.py):

        visible = strip_ansi(s._screen_buf).decode("utf-8", errors="replace")
        print(f"\\nDEBUG visible (first 600 chars): {repr(visible[:600])}")
    """
    print("TEST: capture vtm-tile startup bytes and exit ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False

        # Drain the initial paint with generous retries (the first pty
        # delivery can be slow when other tests run back-to-back).
        for _ in range(4):
            s.snapshot(timeout=1.5)
            if s._screen_buf:
                break

        # ── Type "1234567890abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ" into the focused terminal pane ────────────
        # Do NOT reset the buffer: _screen_buf accumulates every byte
        # from startup through the keystroke echo so print_visible_capture
        # reflects the full rendered state of the screen.
        s.write(b"1234567890abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ")
        s.snapshot(timeout=1.5)

        # ── Print the full accumulated visible output ────────────────────
        print_visible_capture(s._screen_buf, label="full output after typing '1234567890abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'")

        # ── Verify "1234567890abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ" is visible on screen ─────────────────────
        visible = strip_ansi(s._screen_buf).decode("utf-8", errors="replace")
        if "1234567890abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ" not in visible:
            print("FAIL - '1234567890abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ' not found in screen output")
            return False

        # ── Exit cleanly via the title-bar close button ─────────────────
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit after clicking close button")
            return False

        print("PASS")
        return True


TESTS = [
    test_capture_startup_bytes,
]


def main():
    if not os.path.isfile(VTM_TILE_BINARY):
        print(f"ERROR: vtm-tile binary not found at {VTM_TILE_BINARY}")
        print("Set VTM_TILE_BINARY env var or build vtm-tile first.")
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
