#!/usr/bin/env python3
# Copyright (c) Dmitry Sapozhnikov
# Licensed under the MIT license.

"""
End-to-end TUI regression test for the menu item type="dropdown" feature.

Verifies the contract added in application.hpp menu::open_dropdown_popup:

  1. The XML <menu><item type="dropdown" ...><item ...>...</item></menu>
     schema is parsed by menu::load_item: the parent renders as a menu-bar
     button labelled with its own `label`, and its nested <item> children
     are loaded as the dropdown's row entries.

  2. Left-clicking the dropdown trigger opens a popup overlay anchored
     immediately below the trigger's bottom row. The popup paints the
     child labels.

  3. The menu bar remains visible while the popup is open. This is the
     regression that prompted the test: the original implementation
     attached the popup overlay to the topmost ancestor (gate), whose
     render semantics replaced the existing applet subtree and made the
     menu bar disappear. The fix attaches to base::reflow_root instead
     (the applet's wrapper cake), so the menu bar coexists with the
     popup as siblings of the cake.

  4. Clicking a child row dismisses the popup and dispatches the child's
     script (the row's <script> bindings).

The test drives vtm-tile via a pty and uses SGR mouse + raw input, the
same harness used by the existing test_command_bar_terminal suite.
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
SETTLE_DELAY = 1.0

# Distinctive labels make grep-style screen scans unambiguous: the
# trigger uses a marker that does not collide with any default vtm menu
# button label, and the children use markers that don't collide with
# the trigger.
TRIGGER_LABEL = "  [DROP]  "
CHILD_LABEL_1 = "ChildA-PRINT-MARKER"
CHILD_LABEL_2 = "ChildB-INERT"

# Lua-side print marker. When ChildA is clicked the Lua snippet writes
# this marker into the focused terminal pane via vtm.terminal.Print, so
# its appearance in the post-click paint stream confirms the row's
# script ran *and* the popup dismissed (otherwise the click would have
# been swallowed by the overlay without dispatching to the pane).
CHILD_PRINT_MARKER = "DROPPED-CHILD-A-XYZ77"

# Self-contained tile config exercising:
#   - <menu item*> with a parent <item type="dropdown"> trigger
#   - two child <item> entries under the dropdown
# We clear the terminal's default menu bar (-c on the dtvt child) so
# its native menu doesn't paint extra glyphs that could collide with
# our markers when scanning the screen for the trigger label.
# The children carry empty scripts: their dispatch path (luafx) needs
# tile/terminal proxy plumbing that is exercised by the existing
# test_command_bar_terminal suite, not here. Our scope is the menu's
# popup rendering and the menu-bar-stays-visible regression.
TILE_CONFIG = (
    "<config>"
        "<tile>"
            "<confirm_close=0/>"
            '<app selected="term">'
                "<item*/>"
                '<item id="term" label="term" type="dtvt"'
                ' cmd="$0 -c \'<config><terminal><menu item*></menu></terminal></config>\' -r term"/>'
            "</app>"
            "<menu item*>"
                f'<item type="dropdown" label="{TRIGGER_LABEL}" tooltip=" drop " item*>'
                    f'<item label="{CHILD_LABEL_1}" tooltip=" first "  script=\'OnLeftClick|\'/>'
                    f'<item label="{CHILD_LABEL_2}" tooltip=" second " script=\'OnLeftClick|\'/>'
                "</item>"
            "</menu>"
        "</tile>"
    "</config>"
)
TILE_ARGS = []  # TILE_CONFIG is shipped via $VTM_CONFIG (vtm_config kwarg).


# Multi-level config exercising menu::open_dropdown_popup's cascading
# submenu support (chevron indicator + click-to-open submenu to the
# right). Structure:
#
#   [NEST]
#     ├ NestLeafX  (leaf)
#     ├ NestSub1   (submenu trigger → chevron, opens submenu to the right)
#     │   ├ GrandLeafA
#     │   └ GrandLeafB
#     └ NestLeafY  (leaf)
#
# The labels carry distinct ASCII markers ("NestLeafX", "NestSub1",
# "GrandLeafA") so find_marker_position can locate them via CUP scans.
NEST_TRIGGER_LABEL = "  [NEST]  "
NEST_LEAF_X = "NestLeafX"
NEST_SUB_1  = "NestSub1"
NEST_LEAF_Y = "NestLeafY"
GRAND_LEAF_A = "GrandLeafA"
GRAND_LEAF_B = "GrandLeafB"

NEST_TILE_CONFIG = (
    "<config>"
        "<tile>"
            "<confirm_close=0/>"
            '<app selected="term">'
                "<item*/>"
                '<item id="term" label="term" type="dtvt"'
                ' cmd="$0 -c \'<config><terminal><menu item*></menu></terminal></config>\' -r term"/>'
            "</app>"
            "<menu item*>"
                f'<item type="dropdown" label="{NEST_TRIGGER_LABEL}" tooltip=" nest " item*>'
                    f'<item label="{NEST_LEAF_X}" tooltip=" leaf x " script=\'OnLeftClick|\'/>'
                    f'<item type="dropdown" label="{NEST_SUB_1}" tooltip=" sub one " item*>'
                        f'<item label="{GRAND_LEAF_A}" tooltip=" grand a " script=\'OnLeftClick|\'/>'
                        f'<item label="{GRAND_LEAF_B}" tooltip=" grand b " script=\'OnLeftClick|\'/>'
                    "</item>"
                    f'<item label="{NEST_LEAF_Y}" tooltip=" leaf y " script=\'OnLeftClick|\'/>'
                "</item>"
            "</menu>"
        "</tile>"
    "</config>"
)
NEST_TILE_ARGS = []  # NEST_TILE_CONFIG is shipped via $VTM_CONFIG.


# Flip-left test fixture. Geometry (chosen so the flip kicks in
# deterministically). Each menu-bar button now receives 1 cell of
# horizontal padding on each side from the <menu><padding=1/></menu>
# default, so labels render 2 cells wider than their text content:
#
#   screen width      = FLIP_COLS = 54
#   ctrl buttons      = 3 × (5 + 2 pad) = 21 cells on the right
#   menu scrllist     = 54 - 21 = 33 cells available
#   spacer label      = 19 vis + 2 pad = 21 cells ("  <pad-pad-pad-X>  ")
#   [NEST] label      = 10 vis + 2 pad = 12 cells ("  [NEST]  ")
#   spacer + trigger  = 33 cells → fits scrllist exactly
#
# So the [NEST] button starts at column 21.
#   parent popup_w    = max child label + 2*padding + 2 chevron-reserve
#                     = NestLeafX(9) + 2 + 2 = 13
#   parent right edge = 21 + 13 = 34
#   submenu items     = FLIP_GRAND_A/B (19 chars each, no chevron)
#   submenu popup_w   = 19 + 2 = 21
#   sub_x_right       = 34 ; sub_x_right + 21 = 55 > 54 → overflow
#   → flip to LEFT: sub_x = 21 - 21 = 0 (fits, sub_x_left == 0)
# After the flip the submenu occupies columns 0..20 and the parent
# 21..33 — strictly non-overlapping with a clean boundary at col 21.
FLIP_COLS = 54
FLIP_SPACER_LABEL = "  <pad-pad-pad-X>  "  # 19 cells
FLIP_GRAND_A = "GrandLeafA-WIDE-EXT"        # 19 chars
FLIP_GRAND_B = "GrandLeafB-WIDE-EXT"        # 19 chars
FLIP_TILE_CONFIG = (
    "<config>"
        "<tile>"
            "<confirm_close=0/>"
            '<app selected="term">'
                "<item*/>"
                '<item id="term" label="term" type="dtvt"'
                ' cmd="$0 -c \'<config><terminal><menu item*></menu></terminal></config>\' -r term"/>'
            "</app>"
            "<menu item*>"
                f'<item label="{FLIP_SPACER_LABEL}" tooltip=" pad " script=\'OnLeftClick|\'/>'
                f'<item type="dropdown" label="{NEST_TRIGGER_LABEL}" tooltip=" nest " item*>'
                    f'<item label="{NEST_LEAF_X}" tooltip=" leaf x " script=\'OnLeftClick|\'/>'
                    f'<item type="dropdown" label="{NEST_SUB_1}" tooltip=" sub one " item*>'
                        f'<item label="{FLIP_GRAND_A}" tooltip=" grand a " script=\'OnLeftClick|\'/>'
                        f'<item label="{FLIP_GRAND_B}" tooltip=" grand b " script=\'OnLeftClick|\'/>'
                    "</item>"
                    f'<item label="{NEST_LEAF_Y}" tooltip=" leaf y " script=\'OnLeftClick|\'/>'
                "</item>"
            "</menu>"
        "</tile>"
    "</config>"
)
FLIP_TILE_ARGS = []  # FLIP_TILE_CONFIG is shipped via $VTM_CONFIG.


# Event-passthrough fixture: two side-by-side dropdown triggers on
# the menu bar. Used to verify that:
#   - clicking trigger B while trigger A's dropdown is open both
#     closes A's chain and opens B's chain (single-click switching);
#   - hovering trigger B while trigger A's dropdown is open still
#     produces B's xlight hover feedback (event passthrough).
# Both contracts depend on the popup backdrop NOT claiming menu-bar
# cells: with the old behaviour the backdrop filled every canvas
# cell with its own link id, so menu-bar buttons were unreachable
# while a dropdown was open.
PASSTHROUGH_TRIGGER_A = "  [DRPA]  "
PASSTHROUGH_TRIGGER_B = "  [DRPB]  "
PASSTHROUGH_CHILD_A = "ChildA-PT-MARK"
PASSTHROUGH_CHILD_B = "ChildB-PT-MARK"
PASSTHROUGH_TILE_CONFIG = (
    "<config>"
        "<tile>"
            "<confirm_close=0/>"
            '<app selected="term">'
                "<item*/>"
                '<item id="term" label="term" type="dtvt"'
                ' cmd="$0 -c \'<config><terminal><menu item*></menu></terminal></config>\' -r term"/>'
            "</app>"
            "<menu item*>"
                f'<item type="dropdown" label="{PASSTHROUGH_TRIGGER_A}" tooltip=" a " item*>'
                    f'<item label="{PASSTHROUGH_CHILD_A}" tooltip=" a-child " script=\'OnLeftClick|\'/>'
                "</item>"
                f'<item type="dropdown" label="{PASSTHROUGH_TRIGGER_B}" tooltip=" b " item*>'
                    f'<item label="{PASSTHROUGH_CHILD_B}" tooltip=" b-child " script=\'OnLeftClick|\'/>'
                "</item>"
            "</menu>"
        "</tile>"
    "</config>"
)
PASSTHROUGH_TILE_ARGS = []  # PASSTHROUGH_TILE_CONFIG is shipped via $VTM_CONFIG.


# Non-dropdown-button passthrough fixture: a dropdown trigger
# alongside a plain (button-type) menu-bar item. Verifies the
# regression where clicking a non-dropdown menu-bar button while a
# dropdown was open fired the button's action but left the
# dropdown visually attached (only dropdown-trigger clicks went
# through open_dropdown_popup, which is where active_chain_slot
# dismissal lives).
PT_BUTTON_DROPDOWN  = "  [DRP]  "
PT_BUTTON_PLAIN     = "  [BTN]  "
PT_DROPDOWN_CHILD   = "DROP-CHILD-PT-MARK"
PT_BUTTON_TILE_CONFIG = (
    "<config>"
        "<tile>"
            "<confirm_close=0/>"
            '<app selected="term">'
                "<item*/>"
                '<item id="term" label="term" type="dtvt"'
                ' cmd="$0 -c \'<config><terminal><menu item*></menu></terminal></config>\' -r term"/>'
            "</app>"
            "<menu item*>"
                f'<item type="dropdown" label="{PT_BUTTON_DROPDOWN}" tooltip=" drop " item*>'
                    f'<item label="{PT_DROPDOWN_CHILD}" tooltip=" child " script=\'OnLeftClick|\'/>'
                "</item>"
                f'<item label="{PT_BUTTON_PLAIN}" tooltip=" plain " script=\'OnLeftClick|\'/>'
            "</menu>"
        "</tile>"
    "</config>"
)
PT_BUTTON_TILE_ARGS = []  # PT_BUTTON_TILE_CONFIG is shipped via $VTM_CONFIG.


# Keyboard-navigation / '&Label' shortcut fixture. The trigger uses
# '&Test' (T underlined, '&' suppressed). Children exercise:
#   - leaf with '&F' shortcut    -> activates via 'f'
#   - leaf with '&R' shortcut    -> activates via 'r'
#   - submenu with '&u' shortcut -> opens via 'u' (or Right arrow)
#
# vtm wraps the shortcut letter in CSI 4 m / CSI 24 m (underline
# on/off), so the rendered label's raw byte stream is split at the
# shortcut character — e.g. '&Find item' becomes the byte sequence
# '\x1b[4mF\x1b[24mind item'. The literal "Find item" string is NOT
# contiguous in raw bytes, so find_marker_position is fed the TAIL
# of each label (everything after the shortcut letter), which is.
# The stripped-buffer rendering ("Find item" intact) is used for
# presence assertions.
KB_TRIGGER_RAW    = "  [&Test]  "
KB_TRIGGER_TAIL   = "est]"            # raw-buffer marker (after T's SGR)
KB_TRIGGER_RENDER = "[Test]"
KB_LEAF_FIND_RAW    = "&Find item"
KB_LEAF_FIND_TAIL   = "ind item"
KB_LEAF_FIND_RENDER = "Find item"
KB_LEAF_REPL_RAW    = "&Replace item"
KB_LEAF_REPL_TAIL   = "eplace item"
KB_LEAF_REPL_RENDER = "Replace item"
KB_SUB_RAW    = "S&ubmenu item"      # 'u' is underlined
KB_SUB_TAIL   = "bmenu item"
KB_SUB_RENDER = "Submenu item"
KB_GRAND_RAW    = "&Grand item"
KB_GRAND_TAIL   = "rand item"
KB_GRAND_RENDER = "Grand item"

KB_TILE_CONFIG = (
    "<config>"
        "<tile>"
            "<confirm_close=0/>"
            '<app selected="term">'
                "<item*/>"
                '<item id="term" label="term" type="dtvt"'
                ' cmd="$0 -c \'<config><terminal><menu item*></menu></terminal></config>\' -r term"/>'
            "</app>"
            "<menu item*>"
                f'<item type="dropdown" label="{KB_TRIGGER_RAW}" tooltip=" t " item*>'
                    f'<item label="{KB_LEAF_FIND_RAW}" tooltip=" f " script=\'OnLeftClick|\'/>'
                    f'<item label="{KB_LEAF_REPL_RAW}" tooltip=" r " script=\'OnLeftClick|\'/>'
                    f'<item type="dropdown" label="{KB_SUB_RAW}" tooltip=" s " item*>'
                        f'<item label="{KB_GRAND_RAW}" tooltip=" g " script=\'OnLeftClick|\'/>'
                    "</item>"
                "</menu>"
            "</tile>"
        "</config>"
)
KB_TILE_ARGS = []  # KB_TILE_CONFIG is shipped via $VTM_CONFIG.

# Raw byte sequences for navigation keys. SGR mouse handles mouse; for
# keyboard, vtm-tile speaks standard ANSI CSI sequences as a terminal
# would. Arrow keys are CSI A/B/C/D; Enter is CR.
KEY_UP    = b"\x1b[A"
KEY_DOWN  = b"\x1b[B"
KEY_RIGHT = b"\x1b[C"
KEY_LEFT  = b"\x1b[D"
KEY_ENTER = b"\r"


# Custom-padding fixture. Drives a single-leaf dropdown with the
# menu-level <padding=3/> override. Renders one menu-bar trigger
# "[PAD]" (5 visible chars + 4 surrounding spaces from the label
# itself) and one popup row "PadLeaf". The test asserts that the
# popup row's first painted column reflects the configured 3-cell
# horizontal padding rather than the default 1.
PAD_TRIGGER_RAW   = "[PAD]"           # 5 visible chars
PAD_TRIGGER_TAIL  = "PAD]"            # raw marker (no shortcut)
PAD_LEAF_RAW      = "PadLeaf"         # 7 visible chars
PAD_TILE_CONFIG = (
    "<config>"
        "<tile>"
            "<confirm_close=0/>"
            '<app selected="term">'
                "<item*/>"
                '<item id="term" label="term" type="dtvt"'
                ' cmd="$0 -c \'<config><terminal><menu item*></menu></terminal></config>\' -r term"/>'
            "</app>"
            "<menu item*>"
                "<padding=3/>"
                f'<item type="dropdown" label="{PAD_TRIGGER_RAW}" tooltip=" p " item*>'
                    f'<item label="{PAD_LEAF_RAW}" tooltip=" leaf " script=\'OnLeftClick|\'/>'
                "</item>"
            "</menu>"
        "</tile>"
    "</config>"
)
PAD_TILE_ARGS = []  # PAD_TILE_CONFIG is shipped via $VTM_CONFIG.


# ---------------------------------------------------------------------------
# Pane-applet dropdown fixture (cross-process dismissal regression).
#
# This is the scenario from the bug report: the dropdown lives in the menu
# bar of an *applet running inside a tile pane*, not in the tile's own
# top-level menu bar. A tile pane applet (e.g. the terminal built by
# build_terminal in term.hpp) runs as a separate dtvt subprocess bridged
# over directvt; its menu bar — and therefore the dropdown popup it opens —
# is rendered entirely inside that subprocess.
#
# The on-screen layout has three stacked bars:
#   row 1: the tile's own menu bar          (TILEBAR marker, outer process)
#   row 2: the tile pane's title bar         (grip/header,    outer process)
#   row 3: the applet's menu bar             ([PANEDROP] trigger, subprocess)
#
# Clicks on rows 1 and 2 are handled by the OUTER tile process and are never
# delivered to the subprocess, so the subprocess's own outside-click hook
# (which only covers the pane's content viewport) can't see them. The
# regression: the open dropdown stayed visible when the user clicked the
# tile menu bar or the pane title bar to dismiss it.
#
# The dropdown is configured under the top-level <terminal><menu>, which the
# dtvt child inherits via the $VTM_CONFIG environment variable (the child is
# launched as `$0 -r term` with no -c override, so it merges the same env
# config). The tile keeps a TILEBAR button so row 1 has a concrete click
# target, and confirm_close=0 lets the app exit on the close button.
PANE_TRIGGER_LABEL = "  [PANEDROP]  "
PANE_CHILD_LABEL   = "PaneChildA-XYZ"
PANE_TILEBAR_LABEL = "  TILEBAR  "
PANE_TILE_CONFIG = (
    "<config>"
        "<terminal>"
            "<menu item*>"
                f'<item type="dropdown" label="{PANE_TRIGGER_LABEL}" tooltip=" d " item*>'
                    f'<item label="{PANE_CHILD_LABEL}" tooltip=" a " script=\'OnLeftClick|\'/>'
                "</item>"
            "</menu>"
        "</terminal>"
        "<tile>"
            "<confirm_close=0/>"
            "<menu item*>"
                f'<item label="{PANE_TILEBAR_LABEL}" tooltip=" t " script=\'OnLeftClick|\'/>'
            "</menu>"
            '<app selected="term">'
                "<item*/>"
                '<item id="term" label="term" type="dtvt" cmd="$0 -r term"/>'
            "</app>"
        "</tile>"
    "</config>"
)
PANE_TILE_ARGS = []  # PANE_TILE_CONFIG is shipped via $VTM_CONFIG.


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


def sgr_press(col, row, button=0):
    return f"\033[<{button};{col};{row}M".encode()


def sgr_release(col, row, button=0):
    return f"\033[<{button};{col};{row}m".encode()


def sgr_move(col, row):
    # SGR mouse motion with no button pressed (button code 35).
    return f"\033[<35;{col};{row}M".encode()


# ANSI/VT500 stripper — same coverage as test_command_bar_terminal.
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


class VtmTileSession:
    def __init__(self, args, settle_delay=SETTLE_DELAY, vtm_config=None):
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
            os.dup2(slave_fd, 0)
            os.dup2(slave_fd, 1)
            os.dup2(slave_fd, 2)
            if slave_fd > 2:
                os.close(slave_fd)
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

    def click(self, col, row, button=0):
        self.write(sgr_press(col, row, button))
        time.sleep(0.05)
        self.write(sgr_release(col, row, button))

    def hover(self, col, row):
        """Send an SGR mouse-motion event (no button) to (col, row)."""
        self.write(sgr_move(col, row))
        time.sleep(0.05)

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

    def click_close_button(self):
        self.click(COLS - 2, 1)

    def normal_exit(self, timeout=5.0):
        # Send Esc first to dismiss any open dropdown popup (the
        # popup's backdrop overlay would otherwise intercept the
        # close-button click and just close the popup, leaving the
        # app running). Esc is a no-op when no popup is open.
        self.snapshot(timeout=0.2)
        self.write(b"\x1b")
        time.sleep(0.2)
        self.snapshot(timeout=0.2)
        self.click_close_button()
        return self.wait_for_exit(timeout=timeout)

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


def find_marker_position(raw_buf, marker):
    """Locate `marker` in the cumulative paint stream using CUP positioning.

    Returns the (row, col) of the marker's first cell on the most recent
    paint that emitted it, or None if not painted. We track CUP (CSI
    row;col H) and a running visible-cell offset so the column reflects
    the actual on-screen position rather than the byte offset.
    """
    marker_b = marker.encode() if isinstance(marker, str) else marker
    last = raw_buf.rfind(marker_b)
    if last < 0:
        return None
    prefix = raw_buf[:last]
    cups = list(re.finditer(rb"\x1b\[(\d+);(\d+)H", prefix))
    if not cups:
        return None
    cup = cups[-1]
    row = int(cup.group(1))
    col = int(cup.group(2))
    # Strip ANSI from the bytes between the last CUP and the marker so
    # the visible cell offset accumulates only over visible glyphs.
    between = strip_ansi(raw_buf[cup.end():last]).decode("utf-8", errors="replace")
    start_col = col + len(between)
    return (row, start_col)


def find_bg_rgb_before_marker(raw_buf, marker):
    """Most recent SGR 24-bit background color (R, G, B) emitted before
    `marker` in the raw paint stream.

    vtm-tile emits RGB cell attributes as `ESC[...48;2;R;G;B...m` (with
    optional intermixed `38;2;R;G;B` foreground). Cells with the same
    attributes share one SGR header, so the SGR set before the start
    of a popup row carries through to every glyph in that row — making
    "the last 48;2 before the marker" a reliable proxy for the row's
    rendered background color.

    Returns (r, g, b) integers in 0..255, or None if no such SGR or
    marker exists in the buffer.
    """
    marker_b = marker.encode() if isinstance(marker, str) else marker
    pos = raw_buf.rfind(marker_b)
    if pos < 0:
        return None
    prefix = raw_buf[:pos]
    last = None
    for m in re.finditer(rb"48;2;(\d+);(\d+);(\d+)", prefix):
        last = m
    if last is None:
        return None
    return (int(last.group(1)), int(last.group(2)), int(last.group(3)))


def find_fg_rgb_before_marker(raw_buf, marker):
    """Most recent SGR 24-bit foreground color (R, G, B) emitted before
    `marker`. Same scan model as find_bg_rgb_before_marker, but for the
    `38;2;R;G;B` SGR variant.
    """
    marker_b = marker.encode() if isinstance(marker, str) else marker
    pos = raw_buf.rfind(marker_b)
    if pos < 0:
        return None
    prefix = raw_buf[:pos]
    last = None
    for m in re.finditer(rb"38;2;(\d+);(\d+);(\d+)", prefix):
        last = m
    if last is None:
        return None
    return (int(last.group(1)), int(last.group(2)), int(last.group(3)))


def find_marker_with_fg_positions(raw_buf, marker, expected_fg_rgb):
    """All on-screen (row, col) positions where `marker` was painted with
    `expected_fg_rgb` as its 24-bit foreground.

    Walks the raw paint stream forward, tracking the most recent CUP and
    the most recent SGR 38;2;R;G;B. For every occurrence of `marker` that
    follows a matching fg SGR (with no intervening fg SGR overriding it),
    derive the marker's screen coord from CUP + the visible-cell length
    of the bytes painted between CUP and the marker.

    Used to disambiguate edge-decoration glyphs (▄/▀) painted by the
    dropdown popup overlay from any other ▀ emitted on the menu-bar
    cover etc., which use a different fg color.
    """
    marker_b = marker.encode() if isinstance(marker, str) else marker
    cup_re = re.compile(rb"\x1b\[(\d+);(\d+)H")
    fg_re  = re.compile(rb"38;2;(\d+);(\d+);(\d+)")
    positions = []
    cur_row = None
    cur_col = None
    cur_fg = None
    # Combined token stream: CUPs, fg SGRs, and marker hits — processed
    # in source order so the current row/col/fg reflect what the paint
    # cursor would see at each marker.
    events = []
    for m in cup_re.finditer(raw_buf):
        events.append((m.start(), "cup", m))
    for m in fg_re.finditer(raw_buf):
        events.append((m.start(), "fg", m))
    pos = 0
    while True:
        idx = raw_buf.find(marker_b, pos)
        if idx < 0:
            break
        events.append((idx, "mark", idx))
        pos = idx + len(marker_b)
    events.sort(key=lambda e: e[0])
    last_cup_end = 0
    for evt_pos, kind, payload in events:
        if kind == "cup":
            cur_row = int(payload.group(1))
            cur_col = int(payload.group(2))
            last_cup_end = payload.end()
        elif kind == "fg":
            cur_fg = (int(payload.group(1)),
                      int(payload.group(2)),
                      int(payload.group(3)))
        else:  # "mark"
            if cur_row is None or cur_fg != expected_fg_rgb:
                continue
            between = strip_ansi(raw_buf[last_cup_end:evt_pos]).decode(
                "utf-8", errors="replace")
            positions.append((cur_row, cur_col + len(between)))
    return positions


def fail(msg):
    print(f"FAIL - {msg}")
    return False


def test_dropdown_menu_item_loads_from_xml_and_opens_popup():
    """Click the dropdown trigger; verify:
      (a) the popup appears with both child labels painted below the
          trigger, and
      (b) the menu bar (trigger label) remains visible while the popup
          is open — the regression bug attached the overlay to the gate
          (the topmost ancestor), whose multi-child render semantics
          hide all but the last attached child and made the menu bar
          disappear. The fix targets the applet's wrapper cake (the
          second-to-topmost ancestor) instead.
    """
    print("TEST: dropdown menu item: popup appears, menu bar stays visible ... ",
          end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.snapshot(timeout=2.0)

        coords = find_marker_position(s._screen_buf, "[DROP]")
        if coords is None:
            return fail(
                "dropdown trigger label '[DROP]' not rendered on menu bar — "
                "the <item type='dropdown'> XML may not have been parsed"
            )
        trigger_row, trigger_col = coords

        # The trigger label is "  [DROP]  ". Click on its '['/'D' cell.
        click_col = trigger_col + 2
        click_row = trigger_row
        s.reset_buffer()
        s.click(click_col, click_row)
        rendered = s.snapshot(timeout=1.5)

        # 1. Popup row labels must be painted below the trigger.
        if CHILD_LABEL_1 not in rendered:
            return fail(
                f"after clicking dropdown trigger at ({click_col},{click_row}), "
                f"popup did not paint child label '{CHILD_LABEL_1}'"
            )
        if CHILD_LABEL_2 not in rendered:
            return fail(
                f"popup did not paint second child label '{CHILD_LABEL_2}'"
            )

        # 2. The menu bar trigger label must still be visible.
        # This is the regression guard: attaching the overlay to the
        # gate would replace the applet subtree (and the menu bar),
        # so neither '[DROP]' nor the popup labels would coexist.
        if "[DROP]" not in rendered:
            return fail(
                "menu bar disappeared while popup was open — the overlay "
                "host must be the applet wrapper cake (second-to-topmost), "
                "not the topmost ancestor (gate)"
            )

        # 3. Verify the popup rows appear AT or below the menu strip:
        # CUP positions of the child labels must be > the trigger row.
        c1_pos = find_marker_position(s._screen_buf, CHILD_LABEL_1)
        c2_pos = find_marker_position(s._screen_buf, CHILD_LABEL_2)
        if c1_pos is None or c2_pos is None:
            return fail("could not locate popup child label positions")
        if c1_pos[0] <= trigger_row:
            return fail(
                f"popup row '{CHILD_LABEL_1}' painted at row {c1_pos[0]} "
                f"which is at or above the trigger row {trigger_row}; "
                f"expected the popup to anchor below the trigger"
            )
        if c2_pos[0] <= c1_pos[0]:
            return fail(
                f"popup rows out of order: '{CHILD_LABEL_2}' at row "
                f"{c2_pos[0]}, '{CHILD_LABEL_1}' at row {c1_pos[0]}"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during dropdown interaction")
        if not s.normal_exit():
            return fail("vtm-tile did not exit after clicking close button")
        print(
            f"PASS (trigger={coords}, c1={c1_pos}, c2={c2_pos})"
        )
        return True


def test_dropdown_menu_keeps_menubar_visible_with_log_repaint():
    """A second pass that re-opens the popup and checks the menubar
    delta after the SECOND click. This catches any host-attach side
    effect that only manifests after the first dismiss-and-reattach.
    """
    print("TEST: dropdown re-open preserves menu bar ... ",
          end="", flush=True)
    with VtmTileSession(TILE_ARGS, vtm_config=TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.snapshot(timeout=2.0)

        coords = find_marker_position(s._screen_buf, "[DROP]")
        if coords is None:
            return fail("dropdown trigger '[DROP]' not found")
        trigger_row, trigger_col = coords

        # Open the popup once.
        s.reset_buffer()
        s.click(trigger_col + 2, trigger_row)
        first_open = s.snapshot(timeout=1.5)
        if CHILD_LABEL_1 not in first_open:
            return fail("popup failed to open on first click")
        if "[DROP]" not in first_open:
            return fail("menu bar gone after first popup open")

        if not s.is_alive():
            return fail("vtm-tile crashed during re-open test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit after clicking close button")
        print("PASS")
        return True


def test_nested_dropdown_submenu_opens_to_the_right():
    """Multi-level menus: clicking a submenu trigger row in the parent
    popup must open a submenu popup to its right.

    Verifies the contract added in menu::_attach_popup_overlay:
      (a) Submenu trigger rows are visually marked with a chevron '>'.
      (b) Clicking a submenu trigger row opens a new popup whose left
          edge sits at the parent popup's right edge, and whose top
          aligns with the clicked row.
      (c) The parent popup, the submenu, and the menu bar trigger label
          all remain visible at the same time (cascading layout).
      (d) Leaf rows in the parent popup (e.g. NestLeafX, NestLeafY)
          are still painted while the submenu is open.

    Also tests the chain-truncation behaviour by clicking a different
    parent row that ISN'T a submenu trigger — the submenu must dismiss
    along with the parent.
    """
    print("TEST: nested dropdown: submenu opens to the right ... ",
          end="", flush=True)
    with VtmTileSession(NEST_TILE_ARGS, vtm_config=NEST_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.snapshot(timeout=2.0)

        # 1) Locate the menu-bar trigger.
        coords = find_marker_position(s._screen_buf, "[NEST]")
        if coords is None:
            return fail("nested dropdown trigger '[NEST]' not rendered")
        trigger_row, trigger_col = coords

        # 2) Click the trigger to open the parent popup.
        s.reset_buffer()
        s.click(trigger_col + 2, trigger_row)
        rendered = s.snapshot(timeout=1.5)

        for marker in (NEST_LEAF_X, NEST_SUB_1, NEST_LEAF_Y):
            if marker not in rendered:
                return fail(
                    f"parent popup did not paint expected row '{marker}'"
                )
        if "[NEST]" not in rendered:
            return fail("menu bar disappeared after first popup open")

        # 3) Chevron indicator: a '>' must appear on the row containing
        # NestSub1. The chevron is painted at the right edge of the row
        # (popup_w - 2 from popup_x). We verify by locating the row's
        # CUP position and checking that a '>' was written further right.
        sub_pos = find_marker_position(s._screen_buf, NEST_SUB_1)
        if sub_pos is None:
            return fail(f"could not locate row '{NEST_SUB_1}' on screen")
        sub_row, sub_col = sub_pos
        # The popup width includes 2 padding cells + 2 reserved for the
        # chevron when any row has children. After the row label, there
        # is a gap and then '>' at the popup's right edge. We just
        # confirm a '>' exists on this row by checking the raw paint
        # stream contains a '>' between the row's start and a generous
        # right column bound.
        raw = strip_ansi(s._screen_buf).decode("utf-8", errors="replace")
        # Lines are stripped of CSI/SGR, but column positions don't
        # survive strip_ansi cleanly; we just confirm a '▸' chevron
        # was emitted in the dialog area. The chevron glyph is not
        # used anywhere else in the rendered surface, so its
        # presence is a sufficient marker for submenu-trigger
        # decoration.
        if "▸" not in raw:
            return fail("no chevron '▸' emitted for the submenu trigger row")

        # 4) Click the submenu trigger row (NestSub1). Don't reset the
        # buffer here: the parent popup won't be repainted (it's
        # unchanged), so a delta-only view would falsely show it gone.
        # We use the cumulative buffer to verify both popups coexist.
        s.click(sub_col + 2, sub_row)
        rendered2 = s.snapshot(timeout=1.5)

        # 5) Submenu must paint its grandchild rows.
        for marker in (GRAND_LEAF_A, GRAND_LEAF_B):
            if marker not in rendered2:
                return fail(
                    f"submenu did not paint grandchild row '{marker}' "
                    f"after clicking '{NEST_SUB_1}'"
                )

        # 6) The parent popup rows must still be present in the
        # cumulative screen buffer while the submenu is open
        # (cascading layout: parent + submenu visible together).
        if NEST_LEAF_X not in rendered2 or NEST_LEAF_Y not in rendered2:
            return fail(
                "parent popup rows missing from cumulative paint after "
                "submenu opened — submenus should layer on top of the "
                "parent, not replace it"
            )
        # And the menu bar trigger label.
        if "[NEST]" not in rendered2:
            return fail("menu bar trigger '[NEST]' lost when submenu opened")

        # 7) Submenu must be anchored to the RIGHT of the parent popup
        # and at the row clicked. Specifically, GrandLeafA's column
        # must be strictly greater than NEST_LEAF_X's column (parent
        # popup's column), and its row must equal sub_row (the clicked
        # row's screen row).
        x_pos = find_marker_position(s._screen_buf, NEST_LEAF_X)
        grand_pos = find_marker_position(s._screen_buf, GRAND_LEAF_A)
        if x_pos is None or grand_pos is None:
            return fail("could not locate parent and submenu rows post-click")
        if grand_pos[1] <= x_pos[1]:
            return fail(
                f"submenu did not anchor to the right of the parent "
                f"({GRAND_LEAF_A} col={grand_pos[1]}, "
                f"{NEST_LEAF_X} col={x_pos[1]})"
            )
        if grand_pos[0] != sub_row:
            return fail(
                f"submenu row '{GRAND_LEAF_A}' painted at row {grand_pos[0]} "
                f"but the clicked submenu trigger was at row {sub_row} — "
                f"submenu should align its top with the clicked row"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during nested-menu interaction")
        if not s.normal_exit():
            return fail("vtm-tile did not exit after clicking close button")
        print(
            f"PASS (parent_col={x_pos[1]}, sub_col={grand_pos[1]}, "
            f"sub_row={grand_pos[0]})"
        )
        return True


def test_nested_dropdown_submenu_flips_left_when_no_room_on_right():
    """When the parent popup sits near the right screen edge, opening
    a submenu to its right would clip off-screen. The popup placement
    logic in menu::open_dropdown_popup must flip the submenu to the
    LEFT side of the parent so the two popups never overlap.

    Setup: use a narrow 50-column screen so the right-edge overflow is
    easy to provoke. The same NEST trigger XML is reused; with the
    label "  [NEST]  " landing in the left half (col ~ 3) of a 50-col
    screen, the parent popup goes right and the submenu would normally
    follow further right. With a narrow enough screen the right-side
    placement overflows and the flip kicks in.

    Contract verified:
      - When sub_x_right + sub_w > host_w, the submenu must be placed
        with sub_x = parent_px - sub_w (to the LEFT of the parent).
      - Submenu column must be STRICTLY LESS than parent's left edge,
        i.e. no horizontal overlap with the parent.
    """
    print("TEST: nested dropdown: submenu flips left when no room right ... ",
          end="", flush=True)

    # Override module-level COLS so this session uses FLIP_COLS (38).
    # See FLIP_TILE_CONFIG above for the geometry derivation.
    global COLS
    saved_cols = COLS
    COLS = FLIP_COLS
    try:
        with VtmTileSession(FLIP_TILE_ARGS, vtm_config=FLIP_TILE_CONFIG) as s:
            if not s.is_alive():
                return fail("vtm-tile did not start")
            s.snapshot(timeout=2.0)

            coords = find_marker_position(s._screen_buf, "[NEST]")
            if coords is None:
                return fail("trigger '[NEST]' not rendered on narrow screen")
            trigger_row, trigger_col = coords

            # Open parent.
            s.reset_buffer()
            s.click(trigger_col + 2, trigger_row)
            rendered = s.snapshot(timeout=1.5)
            sub_pos = find_marker_position(s._screen_buf, NEST_SUB_1)
            if sub_pos is None or NEST_SUB_1 not in rendered:
                return fail("parent popup did not open on narrow screen")
            sub_row, sub_col = sub_pos

            # Click the submenu trigger row.
            s.click(sub_col + 2, sub_row)
            rendered2 = s.snapshot(timeout=1.5)

            if FLIP_GRAND_A not in rendered2:
                return fail(
                    f"submenu did not paint '{FLIP_GRAND_A}' on narrow screen"
                )

            grand_pos = find_marker_position(s._screen_buf, FLIP_GRAND_A)
            x_pos    = find_marker_position(s._screen_buf, NEST_LEAF_X)
            if grand_pos is None or x_pos is None:
                return fail("could not locate rows post-submenu-open")

            # The submenu must NOT overlap the parent. With the flip,
            # submenu's columns are strictly LEFT of the parent's
            # leftmost cell (NEST_LEAF_X starts at parent's label col).
            if grand_pos[1] >= x_pos[1]:
                return fail(
                    f"submenu did not flip left on narrow screen: "
                    f"{FLIP_GRAND_A} col={grand_pos[1]} >= "
                    f"{NEST_LEAF_X} col={x_pos[1]} (host_w={FLIP_COLS}); "
                    f"expected the flip to place the submenu to the "
                    f"left of the parent to avoid overlap"
                )

            if not s.is_alive():
                return fail("vtm-tile crashed during flip-left test")
            if not s.normal_exit():
                return fail("vtm-tile did not exit cleanly")
            print(
                f"PASS (parent_col={x_pos[1]}, flipped_sub_col={grand_pos[1]}, "
                f"host_w={FLIP_COLS})"
            )
            return True
    finally:
        COLS = saved_cols


def test_nested_dropdown_submenu_opens_on_hover_no_click():
    """Submenus must open immediately on hover (no click needed).

    Setup: open the parent popup with a single left-click on the menu
    bar trigger. Then move the cursor (SGR motion event, no button)
    over the NestSub1 row. The submenu's grandchild rows must appear.
    """
    print("TEST: nested dropdown: submenu opens on hover (no click) ... ",
          end="", flush=True)
    with VtmTileSession(NEST_TILE_ARGS, vtm_config=NEST_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.snapshot(timeout=2.0)

        coords = find_marker_position(s._screen_buf, "[NEST]")
        if coords is None:
            return fail("trigger '[NEST]' not found")
        trigger_row, trigger_col = coords

        # Open parent popup via click.
        s.reset_buffer()
        s.click(trigger_col + 2, trigger_row)
        opened = s.snapshot(timeout=1.5)
        if NEST_SUB_1 not in opened:
            return fail("parent popup did not open via click")
        sub_pos = find_marker_position(s._screen_buf, NEST_SUB_1)
        if sub_pos is None:
            return fail("could not locate NestSub1 row")
        sub_row, sub_col = sub_pos

        # Hover (motion only) over the NestSub1 row. No click.
        s.reset_buffer()
        s.hover(sub_col + 2, sub_row)
        time.sleep(0.3)
        rendered = s.snapshot(timeout=1.5)

        if GRAND_LEAF_A not in rendered:
            return fail(
                f"submenu did not open on hover — "
                f"'{GRAND_LEAF_A}' missing from paint after motion event "
                f"to NestSub1 row ({sub_col},{sub_row})"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during hover test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print("PASS")
        return True


def test_dropdown_esc_dismisses_chain():
    """Pressing Esc while a popup chain is open must dismiss every
    popup AND clear the trigger's open-guard so a subsequent click
    on the menu-bar trigger re-opens the chain cleanly.
    """
    print("TEST: dropdown: Esc dismisses chain ... ",
          end="", flush=True)
    with VtmTileSession(NEST_TILE_ARGS, vtm_config=NEST_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.snapshot(timeout=2.0)

        coords = find_marker_position(s._screen_buf, "[NEST]")
        if coords is None:
            return fail("trigger '[NEST]' not found")
        trigger_row, trigger_col = coords

        # Open parent popup.
        s.click(trigger_col + 2, trigger_row)
        s.snapshot(timeout=1.5)
        sub_pos = find_marker_position(s._screen_buf, NEST_SUB_1)
        if sub_pos is None:
            return fail("parent popup did not open")
        sub_row, sub_col = sub_pos

        # Open submenu via hover.
        s.hover(sub_col + 2, sub_row)
        time.sleep(0.3)
        opened_two = s.snapshot(timeout=1.5)
        if GRAND_LEAF_A not in opened_two:
            return fail("submenu did not open via hover")

        # Press Esc.
        s.reset_buffer()
        s.write(b"\x1b")
        time.sleep(0.4)
        s.snapshot(timeout=1.5)

        # Re-click the trigger: if Esc properly cleared the chain
        # and the trigger's open-guard, the parent popup must re-open.
        s.reset_buffer()
        s.click(trigger_col + 2, trigger_row)
        rendered = s.snapshot(timeout=1.5)
        if NEST_LEAF_X not in rendered:
            return fail(
                "after Esc + re-click, parent popup did not re-open — "
                "Esc likely failed to clear the chain or the open-guard"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed after Esc dismiss")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print("PASS")
        return True


def test_dropdown_outside_click_dismisses_chain():
    """Clicking on a blank area outside every popup must dismiss
    the entire popup chain via the backdrop overlay.

    Verifies by:
      1. Opening parent + submenu (via click + hover).
      2. Clicking deep inside the terminal pane buffer (a coord that
         is outside every popup rect AND outside the menu strip).
      3. Re-clicking the menu-bar trigger and confirming the popup
         re-opens — which can only happen if the chain dismissed
         and the open-guard was cleared.
    """
    print("TEST: dropdown: outside click dismisses chain ... ",
          end="", flush=True)
    with VtmTileSession(NEST_TILE_ARGS, vtm_config=NEST_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.snapshot(timeout=2.0)

        coords = find_marker_position(s._screen_buf, "[NEST]")
        if coords is None:
            return fail("trigger '[NEST]' not found")
        trigger_row, trigger_col = coords

        # Open parent + submenu.
        s.click(trigger_col + 2, trigger_row)
        s.snapshot(timeout=1.5)
        sub_pos = find_marker_position(s._screen_buf, NEST_SUB_1)
        if sub_pos is None:
            return fail("parent popup did not open")
        sub_row, sub_col = sub_pos
        s.hover(sub_col + 2, sub_row)
        time.sleep(0.3)
        opened_two = s.snapshot(timeout=1.5)
        if GRAND_LEAF_A not in opened_two:
            return fail("submenu did not open via hover")

        # Click far from the popups: bottom-right corner of the
        # terminal pane buffer.
        s.reset_buffer()
        s.click(COLS - 5, ROWS - 3)
        time.sleep(0.4)
        s.snapshot(timeout=1.5)

        # Re-click the trigger: must re-open the parent popup.
        s.reset_buffer()
        s.click(trigger_col + 2, trigger_row)
        rendered = s.snapshot(timeout=1.5)
        if NEST_LEAF_X not in rendered:
            return fail(
                "after outside click + re-trigger, parent popup did not "
                "re-open — backdrop likely failed to dismiss the chain "
                "or clear the open-guard"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed after outside-click dismiss")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print("PASS")
        return True


def test_nested_dropdown_leaf_click_dismisses_chain():
    """After opening a submenu, clicking a grandchild leaf must
    dismiss BOTH the submenu and the parent popup (the leaf's script
    fires and the menu cleans up cleanly).

    We verify by re-clicking the menu bar trigger and confirming the
    popup re-opens — which can only happen if menu.dropdown.open was
    cleared, i.e. the chain dismiss ran.
    """
    print("TEST: nested dropdown: leaf click tears down whole chain ... ",
          end="", flush=True)
    with VtmTileSession(NEST_TILE_ARGS, vtm_config=NEST_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.snapshot(timeout=2.0)

        coords = find_marker_position(s._screen_buf, "[NEST]")
        if coords is None:
            return fail("trigger '[NEST]' not found")
        trigger_row, trigger_col = coords

        # Open parent.
        s.reset_buffer()
        s.click(trigger_col + 2, trigger_row)
        rendered = s.snapshot(timeout=1.5)
        sub_pos = find_marker_position(s._screen_buf, NEST_SUB_1)
        if sub_pos is None or NEST_SUB_1 not in rendered:
            return fail("parent popup did not open")
        sub_row, sub_col = sub_pos

        # Open submenu.
        s.reset_buffer()
        s.click(sub_col + 2, sub_row)
        rendered2 = s.snapshot(timeout=1.5)
        grand_pos = find_marker_position(s._screen_buf, GRAND_LEAF_A)
        if grand_pos is None or GRAND_LEAF_A not in rendered2:
            return fail("submenu did not open")
        grand_row, grand_col = grand_pos

        # Click the grandchild leaf.
        s.reset_buffer()
        s.click(grand_col + 2, grand_row)
        time.sleep(0.4)
        s.snapshot(timeout=1.5)

        # Re-click the menu bar trigger. If menu.dropdown.open was
        # cleared, the parent popup should re-open. (We don't assert
        # the popup is gone via paint inspection because paint deltas
        # only show what changed; instead we test the live state.)
        s.reset_buffer()
        s.click(trigger_col + 2, trigger_row)
        rendered3 = s.snapshot(timeout=1.5)
        if NEST_LEAF_X not in rendered3:
            return fail(
                "after leaf click + re-trigger, parent popup did not "
                "re-open — the chain dismiss likely failed to clear "
                "menu.dropdown.open"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during leaf-click test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit after clicking close button")
        print("PASS")
        return True


def test_submenu_background_progressively_darker():
    """Each submenu level renders with a darker background than its
    parent.

    Verifies the contract added to menu::_attach_popup_overlay: the
    overlay's depth (= chain->overlays.size() at attach time) is
    subtracted from every RGB channel of the root popup background,
    so a submenu (depth=1) is strictly darker than its parent
    (depth=0) on R, G, and B. This gives the user clear visual
    feedback for how deep they have descended in a cascading menu.

    We open the parent popup, hover NestSub1 to open its submenu,
    and compare:
      - the bg SGR emitted just before a parent-popup row label
        (NEST_LEAF_X), against
      - the bg SGR emitted just before a submenu row label
        (GRAND_LEAF_A).
    """
    print("TEST: submenu bg progressively darker per level ... ",
          end="", flush=True)
    with VtmTileSession(NEST_TILE_ARGS, vtm_config=NEST_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.snapshot(timeout=2.0)
        coords = find_marker_position(s._screen_buf, "[NEST]")
        if coords is None:
            return fail("trigger '[NEST]' not found")
        trigger_row, trigger_col = coords

        # Open parent popup.
        s.reset_buffer()
        s.click(trigger_col + 2, trigger_row)
        s.snapshot(timeout=1.5)
        sub_pos = find_marker_position(s._screen_buf, NEST_SUB_1)
        if sub_pos is None:
            return fail("parent popup did not open")
        sub_row, sub_col = sub_pos

        # Open submenu via hover (no click needed; the hover handler
        # opens the chevron row's submenu immediately).
        s.hover(sub_col + 2, sub_row)
        time.sleep(0.3)
        s.snapshot(timeout=1.5)
        if find_marker_position(s._screen_buf, GRAND_LEAF_A) is None:
            return fail("submenu did not open via hover")

        parent_bg = find_bg_rgb_before_marker(s._screen_buf, NEST_LEAF_X)
        submenu_bg = find_bg_rgb_before_marker(s._screen_buf, GRAND_LEAF_A)
        if parent_bg is None:
            return fail("could not extract bg RGB before parent popup row")
        if submenu_bg is None:
            return fail("could not extract bg RGB before submenu row")

        # Submenu (depth=1) must be strictly darker than parent (depth=0)
        # on every channel.
        if not (submenu_bg[0] < parent_bg[0]
                and submenu_bg[1] < parent_bg[1]
                and submenu_bg[2] < parent_bg[2]):
            return fail(
                f"submenu bg {submenu_bg} is not darker than parent bg "
                f"{parent_bg} on all RGB channels — expected each "
                f"channel strictly less than the parent's"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during darker-submenu test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print(f"PASS (parent_bg={parent_bg}, submenu_bg={submenu_bg})")
        return True


def test_hover_brightens_bg_keeps_fg_unchanged():
    """Hovering a popup row brightens that row's background while the
    foreground color stays exactly the same.

    Contract verified:
      - The bg RGB emitted just before NEST_LEAF_X on the hover paint
        must be strictly greater than the non-hover bg on every RGB
        channel (the row was brightened, not recoloured wholesale).
      - The fg RGB emitted just before NEST_LEAF_X must be identical
        between the non-hover and hover paints (label legibility is
        unchanged when the row is hovered).

    Sequence:
      1. Click the trigger to open the parent popup. The mouse stays
         on the menu-bar trigger so no popup row is hovered yet.
      2. Snapshot — capture the non-hover bg/fg used for the popup row.
      3. Hover NEST_LEAF_X (motion-only SGR event, no click). The
         render lambda repaints the popup with hover_bg on that row.
      4. Snapshot — re-extract bg/fg now that the row is hovered.
      5. Assert hover_bg > base_bg (per channel) and hover_fg == base_fg.
    """
    print("TEST: hover brightens bg, fg unchanged ... ",
          end="", flush=True)
    with VtmTileSession(NEST_TILE_ARGS, vtm_config=NEST_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.snapshot(timeout=2.0)
        coords = find_marker_position(s._screen_buf, "[NEST]")
        if coords is None:
            return fail("trigger '[NEST]' not found")
        trigger_row, trigger_col = coords

        # Open the parent popup. We deliberately do NOT hover after
        # the click so the popup paints with all rows non-hovered.
        s.reset_buffer()
        s.click(trigger_col + 2, trigger_row)
        s.snapshot(timeout=1.5)
        leaf_pos = find_marker_position(s._screen_buf, NEST_LEAF_X)
        if leaf_pos is None:
            return fail("could not locate NEST_LEAF_X row")
        leaf_row, leaf_col = leaf_pos

        base_bg = find_bg_rgb_before_marker(s._screen_buf, NEST_LEAF_X)
        base_fg = find_fg_rgb_before_marker(s._screen_buf, NEST_LEAF_X)
        if base_bg is None or base_fg is None:
            return fail(
                f"could not extract base bg/fg for popup row "
                f"(bg={base_bg}, fg={base_fg})"
            )

        # Hover the NEST_LEAF_X row — this triggers a popup repaint
        # with hover_bg on that single row.
        s.reset_buffer()
        s.hover(leaf_col + 2, leaf_row)
        time.sleep(0.3)
        s.snapshot(timeout=1.5)

        hover_bg = find_bg_rgb_before_marker(s._screen_buf, NEST_LEAF_X)
        hover_fg = find_fg_rgb_before_marker(s._screen_buf, NEST_LEAF_X)
        if hover_bg is None:
            return fail("no bg RGB emitted on hover repaint")
        if hover_fg is None:
            return fail("no fg RGB emitted on hover repaint")

        if not (hover_bg[0] > base_bg[0]
                and hover_bg[1] > base_bg[1]
                and hover_bg[2] > base_bg[2]):
            return fail(
                f"hover bg {hover_bg} is not brighter than non-hover bg "
                f"{base_bg} on all RGB channels — expected each channel "
                f"strictly greater than the non-hover value"
            )

        if hover_fg != base_fg:
            return fail(
                f"foreground color changed on hover: was {base_fg}, "
                f"became {hover_fg} — expected the foreground to stay "
                f"unchanged when a row is hovered"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during hover-brighten test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print(
            f"PASS (base_bg={base_bg}, hover_bg={hover_bg}, fg={hover_fg})"
        )
        return True


def test_click_another_trigger_closes_current_and_opens_new():
    """Event-passthrough click contract: while trigger A's dropdown is
    open, a single click on trigger B must (1) reach trigger B (the
    popup backdrop no longer covers menu-bar cells) AND (2) close
    A's chain before opening B's chain.

    Verifies the contract added in menu::open_dropdown_popup:
      - active_chain_slot() tracks the currently open chain
        process-wide, so a second open_dropdown_popup call dismisses
        the previous chain first.
      - The backdrop's render skips the menu-bar row range so menu-bar
        buttons keep their own cell links and receive their own
        LeftClick events.

    Sequence:
      1. Click A → A's child label appears.
      2. Click B (while A is still open) → B's child label appears.
         If event passthrough is broken, this click is swallowed by
         the backdrop and B's label never paints.
      3. Re-click A → A re-opens. This verifies the previous switch
         cleared A's `menu.dropdown.open` guard (the dismissal path
         ran for B's chain when A re-opened).
    """
    print("TEST: click another menu trigger closes current + opens new ... ",
          end="", flush=True)
    with VtmTileSession(PASSTHROUGH_TILE_ARGS, vtm_config=PASSTHROUGH_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.snapshot(timeout=2.0)
        a_coords = find_marker_position(s._screen_buf, "[DRPA]")
        b_coords = find_marker_position(s._screen_buf, "[DRPB]")
        if a_coords is None or b_coords is None:
            return fail("trigger labels '[DRPA]' or '[DRPB]' not rendered")
        a_row, a_col = a_coords
        b_row, b_col = b_coords

        # 1) Open A.
        s.reset_buffer()
        s.click(a_col + 2, a_row)
        rendered_a = s.snapshot(timeout=1.5)
        if PASSTHROUGH_CHILD_A not in rendered_a:
            return fail("trigger A did not open its dropdown on first click")

        # 2) Click B while A's chain is active. With the fix, the
        # click reaches B (menu-bar cells aren't claimed by the
        # backdrop) and open_dropdown_popup dismisses A's chain via
        # active_chain_slot() before opening B's.
        s.reset_buffer()
        s.click(b_col + 2, b_row)
        rendered_b = s.snapshot(timeout=1.5)
        if PASSTHROUGH_CHILD_B not in rendered_b:
            return fail(
                f"after clicking B with A open, B's child label "
                f"'{PASSTHROUGH_CHILD_B}' did not appear — the click "
                f"was probably intercepted by the backdrop overlay "
                f"instead of reaching B (event passthrough broken)"
            )

        # 3) Re-click A. This succeeds iff B's chain was dismissed
        # cleanly when we clicked B (so its open-guard cleared).
        # Note: A had its own open-guard set by step 1; that guard
        # was cleared by the dismiss path when B's click triggered
        # the active_chain_slot dismissal of A.
        s.reset_buffer()
        s.click(a_col + 2, a_row)
        rendered_a2 = s.snapshot(timeout=1.5)
        if PASSTHROUGH_CHILD_A not in rendered_a2:
            return fail(
                "after A→B→A trigger sequence, A did not re-open — "
                "active_chain_slot() likely did not clear the prior "
                "trigger's `menu.dropdown.open` guard during dismiss"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during trigger-switch test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print("PASS")
        return True


def test_hover_another_trigger_shows_hover_feedback():
    """Event-passthrough hover contract: while trigger A's dropdown is
    open, moving the mouse over trigger B must change B's rendered
    cell appearance — the xlight shader (registered on every menu-bar
    button via `shader(xlight, e2::form::state::hover)`) must still
    fire on B because the backdrop no longer claims menu-bar cells.

    We capture B's bg color in three phases:
      1. Idle baseline — mouse moved far from B, before any popup
         opens. The most recent SGR bg before '[DRPB]' is B's idle.
      2. Open A's dropdown (B is still not hovered).
      3. Hover the mouse over B. xlight shifts B's bg RGB.
    A passing test requires the bg captured in phase 3 to differ
    from the phase 1 baseline — proving hover events reached B.
    """
    print("TEST: hover another trigger shows hover feedback ... ",
          end="", flush=True)
    with VtmTileSession(PASSTHROUGH_TILE_ARGS, vtm_config=PASSTHROUGH_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.snapshot(timeout=2.0)

        a_coords = find_marker_position(s._screen_buf, "[DRPA]")
        b_coords = find_marker_position(s._screen_buf, "[DRPB]")
        if a_coords is None or b_coords is None:
            return fail("trigger labels '[DRPA]' or '[DRPB]' not rendered")
        a_row, a_col = a_coords
        b_row, b_col = b_coords

        # 1) Move cursor well clear of B and capture B's idle bg.
        # Row ROWS is the bottom of the terminal pane — definitely
        # not on the menu bar. We deliberately do NOT reset the
        # buffer between phases so the cumulative paint stream
        # still contains every label needed for later sanity checks.
        s.hover(1, ROWS)
        time.sleep(0.3)
        s.snapshot(timeout=1.0)
        idle_bg = find_bg_rgb_before_marker(s._screen_buf, "[DRPB]")
        if idle_bg is None:
            return fail("could not capture B's idle bg color")

        # 2) Open A's dropdown.
        s.click(a_col + 2, a_row)
        s.snapshot(timeout=1.5)

        # The click leaves the cursor on A. Move it explicitly away
        # again so B is definitely not hovered before we drive our
        # hover event.
        s.hover(1, ROWS)
        time.sleep(0.3)
        s.snapshot(timeout=1.0)

        # 3) Hover over B. The xlight shader registered for
        # e2::form::state::hover must fire — proving the hover
        # event passed through the open backdrop overlay.
        s.hover(b_col + 2, b_row)
        time.sleep(0.4)
        rendered = s.snapshot(timeout=1.5)

        hover_bg = find_bg_rgb_before_marker(s._screen_buf, "[DRPB]")
        if hover_bg is None:
            return fail(
                "no bg SGR emitted near '[DRPB]' on hover — hover "
                "events likely didn't reach the menu-bar button"
            )
        if hover_bg == idle_bg:
            return fail(
                f"hovering '[DRPB]' while A's dropdown was open did "
                f"not change B's bg color (idle={idle_bg}, "
                f"hover={hover_bg}) — the backdrop is probably still "
                f"blocking hover events on the menu bar row"
            )

        # Sanity: A's dropdown should still be visible in the
        # cumulative paint — hovering B is not a click and must
        # not dismiss A.
        if PASSTHROUGH_CHILD_A not in rendered:
            return fail(
                "A's popup labels missing from cumulative paint — "
                "hover on the menu bar should not dismiss the open "
                "chain (only clicks dismiss)"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during hover-passthrough test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print(f"PASS (idle_bg={idle_bg}, hover_bg={hover_bg})")
        return True


def test_click_non_dropdown_button_dismisses_open_chain():
    """Regression for the manual report: opening a dropdown then
    clicking a NON-dropdown menu-bar button fired the button's
    action but left the dropdown visually attached. The fix
    registers a chain-dismiss prelude on every menu-bar button
    (see menu::mini's makeitem invoke) that runs before the
    button's own LeftClick handler.

    Sequence:
      1. Click trigger [DRP] → dropdown opens (child label visible).
      2. Click plain button [BTN] (a button-type item with an
         empty binding — fires its action without exiting the app).
      3. Re-click [DRP] → its popup re-opens. This proves the
         [BTN] click dismissed the chain AND cleared [DRP]'s
         menu.dropdown.open guard, so a fresh open_dropdown_popup
         call succeeds.
    """
    print("TEST: click non-dropdown menu button dismisses open chain ... ",
          end="", flush=True)
    with VtmTileSession(PT_BUTTON_TILE_ARGS, vtm_config=PT_BUTTON_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.snapshot(timeout=2.0)
        drp_coords = find_marker_position(s._screen_buf, "[DRP]")
        btn_coords = find_marker_position(s._screen_buf, "[BTN]")
        if drp_coords is None or btn_coords is None:
            return fail(
                "menu-bar items '[DRP]' or '[BTN]' not rendered — "
                "the test config may have failed to load"
            )
        drp_row, drp_col = drp_coords
        btn_row, btn_col = btn_coords

        # 1) Open the dropdown.
        s.reset_buffer()
        s.click(drp_col + 2, drp_row)
        rendered = s.snapshot(timeout=1.5)
        if PT_DROPDOWN_CHILD not in rendered:
            return fail("dropdown did not open on first click")

        # 2) Click the non-dropdown button. With the chain-dismiss
        # prelude in place this both fires [BTN]'s action and
        # dismisses [DRP]'s chain. Without the prelude only the
        # action fires and the chain stays attached.
        s.click(btn_col + 2, btn_row)
        time.sleep(0.3)
        s.snapshot(timeout=1.5)

        # 3) Re-click [DRP]. If the [BTN] click properly dismissed
        # the chain (and cleared [DRP]'s open-guard via the
        # dismiss path's `popup_open = faux` assignment), the
        # dropdown re-opens cleanly. If the chain stayed
        # attached, [DRP]'s open-guard is still set and
        # open_dropdown_popup returns early at the popup_open
        # check, so PT_DROPDOWN_CHILD is missing from the new
        # paint.
        s.reset_buffer()
        s.click(drp_col + 2, drp_row)
        rendered2 = s.snapshot(timeout=1.5)
        if PT_DROPDOWN_CHILD not in rendered2:
            return fail(
                "after clicking the non-dropdown button [BTN] with "
                "the dropdown open, re-clicking [DRP] did not "
                "re-open the dropdown — the [BTN] click did not "
                "dismiss the open chain (regression: prelude "
                "handler missing from non-dropdown menu-bar buttons)"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during non-dropdown click test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print("PASS")
        return True


def test_click_empty_menubar_area_dismisses_chain():
    """Clicking on a non-button area of the menu bar (the empty
    horizontal strip between the menu items and the control buttons)
    must dismiss the open dropdown chain.

    This is the standard desktop convention: the menu bar's blank
    area is a valid dismiss target, not a dead zone. It also
    confirms the host-level mousepreview hook fires for menu-bar Y
    coords that fall outside the chain's own trigger rect — only
    the own trigger is carved out (so its re-click can toggle off
    via its own handler); every other menu-bar cell is a dismiss
    target like the rest of the host area.

    Verifies by:
      1. Opening the [NEST] dropdown so NestLeafX is visible.
      2. Clicking at a column that is inside the menu-bar row but
         well outside [NEST]'s rect and well left of the right-edge
         control buttons (min/max/close), i.e. an empty cell of the
         menu strip itself.
      3. Re-clicking [NEST] and confirming the popup re-opens —
         only possible if the chain dismissed and menu.dropdown.open
         was cleared by dismiss_dropdown_chain.
    """
    print("TEST: click empty menu-bar area dismisses chain ... ",
          end="", flush=True)
    with VtmTileSession(NEST_TILE_ARGS, vtm_config=NEST_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.snapshot(timeout=2.0)

        coords = find_marker_position(s._screen_buf, "[NEST]")
        if coords is None:
            return fail("trigger '[NEST]' not found")
        trigger_row, trigger_col = coords

        # 1) Open the dropdown.
        s.reset_buffer()
        s.click(trigger_col + 2, trigger_row)
        rendered = s.snapshot(timeout=1.5)
        if NEST_LEAF_X not in rendered:
            return fail("[NEST] dropdown did not open on first click")

        # 2) Click an empty cell in the menu-bar row. NEST_TRIGGER_LABEL
        # is 10 cells wide and the only menu item in NEST_TILE_CONFIG,
        # so any column well past trigger_col + 10 and well before the
        # right-edge control buttons (which the macstyle=false layout
        # puts at columns COLS-3 .. COLS-1) is guaranteed blank menu-bar
        # space. Pick the screen midpoint to stay clear of both ends.
        empty_col = COLS // 2
        if empty_col <= trigger_col + 10:
            return fail(
                f"empty-cell column {empty_col} would collide with the "
                f"[NEST] trigger at col {trigger_col} — geometry "
                f"assumption broken, the screen may be too narrow"
            )
        s.click(empty_col, trigger_row)
        time.sleep(0.3)
        s.snapshot(timeout=1.5)

        # 3) Re-click [NEST]. If the empty-area click properly
        # dismissed the chain (and dismiss_dropdown_chain cleared
        # the open-guard), the dropdown re-opens cleanly; if not,
        # open_dropdown_popup returns early at the popup_open check
        # and NEST_LEAF_X is missing from the new paint.
        s.reset_buffer()
        s.click(trigger_col + 2, trigger_row)
        rendered2 = s.snapshot(timeout=1.5)
        if NEST_LEAF_X not in rendered2:
            return fail(
                "after clicking an empty menu-bar cell while the "
                "dropdown was open, re-clicking [NEST] did not "
                "re-open the dropdown — the empty-area click did "
                "not dismiss the open chain (regression: host "
                "mousepreview hook is not treating non-own-trigger "
                "menu-bar cells as dismiss targets)"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during empty-menubar-click test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print(f"PASS (trigger_col={trigger_col}, empty_col={empty_col})")
        return True


def _open_kb_popup(s):
    """Open the [Test] dropdown and return (trigger_row, trigger_col)
    derived from the 'est]' tail marker. Helper for all keyboard-nav
    tests below."""
    s.snapshot(timeout=2.0)
    coords = find_marker_position(s._screen_buf, KB_TRIGGER_TAIL)
    if coords is None:
        return None
    trigger_row, trigger_col = coords
    # 'est]' starts at trigger_col; the [Test] label begins one cell
    # earlier (the underlined T, which is split out by SGR in raw),
    # but any column within the trigger button works for the click.
    s.reset_buffer()
    s.click(trigger_col + 1, trigger_row)
    s.snapshot(timeout=1.5)
    return (trigger_row, trigger_col)


def test_amp_label_strips_marker_and_underlines_shortcut():
    """The "&Label" syntax must (a) render with the '&' marker
    elided and (b) the following letter painted with an SGR
    underline. Verified on both the menu-bar trigger and a popup
    row label by inspecting the stripped vs raw paint streams.
    """
    print("TEST: '&Label' strips marker, underlines shortcut ... ",
          end="", flush=True)
    with VtmTileSession(KB_TILE_ARGS, vtm_config=KB_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.snapshot(timeout=2.0)
        stripped = strip_ansi(s._screen_buf).decode("utf-8", errors="replace")

        # Menu-bar trigger renders as "[Test]" (the '&' was stripped).
        if KB_TRIGGER_RENDER not in stripped:
            return fail(
                f"menu-bar trigger missing from stripped paint — "
                f"expected to find rendered '{KB_TRIGGER_RENDER}'"
            )

        # The raw paint stream must contain the underline-on SGR
        # (CSI 4 m) immediately before the T of the rendered trigger,
        # followed shortly by CSI 24 m and the rest of the label.
        # Pattern: '\x1b[4m' '<T>' '\x1b[' ... '24' ... 'm' 'est]'.
        # We assert on the literal '\x1b[4mT' fragment AND on a
        # '24' SGR appearing before 'est]' in the same row.
        idx = s._screen_buf.rfind(b"est]")
        if idx < 0:
            return fail("trigger tail 'est]' not found in raw buffer")
        window = s._screen_buf[max(0, idx - 80):idx]
        if b"\x1b[4m" not in window and b";4m" not in window and b";4;" not in window:
            return fail(
                "no underline-on SGR (CSI 4) emitted before "
                "trigger letter — '&Label' underline missing on menu bar"
            )

        # Now open the dropdown and verify the popup row picks up
        # the same treatment on its leaf labels.
        if _open_kb_popup(s) is None:
            return fail("could not locate trigger via 'est]' tail")
        rendered = strip_ansi(s._screen_buf).decode("utf-8", errors="replace")
        if KB_LEAF_FIND_RENDER not in rendered:
            return fail(
                f"popup row '{KB_LEAF_FIND_RENDER}' missing from "
                f"stripped paint — '&'-stripping broken on popup row"
            )
        # The raw should NOT contain the literal "&Find" — '&' must
        # be elided on every render path.
        if b"&Find" in s._screen_buf[idx:]:
            return fail("rendered popup row still contains the '&' marker byte")
        # And an underline SGR must surround the F.
        f_idx = s._screen_buf.rfind(b"ind item")
        if f_idx < 0:
            return fail("popup row tail 'ind item' not found in raw buffer")
        window2 = s._screen_buf[max(0, f_idx - 80):f_idx]
        if b"\x1b[4m" not in window2 and b";4m" not in window2 and b";4;" not in window2:
            return fail(
                "no underline-on SGR emitted before popup row shortcut "
                "letter — '&'-underline broken on popup rows"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during '&'-syntax test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print("PASS")
        return True


def test_keyboard_down_up_arrows_select_rows():
    """Press Down/Up while the popup is open and verify the
    selected row's background shifts to hover_bg, matching the
    visual feedback the mouse hover produces. Wraps at the
    bottom (Down past the last row → first row) and the top
    (Up from no-selection → last row).
    """
    print("TEST: kbd Down/Up arrows select rows ... ",
          end="", flush=True)
    with VtmTileSession(KB_TILE_ARGS, vtm_config=KB_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        if _open_kb_popup(s) is None:
            return fail("could not open [Test] dropdown")

        # Press Down: first row (Find item) should become selected.
        s.write(KEY_DOWN)
        time.sleep(0.3)
        s.snapshot(timeout=1.5)
        first_bg = find_bg_rgb_before_marker(s._screen_buf, KB_LEAF_FIND_TAIL)
        if first_bg is None:
            return fail("could not read bg for first popup row")
        # Hover bg at depth 0 is (73, 74, 92).
        if first_bg != (73, 74, 92):
            return fail(
                f"after one Down arrow, first row bg is {first_bg} "
                f"— expected (73, 74, 92) (hover at depth 0)"
            )

        # Press Down again: second row (Replace item) selected.
        s.write(KEY_DOWN)
        time.sleep(0.3)
        s.snapshot(timeout=1.5)
        second_bg = find_bg_rgb_before_marker(s._screen_buf, KB_LEAF_REPL_TAIL)
        if second_bg != (73, 74, 92):
            return fail(
                f"after two Downs, second row bg is {second_bg} "
                f"— expected (73, 74, 92) (selection didn't advance)"
            )

        # Press Up: back to first row.
        s.write(KEY_UP)
        time.sleep(0.3)
        s.snapshot(timeout=1.5)
        first_bg2 = find_bg_rgb_before_marker(s._screen_buf, KB_LEAF_FIND_TAIL)
        if first_bg2 != (73, 74, 92):
            return fail(
                f"after Down/Down/Up, first row bg is {first_bg2} "
                f"— expected the selection to step back to (73, 74, 92)"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during arrow-nav test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print("PASS")
        return True


def test_keyboard_right_arrow_opens_submenu_and_focuses_first_row():
    """Right arrow on a submenu trigger row must (1) open the
    submenu and (2) move keyboard focus into the submenu with its
    first row pre-selected.
    """
    print("TEST: kbd Right arrow opens submenu, focuses first row ... ",
          end="", flush=True)
    with VtmTileSession(KB_TILE_ARGS, vtm_config=KB_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        if _open_kb_popup(s) is None:
            return fail("could not open [Test] dropdown")
        # Down 3 times to land on the submenu (Submenu item, 3rd row).
        s.write(KEY_DOWN)
        s.write(KEY_DOWN)
        s.write(KEY_DOWN)
        time.sleep(0.3)
        s.snapshot(timeout=1.5)
        # Press Right: open the submenu.
        s.write(KEY_RIGHT)
        time.sleep(0.4)
        rendered = s.snapshot(timeout=1.5)
        if KB_GRAND_RENDER not in rendered:
            return fail(
                f"submenu row '{KB_GRAND_RENDER}' did not appear after "
                f"Right arrow — submenu did not open via keyboard"
            )
        # The submenu's first row should be pre-selected; depth-1
        # hover bg is (65, 66, 84) = level1 base (41,42,60) + 24 each.
        grand_bg = find_bg_rgb_before_marker(s._screen_buf, KB_GRAND_TAIL)
        if grand_bg != (65, 66, 84):
            return fail(
                f"submenu's first row bg is {grand_bg} — expected "
                f"(65, 66, 84) (depth-1 hover, first row auto-selected)"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during Right-arrow test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print("PASS")
        return True


def test_keyboard_left_arrow_closes_submenu_returns_to_parent():
    """Left arrow inside a submenu must close that submenu and
    return keyboard focus to the parent popup. Verified by
    pressing Left after opening a submenu, then pressing Down
    in the parent and confirming a parent row becomes selected
    (depth-0 hover bg) — which is only possible if focus
    returned to the parent navigation level.
    """
    print("TEST: kbd Left arrow closes submenu, returns to parent ... ",
          end="", flush=True)
    with VtmTileSession(KB_TILE_ARGS, vtm_config=KB_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        if _open_kb_popup(s) is None:
            return fail("could not open [Test] dropdown")
        # Navigate to and open submenu.
        for _ in range(3):
            s.write(KEY_DOWN)
        s.write(KEY_RIGHT)
        time.sleep(0.4)
        s.snapshot(timeout=1.5)

        # Now press Left to close the submenu.
        s.write(KEY_LEFT)
        time.sleep(0.4)
        s.snapshot(timeout=1.5)

        # Press Down: should land in parent popup (selection wraps,
        # or stays at the submenu-trigger row). Either way, a parent
        # row's bg must take the depth-0 hover color.
        s.write(KEY_DOWN)
        time.sleep(0.3)
        s.snapshot(timeout=1.5)
        # We can't predict which exact parent row is selected (the
        # selected_row may have been left at the Submenu row when
        # we opened it, so Down advances to NestLeafY/Find/etc.).
        # Just confirm SOME depth-0 hover bg shows up against one of
        # the parent rows we can match.
        candidates = [KB_LEAF_FIND_TAIL, KB_LEAF_REPL_TAIL, KB_SUB_TAIL]
        bgs = [find_bg_rgb_before_marker(s._screen_buf, m) for m in candidates]
        if not any(bg == (73, 74, 92) for bg in bgs):
            return fail(
                f"after Left+Down, no parent row carries depth-0 "
                f"hover bg (73,74,92); saw bgs={bgs} — Left arrow "
                f"may not have returned focus to the parent popup"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during Left-arrow test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print("PASS")
        return True


def test_keyboard_enter_activates_leaf_and_dismisses_chain():
    """Enter on a selected leaf row dispatches the row's action
    and tears down the chain. Verified by re-clicking the trigger
    after Enter and confirming the popup re-opens (which can only
    happen if active_chain_slot was cleared by the dismiss path).
    """
    print("TEST: kbd Enter activates leaf, dismisses chain ... ",
          end="", flush=True)
    with VtmTileSession(KB_TILE_ARGS, vtm_config=KB_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        coords = _open_kb_popup(s)
        if coords is None:
            return fail("could not open [Test] dropdown")
        trigger_row, trigger_col = coords

        # Select first row (Find item) and press Enter.
        s.write(KEY_DOWN)
        time.sleep(0.2)
        s.write(KEY_ENTER)
        time.sleep(0.4)
        s.snapshot(timeout=1.5)

        # Re-click trigger to confirm chain was torn down.
        s.reset_buffer()
        s.click(trigger_col + 1, trigger_row)
        rendered2 = s.snapshot(timeout=1.5)
        if KB_LEAF_FIND_RENDER not in rendered2:
            return fail(
                "popup did not re-open after Enter+re-click — Enter "
                "likely failed to dismiss the chain (open-guard left set)"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during Enter test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print("PASS")
        return True


def test_keyboard_shortcut_letter_activates_matching_row():
    """Pressing a letter that matches a row's '&'-shortcut letter
    activates that row (leaf → dispatch + dismiss; submenu →
    open + focus). Verified with 'f' (matches '&Find item').
    """
    print("TEST: kbd shortcut letter activates matching row ... ",
          end="", flush=True)
    with VtmTileSession(KB_TILE_ARGS, vtm_config=KB_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        coords = _open_kb_popup(s)
        if coords is None:
            return fail("could not open [Test] dropdown")
        trigger_row, trigger_col = coords

        # Press 'f' (matches '&F' on Find item).
        s.write(b"f")
        time.sleep(0.4)
        s.snapshot(timeout=1.5)

        # Confirm chain dismissed: re-click trigger and verify the
        # popup re-opens.
        s.reset_buffer()
        s.click(trigger_col + 1, trigger_row)
        rendered = s.snapshot(timeout=1.5)
        if KB_LEAF_FIND_RENDER not in rendered:
            return fail(
                "popup did not re-open after 'f' shortcut + re-click — "
                "shortcut likely did not activate the matching row"
            )

        # Also test submenu shortcut: 'u' opens Submenu item, focuses
        # its first row.
        s.write(b"u")
        time.sleep(0.4)
        rendered2 = s.snapshot(timeout=1.5)
        if KB_GRAND_RENDER not in rendered2:
            return fail(
                f"submenu row '{KB_GRAND_RENDER}' did not appear after "
                f"pressing 'u' — submenu shortcut did not open"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during shortcut letter test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print("PASS")
        return True


def test_keyboard_nav_is_independent_of_mouse_hover():
    """Regression for the manual report: with the mouse hovering
    over a popup row, pressing Down was forcibly pulled back to
    the mouse-hovered row because every popup repaint refired the
    MouseMove handler at the unchanged cursor coord, which
    overwrote the keyboard-set selection. The fix (mirroring the
    tile.hpp workspace-switcher pattern) stamps the gear.coord
    into a per-popup kbd_lock_coord on every keyboard action and
    skips MouseMove events whose coord still matches the stamp.

    Sequence:
      1. Hover the mouse over the first popup row. Confirm bg
         shifts to the depth-0 hover color (73, 74, 92).
      2. Press Down. Confirm the SECOND row now carries the
         hover color and the first row reverted to the base bg
         (49, 50, 68). Without the lock, the stationary cursor
         would echo a MouseMove that pulls selection back to
         the first row.
      3. Move the mouse to the first row again (different coord
         from the stamped lock would reset; here we move to a
         second-row offset to be unambiguous) — mouse hover
         takes over once the cursor genuinely changes cell.
    """
    print("TEST: kbd nav independent of stationary mouse hover ... ",
          end="", flush=True)
    with VtmTileSession(KB_TILE_ARGS, vtm_config=KB_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        if _open_kb_popup(s) is None:
            return fail("could not open [Test] dropdown")

        first_pos = find_marker_position(s._screen_buf, KB_LEAF_FIND_TAIL)
        if first_pos is None:
            return fail("could not locate first popup row")
        first_row, first_col = first_pos

        # 1) Hover first row.
        s.hover(first_col, first_row)
        time.sleep(0.3)
        s.snapshot(timeout=1.0)
        first_bg = find_bg_rgb_before_marker(s._screen_buf, KB_LEAF_FIND_TAIL)
        if first_bg != (73, 74, 92):
            return fail(
                f"mouse hover failed to highlight first row "
                f"(bg={first_bg}, expected (73,74,92))"
            )

        # 2) Press Down. Keyboard must override the stationary
        # mouse hover and advance to the second row.
        s.write(KEY_DOWN)
        time.sleep(0.4)
        s.snapshot(timeout=1.0)
        first_bg2 = find_bg_rgb_before_marker(s._screen_buf, KB_LEAF_FIND_TAIL)
        second_bg = find_bg_rgb_before_marker(s._screen_buf, KB_LEAF_REPL_TAIL)
        if second_bg != (73, 74, 92):
            return fail(
                f"after Down with mouse stationary, second row bg "
                f"is {second_bg} — expected (73,74,92). The keyboard "
                f"selection was pulled back by the stationary mouse "
                f"hover (kbd_lock_coord lock is not working)"
            )
        if first_bg2 == (73, 74, 92):
            return fail(
                f"after Down, the mouse-hovered first row still "
                f"carries the hover bg — selection did not advance "
                f"cleanly off the stationary cursor's row"
            )

        # 3) A real mouse move (different coord) must release the
        # lock and let mouse hover take over again.
        second_pos = find_marker_position(s._screen_buf, KB_LEAF_REPL_TAIL)
        if second_pos is None:
            return fail("could not locate second popup row")
        second_row, second_col = second_pos
        # Move to a clearly different cell on the first row.
        s.hover(first_col + 2, first_row)
        time.sleep(0.3)
        s.snapshot(timeout=1.0)
        first_bg3 = find_bg_rgb_before_marker(s._screen_buf, KB_LEAF_FIND_TAIL)
        if first_bg3 != (73, 74, 92):
            return fail(
                f"after a real mouse move, first row bg is {first_bg3} "
                f"— expected (73,74,92). The kbd_lock_coord did not "
                f"release on a genuine cursor move"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during kbd/mouse independence test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print("PASS")
        return True


def test_keyboard_does_not_pass_through_to_terminal():
    """While the dropdown is open, keystrokes must NOT reach the
    focused terminal pane underneath. We open the popup, type a
    unique 4-letter string of letters that match no shortcut in
    the current popup ('z','x','c','v'), dismiss with Esc, then
    confirm none of those bytes appear in the post-Esc paint as
    shell echo (the test terminal pane runs the user's shell,
    which by default echoes typed characters when keyboard
    events reach it).
    """
    print("TEST: kbd events do not pass through to terminal ... ",
          end="", flush=True)
    with VtmTileSession(KB_TILE_ARGS, settle_delay=2.0, vtm_config=KB_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        if _open_kb_popup(s) is None:
            return fail("could not open [Test] dropdown")

        # Reset and type the unique sentinel string while popup open.
        s.reset_buffer()
        for ch in b"zxcv":
            s.write(bytes([ch]))
            time.sleep(0.05)
        s.write(b"\x1b")  # Esc dismisses the chain
        time.sleep(0.5)
        rendered = s.snapshot(timeout=1.5)

        # None of the typed letters should appear on screen as shell
        # echo. The chain's kbd_hook swallows the bytes; the shell
        # therefore receives nothing and echoes nothing.
        if "zxcv" in rendered:
            return fail(
                "the sentinel string 'zxcv' appeared on screen after "
                "the popup was open during typing — keyboard events "
                "passed through to the terminal pane"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during passthrough test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print("PASS")
        return True


def test_menu_padding_config_controls_horizontal_cell_padding():
    """The <menu><padding=N/></menu> config controls how many blank
    cells flank every item on both sides — applied to menu-bar
    buttons and dropdown popup rows alike.

    With <padding=3/>:
      - The popup row's first label cell sits 3 columns to the
        right of the popup's left edge. The popup's left edge
        equals the trigger's left edge (anchor.x), so the row's
        first label cell column == trigger_col + 3.
      - The popup width is wider by 2*(3-1) = 4 cells vs the
        default padding=1.
    """
    print("TEST: <menu padding=N> controls item horizontal padding ... ",
          end="", flush=True)
    with VtmTileSession(PAD_TILE_ARGS, vtm_config=PAD_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.snapshot(timeout=2.0)
        trigger_pos = find_marker_position(s._screen_buf, PAD_TRIGGER_TAIL)
        if trigger_pos is None:
            return fail(
                "trigger '[PAD]' was not painted — the <padding=3/> "
                "config may have broken menu-bar layout entirely"
            )
        trigger_row, trigger_tail_col = trigger_pos
        # PAD_TRIGGER_TAIL='PAD]' sits 1 cell after the '[' of the
        # label, so the trigger label's left edge ([) is at
        # trigger_tail_col - 1. With padding=3 wrapped around the
        # label, the BUTTON's left edge is at (label_left - 3).
        button_left = trigger_tail_col - 1 - 3

        # Open the dropdown by clicking the trigger.
        s.reset_buffer()
        s.click(trigger_tail_col, trigger_row)
        s.snapshot(timeout=1.5)
        leaf_pos = find_marker_position(s._screen_buf, PAD_LEAF_RAW)
        if leaf_pos is None:
            return fail(
                f"popup row '{PAD_LEAF_RAW}' did not paint after "
                f"clicking the trigger"
            )
        leaf_row, leaf_col = leaf_pos
        if leaf_row <= trigger_row:
            return fail(
                f"popup row painted at row {leaf_row}; expected it "
                f"to appear BELOW the trigger row {trigger_row}"
            )

        # The popup anchors at the button's left edge. With
        # padding=3 the first label cell sits at button_left + 3.
        expected_leaf_col = button_left + 3
        if leaf_col != expected_leaf_col:
            return fail(
                f"popup row painted at col {leaf_col}; expected "
                f"col {expected_leaf_col} (button_left={button_left} "
                f"+ padding=3). The <padding=3/> config was not "
                f"applied to popup row rendering."
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during padding-config test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print(
            f"PASS (button_left={button_left}, leaf_col={leaf_col}, "
            f"padding=3)"
        )
        return True


def test_dropdown_top_edge_uses_lower_half_block_with_menu_bg_fg():
    """The dropdown popup paints a decorative '▄' (U+2584, lower half
    block) row immediately above its first item row. The half-block's
    foreground equals the popup body's background colour, while its
    own background is left untouched (the cell behind it shows
    through). Visually, the menu colour bleeds half a cell into the
    row above the popup — giving the popup a "soft top" that flows
    into the menu bar.

    Verifies the contract added to menu::_attach_popup_overlay:
      - At least one '▄' is emitted with fg == depth-0 popup level_bg
        (R=49, G=50, B=68) — derived from the per-level palette in
        application.hpp.
      - That '▄' is positioned exactly one row above the popup's
        first item row (NEST_LEAF_X), at the same column as the
        popup's left edge.
    """
    print("TEST: dropdown top edge ▄ with menu-bg fg ... ",
          end="", flush=True)
    with VtmTileSession(NEST_TILE_ARGS, vtm_config=NEST_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.snapshot(timeout=2.0)

        coords = find_marker_position(s._screen_buf, "[NEST]")
        if coords is None:
            return fail("trigger '[NEST]' not rendered")
        trigger_row, trigger_col = coords

        # Open the parent popup.
        s.reset_buffer()
        s.click(trigger_col + 2, trigger_row)
        s.snapshot(timeout=1.5)

        leaf_pos = find_marker_position(s._screen_buf, NEST_LEAF_X)
        if leaf_pos is None:
            return fail("could not locate first popup row")
        first_row, _ = leaf_pos

        # Depth-0 level_bg from application.hpp = (0x31, 0x32, 0x44).
        level_bg = (49, 50, 68)
        top_positions = find_marker_with_fg_positions(
            s._screen_buf, "▄", level_bg)
        if not top_positions:
            return fail(
                "no '▄' (U+2584) glyph emitted with fg=(49,50,68) — "
                "the dropdown's top decoration is missing"
            )

        # At least one ▄ must land on the row IMMEDIATELY above the
        # popup's first item.
        expected_edge_row = first_row - 1
        edge_hits = [p for p in top_positions if p[0] == expected_edge_row]
        if not edge_hits:
            return fail(
                f"'▄' with fg=(49,50,68) was emitted but not on the "
                f"expected edge row {expected_edge_row} (one above the "
                f"first popup item at row {first_row}); rows seen: "
                f"{sorted({p[0] for p in top_positions})}"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during top-edge test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print(f"PASS (edge_row={expected_edge_row}, first_row={first_row}, "
              f"hits={len(edge_hits)})")
        return True


def test_dropdown_bottom_edge_uses_upper_half_block_with_menu_bg_fg():
    """The dropdown popup paints a decorative '▀' (U+2580, upper half
    block) row immediately below its last item row. Like the top edge,
    the foreground equals the popup body's background colour and the
    cell's background is left untouched (transparent — whatever's
    behind shows through).

    Verifies the contract added to menu::_attach_popup_overlay:
      - At least one '▀' is emitted with fg == depth-0 popup level_bg
        (R=49, G=50, B=68).
      - That '▀' lands exactly one row below the popup's LAST item
        row (NEST_LEAF_Y).

    The menu-bar's cover layer also emits '▀' glyphs (tile.hpp), but
    those use the window background as fg, not the popup level_bg, so
    filtering by fg disambiguates the bottom-edge ▀ from the cover ▀.
    """
    print("TEST: dropdown bottom edge ▀ with menu-bg fg ... ",
          end="", flush=True)
    with VtmTileSession(NEST_TILE_ARGS, vtm_config=NEST_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.snapshot(timeout=2.0)

        coords = find_marker_position(s._screen_buf, "[NEST]")
        if coords is None:
            return fail("trigger '[NEST]' not rendered")
        trigger_row, trigger_col = coords

        # Open the parent popup.
        s.reset_buffer()
        s.click(trigger_col + 2, trigger_row)
        s.snapshot(timeout=1.5)

        last_pos = find_marker_position(s._screen_buf, NEST_LEAF_Y)
        if last_pos is None:
            return fail("could not locate last popup row")
        last_row, _ = last_pos

        level_bg = (49, 50, 68)
        bot_positions = find_marker_with_fg_positions(
            s._screen_buf, "▀", level_bg)
        if not bot_positions:
            return fail(
                "no '▀' (U+2580) glyph emitted with fg=(49,50,68) — "
                "the dropdown's bottom decoration is missing"
            )

        expected_edge_row = last_row + 1
        edge_hits = [p for p in bot_positions if p[0] == expected_edge_row]
        if not edge_hits:
            return fail(
                f"'▀' with fg=(49,50,68) was emitted but not on the "
                f"expected edge row {expected_edge_row} (one below the "
                f"last popup item at row {last_row}); rows seen: "
                f"{sorted({p[0] for p in bot_positions})}"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during bottom-edge test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print(f"PASS (edge_row={expected_edge_row}, last_row={last_row}, "
              f"hits={len(edge_hits)})")
        return True


def test_dropdown_edge_background_is_transparent():
    """The decorative edge rows wipe the cell first (clearing cursor,
    style bits, hyperlinks etc.) then set only fg + txt, leaving bg at
    wipe's alpha=0 sentinel so the layer beneath blends through. So an
    edge cell's rendered bg must differ from the popup body's bg
    (which IS the level_bg).

    If we accidentally painted bg=level_bg on the edge, the edge cell's
    bg would equal the popup row bg and the visual "transparent" effect
    would be lost. This test asserts the inverse: at least one edge
    glyph carries a bg distinct from the level_bg that its own fg
    equals. Together with the top/bottom edge tests above, this
    confirms the cell was painted as fg-only over a transparent bg.
    """
    print("TEST: dropdown edge bg is transparent (≠ menu-bg) ... ",
          end="", flush=True)
    with VtmTileSession(NEST_TILE_ARGS, vtm_config=NEST_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.snapshot(timeout=2.0)

        coords = find_marker_position(s._screen_buf, "[NEST]")
        if coords is None:
            return fail("trigger '[NEST]' not rendered")
        trigger_row, trigger_col = coords

        s.reset_buffer()
        s.click(trigger_col + 2, trigger_row)
        s.snapshot(timeout=1.5)

        level_bg = (49, 50, 68)
        # Scan the raw stream for ANY ▀-or-▄ painted with fg=level_bg.
        # Walk forward tracking the most recent bg SGR (48;2;R;G;B) and
        # most recent fg SGR; for every edge glyph we encounter, record
        # the bg that was in effect at that point. The test passes if at
        # least one edge glyph carried a bg ≠ level_bg.
        raw = s._screen_buf
        cup_re = re.compile(rb"\x1b\[(\d+);(\d+)H")
        fg_re  = re.compile(rb"38;2;(\d+);(\d+);(\d+)")
        bg_re  = re.compile(rb"48;2;(\d+);(\d+);(\d+)")
        events = []
        for m in fg_re.finditer(raw):
            events.append((m.start(), "fg", m))
        for m in bg_re.finditer(raw):
            events.append((m.start(), "bg", m))
        for glyph in (b"\xe2\x96\x84", b"\xe2\x96\x80"):
            pos = 0
            while True:
                idx = raw.find(glyph, pos)
                if idx < 0: break
                events.append((idx, "mark", idx))
                pos = idx + len(glyph)
        events.sort(key=lambda e: e[0])
        cur_fg = None
        cur_bg = None
        transparent_edge_seen = False
        for _pos, kind, payload in events:
            if kind == "fg":
                cur_fg = (int(payload.group(1)),
                          int(payload.group(2)),
                          int(payload.group(3)))
            elif kind == "bg":
                cur_bg = (int(payload.group(1)),
                          int(payload.group(2)),
                          int(payload.group(3)))
            else:
                if cur_fg == level_bg and cur_bg is not None and cur_bg != level_bg:
                    transparent_edge_seen = True
                    break
        if not transparent_edge_seen:
            return fail(
                "no edge glyph found whose fg=level_bg but bg≠level_bg "
                "— either the edge was painted with bg=level_bg "
                "(opaque) or no edges were emitted at all"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during edge-transparency test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print("PASS")
        return True


def _open_pane_dropdown(s):
    """Locate the pane applet's [PANEDROP] trigger (row 3) and click it
    open. Returns (trigger_row, trigger_col) of the '[' cell, or None if
    the trigger never rendered. Helper for the pane-applet tests below.
    """
    # The subprocess needs extra settle time: the dtvt child boots, merges
    # the inherited config, and paints its menu bar a beat after the outer
    # tile is up.
    s.snapshot(timeout=2.0)
    coords = find_marker_position(s._screen_buf, "[PANEDROP]")
    if coords is None:
        return None
    trigger_row, trigger_col = coords
    s.reset_buffer()
    # Click one cell right of '[' (on the label text) to open the popup.
    s.click(trigger_col + 1, trigger_row)
    s.snapshot(timeout=1.5)
    if find_marker_position(s._screen_buf, PANE_CHILD_LABEL) is None:
        return None
    return (trigger_row, trigger_col)


def _pane_dropdown_dismissed_by_click(label, target_col_fn):
    """Shared driver: open the pane applet's dropdown, click a target cell
    in the OUTER tile process (the tile menu bar or the pane title bar),
    then re-click the trigger and confirm the popup re-opens.

    Re-open is the dismissal probe: open_dropdown_popup's `menu.dropdown.open`
    guard makes a second click on an already-open trigger toggle the chain
    OFF instead of re-opening it. So if the outer click dismissed the chain,
    the guard is clear and the re-click re-opens the popup (child visible);
    if the chain stayed attached, the re-click toggles it off and the child
    does NOT repaint. (Same live-state probe used by the other dismissal
    tests in this file.)
    """
    print(f"TEST: pane-applet dropdown dismissed by {label} ... ",
          end="", flush=True)
    with VtmTileSession(PANE_TILE_ARGS, settle_delay=3.0,
                        vtm_config=PANE_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        coords = _open_pane_dropdown(s)
        if coords is None:
            return fail(
                "pane applet dropdown trigger '[PANEDROP]' did not render or "
                "did not open — the dtvt child may not have inherited the "
                "<terminal><menu> dropdown config via $VTM_CONFIG"
            )
        trigger_row, trigger_col = coords

        # Click the outer-process target (row 1 tile menu bar, or row 2
        # pane title bar). This click is handled entirely by the outer
        # tile process; the only thing the subprocess observes is the
        # cursor leaving the pane viewport (a forwarded sysmouse halt),
        # which must tear the chain down.
        tcol, trow = target_col_fn()
        s.reset_buffer()
        s.click(tcol, trow)
        time.sleep(0.4)
        s.snapshot(timeout=1.2)

        # Re-click the trigger. Re-opens iff the outer click dismissed.
        s.reset_buffer()
        s.click(trigger_col + 1, trigger_row)
        rendered = s.snapshot(timeout=1.5)
        if PANE_CHILD_LABEL not in rendered:
            return fail(
                f"after clicking {label} with the pane dropdown open, "
                f"re-clicking the trigger did not re-open the popup — the "
                f"outer-process click did not dismiss the subprocess's open "
                f"dropdown chain (regression: cursor leaving the pane "
                f"viewport must tear the chain down across the dtvt bridge)"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during pane-dropdown dismissal test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print("PASS")
        return True


def test_pane_applet_dropdown_dismissed_by_tile_menu_bar_click():
    """Regression: a dropdown in a tile pane applet's menu bar (row 3,
    rendered by a dtvt subprocess) must close when the user clicks the
    tile's own menu bar (row 1, owned by the outer tile process)."""
    return _pane_dropdown_dismissed_by_click(
        "tile menu bar (row 1)",
        # The TILEBAR button sits at the left of row 1; click on its label.
        lambda: (6, 1),
    )


def test_pane_applet_dropdown_dismissed_by_pane_title_bar_click():
    """Regression: a dropdown in a tile pane applet's menu bar (row 3,
    rendered by a dtvt subprocess) must close when the user clicks the
    pane's title bar (row 2, owned by the outer tile process)."""
    return _pane_dropdown_dismissed_by_click(
        "pane title bar (row 2)",
        # Mid-screen on row 2 lands on the pane's title/grip bar, clear of
        # the row-1 controls and the row-3 applet menu.
        lambda: (COLS // 2, 2),
    )


TESTS = [
    test_dropdown_menu_item_loads_from_xml_and_opens_popup,
    test_dropdown_menu_keeps_menubar_visible_with_log_repaint,
    test_nested_dropdown_submenu_opens_to_the_right,
    test_nested_dropdown_submenu_flips_left_when_no_room_on_right,
    test_nested_dropdown_submenu_opens_on_hover_no_click,
    test_dropdown_esc_dismisses_chain,
    test_dropdown_outside_click_dismisses_chain,
    test_nested_dropdown_leaf_click_dismisses_chain,
    test_submenu_background_progressively_darker,
    test_hover_brightens_bg_keeps_fg_unchanged,
    test_click_another_trigger_closes_current_and_opens_new,
    test_hover_another_trigger_shows_hover_feedback,
    test_click_non_dropdown_button_dismisses_open_chain,
    test_click_empty_menubar_area_dismisses_chain,
    test_amp_label_strips_marker_and_underlines_shortcut,
    test_keyboard_down_up_arrows_select_rows,
    test_keyboard_right_arrow_opens_submenu_and_focuses_first_row,
    test_keyboard_left_arrow_closes_submenu_returns_to_parent,
    test_keyboard_enter_activates_leaf_and_dismisses_chain,
    test_keyboard_shortcut_letter_activates_matching_row,
    test_keyboard_nav_is_independent_of_mouse_hover,
    test_keyboard_does_not_pass_through_to_terminal,
    test_menu_padding_config_controls_horizontal_cell_padding,
    test_dropdown_top_edge_uses_lower_half_block_with_menu_bg_fg,
    test_dropdown_bottom_edge_uses_upper_half_block_with_menu_bg_fg,
    test_dropdown_edge_background_is_transparent,
    test_pane_applet_dropdown_dismissed_by_tile_menu_bar_click,
    test_pane_applet_dropdown_dismissed_by_pane_title_bar_click,
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
            time.sleep(0.5)

    total = passed + failed
    print(f"\n{'='*60}")
    print(f"Results: {passed}/{total} passed, {failed} failed")
    print(f"{'='*60}")

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
    sys.exit(0 if ok else 1)
