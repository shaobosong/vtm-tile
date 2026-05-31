#!/usr/bin/env python3
# Copyright (c) Dmitry Sapozhnikov
# Licensed under the MIT license.

"""
End-to-end TUI regression test for the menu item type="radiomenu" feature.

Verifies the contract added in application.hpp menu::open_dropdown_popup /
_attach_popup_overlay / load_item:

  1. The XML <menu><item type="radiomenu" ...><item ...>...</item></menu>
     schema is parsed by menu::load_item: the parent renders as a menu-bar
     trigger labelled with its own `label` (plus the dropdown chevron), and
     its nested <item> children are loaded as the radio-group rows.

  2. Left-clicking the trigger opens a popup anchored below it whose rows
     paint each child label prefixed with a radio bullet in the left
     gutter: ◉ (U+25C9) for the selected row, ○ (U+25CB) for the rest.

  3. The statically-selected row (the child marked checked="true" in the
     config) is the one painted filled (◉); every other row is painted
     hollow (○). The bullet sits in the left gutter, left of the label.

  4. Clicking a radio row dispatches its <script> and tears the chain down
     (re-clicking the trigger re-opens the popup, proving the chain was
     dismissed and the open-guard cleared — same contract as a dropdown
     leaf click).

The harness mirrors test_dropdown_menu.py: vtm-tile is driven via a pty
with SGR mouse + raw input, and the cumulative paint stream is scanned for
markers and glyphs.
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
import tempfile
import subprocess

VTM_TILE_BINARY = os.environ.get(
    "VTM_TILE_BINARY",
    os.path.join(os.path.dirname(__file__), "..", "build", "vtm-tile"),
)

COLS = 120
ROWS = 30
READ_TIMEOUT = 5.0
SETTLE_DELAY = 1.0

# Radio bullet glyphs painted in the popup's left gutter.
RADIO_FILLED = "◉"  # ◉  selected row
RADIO_HOLLOW = "○"  # ○  unselected row
RADIO_FILLED_B = RADIO_FILLED.encode()
RADIO_HOLLOW_B = RADIO_HOLLOW.encode()

# Distinctive markers so screen scans are unambiguous.
TRIGGER_LABEL = "  [RADIO]  "
OPT_A = "RadioOptA-MARK"   # checked="true": the statically selected option.
OPT_B = "RadioOptB-MARK"
OPT_C = "RadioOptC-MARK"

# Self-contained tile config exercising a top-level radiomenu trigger with
# three child options; the middle one is statically selected via
# checked="true". The children carry empty scripts (their luafx dispatch
# path needs terminal proxy plumbing covered elsewhere); our scope here is
# the radio-group rendering and the static-selection contract. The dtvt
# child clears its own menu (-c) so no native glyphs collide with markers.
RADIO_TILE_CONFIG = (
    "<config>"
        "<tile>"
            "<confirm_close=0/>"
            '<app selected="term">'
                "<item*/>"
                '<item id="term" label="term" type="dtvt"'
                ' cmd="$0 -c \'<config><terminal><menu item*></menu></terminal></config>\' -r term"/>'
            "</app>"
            "<menu item*>"
                f'<item type="radiomenu" label="{TRIGGER_LABEL}" tooltip=" radio " item*>'
                    f'<item label="{OPT_A}" tooltip=" a " script=\'OnLeftClick|\'/>'
                    f'<item label="{OPT_B}" checked="true" tooltip=" b " script=\'OnLeftClick|\'/>'
                    f'<item label="{OPT_C}" tooltip=" c " script=\'OnLeftClick|\'/>'
                "</item>"
            "</menu>"
        "</tile>"
    "</config>"
)
RADIO_TILE_ARGS = []  # RADIO_TILE_CONFIG is shipped via $VTM_CONFIG.


# ---------------------------------------------------------------------------
# Nested radiomenu (radiomenu as a row inside a dropdown popup).
# ---------------------------------------------------------------------------
# A radiomenu can be nested inside a plain dropdown: its row in the parent
# popup carries the submenu chevron, and opening it (click or hover) reveals
# the radio group to the right. The selected row is resolved when the submenu
# opens — for the STATIC case below, via the child marked checked="true"
# (no terminal scripting required), so the assertions stay deterministic.
NEST_TRIGGER = "  [EDIT]  "
NEST_RADIO_ROW = "ClipFmtRadio"   # radiomenu submenu-trigger label (contiguous).
NOPT_A = "NfmtDisabled-MARK"
NOPT_B = "NfmtPlain-MARK"          # checked="true": the selected option.
NOPT_C = "NfmtRich-MARK"
NESTED_TILE_CONFIG = (
    "<config>"
        "<tile>"
            "<confirm_close=0/>"
            '<app selected="term">'
                "<item*/>"
                '<item id="term" label="term" type="dtvt"'
                ' cmd="$0 -c \'<config><terminal><menu item*></menu></terminal></config>\' -r term"/>'
            "</app>"
            "<menu item*>"
                f'<item type="dropdown" label="{NEST_TRIGGER}" tooltip=" edit " item*>'
                    f'<item type="radiomenu" label="{NEST_RADIO_ROW}" tooltip=" fmt " item*>'
                        f'<item label="{NOPT_A}" tooltip=" a " script=\'OnLeftClick|\'/>'
                        f'<item label="{NOPT_B}" checked="true" tooltip=" b " script=\'OnLeftClick|\'/>'
                        f'<item label="{NOPT_C}" tooltip=" c " script=\'OnLeftClick|\'/>'
                    "</item>"
                "</item>"
            "</menu>"
        "</tile>"
    "</config>"
)
NESTED_TILE_ARGS = []  # NESTED_TILE_CONFIG is shipped via $VTM_CONFIG.


# ---------------------------------------------------------------------------
# Dynamic radiomenu wired to live terminal state (the headline use case).
# ---------------------------------------------------------------------------
# The radiomenu's <script> publishes the terminal's current clipboard format
# as the selected row via vtm.item.Check(vtm.terminal.ClipboardFormat()) — the
# same event-driven pattern the ClipboardFormat button uses. For vtm.terminal
# to resolve, the menu must live in the TERMINAL process, so this config is
# loaded by the dtvt child via `-c <file>` (a temp file, written per-test to
# avoid inline-quote escaping). The radiomenu is nested inside a dropdown to
# mirror the real-world layout; the bullet must track the live format and
# move when a different format row is chosen.
DYN_TRIGGER = "  [TERMMENU]  "
DYN_RADIO_ROW = "ClipboardFmt"
DYN_OPTS = ["Disabled", "PlainText", "AnsiText", "RichText", "HtmlText", "Protected"]


def _write_dyn_term_menu():
    """Write the terminal-process menu config (dropdown > radiomenu) to a temp
    file and return its path. Caller is responsible for deleting it."""
    rows = "".join(
        f'<item label="{lbl}" script=OnLeftClick|"vtm.terminal.ClipboardFormat({i})"/>'
        for i, lbl in enumerate(DYN_OPTS)
    )
    cfg = (
        "<config><terminal><menu item*>"
        f'<item type="dropdown" label="{DYN_TRIGGER}" item*>'
        f'<item type="radiomenu" label="{DYN_RADIO_ROW}" item*>'
        "<script>"
        '<on="release: e2::form::upon::started"  source="applet"/>'
        '<on="release: terminal::events::selmod" source="terminal"/>'
        "vtm.item.Check(vtm.terminal.ClipboardFormat())"
        "</script>"
        f"{rows}"
        "</item>"
        "</item>"
        "</menu></terminal></config>"
    )
    fd, path = tempfile.mkstemp(prefix="vtm_radiomenu_", suffix=".xml")
    with os.fdopen(fd, "w") as fh:
        fh.write(cfg)
    return path


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
    return f"\033[<35;{col};{row}M".encode()


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
        # Esc first to dismiss any open popup (its backdrop would otherwise
        # eat the close-button click). Esc is a no-op when nothing is open.
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
    """(row, col) of the marker's first cell on the most recent paint that
    emitted it, or None. Tracks CUP + a running visible-cell offset so the
    column reflects the on-screen position rather than a byte offset.
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
    between = strip_ansi(raw_buf[cup.end():last]).decode("utf-8", errors="replace")
    start_col = col + len(between)
    return (row, start_col)


def fail(msg):
    print(f"FAIL - {msg}")
    return False


def _open_radio_popup(s):
    """Locate + click the radiomenu trigger; return (trigger_row, trigger_col)
    or None on failure."""
    coords = find_marker_position(s._screen_buf, "[RADIO]")
    if coords is None:
        return None
    trigger_row, trigger_col = coords
    s.reset_buffer()
    s.click(trigger_col + 2, trigger_row)
    s.snapshot(timeout=1.5)
    return (trigger_row, trigger_col)


def test_radiomenu_loads_from_xml_and_opens_popup():
    """Click the radiomenu trigger; the popup must paint every child label
    below the trigger and the menu bar must stay visible."""
    print("TEST: radiomenu: popup opens, child rows + menu bar visible ... ",
          end="", flush=True)
    with VtmTileSession(RADIO_TILE_ARGS, vtm_config=RADIO_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.snapshot(timeout=2.0)

        opened = _open_radio_popup(s)
        if opened is None:
            return fail(
                "radiomenu trigger '[RADIO]' not rendered — the "
                "<item type='radiomenu'> XML may not have been parsed"
            )
        trigger_row, _ = opened
        rendered = s.snapshot(timeout=1.0)

        for marker in (OPT_A, OPT_B, OPT_C):
            if marker not in rendered:
                return fail(f"popup did not paint radio row '{marker}'")
        if "[RADIO]" not in rendered:
            return fail("menu bar trigger disappeared while popup was open")

        # Rows anchored below the trigger and in config order.
        a_pos = find_marker_position(s._screen_buf, OPT_A)
        b_pos = find_marker_position(s._screen_buf, OPT_B)
        c_pos = find_marker_position(s._screen_buf, OPT_C)
        if None in (a_pos, b_pos, c_pos):
            return fail("could not locate all radio row positions")
        if not (a_pos[0] > trigger_row and a_pos[0] < b_pos[0] < c_pos[0]):
            return fail(
                f"radio rows not below trigger / out of order: "
                f"trigger_row={trigger_row}, A={a_pos}, B={b_pos}, C={c_pos}"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during radiomenu interaction")
        if not s.normal_exit():
            return fail("vtm-tile did not exit after clicking close button")
        print(f"PASS (A={a_pos}, B={b_pos}, C={c_pos})")
        return True


def test_radiomenu_renders_radio_bullets():
    """The open popup must emit both bullet glyphs: exactly one ◉ (the
    selected row) and one ○ per remaining option."""
    print("TEST: radiomenu: ◉/○ bullets rendered (1 filled, rest hollow) ... ",
          end="", flush=True)
    with VtmTileSession(RADIO_TILE_ARGS, vtm_config=RADIO_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.snapshot(timeout=2.0)

        if _open_radio_popup(s) is None:
            return fail("radiomenu trigger '[RADIO]' not found")
        s.snapshot(timeout=1.0)

        raw = s._screen_buf
        filled = raw.count(RADIO_FILLED_B)
        hollow = raw.count(RADIO_HOLLOW_B)
        # 3 options, 1 selected → at least one ◉ and at least two ○ painted.
        if filled < 1:
            return fail(
                "no ◉ (U+25C9) bullet emitted — the selected radio row "
                "was not rendered filled"
            )
        if hollow < 2:
            return fail(
                f"expected ≥2 ○ (U+25CB) hollow bullets for the unselected "
                f"rows, saw {hollow}"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during bullet-render test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print(f"PASS (filled={filled}, hollow={hollow})")
        return True


def test_radiomenu_checked_row_is_filled():
    """The ◉ bullet must sit on the statically-selected row (OPT_B,
    checked='true'), in the left gutter to the left of the label; the
    other rows must carry ○."""
    print("TEST: radiomenu: checked='true' row gets ◉, others ○ ... ",
          end="", flush=True)
    with VtmTileSession(RADIO_TILE_ARGS, vtm_config=RADIO_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.snapshot(timeout=2.0)

        if _open_radio_popup(s) is None:
            return fail("radiomenu trigger '[RADIO]' not found")
        s.snapshot(timeout=1.0)

        filled_pos = find_marker_position(s._screen_buf, RADIO_FILLED_B)
        if filled_pos is None:
            return fail("no ◉ bullet located on screen")
        filled_row, filled_col = filled_pos

        b_pos = find_marker_position(s._screen_buf, OPT_B)
        if b_pos is None:
            return fail("could not locate the checked row OPT_B")
        b_row, b_col = b_pos

        if filled_row != b_row:
            a_pos = find_marker_position(s._screen_buf, OPT_A)
            c_pos = find_marker_position(s._screen_buf, OPT_C)
            return fail(
                f"◉ painted on row {filled_row}, but the checked option "
                f"OPT_B is on row {b_row} (A={a_pos}, C={c_pos}) — the "
                f"static checked='true' selection was not honoured"
            )
        if filled_col >= b_col:
            return fail(
                f"◉ at col {filled_col} is not in the left gutter before "
                f"the label (label col {b_col}) — the bullet should sit "
                f"left of the row text"
            )

        # The unchecked rows must carry the hollow bullet (◉ appears once).
        if s._screen_buf.count(RADIO_FILLED_B) != 1:
            return fail(
                f"expected exactly one ◉ (only the checked row), saw "
                f"{s._screen_buf.count(RADIO_FILLED_B)}"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during checked-row test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print(f"PASS (◉ at row {filled_row} col {filled_col}, label col {b_col})")
        return True


def test_radiomenu_click_option_dismisses_chain():
    """Clicking a radio row must dispatch its script and tear the chain
    down. Verified by re-clicking the trigger and confirming the popup
    re-opens (only possible once menu.dropdown.open was cleared)."""
    print("TEST: radiomenu: clicking a row dismisses the chain ... ",
          end="", flush=True)
    with VtmTileSession(RADIO_TILE_ARGS, vtm_config=RADIO_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.snapshot(timeout=2.0)

        opened = _open_radio_popup(s)
        if opened is None:
            return fail("radiomenu trigger '[RADIO]' not found")
        trigger_row, trigger_col = opened
        rendered = s.snapshot(timeout=1.0)
        if OPT_C not in rendered:
            return fail("popup did not open on first click")

        c_pos = find_marker_position(s._screen_buf, OPT_C)
        if c_pos is None:
            return fail("could not locate OPT_C row")
        c_row, c_col = c_pos

        # Click the OPT_C radio row.
        s.reset_buffer()
        s.click(c_col + 2, c_row)
        time.sleep(0.4)
        s.snapshot(timeout=1.5)

        # Re-click the trigger: must re-open if the chain dismissed.
        s.reset_buffer()
        s.click(trigger_col + 2, trigger_row)
        reopened = s.snapshot(timeout=1.5)
        if OPT_A not in reopened:
            return fail(
                "after row click + re-trigger, popup did not re-open — "
                "the radio row click likely failed to dismiss the chain"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during row-click test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print("PASS")
        return True


def test_radiomenu_nested_in_dropdown_renders_radio_group():
    """A radiomenu nested inside a dropdown: opening the parent dropdown then
    the radiomenu submenu must render the children as a radio group with the
    checked='true' row filled (◉) and the rest hollow (○)."""
    print("TEST: radiomenu: nested in dropdown renders radio group ... ",
          end="", flush=True)
    with VtmTileSession(NESTED_TILE_ARGS, vtm_config=NESTED_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.snapshot(timeout=2.0)

        coords = find_marker_position(s._screen_buf, "[EDIT]")
        if coords is None:
            return fail("dropdown trigger '[EDIT]' not rendered")
        tr, tc = coords

        # Open the parent dropdown.
        s.reset_buffer()
        s.click(tc + 2, tr)
        s.snapshot(timeout=1.5)
        row = find_marker_position(s._screen_buf, NEST_RADIO_ROW)
        if row is None:
            return fail(f"radiomenu row '{NEST_RADIO_ROW}' not painted in parent popup")
        rr, rc = row

        # Open the radiomenu submenu by hovering its row (chevron submenu).
        s.reset_buffer()
        s.hover(rc + 2, rr)
        time.sleep(0.3)
        rendered = s.snapshot(timeout=1.5)
        for marker in (NOPT_A, NOPT_B, NOPT_C):
            if marker not in rendered:
                return fail(f"nested radio submenu did not paint row '{marker}'")

        raw = s._screen_buf
        filled = raw.count(RADIO_FILLED_B)
        hollow = raw.count(RADIO_HOLLOW_B)
        if filled != 1:
            return fail(
                f"expected exactly one ◉ in the nested radio group, saw {filled}"
            )
        if hollow < 2:
            return fail(f"expected ≥2 ○ hollow bullets, saw {hollow}")

        # The ◉ must sit on the checked option (NOPT_B).
        fp = find_marker_position(raw, RADIO_FILLED_B)
        b_pos = find_marker_position(raw, NOPT_B)
        if fp is None or b_pos is None:
            return fail("could not locate ◉ bullet / checked row")
        if fp[0] != b_pos[0]:
            return fail(
                f"◉ on row {fp[0]} but checked option NOPT_B is on row "
                f"{b_pos[0]} — static checked='true' not honoured when nested"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during nested radiomenu test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print(f"PASS (◉ row {fp[0]} == checked row {b_pos[0]})")
        return True


def test_radiomenu_dynamic_reflects_and_tracks_terminal_state():
    """End-to-end: a radiomenu wired to vtm.terminal.ClipboardFormat() must
    render the bullet on the terminal's CURRENT format, and the bullet must
    MOVE when a different format row is selected.

    This exercises the real headline behaviour (and the nested-in-dropdown
    layout that previously rendered ○ on every row): the radiomenu lives in
    the terminal process so vtm.terminal resolves, and its <script> updater
    publishes the live selection via vtm.item.Check(idx) when the submenu
    opens.
    """
    print("TEST: radiomenu: dynamic bullet tracks live ClipboardFormat ... ",
          end="", flush=True)
    cfg_path = _write_dyn_term_menu()
    tile_config = (
        "<config><tile><confirm_close=0/>"
        '<app selected="term"><item*/>'
        f'<item id="term" label="term" type="dtvt" cmd="$0 -c {cfg_path} -r term"/>'
        "</app><menu item*></menu></tile></config>"
    )

    def open_radio(s):
        coords = find_marker_position(s._screen_buf, "[TERMMENU]")
        if coords is None:
            return None
        tr, tc = coords
        s.reset_buffer()
        s.click(tc + 2, tr)
        s.snapshot(timeout=1.5)
        row = find_marker_position(s._screen_buf, DYN_RADIO_ROW)
        if row is None:
            return None
        rr, rc = row
        s.hover(rc + 2, rr)
        time.sleep(0.35)
        s.snapshot(timeout=1.5)
        return (tr, tc)

    try:
        with VtmTileSession([], vtm_config=tile_config) as s:
            if not s.is_alive():
                return fail("vtm-tile did not start")
            # The dtvt child loads its menu from the -c <file>; give it a beat
            # longer than an inline-config session to come up and paint.
            time.sleep(1.0)
            s.snapshot(timeout=3.0)

            if open_radio(s) is None:
                return fail(
                    "radiomenu submenu did not open — '[TERMMENU]' dropdown "
                    "or its nested radiomenu row was not rendered"
                )
            raw = s._screen_buf
            if raw.count(RADIO_FILLED_B) != 1:
                return fail(
                    f"expected exactly one ◉ reflecting the live clipboard "
                    f"format, saw {raw.count(RADIO_FILLED_B)} (◉ absent means "
                    f"vtm.item.Check(vtm.terminal.ClipboardFormat()) did not "
                    f"publish an index — the nested-radiomenu bug)"
                )
            fp1 = find_marker_position(raw, RADIO_FILLED_B)

            # Pick the RichText row (format 3); its script sets the live format.
            rich = find_marker_position(s._screen_buf, "RichText")
            if rich is None:
                return fail("could not locate the 'RichText' radio row")
            s.click(rich[1] + 1, rich[0])
            time.sleep(0.4)
            s.snapshot(timeout=1.5)

            # Re-open: the bullet must now sit on the RichText row. (Do NOT
            # reset the buffer here — open_radio locates the menu-bar trigger
            # from the cumulative paint before opening, then resets internally
            # so raw2 captures only the fresh popup.)
            if open_radio(s) is None:
                return fail("radiomenu did not re-open after selecting a format")
            raw2 = s._screen_buf
            fp2 = find_marker_position(raw2, RADIO_FILLED_B)
            rich2 = find_marker_position(raw2, "RichText")
            if fp2 is None or rich2 is None:
                return fail("could not locate ◉ / RichText row after re-open")
            if fp2[0] != rich2[0]:
                return fail(
                    f"after selecting RichText the ◉ is on row {fp2[0]} but "
                    f"RichText is on row {rich2[0]} — the bullet did not track "
                    f"the new live clipboard format"
                )
            if fp1[0] == fp2[0]:
                return fail(
                    f"the ◉ did not move when the format changed (stayed on "
                    f"row {fp1[0]}) — the radiomenu is not reflecting live state"
                )

            if not s.is_alive():
                return fail("vtm-tile crashed during dynamic radiomenu test")
            if not s.normal_exit():
                return fail("vtm-tile did not exit cleanly")
            print(f"PASS (◉ {fp1[0]} -> {fp2[0]} tracking RichText)")
            return True
    finally:
        try:
            os.unlink(cfg_path)
        except OSError:
            pass


TESTS = [
    test_radiomenu_loads_from_xml_and_opens_popup,
    test_radiomenu_renders_radio_bullets,
    test_radiomenu_checked_row_is_filled,
    test_radiomenu_click_option_dismisses_chain,
    test_radiomenu_nested_in_dropdown_renders_radio_group,
    test_radiomenu_dynamic_reflects_and_tracks_terminal_state,
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
