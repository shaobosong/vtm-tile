#!/usr/bin/env python3
# Copyright (c) Dmitry Sapozhnikov
# Licensed under the MIT license.

"""
TUI regression tests for the close confirmation dialog.

Tests that clicking the x button in term/tile apps shows a confirmation
dialog (when confirm_close=true), and that pressing Esc/Tab/Enter or
clicking the Confirm/Cancel buttons correctly confirms or cancels the
close.

Uses SGR mouse protocol and raw keyboard input to simulate user activity.
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

VTM_DESK_BINARY = os.environ.get(
    "VTM_DESK_BINARY",
    os.path.join(os.path.dirname(__file__), "..", "build", "vtm-desk"),
)
VTM_TILE_BINARY = os.environ.get(
    "VTM_TILE_BINARY",
    os.path.join(os.path.dirname(__file__), "..", "build", "vtm-tile"),
)

# Terminal size for tests.
COLS = 80
ROWS = 24

# Timeout for reads (seconds).
READ_TIMEOUT = 5.0

# Time to let vtm fully start up (shell launch + initial render).
SETTLE_DELAY = 2.5

# Tile (C/S architecture) needs more startup time.
TILE_SETTLE_DELAY = 1.0

# Self-contained desk/term configuration: confirm_close enabled for the
# terminal, so tests do not depend on vtm.xml built-in defaults.
DESK_TERM_ARGS = ["-c", "<config><terminal><confirm_close=1/></terminal></config>", "-r", "term"]

# Self-contained tile configuration: confirm_close enabled, split key binding
# and scripting definition included for split regression tests.
TILE_CONFIG = (
    "<config>"
        "<tile><confirm_close=1/></tile>"
        "<events><tile>"
            """<script=TileSplitHorizontally on="Alt+Shift+'|'"/>"""
        "</tile></events>"
    "</config>"
    "<Scripting>"
        '<TileSplitHorizontally="vtm.tile.SplitPane(0);"/>'
    "</Scripting>"
)
TILE_ARGS = ["-c", TILE_CONFIG]


def kill_all_vtm():
    """Kill all vtm-desk and vtm-tile processes to clean up after tests."""
    for name in ("vtm-desk", "vtm-tile"):
        subprocess.run(["pkill", "-9", "-x", name], capture_output=True)
    # Wait until all vtm processes are actually gone.
    for _ in range(20):
        time.sleep(0.3)
        gone = True
        for name in ("vtm-desk", "vtm-tile"):
            result = subprocess.run(["pgrep", "-x", name], capture_output=True)
            if result.returncode == 0:
                gone = False
                break
        if gone:
            return
    # If still running after 6s, try again.
    for name in ("vtm-desk", "vtm-tile"):
        subprocess.run(["pkill", "-9", "-x", name], capture_output=True)
    time.sleep(0.5)


def set_winsize(fd, rows, cols):
    """Set terminal window size via ioctl."""
    winsize = struct.pack("HHHH", rows, cols, 0, 0)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, winsize)


def read_all(fd, timeout=READ_TIMEOUT):
    """Read all available data from fd within timeout."""
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


def sgr_mouse_press(col, row, button=0):
    """Return bytes for an SGR mouse press at (col, row). 1-indexed."""
    return f"\033[<{button};{col};{row}M".encode()


def sgr_mouse_release(col, row, button=0):
    """Return bytes for an SGR mouse release at (col, row). 1-indexed."""
    return f"\033[<{button};{col};{row}m".encode()


def sgr_mouse_move(col, row):
    """Return bytes for an SGR mouse move (no button) at (col, row). 1-indexed."""
    return f"\033[<35;{col};{row}M".encode()


class VtmTestSession:
    """Manage a vtm-desk or vtm-tile process running in a pty for testing."""

    def __init__(self, binary, args, settle_delay=SETTLE_DELAY):
        self.binary = binary
        self.args = args
        self.settle_delay = settle_delay
        self.master_fd = None
        self.pid = None

    def __enter__(self):
        self.master_fd, slave_fd = pty.openpty()
        set_winsize(self.master_fd, ROWS, COLS)

        self.pid = os.fork()
        if self.pid == 0:
            # Child process.
            os.close(self.master_fd)
            os.setsid()
            fcntl.ioctl(slave_fd, termios.TIOCSCTTY, 0)
            os.dup2(slave_fd, 0)
            os.dup2(slave_fd, 1)
            os.dup2(slave_fd, 2)
            if slave_fd > 2:
                os.close(slave_fd)
            os.execvp(self.binary, [self.binary] + self.args)
            sys.exit(1)
        else:
            os.close(slave_fd)
            # Wait for vtm to initialize.
            time.sleep(self.settle_delay)
            # Drain initial output.
            read_all(self.master_fd, timeout=1.0)
            return self

    def __exit__(self, *args):
        self.cleanup()

    def cleanup(self):
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
        """Write data to the master fd."""
        if isinstance(data, str):
            data = data.encode()
        os.write(self.master_fd, data)

    def read(self, timeout=READ_TIMEOUT):
        """Read all available output."""
        return read_all(self.master_fd, timeout=timeout)

    def click(self, col, row, button=0):
        """Simulate a mouse click at (col, row). 1-indexed."""
        self.write(sgr_mouse_press(col, row, button))
        time.sleep(0.05)
        self.write(sgr_mouse_release(col, row, button))

    def mouse_move(self, col, row):
        """Send a mouse move event to (col, row). 1-indexed."""
        self.write(sgr_mouse_move(col, row))

    def click_close_button(self):
        """Click the x close button (row 1, near right edge)."""
        # The x button is at col COLS-2 (78 for 80-col terminal), row 1.
        self.click(COLS - 2, 1)

    def is_alive(self):
        """Check if the vtm process is still running."""
        if self.pid is None:
            return False
        try:
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid == 0:
                return True
            self.pid = None
            return False
        except ChildProcessError:
            self.pid = None
            return False

    def wait_for_exit(self, timeout=5.0):
        """Wait for vtm to exit, return True if it exited."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if not self.is_alive():
                return True
            time.sleep(0.1)
        return False


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def open_dialog_and_verify(session):
    """Click x button and verify vtm is still alive (dialog should be shown).
    Returns True if vtm stayed alive after clicking x.
    """
    session.click_close_button()
    time.sleep(0.8)
    # Drain render output.
    session.read(timeout=0.5)
    return session.is_alive()


def verify_cancel_via_reconfirm(session):
    """After a cancel action, verify the dialog was really dismissed by
    opening it again and confirming with Enter.  If vtm exits, the dialog
    was truly functional.  Returns True on success.
    """
    time.sleep(0.5)
    if not session.is_alive():
        return False  # vtm already exited (bad)
    # Open dialog again.
    session.click_close_button()
    time.sleep(0.8)
    session.read(timeout=0.3)
    if not session.is_alive():
        return False  # vtm exited when opening dialog (bad)
    # Confirm with Enter (default selection is Confirm).
    session.write(b"\r")
    return session.wait_for_exit(timeout=5.0)


# ---------------------------------------------------------------------------
# Tests for `vtm -r term`
# ---------------------------------------------------------------------------

def test_term_close_button_shows_dialog():
    """Clicking x in term with confirm_close=true keeps vtm-desk alive (dialog shown)."""
    print("TEST: term - close button shows dialog ... ", end="", flush=True)
    with VtmTestSession(VTM_DESK_BINARY, DESK_TERM_ARGS) as s:
        if not open_dialog_and_verify(s):
            print("FAIL - vtm exited immediately")
            return False
        # Confirm with Enter so vtm exits normally instead of being SIGKILLed.
        s.write(b"\r")
        if not s.wait_for_exit(timeout=5.0):
            print("FAIL - vtm did not exit after confirming dialog")
            return False
        print("PASS")
        return True


def test_term_confirm_enter():
    """Pressing Enter while dialog is open confirms close."""
    print("TEST: term - confirm by Enter ... ", end="", flush=True)
    with VtmTestSession(VTM_DESK_BINARY, DESK_TERM_ARGS) as s:
        if not open_dialog_and_verify(s):
            print("FAIL - vtm exited before dialog")
            return False
        s.write(b"\r")
        if s.wait_for_exit(timeout=5.0):
            print("PASS")
            return True
        print("FAIL - vtm did not exit after Enter")
        return False


def test_term_tab_then_enter_cancels():
    """Default selection is Confirm; Tab toggles to Cancel; Enter then cancels."""
    print("TEST: term - Tab then Enter cancels ... ", end="", flush=True)
    with VtmTestSession(VTM_DESK_BINARY, DESK_TERM_ARGS) as s:
        if not open_dialog_and_verify(s):
            print("FAIL - vtm exited before dialog")
            return False
        # Tab → highlight Cancel.
        s.write(b"\t")
        time.sleep(0.3)
        # Enter dispatches to currently selected (Cancel) → dialog dismisses,
        # vtm stays alive.
        s.write(b"\r")
        time.sleep(0.5)
        if not s.is_alive():
            print("FAIL - vtm exited after Tab+Enter (should have cancelled)")
            return False
        # Reconfirm by re-opening dialog and pressing Enter (default Confirm).
        if verify_cancel_via_reconfirm(s):
            print("PASS")
            return True
        print("FAIL - could not reconfirm after Tab+Enter cancel")
        return False


def test_term_tab_twice_then_enter_confirms():
    """Tab twice toggles back to Confirm; Enter then confirms close."""
    print("TEST: term - Tab x2 then Enter confirms ... ", end="", flush=True)
    with VtmTestSession(VTM_DESK_BINARY, DESK_TERM_ARGS) as s:
        if not open_dialog_and_verify(s):
            print("FAIL - vtm exited before dialog")
            return False
        # Tab Tab → back to Confirm.
        s.write(b"\t")
        time.sleep(0.2)
        s.write(b"\t")
        time.sleep(0.3)
        s.write(b"\r")
        if s.wait_for_exit(timeout=5.0):
            print("PASS")
            return True
        print("FAIL - vtm did not exit after Tab x2 + Enter")
        return False


def test_term_hover_cancel_then_enter_cancels():
    """Hovering Cancel button selects it; Enter then cancels."""
    print("TEST: term - hover Cancel then Enter cancels ... ", end="", flush=True)
    with VtmTestSession(VTM_DESK_BINARY, DESK_TERM_ARGS) as s:
        if not open_dialog_and_verify(s):
            print("FAIL - vtm exited before dialog")
            return False
        # Compute Cancel button center (see test_term_click_no_button).
        dialog_left = (COLS - 44) // 2 + 1
        dialog_top = (ROWS - 5) // 2 + 1
        inner_left = dialog_left + 3
        button_row = dialog_top + 1 + 2
        cancel_center = inner_left + 18 + 2 + 18 // 2
        # MouseEnter handler fires on a move into the cell.
        s.mouse_move(cancel_center, button_row)
        time.sleep(0.4)
        # Enter dispatches to currently selected (Cancel).
        s.write(b"\r")
        time.sleep(0.5)
        if not s.is_alive():
            print("FAIL - vtm exited after hover-Cancel + Enter")
            return False
        if verify_cancel_via_reconfirm(s):
            print("PASS")
            return True
        print("FAIL - could not reconfirm after hover-Cancel cancel")
        return False


def test_term_hover_cancel_then_tab_then_enter_confirms():
    """Hover Cancel, then Tab toggles back to Confirm; Enter confirms close."""
    print("TEST: term - hover Cancel then Tab then Enter confirms ... ", end="", flush=True)
    with VtmTestSession(VTM_DESK_BINARY, DESK_TERM_ARGS) as s:
        if not open_dialog_and_verify(s):
            print("FAIL - vtm exited before dialog")
            return False
        dialog_left = (COLS - 44) // 2 + 1
        dialog_top = (ROWS - 5) // 2 + 1
        inner_left = dialog_left + 3
        button_row = dialog_top + 1 + 2
        cancel_center = inner_left + 18 + 2 + 18 // 2
        s.mouse_move(cancel_center, button_row)  # selects Cancel via hover
        time.sleep(0.3)
        s.write(b"\t")  # Toggles selection back to Confirm.
        time.sleep(0.2)
        s.write(b"\r")
        if s.wait_for_exit(timeout=5.0):
            print("PASS")
            return True
        print("FAIL - vtm did not exit after hover+Tab+Enter")
        return False


def test_term_cancel_esc():
    """Pressing Esc while dialog is open cancels the close."""
    print("TEST: term - cancel by Esc ... ", end="", flush=True)
    with VtmTestSession(VTM_DESK_BINARY, DESK_TERM_ARGS) as s:
        if not open_dialog_and_verify(s):
            print("FAIL - vtm exited before dialog")
            return False
        s.write(b"\x1b")
        time.sleep(0.5)
        if not s.is_alive():
            print("FAIL - vtm exited after Esc")
            return False
        # Verify dialog was real by re-opening and confirming.
        if verify_cancel_via_reconfirm(s):
            print("PASS")
            return True
        print("FAIL - could not reconfirm after Esc cancel")
        return False


def test_term_cancel_click_outside():
    """Clicking outside the dialog cancels the close."""
    print("TEST: term - cancel by click outside ... ", end="", flush=True)
    with VtmTestSession(VTM_DESK_BINARY, DESK_TERM_ARGS) as s:
        if not open_dialog_and_verify(s):
            print("FAIL - vtm exited before dialog")
            return False
        # Click bottom-left corner (outside dialog).
        s.click(1, ROWS)
        time.sleep(0.5)
        if not s.is_alive():
            print("FAIL - vtm exited after clicking outside")
            return False
        if verify_cancel_via_reconfirm(s):
            print("PASS")
            return True
        print("FAIL - could not reconfirm after click-outside cancel")
        return False


def test_term_click_yes_button():
    """Clicking the Yes button in the dialog confirms close."""
    print("TEST: term - click Yes button ... ", end="", flush=True)
    with VtmTestSession(VTM_DESK_BINARY, DESK_TERM_ARGS) as s:
        if not open_dialog_and_verify(s):
            print("FAIL - vtm exited before dialog")
            return False
        # Dialog is 44 × 5, centered, with setpad(l=3, r=3, t=1, b=1) → inner 38 × 3.
        # Layout: slot_1 (message) 2 rows, slot_2 (buttons) 1 row.
        # Button row (X-fork, gap=2): slot_1 = Confirm (18 cols), gap 2, slot_2 = Cancel (18 cols).
        #   dialog_left = (80-44)//2 + 1 = 19   (1-indexed)
        #   dialog_top  = (24-5)//2 + 1  = 10
        #   inner_left  = 19 + 3 = 22
        #   button_row  = 10 + 1 + 2    = 13   (outer_top + t_pad + 2 message rows)
        #   confirm_x   = 22 + 18//2    = 31
        dialog_left = (COLS - 44) // 2 + 1
        dialog_top = (ROWS - 5) // 2 + 1
        inner_left = dialog_left + 3           # setpad l=3
        button_row = dialog_top + 1 + 2        # t_pad(1) + message rows(2)
        confirm_center = inner_left + 18 // 2  # center of slot_1 (18 cols wide)
        s.click(confirm_center, button_row)
        if s.wait_for_exit(timeout=5.0):
            print("PASS")
            return True
        print("FAIL - vtm did not exit after clicking Yes")
        return False


def test_term_click_no_button():
    """Clicking the No button in the dialog cancels close."""
    print("TEST: term - click No button ... ", end="", flush=True)
    with VtmTestSession(VTM_DESK_BINARY, DESK_TERM_ARGS) as s:
        if not open_dialog_and_verify(s):
            print("FAIL - vtm exited before dialog")
            return False
        # See test_term_click_yes_button for layout derivation.
        #   cancel_x = inner_left + 18 + 2 + 18//2
        dialog_left = (COLS - 44) // 2 + 1
        dialog_top = (ROWS - 5) // 2 + 1
        inner_left = dialog_left + 3
        button_row = dialog_top + 1 + 2
        cancel_center = inner_left + 18 + 2 + 18 // 2  # center of slot_2
        s.click(cancel_center, button_row)
        time.sleep(0.5)
        if not s.is_alive():
            print("FAIL - vtm exited after clicking No")
            return False
        if verify_cancel_via_reconfirm(s):
            print("PASS")
            return True
        print("FAIL - could not reconfirm after No click")
        return False


# ---------------------------------------------------------------------------
# Tests for `vtm-tile`
# ---------------------------------------------------------------------------

def test_tile_close_button_shows_dialog():
    """Clicking x in tile with confirm_close=true keeps vtm-tile alive."""
    print("TEST: tile - close button shows dialog ... ", end="", flush=True)
    with VtmTestSession(VTM_TILE_BINARY, TILE_ARGS, settle_delay=TILE_SETTLE_DELAY) as s:
        if not open_dialog_and_verify(s):
            print("FAIL - vtm exited immediately")
            return False
        # Confirm with Enter so vtm exits normally instead of being SIGKILLed.
        s.write(b"\r")
        if not s.wait_for_exit(timeout=5.0):
            print("FAIL - vtm did not exit after confirming dialog")
            return False
        print("PASS")
        return True


def test_tile_cancel_esc():
    """Pressing Esc while tile dialog is open cancels the close."""
    print("TEST: tile - cancel by Esc ... ", end="", flush=True)
    with VtmTestSession(VTM_TILE_BINARY, TILE_ARGS, settle_delay=TILE_SETTLE_DELAY) as s:
        if not open_dialog_and_verify(s):
            print("FAIL - vtm exited before dialog")
            return False
        s.write(b"\x1b")
        time.sleep(0.5)
        if not s.is_alive():
            print("FAIL - vtm exited after Esc")
            return False
        if verify_cancel_via_reconfirm(s):
            print("PASS")
            return True
        print("FAIL - could not reconfirm after Esc cancel")
        return False


# ---------------------------------------------------------------------------
# Tests for `vtm-tile` after split (Bug 1 & Bug 2 regression tests)
# ---------------------------------------------------------------------------

# The empty_slot mini menu has autohide=true. Using the keyboard shortcut
# Alt+Shift+| (sent as ESC |) for splitting is more reliable than clicking.

# Keyboard shortcut for horizontal split in tile: Alt+Shift+| = ESC |
SPLIT_HZ_KEY = b"\x1b|"


def do_split(session, count=1):
    """Perform horizontal split(s) using keyboard shortcut."""
    for _ in range(count):
        session.write(SPLIT_HZ_KEY)
        time.sleep(1.5)  # Wait for the split to complete and new pane to settle.
        session.read(timeout=0.5)  # Drain render.


def check_all_vtm_exited(timeout=8.0):
    """Wait for all vtm-desk/vtm-tile processes to exit.  Return True if all gone.
    Ignores zombie (defunct) processes — they are already dead but not yet
    reaped by their parent.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        all_gone = True
        for name in ("vtm-desk", "vtm-tile"):
            # Check for non-zombie processes.
            result = subprocess.run(
                ["ps", "-C", name, "-o", "pid=,stat="],
                capture_output=True, text=True,
            )
            if result.returncode != 0:
                continue  # No processes with this name.
            # Filter out zombies (stat starts with 'Z').
            alive = [
                line.strip()
                for line in result.stdout.strip().splitlines()
                if line.split() and not line.split()[1].startswith("Z")
            ]
            if alive:
                all_gone = False
                break
        if all_gone:
            return True
        time.sleep(0.3)
    return False


def test_tile_split_then_close_confirm():
    """Bug 2 regression: split once, confirm close — all processes must exit."""
    print("TEST: tile - split + confirm close cleans up ... ", end="", flush=True)
    with VtmTestSession(VTM_TILE_BINARY, TILE_ARGS, settle_delay=TILE_SETTLE_DELAY) as s:
        # Split the pane.
        do_split(s)
        if not s.is_alive():
            print("FAIL - vtm died during split")
            return False
        # Click main close button.
        if not open_dialog_and_verify(s):
            print("FAIL - vtm exited without showing dialog after split")
            return False
        # Confirm with Enter.
        s.write(b"\r")
        if not s.wait_for_exit(timeout=8.0):
            print("FAIL - vtm client did not exit after Enter")
            return False
        # Verify all vtm processes exited (Bug 2: server/children lingered).
        if check_all_vtm_exited(timeout=8.0):
            print("PASS")
            return True
        # Debug: show remaining processes.
        result = subprocess.run(["pgrep", "-ax", "vtm-tile"], capture_output=True, text=True)
        print(f"FAIL - vtm-tile processes still running: {result.stdout.strip()}")
        return False


def test_tile_split_twice_then_close_confirm():
    """Bug 2 regression: split twice, confirm close — all processes must exit."""
    print("TEST: tile - split x2 + confirm close cleans up ... ", end="", flush=True)
    with VtmTestSession(VTM_TILE_BINARY, TILE_ARGS, settle_delay=TILE_SETTLE_DELAY) as s:
        # Two splits.
        do_split(s, count=2)
        if not s.is_alive():
            print("FAIL - vtm died during splits")
            return False
        # Click main close button.
        if not open_dialog_and_verify(s):
            print("FAIL - vtm exited without showing dialog after 2 splits")
            return False
        # Confirm with Enter.
        s.write(b"\r")
        if not s.wait_for_exit(timeout=8.0):
            print("FAIL - vtm client did not exit after Enter")
            return False
        # Verify all vtm processes exited.
        if check_all_vtm_exited(timeout=8.0):
            print("PASS")
            return True
        result = subprocess.run(["pgrep", "-ax", "vtm-tile"], capture_output=True, text=True)
        print(f"FAIL - vtm-tile processes still running: {result.stdout.strip()}")
        return False


def test_tile_split_then_close_intercept():
    """Bug 1 regression: split once, close button must be intercepted (not bypass)."""
    print("TEST: tile - split + close intercepted ... ", end="", flush=True)
    with VtmTestSession(VTM_TILE_BINARY, TILE_ARGS, settle_delay=TILE_SETTLE_DELAY) as s:
        # Split the pane.
        do_split(s)
        if not s.is_alive():
            print("FAIL - vtm died during split")
            return False
        # Click main close button — should be intercepted (dialog shown).
        s.click_close_button()
        time.sleep(1.0)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - close bypassed intercept after split (Bug 1)")
            return False
        # Cancel and verify we can re-trigger.
        s.write(b"\x1b")
        time.sleep(0.5)
        if not s.is_alive():
            print("FAIL - vtm exited after Esc cancel")
            return False
        # Re-open and confirm to verify full flow.
        if verify_cancel_via_reconfirm(s):
            print("PASS")
            return True
        print("FAIL - could not reconfirm after split+cancel")
        return False


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    for label, path in [("vtm-desk", VTM_DESK_BINARY), ("vtm-tile", VTM_TILE_BINARY)]:
        if not os.path.isfile(path):
            print(f"ERROR: {label} binary not found at {path}")
            print(f"Set {label.upper().replace('-', '_')}_BINARY env var or build {label} first.")
            return 1

    kill_all_vtm()

    tests = [
        # Term tests.
        test_term_close_button_shows_dialog,
        test_term_confirm_enter,
        test_term_cancel_esc,
        test_term_cancel_click_outside,
        test_term_click_yes_button,
        test_term_click_no_button,
        # Term tests (Tab/selection cursor).
        test_term_tab_then_enter_cancels,
        test_term_tab_twice_then_enter_confirms,
        test_term_hover_cancel_then_enter_cancels,
        test_term_hover_cancel_then_tab_then_enter_confirms,
        # Tile tests (basic).
        test_tile_close_button_shows_dialog,
        test_tile_cancel_esc,
        # Tile tests (split regression — Bug 1 & Bug 2).
        test_tile_split_then_close_intercept,
        test_tile_split_then_close_confirm,
        test_tile_split_twice_then_close_confirm,
    ]

    passed = 0
    failed = 0

    for test in tests:
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
