#!/usr/bin/env python3
# Copyright (c) Shaobo Song
# Licensed under the MIT license.

"""
Regression test for the phantom terminal bug when clicking the × close button
on a maximized (zoomed) tile pane.

Bug: when a pane is maximized and the user clicks its × close button,
workspace_0's release-quit handler calls base::signal (local reactor only)
instead of base::riseup to re-dispatch the quit after restoring the pane.
The signal never reaches the enclosing node_veer, so pop_back() is never
called and the dead app_window lingers as a phantom terminal.

Fix: change the signal() call to riseup() so the event propagates up to the
node_veer, which removes the dead app_window and leaves a clean empty slot.

Detection strategy
------------------
After closing, the right-hand slot must be replaced by node_veer's empty
placeholder, which renders the literal text "Empty Slot" (see
src/netxs/apps/tile.hpp:1641).  We strip ANSI/VT escape sequences from the
pty output and assert that "Empty Slot" appears in the visible text — same
approach used by test/test_capture_startup_bytes.py.

Tests
-----
1. test_close_button_on_maximized_pane_leaves_empty_slot
   Split → zoom right pane → click × close button → press Enter to confirm
   the close dialog → the visible screen text must contain "Empty Slot".
   FAILS before the fix, PASSES after it.

2. test_close_button_on_split_pane_leaves_empty_slot
   Split (no zoom) → click × close button on right pane → press Enter →
   visible text must contain "Empty Slot".

3. test_closepane_on_maximized_pane_leaves_empty_slot
   Same setup as #1 but uses the ClosePane scripting method (fast=true,
   bypasses confirm dialog).  ClosePane was already working; this is the
   passing baseline contrast case.
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


# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

VTM_TILE_BINARY = os.environ.get(
    "VTM_TILE_BINARY",
    os.path.join(os.path.dirname(__file__), "..", "build", "vtm-tile"),
)

COLS = 80
ROWS = 24
READ_TIMEOUT = 5.0
TILE_SETTLE_DELAY = 1.0

# <include*/> clears the bundled include list (/etc/vtm/settings.xml and
# ~/.config/vtm/settings.xml from vtm.xml) so user-site config cannot
# shadow the inline -c overrides below.
# confirm_close=true so × shows the confirmation dialog; the tests press
# Enter to confirm (Confirm is the default selection) and exercise the
# post-confirm close path.
# Keyboard shortcuts: Alt+Shift+| for SplitPane (horizontal),
#                     Alt+Shift+Z for ZoomPane,
#                     Alt+Shift+C for ClosePane.
TILE_CONFIG = (
    "<include*/>"
    "<config>"
        "<tile><confirm_close=true/></tile>"
        "<terminal><confirm_close=true/></terminal>"
        "<events><tile>"
            """<script=TileSplitHorizontally on="Alt+Shift+'|'"/>"""
            """<script=TileZoomPane on="Alt+Shift+Z"/>"""
            """<script=TileClosePane on="Alt+Shift+C"/>"""
        "</tile></events>"
    "</config>"
    "<Scripting>"
        '<TileSplitHorizontally="vtm.tile.SplitPane(0);"/>'
        '<TileZoomPane="vtm.tile.ZoomPane();"/>'
        '<TileClosePane="vtm.tile.ClosePane();"/>'
    "</Scripting>"
)
TILE_ARGS = []  # TILE_CONFIG is shipped via $VTM_CONFIG (vtm_config kwarg).

SPLIT_HZ_KEY  = b"\x1b|"   # Alt+Shift+|  → SplitPane(horizontal)
ZOOM_PANE_KEY = b"\x1bZ"   # Alt+Shift+Z  → ZoomPane
CLOSE_PANE_KEY = b"\x1bC"  # Alt+Shift+C  → ClosePane

# Literal label rendered by node_veer's empty placeholder
# (src/netxs/apps/tile.hpp:1641).  Its presence after a close confirms the
# slot is empty rather than occupied by a phantom app_window.
EMPTY_SLOT_LABEL = "Empty Slot"


# ---------------------------------------------------------------------------
# ANSI strip helpers (mirrors test_capture_startup_bytes.py)
# ---------------------------------------------------------------------------

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


# ---------------------------------------------------------------------------
# Process / pty helpers (inlined; no imports from other test files)
# ---------------------------------------------------------------------------

def kill_all_vtm():
    for name in ("vtm-desk", "vtm-tile"):
        subprocess.run(["pkill", "-9", "-x", name], capture_output=True)
    for _ in range(20):
        time.sleep(0.3)
        gone = True
        for name in ("vtm-desk", "vtm-tile"):
            r = subprocess.run(["pgrep", "-x", name], capture_output=True)
            if r.returncode == 0:
                gone = False
                break
        if gone:
            return
    for name in ("vtm-desk", "vtm-tile"):
        subprocess.run(["pkill", "-9", "-x", name], capture_output=True)
    time.sleep(0.5)


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


def sgr_press(col, row, button=0):   return f"\033[<{button};{col};{row}M".encode()
def sgr_release(col, row, button=0): return f"\033[<{button};{col};{row}m".encode()


class VtmSession:
    def __init__(self, binary, args, settle_delay=TILE_SETTLE_DELAY, vtm_config=None):
        self.binary = binary
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
            os.dup2(slave_fd, 0); os.dup2(slave_fd, 1); os.dup2(slave_fd, 2)
            if slave_fd > 2:
                os.close(slave_fd)
            if self.vtm_config is not None:
                os.environ["VTM_CONFIG"] = self.vtm_config
            os.execvp(self.binary, [self.binary] + self.args)
            sys.exit(1)
        os.close(slave_fd)
        time.sleep(self.settle_delay)
        self._screen_buf += read_all(self.master_fd, timeout=1.0)
        return self

    def __exit__(self, *exc):
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

    def read(self, timeout=READ_TIMEOUT):
        chunk = read_all(self.master_fd, timeout=timeout)
        self._screen_buf += chunk
        return chunk

    def reset_buffer(self):
        self._screen_buf = b""

    def visible(self):
        return strip_ansi(self._screen_buf).decode("utf-8", errors="replace")

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


# ---------------------------------------------------------------------------
# Shared setup helpers
# ---------------------------------------------------------------------------

def do_split_and_zoom_right(session):
    """Split horizontally and zoom the right pane.  Returns True on success."""
    # Split: the new right pane gets focus automatically.
    session.write(SPLIT_HZ_KEY)
    time.sleep(1.5)
    session.read(timeout=0.5)
    if not session.is_alive():
        return False

    # Click center of right pane to ensure focus, then zoom.
    session.click(3 * COLS // 4, ROWS // 2)
    time.sleep(0.3)
    session.read(timeout=0.3)

    session.write(ZOOM_PANE_KEY)
    time.sleep(1.0)
    session.read(timeout=0.5)
    return session.is_alive()


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_close_button_on_maximized_pane_leaves_empty_slot():
    """Close button on maximized pane must leave an empty slot, not a phantom.

    This test exposes the bug where workspace_0's release-quit handler used
    base::signal (local-reactor only) instead of base::riseup after restoring
    the pane, so node_veer never received the quit and never called pop_back().

    Expected after the fix:
      - vtm-tile stays alive (left terminal still running).
      - The visible screen text contains "Empty Slot" — node_veer replaced
        the closed app_window with its empty placeholder.
    """
    print("TEST: tile - x close button on maximized pane leaves empty slot ... ",
          end="", flush=True)
    with VtmSession(VTM_TILE_BINARY, TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not do_split_and_zoom_right(s):
            print("FAIL - vtm died during split/zoom setup")
            return False

        # Drop everything emitted during setup so the post-close buffer only
        # reflects the screen state after the close action.
        s.reset_buffer()

        # Click the × close button of the maximized right pane.
        # Row 3 is the title bar; COLS-2 is where the × sits (1-indexed).
        s.click(COLS - 2, 3)
        # The close button triggers the "Confirm closing this window?" dialog.
        # Pressing Enter activates the default "Confirm" button and proceeds
        # with the close — this is the path that exposes the phantom bug.
        time.sleep(0.6)
        s.read(timeout=0.3)
        s.write(b"\r")
        # Allow time for the shell to receive SIGHUP, exit, and vtm to
        # process the quit event and re-render the split view.
        time.sleep(2.5)
        s.read(timeout=0.8)

        if not s.is_alive():
            print("FAIL - vtm-tile exited (left terminal should still be running)")
            return False

        visible = s.visible()
        if EMPTY_SLOT_LABEL not in visible:
            print(f"FAIL - phantom terminal detected: '{EMPTY_SLOT_LABEL}' not "
                  f"found in visible output after close")
            print(f"--- visible (last 800 chars) ---\n{visible[-800:]}\n--- end ---")
            return False

        print("PASS")
        return True


def test_close_button_on_split_pane_leaves_empty_slot():
    """Close button on a (non-zoomed) split right pane must leave an empty slot.

    Contrast case for the maximized test above: same close-button + confirm
    flow but the pane is just side-by-side split (not zoomed). This catches
    regressions where the non-maximized close path itself fails to drop the
    app_window from the node_veer.
    """
    print("TEST: tile - x close button on split right pane leaves empty slot ... ",
          end="", flush=True)
    with VtmSession(VTM_TILE_BINARY, TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        # Split horizontally; the new right pane gets focus.
        s.write(SPLIT_HZ_KEY)
        time.sleep(1.5)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - vtm died during split setup")
            return False

        # Click center of right pane to ensure focus (no zoom this time).
        s.click(3 * COLS // 4, ROWS // 2)
        time.sleep(0.3)
        s.read(timeout=0.3)

        s.reset_buffer()

        # Click × close button of the right pane (row 3, col COLS-2).
        s.click(COLS - 2, 3)
        # Confirm dialog: Enter activates the default "Confirm" button.
        time.sleep(0.6)
        s.read(timeout=0.3)
        s.write(b"\r")
        time.sleep(2.5)
        s.read(timeout=0.8)

        if not s.is_alive():
            print("FAIL - vtm-tile exited (left terminal should still be running)")
            return False

        visible = s.visible()
        if EMPTY_SLOT_LABEL not in visible:
            print(f"FAIL - phantom terminal detected: '{EMPTY_SLOT_LABEL}' not "
                  f"found in visible output after close")
            print(f"--- visible (last 800 chars) ---\n{visible[-800:]}\n--- end ---")
            return False

        print("PASS")
        return True


def test_closepane_on_maximized_pane_leaves_empty_slot():
    """ClosePane on a maximized pane already leaves a clean empty slot.

    This is the contrast/baseline test: ClosePane uses riseup(preview quit::one)
    which correctly propagates through workspace_0 and then to the node_veer,
    triggering pop_back().  It passes both before and after the close-button fix.

    Since commit 64603594 ("Route tile destructive actions through reusable
    confirm dialog"), ClosePane honors /config/tile/confirm_close just like
    the × close button does, so we press Enter to accept the dialog before
    asserting on the empty slot.
    """
    print("TEST: tile - ClosePane on maximized pane leaves empty slot ... ",
          end="", flush=True)
    with VtmSession(VTM_TILE_BINARY, TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not do_split_and_zoom_right(s):
            print("FAIL - vtm died during split/zoom setup")
            return False

        s.reset_buffer()

        # Close the maximized right pane via the ClosePane scripting method.
        s.write(CLOSE_PANE_KEY)
        time.sleep(0.6)
        s.read(timeout=0.3)
        # confirm_close=true is set; accept the dialog (Confirm is default).
        s.write(b"\r")
        time.sleep(2.5)
        s.read(timeout=0.8)

        if not s.is_alive():
            print("FAIL - vtm-tile exited (left terminal should still be running)")
            return False

        visible = s.visible()
        if EMPTY_SLOT_LABEL not in visible:
            print(f"FAIL - '{EMPTY_SLOT_LABEL}' not found after ClosePane (unexpected)")
            print(f"--- visible (last 800 chars) ---\n{visible[-800:]}\n--- end ---")
            return False

        print("PASS")
        return True


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

TESTS = [
    test_close_button_on_maximized_pane_leaves_empty_slot,
    test_close_button_on_split_pane_leaves_empty_slot,
    test_closepane_on_maximized_pane_leaves_empty_slot,
]


def main():
    if not os.path.isfile(VTM_TILE_BINARY):
        print(f"ERROR: vtm-tile not found at {VTM_TILE_BINARY}")
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
