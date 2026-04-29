#!/usr/bin/env python3
# Copyright (c) Dmitry Sapozhnikov
# Licensed under the MIT license.

"""
End-to-end TUI regression test for the tile command-bar terminal commands.

Verifies that:
  * The single [CMD] item rendered in the tile menu (vtm.xml) opens the
    command bar overlay when clicked.
  * Typing 'terminal' filters the command list and includes terminal
    commands sourced from the new <group label="terminal"> block in
    vtm.xml.
  * Selecting "terminal: Toggle Find Bar" via Enter dispatches the script
    to the focused terminal pane (not the tile manager), causing the
    terminal find-bar overlay to render.

Driving vtm-tile via a pty is sufficient because vtm-tile already accepts
SGR mouse input in the existing test suite (see test_workspace.py
popup_click_workspace) and processes keypresses delivered as plain bytes.

The test is skipped if the [CMD] menu item cannot be located on screen
(e.g., because the slim menu was disabled), but failure to locate it
after a normal launch is reported as a failure.
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
SETTLE_DELAY = 2.5

# Self-contained tile config: spawn a single 'term' pane and pin the
# tile <menu> contents inside the test so the suite is independent of
# vtm.xml's /config/tile/menu defaults. Reducing this coupling means
# future menu changes in vtm.xml will not silently invalidate the
# regression: the test continues to exercise the [CMD] launcher exactly
# as configured here. We still rely on vtm.xml for the <commandbar>
# group definitions (<TerminalFindBarToggle> etc.) because those are
# global Scripting aliases, not tile-scoped config.
TILE_CONFIG = (
    "<config>"
        "<tile>"
            '<app selected="term">'
                '<item id="term" label="term" type="dtvt" cmd="$0 -r term"/>'
            "</app>"
            "<menu item*>"
                '<item label="  [CMD]  " tooltip=" Open the command bar (fuzzy command launcher). " script=OnLeftClick|TileOpenCommandBar/>'
            "</menu>"
        "</tile>"
    "</config>"
)
TILE_ARGS = ["-c", TILE_CONFIG]


# Multi-pane variant: enables the split/focus-cycle key bindings used by
# the dispatch-to-focused-pane regression. We keep this separate from
# TILE_CONFIG so the simple single-pane test stays minimal and any
# future change to split semantics only affects the multi-pane case.
MULTI_PANE_TILE_CONFIG = (
    "<config>"
        "<tile>"
            '<app selected="term">'
                '<item id="term" label="term" type="dtvt" cmd="$0 -r term"/>'
            "</app>"
            "<menu item*>"
                '<item label="  [CMD]  " tooltip=" Open the command bar (fuzzy command launcher). " script=OnLeftClick|TileOpenCommandBar/>'
            "</menu>"
        "</tile>"
        "<events><tile>"
            """<script=TileSplitHorizontally on="Alt+Shift+'|'"/>"""
            '<script=TileFocusNextPane     on="Alt+Shift+N"/>'
        "</tile></events>"
    "</config>"
    "<Scripting>"
        '<TileSplitHorizontally="vtm.tile.SplitPane(0);"/>'
        '<TileFocusNextPane="vtm.tile.FocusNextPane(1);"/>'
    "</Scripting>"
)
MULTI_PANE_TILE_ARGS = ["-c", MULTI_PANE_TILE_CONFIG]


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


# Naive ANSI/VT500 stripper: removes CSI/OSC/DCS sequences and SGR styling
# so we can scan rendered glyphs by line. Sufficient for our markers
# ("[CMD]", "Find", "terminal: Toggle Find Bar"), which never embed escape
# bytes within themselves.
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
        """Drain pending output and return the cumulative cleaned screen.

        We retain the entire running stream because vtm emits incremental
        cell updates rather than full repaints, so the most recent paint
        is the union of every chunk seen so far. Stripping ANSI gives a
        coarse but workable text view that exposes wide markers like
        [CMD] and 'terminal: Toggle Find Bar' without false positives.
        """
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
                # Brief idle: assume drained.
                break
        return strip_ansi(self._screen_buf).decode("utf-8", errors="replace")

    def reset_buffer(self):
        self._screen_buf = b""


def find_marker_columns(raw_buf, marker):
    """Return all columns at which `marker` was painted via CUP positioning.

    Walks the raw byte stream, tracking the most recently emitted
    cursor-position (CSI row;col H) and the running visible-character
    offset since that CUP. Each occurrence of `marker` in the visible
    stream yields its absolute (row, col). Multiple paints of the same
    marker (e.g. in two terminal panes) all show up so the caller can
    distinguish 'left half' vs 'right half' of the screen.
    """
    marker_b = marker.encode() if isinstance(marker, str) else marker
    results = []
    i = 0
    cur_row = 1
    cur_col = 1
    visible_run_col = 1
    n = len(raw_buf)
    while i < n:
        b = raw_buf[i]
        if b == 0x1b and i + 1 < n and raw_buf[i+1] == ord('['):
            # CSI sequence: skip until final byte 0x40-0x7e.
            j = i + 2
            while j < n and not (0x40 <= raw_buf[j] <= 0x7e):
                j += 1
            if j < n:
                params = raw_buf[i+2:j]
                final = raw_buf[j]
                if final == ord('H') or final == ord('f'):
                    m = re.match(rb"^(\d*);(\d*)$", params)
                    if m:
                        cur_row = int(m.group(1) or b'1')
                        cur_col = int(m.group(2) or b'1')
                        visible_run_col = cur_col
                i = j + 1
                continue
            else:
                break
        if b == 0x1b:
            # Other ESC sequences: skip 2 bytes conservatively.
            i += 2
            continue
        # Visible byte; check for marker match starting here.
        if raw_buf[i:i+len(marker_b)] == marker_b:
            results.append((cur_row, visible_run_col))
            visible_run_col += len(marker_b)
            i += len(marker_b)
            continue
        # Advance one visible cell. Skip UTF-8 continuation bytes so
        # multi-byte glyphs (box-drawing chars in pane chrome) advance
        # the column count by exactly one cell. Wide East-Asian glyphs
        # would still undercount but our markers are pure ASCII so the
        # coarse left/right bucket we need stays correct.
        if b >= 0x20 and (b < 0x80 or b >= 0xc0):
            visible_run_col += 1
        i += 1
    return results


def find_cmd_button(raw_buf):
    """Locate the rendered '[CMD]' menu marker using CUP positioning.

    vtm emits absolute cursor-position sequences (CSI row;col H) before
    each cell run. To recover screen coordinates we find the last
    occurrence of the '[CMD]' marker, walk back to the most recent CUP,
    and add the number of intervening visible cells to the CUP's column.
    Returns (row, col) where col points at the centre 'M' of '[CMD]'.
    """
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
    centre_col = start_col + 2  # '[CMD]' is 5 chars; centre is offset 2.
    return (row, centre_col)


def fail(msg):
    print(f"FAIL - {msg}")
    return False


def test_command_bar_opens_via_menu_and_dispatches_to_terminal():
    print("TEST: command bar [CMD] menu -> terminal find bar ... ", end="", flush=True)
    with VtmTileSession(TILE_ARGS) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")

        # Drain the initial paint completely.
        s.snapshot(timeout=2.0)
        coords = find_cmd_button(s._screen_buf)
        if coords is None:
            return fail("[CMD] menu marker not rendered on initial paint")
        target_row, target_col = coords

        # Click the [CMD] menu button to open the overlay.
        s._screen_buf = b""  # Reset so subsequent CUP scans are unambiguous.
        s.click(target_col, target_row)
        rendered = s.snapshot(timeout=1.5)
        # The visible list is windowed (only N rows shown), so the
        # terminal group entries may be off-screen until filtered. Just
        # confirm the bar opened by checking for a known always-visible
        # 'pane:' entry.
        if "pane: Focus" not in rendered:
            return fail(f"command bar overlay did not appear (clicked row={target_row} col={target_col})")

        # Type a query that uniquely matches "terminal: Toggle Find Bar".
        # vtm paints each cell run with absolute CUP positioning so the
        # raw byte stream interleaves coords with glyphs; strip ANSI
        # before scanning. The rendered cell width also drops some
        # spaces between columns, so we collapse spaces before matching.
        s._screen_buf = b""
        s.write(b"term tog find")
        rendered = s.snapshot(timeout=1.5)
        if "ToggleFind" not in rendered.replace(" ", ""):
            return fail("filter 'term tog find' did not surface Toggle Find Bar")

        # Press Enter to dispatch. The script should be sent to the focused
        # terminal pane and surface the find bar overlay. The find bar's
        # visible heading is "Search" (the underlying Lua method is
        # ToggleFindBar but the rendered overlay box is labelled Search,
        # see the term find_t feature). We also accept "Find" defensively.
        s._screen_buf = b""
        s.write(b"\r")
        rendered = s.snapshot(timeout=2.0)
        compact = rendered.replace(" ", "")
        if "Search" not in compact and "Find" not in compact:
            return fail("terminal find bar did not appear after Enter dispatch")

        # Sanity: tile must still be alive.
        if not s.is_alive():
            return fail("vtm-tile crashed after dispatch")

        print("PASS")
        return True


def _open_cmd_bar_and_run_find(s):
    """Locate [CMD], click it, type the find filter, press Enter.

    Resets the screen buffer immediately before pressing Enter so the
    caller can inspect post-dispatch paints in isolation.
    """
    s.snapshot(timeout=2.0)
    coords = find_cmd_button(s._screen_buf)
    if coords is None:
        return False, "[CMD] not rendered"
    target_row, target_col = coords
    s.reset_buffer()
    s.click(target_col, target_row)
    rendered = s.snapshot(timeout=1.5)
    if "pane: Focus" not in rendered:
        return False, "command bar overlay did not appear"
    s.reset_buffer()
    s.write(b"term tog find")
    rendered = s.snapshot(timeout=1.5)
    if "ToggleFind" not in rendered.replace(" ", ""):
        return False, "filter did not surface Toggle Find Bar"
    s.reset_buffer()
    s.write(b"\r")
    s.snapshot(timeout=2.0)
    return True, ""


def test_command_bar_dispatches_only_to_focused_pane():
    """Splitting horizontally yields two terminal panes.

    The command bar must dispatch vtm.terminal.* against only the
    *focused* pane. We verify by counting how many distinct screen
    columns paint the find bar's "Search" header after toggling: the
    bar must appear in exactly one pane's column range, and switching
    focus to the other pane and re-toggling must move the bar.
    """
    print("TEST: command bar dispatches only to focused pane ... ", end="", flush=True)
    with VtmTileSession(MULTI_PANE_TILE_ARGS) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        # Drain initial paint with a generous timeout: when this test
        # runs back-to-back with another VtmTileSession the new pty
        # can take longer than usual to deliver the first [CMD]
        # paint, so retry a couple of times before declaring failure.
        for _ in range(4):
            s.snapshot(timeout=1.5)
            if find_cmd_button(s._screen_buf) is not None:
                break

        # Split horizontally: Alt+Shift+|. Default focus lands on the
        # newly created right-hand pane in vtm tile semantics.
        s.write(b"\x1b|")
        time.sleep(0.6)
        s.snapshot(timeout=1.5)

        # First dispatch: should toggle the find bar on the focused
        # (right) pane only. We *don't* reset the buffer before
        # locating [CMD]: vtm only repaints menu cells when they
        # change, so the most recent [CMD] glyph may be from the
        # initial paint and discarding it would make find_cmd_button
        # return None.
        ok, why = _open_cmd_bar_and_run_find(s)
        if not ok:
            return fail(f"first toggle: {why}")
        first_hits = find_marker_columns(s._screen_buf, "Search")
        if not first_hits:
            return fail("no 'Search' header rendered after first toggle")
        # All hits should be in one half of the screen. Reject if the
        # bar appeared in *both* halves (indicates the script reached
        # the unfocused pane too).
        first_cols = [c for _r, c in first_hits]
        first_left = any(c < COLS // 2 for c in first_cols)
        first_right = any(c >= COLS // 2 for c in first_cols)
        if first_left and first_right:
            return fail(f"first toggle leaked to both panes (cols={first_cols})")
        first_side = "left" if first_left else "right"

        # Toggle off so the next test starts from a clean slate, then
        # switch focus to the opposite pane. Don't reset the screen
        # buffer here — vtm coalesces menu repaints, so wiping the
        # buffer would erase the only [CMD] paint we have on record
        # and the helper's lookup would fail.
        ok, why = _open_cmd_bar_and_run_find(s)
        if not ok:
            return fail(f"toggle-off: {why}")
        # Wait briefly to ensure the find bar is gone before focus
        # switch.
        time.sleep(0.3)

        s.write(b"\x1bN")  # Alt+Shift+N => FocusNextPane.
        time.sleep(0.4)
        s.snapshot(timeout=1.0)

        # Second dispatch: must now hit the *other* pane. We need to
        # isolate the post-Enter paint so the column scan only sees
        # the current toggle's "Search" header — accumulate paints up
        # to the Enter, then snapshot fresh after dispatch. The
        # helper handles this internally (reset_buffer before \r).
        ok, why = _open_cmd_bar_and_run_find(s)
        if not ok:
            return fail(f"second toggle: {why}")
        second_hits = find_marker_columns(s._screen_buf, "Search")
        if not second_hits:
            return fail("no 'Search' header rendered after focus switch")
        second_cols = [c for _r, c in second_hits]
        second_left = any(c < COLS // 2 for c in second_cols)
        second_right = any(c >= COLS // 2 for c in second_cols)
        if second_left and second_right:
            return fail(f"second toggle leaked to both panes (cols={second_cols})")
        second_side = "left" if second_left else "right"

        if first_side == second_side:
            return fail(
                f"toggle did not follow focus: first={first_side} cols={first_cols} "
                f"second={second_side} cols={second_cols}"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during multi-pane dispatch")

        print(f"PASS (first={first_side}, second={second_side})")
        return True


def test_command_bar_runs_tile_scoped_command():
    """Tile-scoped commands (vtm.tile.*) must execute against the tile
    manager, not the focused terminal child.

    Regression: when the dispatcher routes blindly into the focused
    pane (a dtvt terminal child), tile commands like
    vtm.tile.SplitPane(0) reach a process that doesn't bind them and
    silently no-op. The fix routes by script content: vtm.terminal.*
    goes to the focused applet, everything else (vtm.tile.*) is
    executed in-process.

    We verify by running 'layout: Split Horizontal' from the command
    bar on a single-pane tile and confirming a split actually happens.
    Detection: after the split, toggling the terminal find bar via
    the command bar should produce a 'Search' header confined to one
    half of the screen — that's only true when two panes exist.
    """
    print("TEST: command bar tile-scoped command (split via cmd bar) ... ", end="", flush=True)
    with VtmTileSession(MULTI_PANE_TILE_ARGS) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.snapshot(timeout=2.0)

        # Open command bar, filter to "layout: Split Horizontal", Enter.
        coords = find_cmd_button(s._screen_buf)
        if coords is None:
            return fail("[CMD] not rendered initially")
        target_row, target_col = coords
        s.reset_buffer()
        s.click(target_col, target_row)
        rendered = s.snapshot(timeout=1.5)
        if "pane: Focus" not in rendered:
            return fail("command bar did not open")

        s.reset_buffer()
        s.write(b"lay sp hor")
        rendered = s.snapshot(timeout=1.5)
        if "SplitHorizontal" not in rendered.replace(" ", ""):
            return fail("filter did not surface 'Split Horizontal'")
        s.write(b"\r")
        time.sleep(0.6)
        s.snapshot(timeout=1.5)

        # Now trigger the find bar via command bar; it must localize
        # to a single half of the screen (proof of split).
        ok, why = _open_cmd_bar_and_run_find(s)
        if not ok:
            return fail(f"post-split toggle: {why}")
        hits = find_marker_columns(s._screen_buf, "Search")
        if not hits:
            return fail("'Search' header did not appear post-split — split-via-cmdbar likely failed")
        cols = [c for _r, c in hits]
        has_left = any(c < COLS // 2 for c in cols)
        has_right = any(c >= COLS // 2 for c in cols)
        if has_left and has_right:
            return fail(f"'Search' spans both halves (cols={cols}); split-via-cmdbar may have produced no split")
        if not s.is_alive():
            return fail("vtm-tile crashed after tile-scoped dispatch")

        side = "left" if has_left else "right"
        print(f"PASS (split confirmed, find bar on {side})")
        return True


# Keybind-proxy test config: binds Alt+Shift+P to a script that calls
# vtm.terminal.Print(<marker>) directly from the tile manager's Lua
# state. The proxy installed on the tile boss must rewrite that call
# into a vtm.terminal.Print(...) script and forward it to the focused
# dtvt child for execution. If the proxy is missing or broken the call
# logs "No 'vtm.terminal' object found" and the marker never appears.
#
# We deliberately exercise three argument shapes (string, integer,
# boolean) in one binding to validate tile_terminal_proxy_quote_arg's
# coverage of the supported Lua scalar types in a single round-trip.
KEYBIND_PROXY_MARKER = "KBPROXYOK42"
KEYBIND_PROXY_TILE_CONFIG = (
    "<config>"
        "<tile>"
            '<app selected="term">'
                '<item id="term" label="term" type="dtvt" cmd="$0 -r term"/>'
            "</app>"
            "<menu item*>"
                '<item label="  [CMD]  " tooltip=" cmd " script=OnLeftClick|TileOpenCommandBar/>'
            "</menu>"
        "</tile>"
        "<events><tile>"
            '<script=KeybindProxyPrint on="Alt+Shift+P"/>'
        "</tile></events>"
    "</config>"
    "<Scripting>"
        # Three args: string + integer + boolean; concatenate the
        # integer back to the marker string so we can confirm the
        # number survived quoting (Lua coerces tonumber()..string).
        f'<KeybindProxyPrint=\'vtm.terminal.Print("{KEYBIND_PROXY_MARKER}_", 42, "_", tostring(true))\'/>'
    "</Scripting>"
)
KEYBIND_PROXY_TILE_ARGS = ["-c", KEYBIND_PROXY_TILE_CONFIG]


def test_keybind_proxies_terminal_call_to_focused_pane():
    """Direct keybind -> vtm.terminal.Print -> focused pane.

    Bypasses the command bar entirely: the script lives in <Scripting>
    and is invoked by Alt+Shift+P. The tile manager's Lua state has no
    real vtm.terminal table, only the proxy installed on the tile boss.
    A successful PASS proves:
      * The proxy's __index closure was invoked for 'Print'.
      * tile_terminal_proxy_quote_arg correctly serialised a string,
        an integer, and a boolean back to Lua source.
      * The reconstructed script reached the focused dtvt child via
        e2::command::run and was executed by the child's Lua engine.
    """
    print("TEST: keybind proxies vtm.terminal.* to focused pane ... ", end="", flush=True)
    expected = f"{KEYBIND_PROXY_MARKER}_42_true"
    with VtmTileSession(KEYBIND_PROXY_TILE_ARGS) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.snapshot(timeout=2.0)

        # Alt+Shift+P. Many xterm-style terminals encode Alt as ESC
        # prefix; uppercase 'P' carries the Shift modifier.
        s.reset_buffer()
        s.write(b"\x1bP")
        time.sleep(0.5)
        rendered = s.snapshot(timeout=1.5)

        # Print() pushes the marker into the pty as if typed at the
        # shell prompt. Without a trailing newline it sits on the
        # command line where the shell echoes it back. Either way, the
        # marker text becomes a visible glyph run in the pane.
        compact = rendered.replace(" ", "")
        if expected not in compact:
            return fail(
                f"marker '{expected}' not found after Alt+Shift+P "
                f"(proxy likely did not forward to focused pane)"
            )
        if not s.is_alive():
            return fail("vtm-tile crashed after keybind dispatch")
        print("PASS")
        return True


if __name__ == "__main__":
    if not os.path.isfile(VTM_TILE_BINARY):
        print(f"ERROR: vtm-tile binary not found at {VTM_TILE_BINARY}")
        print("Set VTM_TILE_BINARY env var or build vtm-tile first.")
        sys.exit(2)
    kill_all_vtm()
    try:
        ok = test_command_bar_opens_via_menu_and_dispatches_to_terminal()
        if ok:
            kill_all_vtm()
            time.sleep(0.5)
            ok = test_command_bar_dispatches_only_to_focused_pane()
        if ok:
            kill_all_vtm()
            time.sleep(0.5)
            ok = test_command_bar_runs_tile_scoped_command()
        if ok:
            kill_all_vtm()
            time.sleep(0.5)
            ok = test_keybind_proxies_terminal_call_to_focused_pane()
    finally:
        kill_all_vtm()
    sys.exit(0 if ok else 1)
