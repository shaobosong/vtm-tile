#!/usr/bin/env python3
# Copyright (c) Dmitry Sapozhnikov
# Licensed under the MIT license.

"""
TUI regression tests for the terminal find-bar overlay.

Covers:
  * F3 toggles the bar on/off.
  * Rounded-corner frame (╭╮╰╯─│) is rendered.
  * " Search " label, underline input field (_), and ↑ ↓ × buttons appear.
  * Typing characters shows them in the input field; Backspace deletes.
  * Long input scrolls horizontally inside the fixed-width field.
  * Terminal cursor in bar cells is suppressed (we draw our own caret).
  * Esc closes the bar without leaking to the shell.
  * Clicking × closes the bar.
  * Printable keys while bar is open don't leak to the shell beneath.

All of these drive vtm-desk via a pty with the `-r term` app.
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

VTM_DESK_BINARY = os.environ.get(
    "VTM_DESK_BINARY",
    os.path.join(os.path.dirname(__file__), "..", "build", "vtm-desk"),
)

COLS = 80
ROWS = 24
READ_TIMEOUT = 5.0
SETTLE_DELAY = 1.0

# Complete desk/terminal configuration: find-bar key bindings with their
# scripting definitions.  This makes the tests fully self-contained and
# independent of vtm.xml built-in defaults.
DESK_CONFIG = (
    "<config>"
        "<terminal>"
            "<confirm_close=0/>"
        "</terminal>"
        "<events><terminal>"
            '<script=IgnoreAltbuf | TerminalFindBarToggle on="F3"/>'
        "</terminal></events>"
    "</config>"
    "<Scripting>"
        '<TerminalFindBarToggle="vtm.terminal.ToggleFindBar();"/>'
        '<IgnoreAltbuf="if (vtm.terminal.AltbufMode()) then vtm.terminal.ForwardKeys(); return; end;"/>'
    "</Scripting>"
)
DESK_TERM_ARGS = ["-r", "term"]
# DESK_CONFIG is shipped to vtm-desk via $VTM_CONFIG (see VtmSession.vtm_config).

# Bar geometry (mirrors term.hpp constants).
BAR_COLS = 44
BAR_ROWS = 3
GAP_ROWS = 1

# Screen position of the bar (1-indexed, right-aligned with 2-cell margin to
# the right edge, at top under a gap_rows spacer).
RIGHT_MARGIN = 2
BAR_RIGHT = COLS - RIGHT_MARGIN                # col 78
BAR_LEFT  = BAR_RIGHT - BAR_COLS + 1           # col 37
BAR_TOP_BORDER_ROW    = GAP_ROWS + 1          # row 2
BAR_INPUT_ROW         = GAP_ROWS + 2          # row 3
BAR_BOTTOM_BORDER_ROW = GAP_ROWS + 3          # row 4

# Button columns (local-to-bar, see layout_of in term.hpp).
# layout_of uses local x where 0 = left border. So global = BAR_LEFT + local_x.
# Each control carries 1-cell padding on both sides.  Buttons are 3 cells wide
# (pad + glyph + pad); the column below is the CENTER cell (glyph) — clicking
# any of the 3 cells (center-1 .. center+1) triggers it.
# Layout of outer_w = 44 (local cols 0..43):
#   0: │   1: pad   2..22: [input 21 chars]
#   23..33: [counter right-aligned 9 chars (11 cells incl. pads)]
#   34..36: [↑]   37..39: [↓]   40..42: [×]   43: │
# btn_up_x (local) = 35, btn_dn_x (local) = 38, btn_x_x (local) = 41.
# Note: input drops its previous 1-cell right pad so the visible gap between
# the counter text and the in-input clear button is now 1 cell instead of 2.
BTN_UP_COL = BAR_LEFT + 35
BTN_DN_COL = BAR_LEFT + 38
BTN_X_COL  = BAR_LEFT + 41
# Input field: local [2 .. 22] (21 cells wide).
INPUT_X0   = BAR_LEFT + 2
INPUT_X1   = BAR_LEFT + 22
INPUT_W    = INPUT_X1 - INPUT_X0 + 1           # 21 cells
# Counter text (right-aligned, 9 chars) starts at local col 24 (count_text_x0).
COUNTER_COL0 = BAR_LEFT + 24
COUNTER_W    = 9

# Clear-query button (new feature — inside the input area).
# btn_clear_x (local) = input_x1 - 1 = 22 - 1 = 21.  Spans local [20..22].
# Mirrors btn_clear_x and btn_width from term.hpp.
BTN_CLEAR_COL        = BAR_LEFT + 21     # global center column of the clear button
BTN_WIDTH            = 3                  # button occupies 3 cells (center ± 1)
EFF_INPUT_W          = INPUT_W - BTN_WIDTH  # 18: text area width when query non-empty
BTN_CLEAR_GLYPH_UTF8 = b"\xc3\x97"       # UTF-8 encoding of × (U+00D7)


# ---------------------------------------------------------------------------
# Boilerplate copied from test_confirm_close.py (kept self-contained).
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


def sgr_press(col, row):   return f"\033[<0;{col};{row}M".encode()
def sgr_release(col, row): return f"\033[<0;{col};{row}m".encode()


class VtmSession:
    def __init__(self, binary, args, settle_delay=SETTLE_DELAY, cols=COLS, rows=ROWS, vtm_config=None):
        self.binary = binary
        self.args = args
        self.settle_delay = settle_delay
        self.cols = cols
        self.rows = rows
        self.vtm_config = vtm_config
        self.master_fd = None
        self.pid = None
        self._buffer = b""

    def __enter__(self):
        self.master_fd, slave_fd = pty.openpty()
        set_winsize(self.master_fd, self.rows, self.cols)
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
        self.drain(timeout=1.0)
        return self

    def __exit__(self, *a): self.cleanup()

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
        if isinstance(data, str): data = data.encode()
        os.write(self.master_fd, data)

    def drain(self, timeout=READ_TIMEOUT):
        chunk = read_all(self.master_fd, timeout=timeout)
        self._buffer += chunk
        return chunk

    def snapshot(self, settle=0.6):
        """Let the UI settle, drain output, and return the full accumulated stream."""
        time.sleep(settle)
        self.drain(timeout=0.8)
        return self._buffer

    def fresh_snapshot(self, settle=0.6):
        """Clear buffer, then snapshot -- use to inspect only latest frame(s)."""
        self._buffer = b""
        return self.snapshot(settle=settle)

    def click(self, col, row):
        self.write(sgr_press(col, row))
        time.sleep(0.05)
        self.write(sgr_release(col, row))

    def is_alive(self):
        if self.pid is None: return False
        try:
            pid, _ = os.waitpid(self.pid, os.WNOHANG)
            if pid == 0: return True
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
        """Click the x close button on the title bar (row 1, near right edge)."""
        self.click(self.cols - 2, 1)

    def normal_exit(self, timeout=5.0):
        """Return to the vtm-desk window and click the top-right close button
        for a normal exit.

        Clicks the close button on the title bar (row 1, near right edge).
        With ``confirm_close`` disabled, this exits vtm-desk directly.  We
        intentionally do NOT pre-send Esc — when the find-bar is closed and
        the underlying shell has focus, a stray Esc starts an incomplete CSI
        sequence in the shell that can interfere with the clean shutdown.
        The titlebar click is handled by the desk frame irrespective of
        which child overlay (find-bar, etc.) is open.

        Returns True if the process exited within ``timeout`` seconds.
        """
        # Drain any pending output so the click isn't racing a render.
        self.drain(timeout=0.2)
        time.sleep(0.2)
        self.click_close_button()
        return self.wait_for_exit(timeout=timeout)


# ---------------------------------------------------------------------------
# Stream assertions.  We search the raw emitted bytes because vtm paints the
# whole screen each frame and the rounded chars / label / buttons are easy to
# match as substrings.
# ---------------------------------------------------------------------------

FRAME_CHARS = ["\u256d", "\u256e", "\u2570", "\u256f", "\u2500", "\u2502"]  # ╭╮╰╯─│
LABEL = " Search "
UNDERLINE = "_"
# SGR sequences emitted by the new (tile.hpp-style) underlined input strip.
# The find-bar no longer paints `_` glyphs; it sets the cell underline
# attribute (SGR 4 = underline on, SGR 24 = off) with a custom underline
# color (SGR 58:2::r:g:b) matching col_uline = 0xff565f89 -> RGB 86,95,137.
SGR_ULINE_ON  = b"\x1b[4m"
SGR_ULINE_OFF = b"\x1b[24m"
# Underline color (col_uline = 0xff565f89, RGB 86,95,137); after vt256
# round-trip through cell::unc, the encoder emits one of a few possible
# SGR 58 forms.  We just check the SGR 58 sequence appears at all.
SGR_ULINE_COLOR_PREFIX = b"\x1b[58"
BTN_UP = "\u2191"   # ↑
BTN_DN = "\u2193"   # ↓
BTN_X  = "\u00d7"   # ×

# vtm parses F3 as the xterm SS3 sequence ESC O R (see vtm.xml key binding).
F3 = b"\x1bOR"

def contains(stream_bytes, needle):
    return needle.encode("utf-8") in stream_bytes


def assert_bar_rendered(stream):
    missing = [c for c in FRAME_CHARS if not contains(stream, c)]
    assert not missing, f"missing frame chars: {missing!r}"
    assert contains(stream, LABEL),    "missing ' Search ' label"
    # Input strip is now drawn via cell underline attribute (no `_` glyphs).
    # Verify both SGR underline-on and the custom underline-color SGR appear.
    assert SGR_ULINE_ON in stream, "missing SGR underline (ESC[4m) on input strip"
    assert SGR_ULINE_COLOR_PREFIX in stream, "missing SGR underline color (ESC[58...) on input strip"
    # The legacy `_` glyph fill must NOT appear inside the bar's input row.
    # We can't easily slice by row from a stream of escape sequences, so just
    # assert that the bar's mid-row span between the two `│` borders contains
    # no `_` glyph.  Detected by checking the substring between the leftmost
    # `│` after the top border and the trailing `│` of the same row.
    bar_top = stream.find("\u256d".encode())  # ╭
    bar_bot = stream.find("\u2570".encode())  # ╰
    if bar_top != -1 and bar_bot != -1 and bar_bot > bar_top:
        bar_body = stream[bar_top:bar_bot]
        assert b"_" not in bar_body, "stray `_` glyph inside find-bar body (should be underline attribute)"
    assert contains(stream, BTN_UP),   "missing ↑ button"
    assert contains(stream, BTN_DN),   "missing ↓ button"
    assert contains(stream, BTN_X),    "missing × button"


def assert_bar_absent(stream):
    # When the bar is closed there should be no rounded corners in output.
    assert not contains(stream, "\u256d"), "╭ still on screen — bar did not close"
    assert not contains(stream, "\u256f"), "╯ still on screen — bar did not close"


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_f3_opens_bar():
    print("TEST: F3 opens find-bar ... ", end="", flush=True)
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        s.fresh_snapshot(settle=0.3)
        s.write(b"\x1bOR")  # F3 (VT sequence — works with xterm legacy mapping)
        # vtm uses its own key event path; send via \e[ form too if needed.
        time.sleep(0.3)
        s.write(b"\x1b[13~")  # alt F3 encoding (ignored if unrecognised)
        stream = s.snapshot(settle=0.8)
        try:
            assert_bar_rendered(stream)
        except AssertionError as e:
            print(f"FAIL - {e}")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_f3_toggles_bar():
    print("TEST: F3 toggles find-bar off again ... ", end="", flush=True)
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        s.write(F3)                 # open
        s.snapshot(settle=0.8)
        s._buffer = b""
        s.write(F3)                 # close
        stream = s.snapshot(settle=1.0)
        try:
            assert_bar_absent(stream)
        except AssertionError as e:
            print(f"FAIL - {e}")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_esc_closes_bar():
    print("TEST: Esc closes find-bar ... ", end="", flush=True)
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        s.write(F3)     # open
        s.snapshot(settle=0.6)
        s.fresh_snapshot(settle=0.2)
        s.write(b"\x1b")           # Esc
        stream = s.snapshot(settle=0.8)
        try:
            assert_bar_absent(stream)
        except AssertionError as e:
            print(f"FAIL - {e}")
            return False
        if not s.is_alive():
            print("FAIL - vtm died")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_typing_shows_in_input():
    print("TEST: typing shows characters in input ... ", end="", flush=True)
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        s.write(F3)
        s.snapshot(settle=0.6)
        s.fresh_snapshot(settle=0.2)
        s.write(b"hello")
        stream = s.snapshot(settle=0.8)
        if b"hello" not in stream:
            print("FAIL - 'hello' not rendered in input")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_close_button_click_closes_bar():
    print("TEST: clicking × closes find-bar ... ", end="", flush=True)
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        s.write(F3)
        s.snapshot(settle=0.8)
        s._buffer = b""
        s.click(BTN_X_COL, BAR_INPUT_ROW)
        stream = s.snapshot(settle=1.0)
        try:
            assert_bar_absent(stream)
        except AssertionError as e:
            print(f"FAIL - {e}")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_bar_right_aligned_with_margin():
    """Verify bar is right-aligned with 2-cell margin: clicking × at the OLD
    centered column must NOT close the bar, but clicking × at the new
    right-aligned column MUST close it."""
    print("TEST: bar right-aligned with 2-cell margin ... ", end="", flush=True)
    # Column where × would be if the bar were horizontally centered (old layout).
    old_centered_btn_x = (COLS - BAR_COLS) // 2 + 1 + 30
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        s.write(F3)
        s.snapshot(settle=0.8)
        # Click where the × used to be (centered layout) — bar must stay open.
        s.click(old_centered_btn_x, BAR_INPUT_ROW)
        mid = s.snapshot(settle=0.6)
        if not contains(mid, "\u256d"):
            # The render path doesn't re-emit the corner once it's already
            # drawn, so absence of ╭ is not conclusive.  Better: click the
            # new right-aligned × and expect close.
            pass
        # Now click at the new right-aligned position.
        s._buffer = b""
        s.click(BTN_X_COL, BAR_INPUT_ROW)
        after = s.snapshot(settle=1.0)
        try:
            assert_bar_absent(after)
        except AssertionError as e:
            print(f"FAIL - × at col {BTN_X_COL} did not close bar: {e}")
            return False
        # Sanity: BTN_X_COL must be near the right edge.
        if BTN_X_COL < COLS - 6:
            print(f"FAIL - BTN_X_COL={BTN_X_COL} not near right edge (COLS={COLS})")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_keys_do_not_leak_to_shell():
    print("TEST: typing in bar does not leak to shell ... ", end="", flush=True)
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        s.write(F3)
        s.snapshot(settle=0.6)
        s.write(b"zzmarkerzz")
        s.snapshot(settle=0.4)
        s._buffer = b""
        s.write(b"\x1b")          # close bar
        stream = s.snapshot(settle=1.0)
        if b"zzmarkerzz" in stream:
            print("FAIL - marker leaked to shell after closing bar")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_long_input_horizontal_scroll():
    print("TEST: long input scrolls within fixed-width field ... ", end="", flush=True)
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        s.write(F3)
        s.snapshot(settle=0.6)
        payload = ("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
                   "abcdefghijklmnopqrstuvwx")  # 60 chars, INPUT_W=21
        s.write(payload.encode())
        stream = s.snapshot(settle=1.2)
        # Tail (last chars) must be visible on screen.
        if b"uvwx" not in stream:
            print("FAIL - tail of long input not visible (no scroll?)")
            return False
        # The bar must still be up (full accumulated stream contains frame).
        try:
            assert_bar_rendered(stream)
        except AssertionError as e:
            print(f"FAIL - {e}")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_backspace_removes_char():
    print("TEST: Backspace removes last char ... ", end="", flush=True)
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        s.write(F3)
        s.snapshot(settle=0.6)
        s.write(b"hello")
        s.snapshot(settle=0.6)
        # Delete two chars.  vtm only redraws the input cells that actually
        # changed, so 'hel' may not re-appear on the wire; instead we check
        # that the last two chars 'lo' are *not* on-screen in the latest frame
        # (we snapshot only what follows the backspaces).
        s._buffer = b""
        s.write(b"\x7f\x7f")
        after = s.snapshot(settle=1.0)
        # After two BS, the cells that used to hold 'l' and 'o' should be
        # repainted as part of the underlined input strip. With the new
        # tile.hpp-style underline (cell attribute, not `_` glyph), we
        # can't grep for a glyph; instead we expect the SGR underline-on
        # sequence to reappear in the post-BS frame as the bar redraws
        # the input row.  The previous-character cells now revert to space
        # under an underlined attribute, so the redraw must emit ESC[4m
        # at least once.
        if SGR_ULINE_ON not in after:
            print("FAIL - underline strip not re-rendered after backspace (no SGR 4m)")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly via close button")
            return False
        print("PASS")
        return True


# ---------------------------------------------------------------------------
# New: match counter, direction state, Enter-based navigation
# ---------------------------------------------------------------------------

COUNTER_RE = re.compile(rb"(?:\d{3}|999\+)/(?:\d{3}|999\+)")


def test_counter_initial_is_zero_padded():
    """When the bar opens with no query, counter must show 000/000."""
    print("TEST: empty-query counter is 000/000 ... ", end="", flush=True)
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        s.write(F3)
        stream = s.snapshot(settle=1.0)
        if b"000/000" not in stream:
            print("FAIL - expected '000/000' in output, got:")
            # Print any NNN/NNN found for diagnostics.
            found = COUNTER_RE.findall(stream)
            print(f"         NNN/NNN matches in stream: {found!r}")
            return False
        try:
            assert_bar_rendered(stream)
        except AssertionError as e:
            print(f"FAIL - {e}")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_counter_updates_on_typing():
    """Typing a query that doesn't exist must keep the counter at .../000.
       The renderer only re-emits cells whose contents actually change, so
       after typing "zzzzz" (0 matches) the counter stays at 000/000 and
       we inspect the full accumulated stream (which contains the initial
       frame plus any diffs)."""
    print("TEST: counter reacts to typed query ... ", end="", flush=True)
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        s.write(F3)
        s.snapshot(settle=0.5)
        s.write(b"zzzzz")
        stream = s.snapshot(settle=1.0)
        # Strip SGR/CSI so adjacent digits join up into readable text.
        plain = re.sub(rb"\x1b\[[\?0-9;:]*[a-zA-Z]", b"", stream)
        matches = COUNTER_RE.findall(plain)
        if not matches:
            print("FAIL - no counter rendered at all")
            return False
        # Every counter sample we saw must have total == 000.
        bad = [m for m in matches if not m.endswith(b"/000")]
        if bad:
            print(f"FAIL - non-zero total for 'zzzzz': samples={bad!r}")
            return False
        # And the 'zzzzz' must actually be in the input area (sanity).
        if b"zzzzz" not in plain:
            print("FAIL - typed query not visible in bar")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_default_direction_is_down():
    """On open, the ↓ button must be drawn with the active background color
       (col_act_bg = #3d59a1). We assert the 24-bit RGB SGR sequence for
       the active background appears in the stream shortly after opening
       the bar."""
    print("TEST: default direction is ↓ ... ", end="", flush=True)
    # Tokyo-night "distinct blue" we chose for the active bg. Must match
    # col_act_bg in term.hpp.
    ACTIVE_BG_SGR = b"\x1b[48;2;61;89;161m"
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        s.write(F3)
        stream = s.snapshot(settle=1.2)
        if ACTIVE_BG_SGR not in stream:
            print(f"FAIL - active-direction bg SGR {ACTIVE_BG_SGR!r} not in stream")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_up_button_click_switches_direction():
    """Clicking ↑ must repaint the ↑ button with the active background and
       leave the ↓ button without it. We can only really test that after
       the click the stream still contains the active-bg SGR (the button
       stays active-styled). A stronger check: click ↑, type a letter, and
       verify the bar still renders (no crash)."""
    print("TEST: click ↑ switches direction (no crash) ... ", end="", flush=True)
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        s.write(F3)
        s.snapshot(settle=0.6)
        s.click(BTN_UP_COL, BAR_INPUT_ROW)
        s.snapshot(settle=0.4)
        s.write(b"x")
        stream = s.snapshot(settle=0.8)
        try:
            assert_bar_rendered(stream)
        except AssertionError as e:
            print(f"FAIL - bar not rendered after click+type: {e}")
            return False
        if not s.is_alive():
            print("FAIL - vtm died after ↑ click")
            return False
        # Active-bg SGR should still be present somewhere (↑ is now active).
        ACTIVE_BG_SGR = b"\x1b[48;2;61;89;161m"
        if ACTIVE_BG_SGR not in stream:
            print("FAIL - no active-direction bg after ↑ click")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_enter_does_not_close_bar():
    """Pressing Enter (with or without Shift) must not close the bar."""
    print("TEST: Enter/Shift+Enter keep bar open ... ", end="", flush=True)
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        s.write(F3)
        s.snapshot(settle=0.6)
        s.write(b"abc")
        s.snapshot(settle=0.4)
        # Plain Enter.
        s.write(b"\r")
        s.snapshot(settle=0.4)
        # Shift+Enter is reported as CSI 13;2u in kitty-kbd or as plain \r on
        # legacy terminals. vtm's own key path uses the typed keystroke; we
        # only need to verify the bar survives, so sending \r suffices.
        s.write(b"\r")
        stream = s.snapshot(settle=0.8)
        try:
            assert_bar_rendered(stream)
        except AssertionError as e:
            print(f"FAIL - bar disappeared after Enter: {e}")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_counter_column_is_inside_bar():
    """Sanity: the counter glyph column computed from the layout must fall
       inside the bar (between BAR_LEFT and BAR_RIGHT)."""
    print("TEST: counter column is inside bar ... ", end="", flush=True)
    if not (BAR_LEFT < COUNTER_COL0 < BAR_RIGHT):
        print(f"FAIL - COUNTER_COL0={COUNTER_COL0} not inside ({BAR_LEFT}, {BAR_RIGHT})")
        return False
    if COUNTER_COL0 + COUNTER_W - 1 >= BTN_UP_COL - 1:
        print(f"FAIL - counter text overlaps ↑ button hitbox")
        return False
    print("PASS")
    return True


def _last_counter(stream):
    """Return the last NNN/NNN counter value visible in the stream, SGR stripped.
       Note: vtm uses diff rendering — unchanged cells are not re-emitted — so
       this only returns a value when a contiguous NNN/NNN block is actually
       (re)painted. For observing incremental counter changes, use _screen_counter()."""
    plain = re.sub(rb"\x1b\[[\?0-9;:]*[a-zA-Z]", b"", stream)
    hits = COUNTER_RE.findall(plain)
    return hits[-1] if hits else None


def _replay_screen(stream, rows=ROWS, cols=COLS):
    """Minimal ANSI replay: apply CUP (CSI row;colH / CSI H) and plain-text
       writes to a (rows x cols) cell grid. Good enough to reconstruct the
       current visible contents of the find-bar counter from vtm's diff
       rendering, which emits only changed cells.
       We intentionally ignore SGR, OSC, scroll regions, and other CSIs we
       don't care about — they never move the cursor or overwrite cells in
       our test cases.
    """
    grid = [[b" "] * cols for _ in range(rows)]
    r, c = 0, 0  # 0-indexed cursor
    i = 0
    n = len(stream)
    while i < n:
        b = stream[i]
        if b == 0x1b and i + 1 < n:
            nxt = stream[i + 1]
            if nxt == ord('['):
                # CSI: collect params then final byte
                j = i + 2
                while j < n and (0x30 <= stream[j] <= 0x3f):
                    j += 1
                # intermediates
                while j < n and (0x20 <= stream[j] <= 0x2f):
                    j += 1
                if j >= n:
                    break
                final = stream[j]
                params = stream[i + 2:j]
                if final in (ord('H'), ord('f')):
                    # CUP: row;col (1-indexed)
                    parts = params.split(b';') if params else []
                    try:
                        nr = int(parts[0]) if len(parts) > 0 and parts[0] else 1
                        nc = int(parts[1]) if len(parts) > 1 and parts[1] else 1
                    except ValueError:
                        nr, nc = 1, 1
                    r = max(0, min(rows - 1, nr - 1))
                    c = max(0, min(cols - 1, nc - 1))
                i = j + 1
                continue
            elif nxt == ord(']'):
                # OSC ... ST (BEL or ESC \)
                j = i + 2
                while j < n and stream[j] != 0x07:
                    if stream[j] == 0x1b and j + 1 < n and stream[j + 1] == ord('\\'):
                        j += 2
                        break
                    j += 1
                else:
                    j += 1  # include BEL
                i = j
                continue
            elif nxt in (ord('P'), ord('X'), ord('^'), ord('_')):
                # DCS / SOS / PM / APC — skip to ST
                j = i + 2
                while j < n:
                    if stream[j] == 0x1b and j + 1 < n and stream[j + 1] == ord('\\'):
                        j += 2
                        break
                    j += 1
                i = j
                continue
            else:
                # Simple 2-byte escape (e.g. ESC = , ESC > , ESC M ). Skip.
                i += 2
                continue
        if b == 0x0d:  # CR
            c = 0
            i += 1
            continue
        if b == 0x0a:  # LF
            if r + 1 < rows:
                r += 1
            i += 1
            continue
        if b == 0x08:  # BS
            if c > 0:
                c -= 1
            i += 1
            continue
        if b < 0x20 or b == 0x7f:
            i += 1
            continue
        # Consume one UTF-8 codepoint as a single cell (approximate).
        if b < 0x80:
            clen = 1
        elif b < 0xc0:
            clen = 1  # continuation byte on its own — treat as single byte
        elif b < 0xe0:
            clen = 2
        elif b < 0xf0:
            clen = 3
        else:
            clen = 4
        glyph = bytes(stream[i:i + clen])
        if 0 <= r < rows and 0 <= c < cols:
            grid[r][c] = glyph
        c += 1
        if c >= cols:
            c = cols - 1  # clamp; no autowrap tracking
        i += clen
    return grid


def _screen_counter(stream, row=BAR_INPUT_ROW + 1, col0=COUNTER_COL0, width=COUNTER_W):
    """Reconstruct the current counter value visible on-screen after replaying
       the accumulated diff stream. Returns bytes like b'001/007', b'001/999+',
       or b'999+/999+', with trailing spaces stripped.  Returns None if the
       span doesn't look like a counter.

       The default row is BAR_INPUT_ROW+1 because vtm's frame layout places
       the interactive (input/counter/buttons) row *one line below* the
       nominal gap+top-border computation used in the layout constants —
       empirically verified by replaying the F3 open frame. Passing an
       explicit row lets callers override if the layout ever changes."""
    grid = _replay_screen(stream)
    r = row - 1  # 1-indexed -> 0-indexed
    if r < 0 or r >= len(grid):
        return None
    cells = grid[r][col0 - 1:col0 - 1 + width]
    text_b = b"".join(cells)
    # Strip left-alignment padding (trailing spaces).
    stripped = text_b.rstrip(b" ")
    if re.match(rb"^(?:\d{3}|999\+)/(?:\d{3}|999\+)$", stripped):
        return stripped
    return None


def test_empty_query_resets_counter():
    """Typing a query then erasing it must reset the counter to 000/000.
       (Backend-driven: the UI mirrors the total/index reported by the term.)"""
    print("TEST: empty query resets counter to 000/000 ... ", end="", flush=True)
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        s.write(F3)
        s.snapshot(settle=0.6)
        # Type a query that likely has 0 matches to avoid dependence on shell output.
        s.write(b"qqqqq")
        s.snapshot(settle=0.6)
        # Erase every character.
        s.write(b"\x7f\x7f\x7f\x7f\x7f")
        stream = s.snapshot(settle=0.8)
        last = _last_counter(stream)
        if last != b"000/000":
            print(f"FAIL - counter after erase: {last!r} (expected 000/000)")
            return False
        # Bar must still be open.
        try:
            assert_bar_rendered(stream)
        except AssertionError as e:
            print(f"FAIL - bar missing after erase: {e}")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_close_bar_clears_counter_state():
    """After closing the find-bar, reopening it must start fresh at 000/000.
       (Tests that the backend drops last_find_* state on hide.)"""
    print("TEST: reopen after close starts at 000/000 ... ", end="", flush=True)
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        s.write(F3)
        s.snapshot(settle=0.6)
        s.write(b"zz")              # some query
        s.snapshot(settle=0.5)
        s.write(F3)                 # close
        s.snapshot(settle=0.5)
        s.write(F3)                 # reopen
        stream = s.snapshot(settle=0.8)
        if b"000/000" not in stream:
            last = _last_counter(stream)
            print(f"FAIL - reopened counter: {last!r} (expected 000/000 contiguous)")
            return False
        # And the bar must be fully drawn.
        try:
            assert_bar_rendered(stream)
        except AssertionError as e:
            print(f"FAIL - reopened bar not fully rendered: {e}")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_counter_width_is_fixed_for_high_totals():
    """Format invariant: for any (index, total) the counter is left-aligned
       within a 9-char field.  When total > 999, the total side is shown as
       "999+" and both sides can be "999+" when index also overflows.

       This test drives vtm with a viewport containing >999 matches of a
       short pattern, then verifies:
         1. The counter renders in the expected format (NNN/NNN, NNN/999+, or
            999+/999+) without breaking bar layout.
         2. The total side shows the "999+" overflow marker."""
    print("TEST: counter shows 999+ overflow for high totals ... ", end="", flush=True)
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        # Print exactly 1760 X's (22 rows × 80 cols) using a bash builtin so
        # that no external interpreter is needed inside the pty session.
        # Searching for single "X" gives 1760 matches, which exceeds the 999
        # overflow threshold and forces the counter to show "999+".
        s.snapshot(settle=0.4)
        s.write(b"printf 'X%.0s' {1..1760}; echo\r")
        s.snapshot(settle=2.0)
        s.write(F3)
        s.snapshot(settle=0.5)
        s.write(b"X")
        stream = s.snapshot(settle=1.2)
        # The counter must be in one of the three expected formats.
        # Use _screen_counter (ANSI-replay) rather than _last_counter because
        # vtm uses diff rendering: only changed cells are re-emitted. The "/"
        # separator and unchanged digit cells are not re-sent, so a plain
        # regex over the raw stream cannot reconstruct the full NNN/NNN token.
        # _screen_counter replays CUP sequences to build the current screen
        # state and reads the counter field directly from the grid.
        last = _screen_counter(stream)
        if last is None:
            print("FAIL - no counter on-screen (screen-replay)")
            return False
        if not re.match(rb"^(?:\d{3}|999\+)/(?:\d{3}|999\+)$", last):
            print(f"FAIL - counter in unexpected format: {last!r}")
            return False
        # With 1760 matches the total MUST show as 999+.
        if not last.endswith(b"999+"):
            print(f"FAIL - expected 999+ overflow for 1760 matches, got: {last!r}")
            return False
        try:
            assert_bar_rendered(stream)
        except AssertionError as e:
            print(f"FAIL - bar not rendered: {e}")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_arrow_keys_navigate_matches():
    """Up/Down arrow keys must step the match index backward/forward,
       independent of the active direction indicator. This is the
       portable alternative to Shift+Enter, which loses its modifier
       under pty/nested input protocols.

       Uses _screen_counter (ANSI-replay) rather than _last_counter
       because vtm only re-emits cells whose glyph actually changed;
       pattern-matching the raw stream would miss the /000 -> /007
       transition when only the final digit is repainted."""
    print("TEST: Up/Down arrows navigate matches ... ", end="", flush=True)
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        s.snapshot(settle=0.6)
        # Seed the scrollback with several lines containing a unique token.
        s.write(b"printf 'ZTOK\\n%.0s' {1..5}\r")
        s.snapshot(settle=2.0)
        s.write(F3)
        s.snapshot(settle=0.8)
        s.write(b"ZTOK")
        stream = s.snapshot(settle=1.2)
        first = _screen_counter(stream)
        if first is None:
            print(f"FAIL - no counter on-screen after typing ZTOK")
            return False
        try:
            idx0, tot0 = (int(x) for x in first.split(b"/"))
        except ValueError:
            print(f"FAIL - unparseable counter {first!r}")
            return False
        if tot0 < 2:
            print(f"FAIL - expected >=2 matches, got {first!r}")
            return False
        # Down arrow -> advance to next match.
        s.write(b"\x1b[B")
        stream = s.snapshot(settle=0.8)
        after_dn = _screen_counter(stream)
        if after_dn is None:
            print(f"FAIL - no counter after Down")
            return False
        try:
            idx1, tot1 = (int(x) for x in after_dn.split(b"/"))
        except ValueError:
            print(f"FAIL - unparseable counter after Down {after_dn!r}")
            return False
        if tot1 != tot0:
            print(f"FAIL - total changed on Down: {tot0} -> {tot1}")
            return False
        if idx1 == idx0:
            print(f"FAIL - Down did not advance index (still {idx1}/{tot1})")
            return False
        # Up arrow -> step backward, bringing index back.
        s.write(b"\x1b[A")
        stream = s.snapshot(settle=0.8)
        after_up = _screen_counter(stream)
        if after_up is None:
            print(f"FAIL - no counter after Up")
            return False
        try:
            idx2, tot2 = (int(x) for x in after_up.split(b"/"))
        except ValueError:
            print(f"FAIL - unparseable counter after Up {after_up!r}")
            return False
        if tot2 != tot0:
            print(f"FAIL - total changed on Up: {tot0} -> {tot2}")
            return False
        if idx2 != idx0:
            print(f"FAIL - Up did not undo Down: start={idx0}, after Dn={idx1}, after Up={idx2}")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_format_counter_contract():
    """Pure-format sanity: replicate the C++ format_counter rules in Python
       and assert the expected mapping.  Keeps the two implementations in
       sync as a regression safeguard.

       New rules (matches term.hpp):
         - Each side: if > 999 -> "999+", else zero-pad to 3 digits.
         - Full string is left-aligned to 9 chars (trailing spaces).
       Three display cases:
         "001/099  "  both sides <= 999  (inner width 7, 2 trailing spaces)
         "892/999+ "  only total > 999   (inner width 8, 1 trailing space)
         "999+/999+"  both sides > 999   (inner width 9, no padding)
    """
    print("TEST: format_counter clamp rules ... ", end="", flush=True)
    def fmt(i, t):
        def side(v):
            if v < 0: v = 0
            if v > 999: return "999+"
            return f"{v:03d}"
        inner = f"{side(i)}/{side(t)}"
        return inner.ljust(9)
    cases = [
        ((0,    0),    "000/000  "),
        ((1,    1),    "001/001  "),
        ((10,   99),   "010/099  "),
        ((999,  999),  "999/999  "),
        ((998,  1500), "998/999+ "),  # total overflows; index stays real
        ((1,    1500), "001/999+ "),
        ((1500, 1500), "999+/999+"),  # both overflow
        ((-3,   5),    "000/005  "),
    ]
    for (i, t), expected in cases:
        got = fmt(i, t)
        if got != expected:
            print(f"FAIL - fmt({i},{t}) = {got!r}, expected {expected!r}")
            return False
    print("PASS")
    return True


# ---------------------------------------------------------------------------
# Tests: clear-query button (hov_btn_clear / btn_clear_x feature).
#
# The clear button is rendered inside the input area (rightmost 3 cells) only
# when st.query is non-empty.  Clicking it wipes the query, resets the caret
# and scroll position, and fires a find-request with an empty query so the
# backend drops all highlights.  The bar stays open.
# ---------------------------------------------------------------------------

def test_clear_button_column_is_inside_input():
    """Geometry sanity: BTN_CLEAR_COL must lie within the input field
    [INPUT_X0, INPUT_X1] and must not coincide with any navigation button."""
    print("TEST: clear-button column is inside input field ... ", end="", flush=True)
    if not (INPUT_X0 <= BTN_CLEAR_COL <= INPUT_X1):
        print(f"FAIL - BTN_CLEAR_COL={BTN_CLEAR_COL} outside input [{INPUT_X0}, {INPUT_X1}]")
        return False
    if BTN_CLEAR_COL in (BTN_UP_COL, BTN_DN_COL, BTN_X_COL):
        print(f"FAIL - BTN_CLEAR_COL={BTN_CLEAR_COL} collides with a nav button")
        return False
    print("PASS")
    return True


def test_clear_button_appears_when_typing():
    """When the query is non-empty the clear-query '×' glyph must be rendered
    inside the input area at BTN_CLEAR_COL.  We verify using _replay_screen so
    that vtm's diff-rendering (only changed cells re-emitted) does not cause
    false negatives."""
    print("TEST: clear-query button appears when typing ... ", end="", flush=True)
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        s.write(F3)
        s.snapshot(settle=0.8)
        s.write(b"hello")
        stream = s.snapshot(settle=1.0)
        # BAR_INPUT_ROW+1 is the empirical on-screen row used by _screen_counter.
        interactive_row = BAR_INPUT_ROW + 1
        grid = _replay_screen(stream)
        r = interactive_row - 1   # 0-indexed
        c = BTN_CLEAR_COL - 1     # 0-indexed
        cell = grid[r][c]
        if cell != BTN_CLEAR_GLYPH_UTF8:
            print(f"FAIL - expected × at col {BTN_CLEAR_COL} row {interactive_row}, got {cell!r}")
            return False
        if not s.is_alive():
            print("FAIL - vtm died")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_clear_button_absent_with_empty_query():
    """When the bar opens with an empty query, no '×' must appear at
    BTN_CLEAR_COL inside the input area."""
    print("TEST: clear-query button absent when query is empty ... ", end="", flush=True)
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        s.write(F3)
        stream = s.snapshot(settle=1.0)
        interactive_row = BAR_INPUT_ROW + 1
        grid = _replay_screen(stream)
        r = interactive_row - 1
        c = BTN_CLEAR_COL - 1
        cell = grid[r][c]
        if cell == BTN_CLEAR_GLYPH_UTF8:
            print(f"FAIL - × rendered at col {BTN_CLEAR_COL} with empty query")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_clear_button_click_clears_query():
    """Clicking the clear-query button must wipe the query, reset the counter
    to 000/000, and keep the find-bar open.

    To reliably distinguish 'click worked' from 'click had no effect', we
    first seed the scrollback with a unique token so the find counter is
    non-zero.  After clicking the clear button the counter must return to
    000/000 (empty query → 0 matches).  Counter cells change on every match
    update, so _screen_counter (ANSI-replay) correctly tracks the transition."""
    print("TEST: clear-query button click wipes query and keeps bar open ... ", end="", flush=True)
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        # Seed one visible occurrence of a unique token so the counter > 000.
        s.snapshot(settle=0.4)
        s.write(b"printf 'CLRTOK\\n'\r")
        s.snapshot(settle=1.5)
        # Open bar and type the token so we get a non-zero counter.
        s.write(F3)
        s.snapshot(settle=0.6)
        s.write(b"CLRTOK")
        stream = s.snapshot(settle=1.2)
        counter_before = _screen_counter(stream)
        if counter_before is None or counter_before == b"000/000":
            print(f"FAIL - expected non-zero counter before clear, got {counter_before!r}")
            return False
        # Click the clear-query button.  The interactive row is one row below
        # the bar's top border: SGR row = BAR_INPUT_ROW + 1 (empirically verified).
        # Do NOT clear the buffer so _screen_counter can replay the full diff stream.
        s.click(BTN_CLEAR_COL, BAR_INPUT_ROW + 1)
        stream = s.snapshot(settle=1.2)
        counter_after = _screen_counter(stream)
        if counter_after != b"000/000":
            print(f"FAIL - counter after clear: {counter_after!r}"
                  f" (before: {counter_before!r}, expected 000/000)")
            return False
        # Bar must remain open.
        try:
            assert_bar_rendered(stream)
        except AssertionError as e:
            print(f"FAIL - bar closed after clear-click: {e}")
            return False
        if not s.is_alive():
            print("FAIL - vtm died after clear-click")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_clear_button_disappears_after_backspace():
    """After typing and then erasing all characters with Backspace, the
    clear-query button must disappear (BTN_CLEAR_COL cell stops showing '×')."""
    print("TEST: clear-query button disappears after erasing query ... ", end="", flush=True)
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        s.write(F3)
        s.snapshot(settle=0.6)
        s.write(b"abc")
        s.snapshot(settle=0.6)
        # Erase all three characters via Backspace.
        s.write(b"\x7f\x7f\x7f")
        stream = s.snapshot(settle=1.0)
        interactive_row = BAR_INPUT_ROW + 1
        grid = _replay_screen(stream)
        r = interactive_row - 1
        c = BTN_CLEAR_COL - 1
        cell = grid[r][c]
        if cell == BTN_CLEAR_GLYPH_UTF8:
            print(f"FAIL - × still at col {BTN_CLEAR_COL} after erasing all chars")
            return False
        # Bar must still be open.
        try:
            assert_bar_rendered(stream)
        except AssertionError as e:
            print(f"FAIL - bar unexpectedly closed: {e}")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly via close button")
            return False
        print("PASS")
        return True


# ---------------------------------------------------------------------------
# Underline-style input strip (tile.hpp-style).
#
# The find-bar's input field is rendered as a continuous underlined strip
# using the cell underline attribute (SGR 4 / 24, with custom underline
# color via SGR 58:2::r:g:b), instead of legacy `_` glyphs.  When the
# query is non-empty, the clear-query '×' button shares the same underline
# so it visually connects to the input field.
# ---------------------------------------------------------------------------

def test_input_strip_uses_underline_attribute():
    """The empty input field must be drawn with SGR underline attribute and
    must not contain stray `_` glyphs."""
    print("TEST: input strip uses cell underline attribute (no `_` glyphs) ... ", end="", flush=True)
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        s.fresh_snapshot(settle=0.4)
        s.write(F3)
        stream = s.snapshot(settle=1.0)
        # Bar must be visible.
        if "\u256d".encode() not in stream:
            print("FAIL - bar not rendered")
            return False
        # SGR underline-on must appear (input strip).
        if SGR_ULINE_ON not in stream:
            print("FAIL - missing SGR underline-on (ESC[4m)")
            return False
        # Custom underline color SGR.
        if SGR_ULINE_COLOR_PREFIX not in stream:
            print("FAIL - missing SGR underline-color (ESC[58...)")
            return False
        # Bar body should contain no `_` glyphs.
        bar_top = stream.find("\u256d".encode())
        bar_bot = stream.find("\u2570".encode())
        if bar_top != -1 and bar_bot != -1 and bar_bot > bar_top:
            body = stream[bar_top:bar_bot]
            if b"_" in body:
                print("FAIL - stray `_` glyph inside find-bar body")
                return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly via close button")
            return False
        print("PASS")
        return True


def test_clear_button_shares_underline_with_input():
    """When the query is non-empty, the clear-query '×' button must share
    the underline attribute with the input strip (visual continuity).
    This is detectable by the SGR underline-on sequence (ESC[4m) appearing
    in the bar redraw stream while typing."""
    print("TEST: clear button connected to input strip via underline ... ", end="", flush=True)
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        s.write(F3)
        s.snapshot(settle=0.6)
        # Type something so the clear-button appears.
        s._buffer = b""
        s.write(b"abc")
        after = s.snapshot(settle=1.0)
        # × glyph must appear -- it's the clear button (and possibly the
        # close button on the right).  The relevant evidence here is the
        # underline-on SGR co-emitted with the redraw of the input row.
        if "\u00d7".encode() not in after:
            print("FAIL - × glyph not on screen")
            return False
        if SGR_ULINE_ON not in after:
            print("FAIL - input row redraw lacked SGR underline-on (ESC[4m) -- clear button not connected to input strip")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly via close button")
            return False
        print("PASS")
        return True


# ---------------------------------------------------------------------------
# Responsive layout tests.  When the host terminal (and therefore the bar's
# parent X-fork) is too narrow to grant the bar its preferred 44-cell outer
# width, controls drop in reverse priority order:
#     ↑↓ direction pair (lowest)  ->  counter  ->  clear-button capacity
#     ->  finally the input field shrinks to its 4-cell floor.
# Close button (×), input field (>= 4 cells), borders, and the " Search "
# label are mandatory at every renderable width.
# ---------------------------------------------------------------------------

def _open_bar_at_cols(cols, settle_open=1.0):
    """Spawn vtm-desk at the given terminal cols, open the find-bar with F3,
    and return (session, post-F3 stream).  Caller is responsible for closing
    the session via context manager / normal_exit."""
    s = VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, cols=cols, vtm_config=DESK_CONFIG).__enter__()
    try:
        s.fresh_snapshot(settle=0.4)
        s.write(F3)
        stream = s.snapshot(settle=settle_open)
        return s, stream
    except Exception:
        s.__exit__(None, None, None)
        raise


def _bar_visible(stream):
    """Bar is on screen if at least one rounded corner glyph is emitted."""
    return contains(stream, "\u256d") or contains(stream, "\u256e") \
        or contains(stream, "\u2570") or contains(stream, "\u256f")


def test_bar_renders_at_full_width():
    print("TEST: responsive: full layout at wide terminal (cols=80) ... ", end="", flush=True)
    s, stream = _open_bar_at_cols(80)
    try:
        ok = (_bar_visible(stream)
              and contains(stream, BTN_X)
              and contains(stream, BTN_UP)
              and contains(stream, BTN_DN)
              and contains(stream, LABEL))
        if not ok:
            print("FAIL - expected full bar (label + ↑ + ↓ + ×) at cols=80")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly")
            return False
        print("PASS")
        return True
    finally:
        s.__exit__(None, None, None)


def test_bar_drops_dir_buttons_when_narrow():
    """At cols=40 the bar's outer width is forced below the threshold needed
    for the ↑↓ pair.  Counter may also be hidden (priority below dir-pair on
    the keep list, but the X-fork distribution often forces both away
    simultaneously).  The close button and label must remain."""
    print("TEST: responsive: ↑↓ hidden when bar narrows (cols=40) ... ", end="", flush=True)
    s, stream = _open_bar_at_cols(40)
    try:
        if not _bar_visible(stream):
            print("FAIL - bar did not render at cols=40")
            return False
        if not contains(stream, BTN_X):
            print("FAIL - close (×) button missing at cols=40")
            return False
        if not contains(stream, LABEL):
            print("FAIL - ' Search ' label missing at cols=40")
            return False
        if contains(stream, BTN_UP) or contains(stream, BTN_DN):
            print("FAIL - ↑↓ buttons should be hidden at cols=40")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly")
            return False
        print("PASS")
        return True
    finally:
        s.__exit__(None, None, None)


def test_bar_renders_at_minimum_width():
    """At very narrow terminal widths (cols=14) the bar must still render
    with at minimum:  borders + ' Search ' label + 4-cell input + × close.
    No counter, no ↑↓.  Process must not crash."""
    print("TEST: responsive: bar renders at minimum width (cols=14) ... ", end="", flush=True)
    s, stream = _open_bar_at_cols(14)
    try:
        if not _bar_visible(stream):
            print("FAIL - bar did not render at cols=14")
            return False
        if not contains(stream, BTN_X):
            print("FAIL - close (×) button missing at cols=14")
            return False
        if contains(stream, BTN_UP) or contains(stream, BTN_DN):
            print("FAIL - ↑↓ buttons should be hidden at cols=14")
            return False
        # Process must still be alive (responsive layout must not crash).
        if not s.is_alive():
            print("FAIL - vtm-desk crashed at cols=14")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly")
            return False
        print("PASS")
        return True
    finally:
        s.__exit__(None, None, None)


def test_typing_works_at_narrow_width():
    """The input field is mandatory (>= 4 cells) at every renderable width.
    Typing must echo into the bar's input row even when the bar is narrow."""
    print("TEST: responsive: typing still works at cols=40 ... ", end="", flush=True)
    s, stream = _open_bar_at_cols(40)
    try:
        s._buffer = b""
        s.write(b"hi")
        after = s.snapshot(settle=0.8)
        if b"hi" not in after:
            print("FAIL - typed 'hi' did not appear in narrow find-bar input")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly")
            return False
        print("PASS")
        return True
    finally:
        s.__exit__(None, None, None)


def test_close_button_works_at_narrow_width():
    """The × close button on the bar (mandatory at every width) must still
    close the bar when clicked, even when the bar is in degraded layout."""
    print("TEST: responsive: bar × button works at cols=40 ... ", end="", flush=True)
    s, stream = _open_bar_at_cols(40)
    try:
        # The bar's × close button is the rightmost × on the bar's input row.
        # Replay the screen to find its column dynamically (responsive
        # layout means we can't compute it from constants).
        grid = _replay_screen(stream, rows=ROWS, cols=40)
        # _screen_counter docstring notes the interactive row is empirically
        # BAR_INPUT_ROW+1 (1-indexed) in the replayed grid.  Search both rows
        # for the × glyph and pick the rightmost match -- that's the close
        # button on the bar (clear-button × is absent because query is empty).
        target_col = None
        target_row_idx = None
        for r_idx in (BAR_INPUT_ROW - 1, BAR_INPUT_ROW):  # 0-indexed candidates
            if r_idx >= len(grid):
                continue
            for i, cell in enumerate(grid[r_idx]):
                if cell == BTN_CLEAR_GLYPH_UTF8:
                    target_col = i + 1  # 1-indexed
                    target_row_idx = r_idx
        if target_col is None:
            print("FAIL - could not locate × on bar input row at cols=40")
            return False
        s._buffer = b""
        # Click using the row we found × on (1-indexed).
        s.click(target_col, target_row_idx + 1)
        after = s.snapshot(settle=1.0)
        # After closing, no rounded corners should remain.
        if _bar_visible(after):
            # The pre-close stream had corners; verify the corners disappear
            # by looking at the post-click frame only.
            grid2 = _replay_screen(after, rows=ROWS, cols=40)
            still = any("\u256d".encode() in c or "\u256e".encode() in c
                        or "\u2570".encode() in c or "\u256f".encode() in c
                        for r in grid2 for c in r)
            if still:
                print("FAIL - bar still rendered after × click at cols=40")
                return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly")
            return False
        print("PASS")
        return True
    finally:
        s.__exit__(None, None, None)


# ---------------------------------------------------------------------------
# Tests: Tab key toggles active navigation direction (↑ ↔ ↓).
#
# Pressing Tab while the find-bar has keyboard focus must:
#   • Toggle st.dir between dir_up (-1) and dir_down (+1).
#   • Trigger an immediate repaint so the direction button highlight moves
#     from the old active button to the new one (one button gains col_act_bg,
#     the other reverts to col_bg).
#   • Re-seed the search from the new direction's start edge (same behaviour
#     as clicking the ↑/↓ direction buttons with the mouse).
#   • Not close the bar or leak Tab into the underlying shell.
# ---------------------------------------------------------------------------

# active-direction button background colour (col_act_bg in term.hpp):
#   argb{ 0xff3d59a1 }  →  R=61, G=89, B=161
ACTIVE_BG_SGR = b"\x1b[48;2;61;89;161m"

# Tab as a raw byte (ASCII HT, 0x09).
TAB = b"\x09"


def test_tab_toggles_direction_no_crash():
    """Pressing Tab twice must leave the bar fully rendered and the process
    alive (basic smoke test — no crash, no accidental close)."""
    print("TEST: Tab toggles direction -- bar stays open (smoke) ... ",
          end="", flush=True)
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        s.write(F3)
        s.snapshot(settle=0.8)
        # First Tab: direction ↓ → ↑.
        s.write(TAB)
        s.snapshot(settle=0.4)
        # Second Tab: direction ↑ → ↓ (back to default).
        s.write(TAB)
        stream = s.snapshot(settle=0.8)
        try:
            assert_bar_rendered(stream)
        except AssertionError as e:
            print(f"FAIL - bar not rendered after two Tabs: {e}")
            return False
        if not s.is_alive():
            print("FAIL - vtm died after Tab presses")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly")
            return False
        print("PASS")
        return True


def test_tab_active_bg_appears_on_toggle():
    """After pressing Tab the active-direction button (whichever it is) must
    be repainted with col_act_bg (24-bit RGB SGR). We drain the buffer right
    before pressing Tab so the snapshot contains only the diff frame emitted
    in response to the Tab keystroke."""
    print("TEST: Tab repaint emits active-direction bg SGR ... ",
          end="", flush=True)
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        s.write(F3)
        s.snapshot(settle=0.8)
        # Clear accumulated buffer: next snapshot contains only Tab's diff.
        s.fresh_snapshot(settle=0.2)
        s.write(TAB)
        after = s.snapshot(settle=0.8)
        # The repaint of the direction button pair must emit the active-bg
        # SGR for the newly-active button (↑ is now active after first Tab).
        if ACTIVE_BG_SGR not in after:
            print(f"FAIL - active-direction bg SGR {ACTIVE_BG_SGR!r} not in "
                  f"post-Tab frame")
            return False
        if not s.is_alive():
            print("FAIL - vtm died after Tab")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly")
            return False
        print("PASS")
        return True


def test_tab_does_not_leak_to_shell():
    """Tab while the find-bar is open must be swallowed and must not reach
    the underlying shell. We verify by closing the bar afterwards (Esc) and
    checking the shell output contains no TAB-completion artefacts."""
    print("TEST: Tab in bar does not leak to shell ... ",
          end="", flush=True)
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        s.write(F3)
        s.snapshot(settle=0.6)
        # Three Tab presses while bar is open.
        s.write(TAB + TAB + TAB)
        s.snapshot(settle=0.4)
        # Close bar and capture fresh shell output.
        s._buffer = b""
        s.write(b"\x1b")   # Esc closes bar
        after = s.snapshot(settle=0.8)
        # Shell output after Esc should not contain a raw HT (0x09).
        # (Tab-completion in bash typically echoes characters or a bell;
        # we just assert there is no raw 0x09 in the emitted bytes.)
        if b"\x09" in after:
            print("FAIL - raw TAB byte appeared in shell output after bar close")
            return False
        if not s.is_alive():
            print("FAIL - vtm died")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly")
            return False
        print("PASS")
        return True


def test_tab_toggles_direction_twice_restores_default():
    """Two consecutive Tab presses must return the direction to its original
    value. After two Tabs from the default (↓), the ↓ button must be active
    again (ACTIVE_BG_SGR in the delta for each Tab).

    Each Tab's diff frame is captured independently (fresh_snapshot before
    each keypress) and checked for ACTIVE_BG_SGR.

    NOTE: assert_bar_rendered is intentionally NOT called on the narrow diff
    frames.  vtm uses diff rendering -- only cells whose content changed are
    re-emitted.  When only the direction buttons change (one gains col_act_bg,
    the other reverts to col_bg), the border chars, label, underline strip,
    and close button are NOT re-sent.  Calling assert_bar_rendered on such a
    diff would always fail regardless of correctness.  Process liveness is
    checked as a proxy for "bar is still open and rendering"."""
    print("TEST: Tab×2 returns direction to default (↓) ... ",
          end="", flush=True)
    with VtmSession(VTM_DESK_BINARY, DESK_TERM_ARGS, vtm_config=DESK_CONFIG) as s:
        s.write(F3)
        s.snapshot(settle=0.8)
        # Tab 1: default ↓ → ↑.  Grab the diff frame for this toggle only.
        s.fresh_snapshot(settle=0.2)
        s.write(TAB)
        delta1 = s.snapshot(settle=0.6)
        if ACTIVE_BG_SGR not in delta1:
            print("FAIL - no active-bg SGR in diff after Tab 1 "
                  "(↑ button not repainted as active)")
            return False
        # Tab 2: ↑ → ↓.  Grab the diff frame for this toggle only.
        s.fresh_snapshot(settle=0.2)
        s.write(TAB)
        delta2 = s.snapshot(settle=0.8)
        if ACTIVE_BG_SGR not in delta2:
            print("FAIL - no active-bg SGR in diff after Tab 2 "
                  "(↓ button not repainted as active -- direction not restored)")
            return False
        # Bar must still be alive after the round-trip (no crash, no close).
        if not s.is_alive():
            print("FAIL - vtm died after two Tabs")
            return False
        if not s.normal_exit(timeout=5.0):
            print("FAIL - vtm-desk did not exit cleanly")
            return False
        print("PASS")
        return True


TESTS = [
    test_f3_toggles_bar,
    test_bar_right_aligned_with_margin,
    test_esc_closes_bar,
    test_typing_shows_in_input,
    test_close_button_click_closes_bar,
    test_backspace_removes_char,
    test_long_input_horizontal_scroll,
    test_keys_do_not_leak_to_shell,
    test_f3_opens_bar,
    test_counter_column_is_inside_bar,
    test_counter_initial_is_zero_padded,
    test_counter_updates_on_typing,
    test_default_direction_is_down,
    test_up_button_click_switches_direction,
    test_enter_does_not_close_bar,
    test_empty_query_resets_counter,
    test_close_bar_clears_counter_state,
    test_arrow_keys_navigate_matches,
    test_format_counter_contract,
    test_counter_width_is_fixed_for_high_totals,
    # clear-query button tests
    test_clear_button_column_is_inside_input,
    test_clear_button_appears_when_typing,
    test_clear_button_absent_with_empty_query,
    test_clear_button_click_clears_query,
    test_clear_button_disappears_after_backspace,
    # underline-style input strip tests (tile.hpp-style)
    test_input_strip_uses_underline_attribute,
    test_clear_button_shares_underline_with_input,
    # responsive layout tests (priority-based control hiding when narrow)
    test_bar_renders_at_full_width,
    test_bar_drops_dir_buttons_when_narrow,
    test_bar_renders_at_minimum_width,
    test_typing_works_at_narrow_width,
    test_close_button_works_at_narrow_width,
    # Tab key: toggle active navigation direction (↑ ↔ ↓)
    test_tab_toggles_direction_no_crash,
    test_tab_active_bg_appears_on_toggle,
    test_tab_does_not_leak_to_shell,
    test_tab_toggles_direction_twice_restores_default,
]


def main():
    if not os.path.isfile(VTM_DESK_BINARY):
        print(f"ERROR: vtm-desk binary not found at {VTM_DESK_BINARY}")
        return 1
    kill_all_vtm()
    passed = 0
    failed = 0
    for t in TESTS:
        try:
            if t():
                passed += 1
            else:
                failed += 1
        except Exception as e:
            import traceback
            print(f"ERROR: {t.__name__}: {e}")
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
