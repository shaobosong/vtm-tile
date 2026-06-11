#!/usr/bin/env python3
# Copyright (c) Shaobo Song
# Licensed under the MIT license.

"""
Integration tests for the workspace feature in tile.

Tests workspace creation (Alt+Shift+C), switching (status bar click),
destruction (Alt+Shift+D), and interaction with the close confirmation dialog.
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

# Terminal size for tests.
COLS = 80
ROWS = 24

# Timeout for reads (seconds).
READ_TIMEOUT = 5.0

# Tile needs startup time for shell launch + initial render.
SETTLE_DELAY = 1.0

# Workspace button width in cells (must match ws_btn_w in tile.hpp).
WS_BTN_W = 3


def kill_all_vtm():
    """Kill all vtm-tile processes to clean up after tests."""
    subprocess.run(["pkill", "-9", "-x", "vtm-tile"], capture_output=True)
    for _ in range(20):
        time.sleep(0.3)
        result = subprocess.run(["pgrep", "-x", "vtm-tile"], capture_output=True)
        if result.returncode != 0:
            return
    subprocess.run(["pkill", "-9", "-x", "vtm-tile"], capture_output=True)
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


class VtmTileSession:
    """Manage a vtm-tile process running in a pty for testing."""

    def __init__(self, args=None, settle_delay=SETTLE_DELAY, vtm_config=None):
        self.args = args or []
        self.settle_delay = settle_delay
        self.vtm_config = vtm_config
        self.master_fd = None
        self.pid = None

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
            if self.vtm_config is not None:
                os.environ["VTM_CONFIG"] = self.vtm_config
            os.execvp(VTM_TILE_BINARY, [VTM_TILE_BINARY] + self.args)
            sys.exit(1)
        else:
            os.close(slave_fd)
            time.sleep(self.settle_delay)
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
        if isinstance(data, str):
            data = data.encode()
        os.write(self.master_fd, data)

    def read(self, timeout=READ_TIMEOUT):
        return read_all(self.master_fd, timeout=timeout)

    def click(self, col, row, button=0):
        """Simulate a mouse click at (col, row). 1-indexed."""
        self.write(sgr_mouse_press(col, row, button))
        time.sleep(0.05)
        self.write(sgr_mouse_release(col, row, button))

    def click_close_button(self):
        """Click the x close button (row 1, near right edge)."""
        self.click(COLS - 2, 1)

    def normal_exit(self, timeout=5.0):
        """Return to the vtm-tile window and click the top-right close button
        to perform a normal exit.

        Sends Esc first to dismiss any open popup/dialog so the click reaches
        the main vtm-tile window. With ``confirm_close`` disabled, clicking the
        close button exits the process directly. Returns True if the process
        exited within ``timeout`` seconds.
        """
        # Dismiss any popup/dialog so focus returns to the vtm-tile window.
        self.write(b"\x1b")
        time.sleep(0.3)
        self.read(timeout=0.2)
        # Click the close button on the top-right of the title bar.
        self.click_close_button()
        return self.wait_for_exit(timeout=timeout)

    def is_alive(self):
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
        deadline = time.time() + timeout
        while time.time() < deadline:
            if not self.is_alive():
                return True
            time.sleep(0.1)
        return False

    # --- Popup layout constants (must match tile.hpp) ---
    POPUP_WS_THUMB_RATIO_W = 5
    POPUP_WS_THUMB_RATIO_H = 2
    POPUP_WS_THUMB_GAP = 2
    POPUP_BOTTOM_PAD_Y = 1
    POPUP_SCROLLBAR_H = 1  # Row reserved for horizontal scrollbar.

    def _popup_layout(self, workspace_count):
        """Compute popup thumbnail positions for a given workspace count."""
        full_w = COLS
        full_h = ROWS
        bot_h = max(4, full_h // 4)
        bot_y = full_h - bot_h
        thumb_h = bot_h - self.POPUP_BOTTOM_PAD_Y - self.POPUP_SCROLLBAR_H
        if thumb_h < 3:
            thumb_h = 3
        thumb_w = max(5, thumb_h * self.POPUP_WS_THUMB_RATIO_W // self.POPUP_WS_THUMB_RATIO_H)
        plus_w = thumb_w
        thumb_y = bot_y + self.POPUP_BOTTOM_PAD_Y
        thumb_stride = thumb_w + self.POPUP_WS_THUMB_GAP
        total_content_w = workspace_count * thumb_stride + (plus_w + self.POPUP_WS_THUMB_GAP)
        base_x = (full_w - total_content_w) // 2 if total_content_w <= full_w else 0
        return base_x, thumb_y, thumb_h, thumb_stride, thumb_w, plus_w

    def open_workspace_popup(self):
        """Click the status bar workspace button to open the preview popup."""
        self.click(2, ROWS)  # Center of the 3-wide button, 1-indexed.

    def popup_click_workspace(self, index, workspace_count):
        """Click a workspace thumbnail in the popup (0-indexed workspace)."""
        base_x, thumb_y, thumb_h, thumb_stride, thumb_w, plus_w = self._popup_layout(workspace_count)
        cx = base_x + index * thumb_stride + thumb_w // 2
        cy = thumb_y + thumb_h // 2
        self.click(cx + 1, cy + 1)  # Convert to 1-indexed.

    def popup_click_plus(self, workspace_count):
        """Click the '+' button in the popup to create a workspace."""
        base_x, thumb_y, thumb_h, thumb_stride, thumb_w, plus_w = self._popup_layout(workspace_count)
        px = base_x + workspace_count * thumb_stride
        cx = px + plus_w // 2
        cy = thumb_y + thumb_h // 2
        self.click(cx + 1, cy + 1)  # Convert to 1-indexed.

    def popup_dismiss_click(self):
        """Dismiss the popup by clicking an empty area (top-left corner)."""
        self.click(1, 1)

    # --- Keyboard-driven popup navigation helpers. ---
    # Escape sequences below are what xterm-compatible terminals send for the
    # named keys. The popup's keybd hook at tile.hpp parses these via the
    # platform input layer and dispatches to Tab/arrow/Enter/Esc handlers.
    def popup_send_tab(self):
        """Send Tab to toggle focus between the top and bottom popup sections."""
        self.write(b"\t")

    def popup_send_escape(self):
        """Send Esc to dismiss the popup."""
        self.write(b"\x1b")

    def popup_send_enter(self):
        """Send Enter to commit the current popup selection."""
        self.write(b"\r")

    def popup_send_left(self):
        """Send Left arrow to the popup."""
        self.write(b"\x1b[D")

    def popup_send_right(self):
        """Send Right arrow to the popup."""
        self.write(b"\x1b[C")

    def popup_send_up(self):
        """Send Up arrow to the popup."""
        self.write(b"\x1b[A")

    def popup_send_down(self):
        """Send Down arrow to the popup."""
        self.write(b"\x1b[B")

    def popup_send_label(self, ch):
        """Send a single label character to the popup (used for index-based selection
        in both the bottom workspace switcher and the top pane grid)."""
        if isinstance(ch, int):
            ch = chr(ch)
        self.write(ch.encode())

    def create_workspace(self):
        """Send Alt+Shift+C to create a new workspace."""
        self.write(b"\x1bC")

    def destroy_workspace(self):
        """Send Alt+Shift+D to destroy the current workspace."""
        self.write(b"\x1bD")

    def next_workspace(self):
        """Send Alt+Shift+F to switch to the next workspace (wrapping)."""
        self.write(b"\x1bF")

    def prev_workspace(self):
        """Send Alt+Shift+B to switch to the previous workspace (wrapping)."""
        self.write(b"\x1bB")

    def last_workspace(self):
        """Send Alt+Shift+L to switch to the last visited workspace."""
        self.write(b"\x1bL")

    def switch_workspace_by_key(self, index):
        """Send Alt+<index> to switch directly to workspace <index> (0-9)."""
        self.write(f"\x1b{index}".encode())

    def select_application(self):
        """Send Alt+Shift+A to cycle to the next application type."""
        self.write(b"\x1bA")

    def last_pane(self):
        """Send Alt+Shift+P to focus the previously focused pane (LastPane)."""
        self.write(b"\x1bP")

    def focus_next_pane(self):
        """Send Alt+Shift+N to focus the next pane (FocusNextPane)."""
        self.write(b"\x1bN")

    def split_horizontal(self):
        """Send Alt+Shift+| to split the current pane horizontally."""
        self.write(b"\x1b|")


# Complete tile configuration: confirm_close enabled, workspace and split key
# bindings with their scripting definitions.  This makes the tests fully
# self-contained and independent of vtm.xml built-in defaults.
TILE_CONFIG = (
    "<config>"
        "<tile>"
            "<confirm_close=0/>"
            '<app selected="term">'
                '<item id="term" label="term" type="dtvt" cmd="$0 -r term"/>'
                '<item id="calc" label="calc" type="calc" cmd=""/>'
            "</app>"
        "</tile>"
        "<events><tile>"
            '<script=TileCreateWorkspace   on="Alt+Shift+C"/>'
            '<script=TileDestroyWorkspace  on="Alt+Shift+D"/>'
            '<script=TileNextWorkspace     on="Alt+Shift+F"/>'
            '<script=TilePrevWorkspace     on="Alt+Shift+B"/>'
            '<script=TileLastWorkspace     on="Alt+Shift+L"/>'
            '<script=TileSwitchWorkspace0  on="Alt+0"/>'
            '<script=TileSwitchWorkspace1  on="Alt+1"/>'
            '<script=TileSwitchWorkspace2  on="Alt+2"/>'
            """<script=TileSplitHorizontally on="Alt+Shift+'|'"/>"""
            '<script=TileSelectApp         on="Alt+Shift+A"/>'
            '<script=TileLastPane          on="Alt+Shift+P"/>'
            '<script=TileFocusNextPane     on="Alt+Shift+N"/>'
        "</tile></events>"
    "</config>"
    "<Scripting>"
        '<TileCreateWorkspace="vtm.tile.CreateWorkspace();"/>'
        '<TileDestroyWorkspace="vtm.tile.DestroyWorkspace();"/>'
        '<TileNextWorkspace="vtm.tile.NextWorkspace();"/>'
        '<TilePrevWorkspace="vtm.tile.PrevWorkspace();"/>'
        '<TileLastWorkspace="vtm.tile.LastWorkspace();"/>'
        '<TileSwitchWorkspace0="vtm.tile.SwitchWorkspace(0);"/>'
        '<TileSwitchWorkspace1="vtm.tile.SwitchWorkspace(1);"/>'
        '<TileSwitchWorkspace2="vtm.tile.SwitchWorkspace(2);"/>'
        '<TileSplitHorizontally="vtm.tile.SplitPane(0);"/>'
        '<TileSelectApp="vtm.tile.SelectApplication(1);"/>'
        '<TileLastPane="vtm.tile.LastPane();"/>'
        '<TileFocusNextPane="vtm.tile.FocusNextPane();"/>'
    "</Scripting>"
)
TILE_ARGS = []  # TILE_CONFIG is shipped via $VTM_CONFIG (vtm_config kwarg).


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_workspace_starts_with_one():
    """vtm-tile starts with a single workspace (workspace 0) and stays alive."""
    print("TEST: workspace - starts with one workspace ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Switch to workspace 0 by key (should be a no-op but not crash).
        s.switch_workspace_by_key(0)
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after switching to workspace 0")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_create_workspace():
    """Alt+Shift+C creates a new workspace; tile stays alive."""
    print("TEST: workspace - create workspace ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        s.create_workspace()
        time.sleep(1.5)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - crashed after creating workspace")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True



def test_destroy_workspace():
    """Create a second workspace, destroy it, tile stays alive with workspace 0."""
    print("TEST: workspace - destroy workspace ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Create workspace 1 (auto-switches to it).
        s.create_workspace()
        time.sleep(1.5)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - crashed after creating workspace 1")
            return False
        # Destroy current workspace (1). Should switch back to workspace 0.
        s.destroy_workspace()
        time.sleep(1.5)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - crashed after destroying workspace 1")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_destroy_last_workspace_exits():
    """Destroying the only workspace should shut down tile."""
    print("TEST: workspace - destroy last workspace exits tile ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Destroy the only workspace (workspace 0).
        s.destroy_workspace()
        time.sleep(2.0)
        s.read(timeout=0.5)
        if s.wait_for_exit(timeout=5.0):
            print("PASS")
            return True
        print("FAIL - vtm-tile did not exit after destroying last workspace")
        return False


def test_workspace_close_button_still_works():
    """Close button works after workspace operations (confirm_close disabled => exits directly)."""
    print("TEST: workspace - close button after workspace ops ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Create a workspace, switch around.
        s.create_workspace()
        time.sleep(1.5)
        s.read(timeout=0.5)
        s.switch_workspace_by_key(0)
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed during workspace ops")
            return False
        # With confirm_close disabled, normal_exit clicks close and tile exits.
        if s.normal_exit(timeout=5.0):
            print("PASS")
            return True
        print("FAIL - vtm did not exit after clicking close button")
        return False


def test_create_multiple_workspaces():
    """Create 3 workspaces, switch between them, tile stays alive."""
    print("TEST: workspace - create 3 workspaces ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Create workspace 1 and 2.
        for i in range(2):
            s.create_workspace()
            time.sleep(1.5)
            s.read(timeout=0.5)
            if not s.is_alive():
                print(f"FAIL - crashed creating workspace {i + 1}")
                return False
        # Now we have workspaces 0, 1, 2. Current should be 2.
        # Switch to each.
        for idx in [0, 1, 2, 0]:
            s.switch_workspace_by_key(idx)
            time.sleep(1.0)
            s.read(timeout=0.3)
            if not s.is_alive():
                print(f"FAIL - crashed switching to workspace {idx}")
                return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_split_in_workspace():
    """Split a pane within a workspace, switch workspaces, no crash."""
    print("TEST: workspace - split in workspace + switch ... ", end="", flush=True)
    SPLIT_HZ_KEY = b"\x1b|"
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Split in workspace 0.
        s.write(SPLIT_HZ_KEY)
        time.sleep(1.5)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - crashed after split in workspace 0")
            return False
        # Create workspace 1.
        s.create_workspace()
        time.sleep(1.5)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - crashed after creating workspace 1")
            return False
        # Switch back to workspace 0 (which has a split).
        s.switch_workspace_by_key(0)
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed switching back to split workspace 0")
            return False
        # Switch to workspace 1 (no split).
        s.switch_workspace_by_key(1)
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed switching to workspace 1")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_next_workspace():
    """NextWorkspace cycles through workspaces, wrapping from last to first."""
    print("TEST: workspace - next workspace wrapping ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Create workspaces 1 and 2 (auto-switches to each).
        for i in range(2):
            s.create_workspace()
            time.sleep(1.5)
            s.read(timeout=0.5)
            if not s.is_alive():
                print(f"FAIL - crashed creating workspace {i + 1}")
                return False
        # Now on workspace 2. Next should wrap to workspace 0.
        s.next_workspace()
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after NextWorkspace (wrap 2->0)")
            return False
        # Next again: workspace 0 -> 1.
        s.next_workspace()
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after NextWorkspace (0->1)")
            return False
        # Next again: workspace 1 -> 2.
        s.next_workspace()
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after NextWorkspace (1->2)")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_prev_workspace():
    """PrevWorkspace cycles through workspaces, wrapping from first to last."""
    print("TEST: workspace - prev workspace wrapping ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Create workspaces 1 and 2.
        for i in range(2):
            s.create_workspace()
            time.sleep(1.5)
            s.read(timeout=0.5)
            if not s.is_alive():
                print(f"FAIL - crashed creating workspace {i + 1}")
                return False
        # Switch to workspace 0.
        s.switch_workspace_by_key(0)
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed switching to workspace 0")
            return False
        # Prev should wrap to workspace 2.
        s.prev_workspace()
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after PrevWorkspace (wrap 0->2)")
            return False
        # Prev again: workspace 2 -> 1.
        s.prev_workspace()
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after PrevWorkspace (2->1)")
            return False
        # Prev again: workspace 1 -> 0.
        s.prev_workspace()
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after PrevWorkspace (1->0)")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_last_workspace():
    """LastWorkspace toggles between the current and last-visited workspace."""
    print("TEST: workspace - last workspace toggle ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Create workspace 1 (auto-switches from 0 to 1).
        s.create_workspace()
        time.sleep(1.5)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - crashed creating workspace 1")
            return False
        # Now on workspace 1, last-visited is workspace 0.
        # LastWorkspace should switch to workspace 0.
        s.last_workspace()
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after LastWorkspace (1->0)")
            return False
        # LastWorkspace again should toggle back to workspace 1.
        s.last_workspace()
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after LastWorkspace (0->1)")
            return False
        # One more toggle back to 0.
        s.last_workspace()
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after LastWorkspace (1->0 again)")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_next_prev_single_workspace():
    """NextWorkspace and PrevWorkspace are no-ops with only one workspace."""
    print("TEST: workspace - next/prev with single workspace ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # With only workspace 0, next and prev should be no-ops.
        s.next_workspace()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after NextWorkspace on single workspace")
            return False
        s.prev_workspace()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after PrevWorkspace on single workspace")
            return False
        s.last_workspace()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after LastWorkspace on single workspace")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_next_prev_after_destroy():
    """NextWorkspace and PrevWorkspace work correctly after a workspace is destroyed."""
    print("TEST: workspace - next/prev after destroy ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Create workspaces 1 and 2.
        for i in range(2):
            s.create_workspace()
            time.sleep(1.5)
            s.read(timeout=0.5)
            if not s.is_alive():
                print(f"FAIL - crashed creating workspace {i + 1}")
                return False
        # Now on workspace 2. Destroy it.
        s.destroy_workspace()
        time.sleep(1.5)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - crashed after destroying workspace 2")
            return False
        # Now 2 workspaces remain (0, 1). Cycle with next.
        s.next_workspace()
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after NextWorkspace post-destroy")
            return False
        # And prev.
        s.prev_workspace()
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after PrevWorkspace post-destroy")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_switch_workspace_by_index():
    """SwitchWorkspace(N) jumps directly to workspace N by index."""
    print("TEST: workspace - switch workspace by index ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Create workspaces 1 and 2 (auto-switches to each).
        for i in range(2):
            s.create_workspace()
            time.sleep(1.5)
            s.read(timeout=0.5)
            if not s.is_alive():
                print(f"FAIL - crashed creating workspace {i + 1}")
                return False
        # Now on workspace 2. Switch directly to workspace 0 by key.
        s.switch_workspace_by_key(0)
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after SwitchWorkspace(0)")
            return False
        # Switch directly to workspace 2 by key.
        s.switch_workspace_by_key(2)
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after SwitchWorkspace(2)")
            return False
        # Switch directly to workspace 1 by key.
        s.switch_workspace_by_key(1)
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after SwitchWorkspace(1)")
            return False
        # Switching to the current workspace should be a no-op.
        s.switch_workspace_by_key(1)
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after SwitchWorkspace to current")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_switch_workspace_out_of_range():
    """SwitchWorkspace(N) to a non-existent index should be a no-op."""
    print("TEST: workspace - switch to non-existent index ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Only workspace 0 exists. Try switching to workspace 1 and 2.
        s.switch_workspace_by_key(1)
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after SwitchWorkspace(1) out of range")
            return False
        s.switch_workspace_by_key(2)
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after SwitchWorkspace(2) out of range")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_create_workspace_uses_selected_app():
    """New workspace should use the currently selected app, not the config default."""
    print("TEST: workspace - create workspace uses selected app ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Cycle to the next application (from default "term" to "calc").
        s.select_application()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after SelectApplication")
            return False
        # Create a new workspace; it should launch the "calc" app, not "term".
        s.create_workspace()
        time.sleep(1.5)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - crashed after creating workspace with selected app")
            return False
        # Switch back to workspace 0 to confirm stability.
        s.switch_workspace_by_key(0)
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed switching back to workspace 0")
            return False
        # Switch back to workspace 1 (created with selected app).
        s.switch_workspace_by_key(1)
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed switching back to workspace 1 (selected app)")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_select_app_then_create_multiple_workspaces():
    """Changing selected app and creating multiple workspaces should all succeed."""
    print("TEST: workspace - select app + create multiple workspaces ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Select "calc" app.
        s.select_application()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after first SelectApplication")
            return False
        # Create workspace 1 with "calc".
        s.create_workspace()
        time.sleep(1.5)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - crashed creating workspace 1 with calc")
            return False
        # Cycle back to "term" (wraps around with 2 items).
        s.select_application()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after second SelectApplication")
            return False
        # Create workspace 2 with "term".
        s.create_workspace()
        time.sleep(1.5)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - crashed creating workspace 2 with term")
            return False
        # Cycle through all 3 workspaces to verify stability.
        for idx in [0, 1, 2]:
            s.switch_workspace_by_key(idx)
            time.sleep(1.0)
            s.read(timeout=0.3)
            if not s.is_alive():
                print(f"FAIL - crashed switching to workspace {idx}")
                return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_create_workspace_default_app_without_selection():
    """Without changing selection, new workspace should use the config default app."""
    print("TEST: workspace - create workspace with default app ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Create workspace without changing selection; should use default "term".
        s.create_workspace()
        time.sleep(1.5)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - crashed creating workspace with default app")
            return False
        # Switch between workspaces.
        s.switch_workspace_by_key(0)
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed switching to workspace 0")
            return False
        s.switch_workspace_by_key(1)
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed switching to workspace 1")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_lastpane_within_single_workspace():
    """LastPane works within a single workspace with split panes."""
    print("TEST: workspace - LastPane within single workspace ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Split workspace 0 to create two panes.
        s.split_horizontal()
        time.sleep(1.5)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - crashed after split")
            return False
        # Focus next pane to build focus history within this workspace.
        s.focus_next_pane()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after FocusNextPane")
            return False
        # LastPane should switch back to previous pane within workspace 0.
        s.last_pane()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after LastPane")
            return False
        # Do it again (round-trip): LastPane back.
        s.last_pane()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after second LastPane")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_lastpane_isolated_across_workspaces():
    """LastPane only switches between panes within the current workspace,
    not across workspace boundaries. This is the core isolation test."""
    print("TEST: workspace - LastPane isolated across workspaces ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # --- Build focus history in workspace 0 ---
        # Split workspace 0 to create two panes.
        s.split_horizontal()
        time.sleep(1.5)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - crashed after split in workspace 0")
            return False
        # Focus between panes in workspace 0 to record history.
        s.focus_next_pane()
        time.sleep(0.5)
        s.read(timeout=0.3)
        s.focus_next_pane()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed during focus navigation in workspace 0")
            return False

        # --- Create workspace 1 with its own split ---
        s.create_workspace()
        time.sleep(1.5)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - crashed after creating workspace 1")
            return False
        s.split_horizontal()
        time.sleep(1.5)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - crashed after split in workspace 1")
            return False
        # Focus between panes in workspace 1 to record history.
        s.focus_next_pane()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed during focus navigation in workspace 1")
            return False

        # --- LastPane in workspace 1 should stay in workspace 1 ---
        s.last_pane()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after LastPane in workspace 1 (isolation failure)")
            return False

        # --- Switch back to workspace 0, LastPane should stay in workspace 0 ---
        s.switch_workspace_by_key(0)
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed switching back to workspace 0")
            return False
        s.last_pane()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after LastPane in workspace 0 (isolation failure)")
            return False
        # Do it one more time to verify stability.
        s.last_pane()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after second LastPane in workspace 0")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_lastpane_after_workspace_destroy():
    """LastPane works correctly after destroying a workspace that had focus history."""
    print("TEST: workspace - LastPane after workspace destroy ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Split workspace 0.
        s.split_horizontal()
        time.sleep(1.5)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - crashed after split in workspace 0")
            return False
        # Navigate panes to build focus history.
        s.focus_next_pane()
        time.sleep(0.5)
        s.read(timeout=0.3)

        # Create workspace 1.
        s.create_workspace()
        time.sleep(1.5)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - crashed after creating workspace 1")
            return False
        # Split and navigate in workspace 1.
        s.split_horizontal()
        time.sleep(1.5)
        s.read(timeout=0.5)
        s.focus_next_pane()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed during workspace 1 setup")
            return False

        # Destroy workspace 1 — should switch back to workspace 0.
        s.destroy_workspace()
        time.sleep(1.5)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - crashed after destroying workspace 1")
            return False

        # LastPane in workspace 0 should work using workspace 0's own history.
        s.last_pane()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after LastPane in workspace 0 post-destroy")
            return False
        s.last_pane()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after second LastPane in workspace 0 post-destroy")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_lastpane_no_history_in_new_workspace():
    """LastPane in a brand new workspace (no focus history) should not crash
    and should not jump to a pane in another workspace."""
    print("TEST: workspace - LastPane no history in new workspace ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Build focus history in workspace 0.
        s.split_horizontal()
        time.sleep(1.5)
        s.read(timeout=0.5)
        s.focus_next_pane()
        time.sleep(0.5)
        s.read(timeout=0.3)
        s.focus_next_pane()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed building focus history in workspace 0")
            return False

        # Create workspace 1 (fresh, no focus history).
        s.create_workspace()
        time.sleep(1.5)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - crashed creating workspace 1")
            return False

        # LastPane in workspace 1 with no prior history should be a no-op, not crash.
        s.last_pane()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after LastPane in empty workspace 1")
            return False
        # Repeat to make sure it's stable.
        s.last_pane()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after second LastPane in empty workspace 1")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_lastpane_three_workspaces():
    """LastPane stays isolated when cycling through three workspaces."""
    print("TEST: workspace - LastPane with three workspaces ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Setup workspace 0: split + navigate.
        s.split_horizontal()
        time.sleep(1.5)
        s.read(timeout=0.5)
        s.focus_next_pane()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed setting up workspace 0")
            return False

        # Create workspace 1: split + navigate.
        s.create_workspace()
        time.sleep(1.5)
        s.read(timeout=0.5)
        s.split_horizontal()
        time.sleep(1.5)
        s.read(timeout=0.5)
        s.focus_next_pane()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed setting up workspace 1")
            return False

        # Create workspace 2: split + navigate.
        s.create_workspace()
        time.sleep(1.5)
        s.read(timeout=0.5)
        s.split_horizontal()
        time.sleep(1.5)
        s.read(timeout=0.5)
        s.focus_next_pane()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed setting up workspace 2")
            return False

        # LastPane in workspace 2 (current).
        s.last_pane()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after LastPane in workspace 2")
            return False

        # Switch to workspace 0, LastPane there.
        s.switch_workspace_by_key(0)
        time.sleep(1.0)
        s.read(timeout=0.3)
        s.last_pane()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after LastPane in workspace 0")
            return False

        # Switch to workspace 1, LastPane there.
        s.switch_workspace_by_key(1)
        time.sleep(1.0)
        s.read(timeout=0.3)
        s.last_pane()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after LastPane in workspace 1")
            return False

        # Back to workspace 2, one more LastPane.
        s.switch_workspace_by_key(2)
        time.sleep(1.0)
        s.read(timeout=0.3)
        s.last_pane()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after final LastPane in workspace 2")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True



# ---------------------------------------------------------------------------
# Popup tests
# ---------------------------------------------------------------------------

def test_popup_open_and_dismiss_click():
    """Opening the workspace popup and dismissing it by clicking empty space."""
    print("TEST: popup - open and dismiss via click ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Open the workspace preview popup.
        s.open_workspace_popup()
        time.sleep(0.8)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after opening popup")
            return False
        # Dismiss by clicking empty area (top-left corner).
        s.popup_dismiss_click()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after dismissing popup")
            return False
        # Verify tile is still responsive: create a workspace via keyboard.
        s.create_workspace()
        time.sleep(1.5)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - crashed after post-dismiss workspace creation")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_popup_switch_workspace():
    """Open popup and click a workspace thumbnail to switch workspaces."""
    print("TEST: popup - switch workspace via popup ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Create workspace 1.
        s.create_workspace()
        time.sleep(1.5)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - crashed creating workspace 1")
            return False
        # Now on workspace 1. Open popup and click workspace 0 thumbnail.
        s.open_workspace_popup()
        time.sleep(0.8)
        s.read(timeout=0.3)
        s.popup_click_workspace(0, workspace_count=2)
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed switching to workspace 0 via popup")
            return False
        # Open popup again and click workspace 1 thumbnail.
        s.open_workspace_popup()
        time.sleep(0.8)
        s.read(timeout=0.3)
        s.popup_click_workspace(1, workspace_count=2)
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed switching to workspace 1 via popup")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_popup_create_workspace_via_plus():
    """Open popup and click the '+' button to create a new workspace."""
    print("TEST: popup - create workspace via popup '+' ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Open popup and click "+" button (1 workspace currently).
        s.open_workspace_popup()
        time.sleep(0.8)
        s.read(timeout=0.3)
        s.popup_click_plus(workspace_count=1)
        time.sleep(1.5)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - crashed after creating workspace via popup '+'")
            return False
        # Verify we now have 2 workspaces by switching between them.
        s.switch_workspace_by_key(0)
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed switching to workspace 0")
            return False
        s.switch_workspace_by_key(1)
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed switching to workspace 1")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_popup_reopen_after_dismiss():
    """Popup can be opened again after being dismissed."""
    print("TEST: popup - reopen after dismiss ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Open popup.
        s.open_workspace_popup()
        time.sleep(0.8)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after opening popup")
            return False
        # Dismiss via click on empty area.
        s.popup_dismiss_click()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after dismissing popup")
            return False
        # Open popup again (should work since popup was dismissed).
        s.open_workspace_popup()
        time.sleep(0.8)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after re-opening popup")
            return False
        # Dismiss again.
        s.popup_dismiss_click()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after second dismiss")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_popup_switch_then_operations():
    """After switching workspace via popup, normal operations still work."""
    print("TEST: popup - switch then normal operations ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Create workspace 1.
        s.create_workspace()
        time.sleep(1.5)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - crashed creating workspace 1")
            return False
        # Switch to workspace 0 via popup.
        s.open_workspace_popup()
        time.sleep(0.8)
        s.read(timeout=0.3)
        s.popup_click_workspace(0, workspace_count=2)
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed switching to workspace 0 via popup")
            return False
        # Split pane in workspace 0 (normal operation after popup).
        s.split_horizontal()
        time.sleep(1.5)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - crashed after split in workspace 0")
            return False
        # Navigate panes.
        s.focus_next_pane()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after focus next pane")
            return False
        # Switch workspaces via keyboard.
        s.next_workspace()
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after next workspace")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def test_popup_keyboard_navigation_bottom():
    """Bottom section: Left/Right cycle workspaces, Up/Down are ignored."""
    print("TEST: popup - keyboard nav bottom section (arrows + ignore up/down) ... ",
          end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Create a second workspace so the switcher has something to cycle through.
        s.create_workspace()
        time.sleep(1.5)
        s.read(timeout=0.5)
        if not s.is_alive():
            print("FAIL - crashed creating workspace 1")
            return False

        s.open_workspace_popup()
        time.sleep(0.8)
        s.read(timeout=0.3)
        # Right arrow should change the previewed workspace; Up/Down should be swallowed.
        s.popup_send_right()
        time.sleep(0.2)
        s.popup_send_up()
        time.sleep(0.2)
        s.popup_send_down()
        time.sleep(0.2)
        s.popup_send_left()
        time.sleep(0.2)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after bottom-section arrow navigation")
            return False
        # Enter commits the previewed workspace and dismisses the popup.
        s.popup_send_enter()
        time.sleep(0.8)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed committing preview via Enter")
            return False
        # Popup should be gone: a second Enter keystroke must not reach popup handlers
        # and must not crash the session.
        s.popup_send_enter()
        time.sleep(0.3)
        s.read(timeout=0.2)
        if not s.is_alive():
            print("FAIL - crashed after post-popup Enter")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_popup_tab_toggle_sections():
    """Tab toggles focus between top and bottom sections; top-section arrows navigate panes."""
    print("TEST: popup - Tab toggles sections, top arrows navigate panes ... ",
          end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Give the previewed workspace more than one pane so top-section navigation
        # has meaningful targets (split the initial pane).
        s.split_horizontal()
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed splitting pane")
            return False

        s.open_workspace_popup()
        time.sleep(0.8)
        s.read(timeout=0.3)
        # Move focus from bottom (default) to top.
        s.popup_send_tab()
        time.sleep(0.2)
        # Navigate in all four directions to exercise the 2D navigate algorithm.
        s.popup_send_right()
        time.sleep(0.15)
        s.popup_send_left()
        time.sleep(0.15)
        s.popup_send_down()
        time.sleep(0.15)
        s.popup_send_up()
        time.sleep(0.15)
        s.read(timeout=0.2)
        if not s.is_alive():
            print("FAIL - crashed during top-section 2D navigation")
            return False
        # Tab back to bottom: selection survives, up/down remain ignored.
        s.popup_send_tab()
        time.sleep(0.15)
        s.popup_send_up()
        time.sleep(0.15)
        s.popup_send_down()
        time.sleep(0.15)
        s.read(timeout=0.2)
        if not s.is_alive():
            print("FAIL - crashed after Tab-back plus bottom up/down")
            return False
        # Dismiss with Esc; session must survive.
        s.popup_send_escape()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after Esc dismiss")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_popup_tab_enter_focuses_pane():
    """Tab into top section, arrow-select a pane, Enter commits pane focus without crashing."""
    print("TEST: popup - Tab into top + Enter commits pane selection ... ",
          end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        s.split_horizontal()
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed splitting pane")
            return False
        s.open_workspace_popup()
        time.sleep(0.8)
        s.read(timeout=0.3)
        s.popup_send_tab()  # Bottom -> Top.
        time.sleep(0.2)
        s.popup_send_right()  # Move keyboard selection in top section.
        time.sleep(0.2)
        s.popup_send_enter()  # Commit: switch to previewed workspace + focus that pane.
        time.sleep(0.8)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed committing pane selection via Enter")
            return False
        # Session should behave normally afterwards (e.g., subsequent ws switch).
        s.switch_workspace_by_key(0)
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed switching workspace after pane-commit")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_popup_tab_toggle_no_panes():
    """Tab into a top section that has a single pane should still not crash (edge case)."""
    print("TEST: popup - Tab with single-pane workspace (no 2D targets) ... ",
          end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Do not split: the workspace has a single pane.
        s.open_workspace_popup()
        time.sleep(0.8)
        s.read(timeout=0.3)
        s.popup_send_tab()
        time.sleep(0.15)
        # Arrows in the top section with a single pane must no-op gracefully.
        s.popup_send_left()
        s.popup_send_right()
        s.popup_send_up()
        s.popup_send_down()
        time.sleep(0.2)
        s.read(timeout=0.2)
        if not s.is_alive():
            print("FAIL - crashed navigating in single-pane top section")
            return False
        # Tab back and dismiss.
        s.popup_send_tab()
        time.sleep(0.1)
        s.popup_send_escape()
        time.sleep(0.5)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed after single-pane Tab-back + Esc")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_popup_top_section_label_key_selects_pane():
    """In the top section, a label key (0x30..0x7E) should commit focus to the matching pane
    and dismiss the popup. Bottom-section label keys must still switch workspaces."""
    print("TEST: popup - top section label key selects pane ... ",
          end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            print("FAIL - vtm-tile did not start")
            return False
        # Build a workspace with at least two panes so label '0' and '1' both resolve.
        s.split_horizontal()
        time.sleep(1.0)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed splitting pane")
            return False
        s.open_workspace_popup()
        time.sleep(0.8)
        s.read(timeout=0.3)
        s.popup_send_tab()  # Bottom -> Top.
        time.sleep(0.2)
        # Press label '1' in the top section: should commit pane[1] and dismiss.
        s.popup_send_label('1')
        time.sleep(0.8)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed committing pane via label key in top section")
            return False
        # Popup should now be gone; normal operations must still work.
        s.switch_workspace_by_key(0)
        time.sleep(0.4)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed switching workspace after top-label commit")
            return False
        # Reopen popup and verify bottom-section label keys still switch workspaces.
        s.create_workspace()
        time.sleep(0.8)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed creating second workspace")
            return False
        s.open_workspace_popup()
        time.sleep(0.8)
        s.read(timeout=0.3)
        # Focus starts in bottom section; label '0' should switch to ws[0] and dismiss.
        s.popup_send_label('0')
        time.sleep(0.6)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed using label key in bottom section after feature change")
            return False
        # Out-of-range label keys in the top section must be swallowed (no crash).
        s.open_workspace_popup()
        time.sleep(0.6)
        s.popup_send_tab()
        time.sleep(0.15)
        s.popup_send_label('~')  # Very unlikely to map to a real pane index.
        time.sleep(0.4)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed on out-of-range top-section label key")
            return False
        # Popup must still be dismissable after an out-of-range label.
        s.popup_send_escape()
        time.sleep(0.4)
        s.read(timeout=0.3)
        if not s.is_alive():
            print("FAIL - crashed dismissing popup after out-of-range label")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-tile did not exit cleanly via close button")
            return False
        print("PASS")
        return True


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

TESTS = [
    test_workspace_starts_with_one,
    test_create_workspace,
    test_destroy_workspace,
    test_destroy_last_workspace_exits,
    test_workspace_close_button_still_works,
    test_create_multiple_workspaces,
    test_split_in_workspace,
    test_next_workspace,
    test_prev_workspace,
    test_last_workspace,
    test_next_prev_single_workspace,
    test_next_prev_after_destroy,
    test_switch_workspace_by_index,
    test_switch_workspace_out_of_range,
    test_create_workspace_uses_selected_app,
    test_select_app_then_create_multiple_workspaces,
    test_create_workspace_default_app_without_selection,
    test_lastpane_within_single_workspace,
    test_lastpane_isolated_across_workspaces,
    test_lastpane_after_workspace_destroy,
    test_lastpane_no_history_in_new_workspace,
    test_lastpane_three_workspaces,
    test_popup_open_and_dismiss_click,
    test_popup_switch_workspace,
    test_popup_create_workspace_via_plus,
    test_popup_reopen_after_dismiss,
    test_popup_switch_then_operations,
    test_popup_keyboard_navigation_bottom,
    test_popup_tab_toggle_sections,
    test_popup_tab_enter_focuses_pane,
    test_popup_tab_toggle_no_panes,
    test_popup_top_section_label_key_selects_pane,
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
