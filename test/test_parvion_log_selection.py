#!/usr/bin/env python3
# Copyright (c) Shaobo Song
# Licensed under the MIT license.

"""
End-to-end TUI tests for Message-log text selection (parvion/queue.hpp, tab 3).

The Message-log tab supports three mouse selection granularities, chosen by click
count (mirroring the built-in terminal): a left-drag selects by character, a
double-click selects the word under the cursor, and a triple-click selects the
whole line. Selected cells paint with theme::sel_bg. The right-click log menu
always carries a "Copy" item: enabled (and copying the selection via OSC 52) when
text is selected, disabled (greyed, inert) otherwise. A plain left-click cancels
the selection, the app menu orders Copy, Select all, and clear actions, and a
live log update (new lines appended) does NOT clear it.

Driven via a pty using the SGR mouse protocol. The app is launched as
`vtm-tile -r parvion` with the message log seeded by $PARVION_DEMO_LOG_N=k
(k numbered "Status: log line NN" lines plus one overflow line), so the body
content is deterministic and the vertical scrollbar is present. $PARVION_DEMO_LOG_TICK=N
makes the poll timer append a line every N polls, exercising live log updates.
"""

import os
import sys
import time
import base64
import re
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_parvion_panes as T  # ParvionSession, COLS, ROWS, row_text, find_text, ...

kill_all_vtm = T.kill_all_vtm  # Let run_all_tests.py reuse our between-test cleanup.

SEL_BG = (69, 71, 90)  # theme::sel_bg (0x45475A): the selection highlight background.
_OSC52 = re.compile(rb"\x1b\]52;[^;]*;([A-Za-z0-9+/=]+)(?:\x07|\x1b\\)")
LOG_ENV = {"PARVION_DEMO_LOG_N": "20"}


def _session(extra=None):
    d = tempfile.mkdtemp(prefix="parvion_logsel_")
    env = dict(LOG_ENV)
    if extra:
        env.update(extra)
    return T.ParvionSession(d, env=env)


def _pump(s, secs):
    """Read the pty for `secs` seconds, ingesting any live repaints (keeps the buffer drained)."""
    end = time.time() + secs
    while time.time() < end:
        s.feed(0.2)


def _enter_log(s):
    """Click the Message log tab; return (row, col, text) of a seeded 'log line 1X' row."""
    chars, _ = s.screen()
    pos = T.find_text(chars, "Message log")
    if pos is None:
        return None
    s.click(pos[1] + 1, pos[0] + 1)
    chars, _ = s.screen()
    lp = T.find_text(chars, "log line 1")
    if lp is None:
        return None
    return (lp[0], lp[1], T.row_text(chars, lp[0]))


def _selbg_cells(s, row):
    _, bg = s.screen()
    return [c for c in range(T.COLS) if bg[row][c] == SEL_BG]


def _multiclick(s, col, row, n, gap=0.05):
    """Send n rapid press/release pairs at one cell (a double/triple click)."""
    for _ in range(n):
        os.write(s.master_fd, f"\x1b[<0;{col};{row}M".encode())
        time.sleep(0.02)
        os.write(s.master_fd, f"\x1b[<0;{col};{row}m".encode())
        time.sleep(gap)
    s.feed(0.6)


def _drag_hold(s, points, hold=0.9, button=0, settle=0.6):
    """Press, move through `points`, keep the button down while the app auto-scrolls, then release."""
    c0, r0 = points[0]
    os.write(s.master_fd, f"\x1b[<{button};{c0};{r0}M".encode())
    time.sleep(0.04)
    for c, r in points[1:]:
        os.write(s.master_fd, f"\x1b[<{button + 32};{c};{r}M".encode())
        time.sleep(0.04)
    _pump(s, hold)
    cl, rl = points[-1]
    os.write(s.master_fd, f"\x1b[<{button};{cl};{rl}m".encode())
    s.feed(settle)


def _find_copy_cell(chars):
    """(row, col) of the dedicated 'Copy' menu row, or None."""
    for r in range(T.ROWS):
        line = T.row_text(chars, r)
        if "Copy" in line and "clipboard" not in line and "Clear" not in line:
            return (r, line.find("Copy"))
    return None


def _copy_via_menu(s, row, col):
    """Right-click at (row,col) [0-based], click the 'Copy' item, return the OSC-52 clipboard text."""
    s.click(col + 1, row + 1, button=2)
    chars, _ = s.screen()
    cell = _find_copy_cell(chars)
    if cell is None:
        return None
    s.click(cell[1] + 1, cell[0] + 1)
    time.sleep(0.3)
    s.feed(0.6)
    hits = _OSC52.findall(s._buf)
    return base64.b64decode(hits[-1]).decode("utf-8", "replace") if hits else None


def test_log_menu_button_tracks_selection():
    print("TEST: parvion message log - textbox menu button selection state ... ", end="", flush=True)
    with _session() as s:
        info = _enter_log(s)
        if info is None:
            print("FAIL: message log / seeded lines not found"); return False
        lr, lc, _ = info
        chars, bg = s.screen()
        log_rows = [r for r in range(T.ROWS) if "Status:" in T.row_text(chars, r)]
        if not log_rows:
            print("FAIL: visible log body not found"); return False
        top = min(log_rows)
        if chars[top][0] != "≡":
            print(f"FAIL: textbox button is {chars[top][0]!r}, expected '≡'"); return False

        # Reuse the sortable-header feedback contract: resting, hover, and held are distinct.
        os.write(s.master_fd, f"\x1b[<35;100;{top + 1}M".encode()); s.feed(0.3)
        resting = s.screen()[1][top][0]
        os.write(s.master_fd, f"\x1b[<35;1;{top + 1}M".encode()); s.feed(0.6)
        hover = s.screen()[1][top][0]
        if hover is None or hover == resting:
            print(f"FAIL: no textbox-button hover highlight ({resting} -> {hover})"); return False
        os.write(s.master_fd, f"\x1b[<0;1;{top + 1}M".encode()); s.feed(0.6)
        held = s.screen()[1][top][0]
        if held is None or held in (resting, hover):
            print(f"FAIL: textbox-button hold is not distinct ({resting}, {hover}, {held})"); return False
        os.write(s.master_fd, f"\x1b[<0;1;{top + 1}m".encode()); s.feed(0.6)

        # With no selection, Copy is present but disabled and inert.
        cell = _find_copy_cell(s.screen()[0])
        if cell is None:
            print("FAIL: button did not open the no-selection menu"); return False
        if cell[0] != top + 2:
            print(f"FAIL: button menu opened on row {cell[0]}, expected {top + 2}"); return False
        before = len(s._buf)
        s.click(cell[1] + 1, cell[0] + 1)
        if _OSC52.findall(s._buf[before:]) or _find_copy_cell(s.screen()[0]) is None:
            print("FAIL: no-selection Copy was interactive"); return False
        os.write(s.master_fd, b"\x1b"); s.feed(0.4)

        # Preserve a character selection and build the enabled menu from that exact selection.
        s.drag_path([(lc + 1, lr + 1), (lc + 9, lr + 1)])
        if not _selbg_cells(s, lr):
            print("FAIL: selection was not established"); return False
        s.click(1, top + 1)
        cell = _find_copy_cell(s.screen()[0])
        if cell is None:
            print("FAIL: selected-text menu missing Copy"); return False
        before = len(s._buf)
        s.click(cell[1] + 1, cell[0] + 1)
        hits = _OSC52.findall(s._buf[before:])
        clip = base64.b64decode(hits[-1]).decode("utf-8", "replace") if hits else None
        if clip != "log line":
            print(f"FAIL: button copied {clip!r}, expected selected text"); return False
    print("PASS"); return True


def test_log_char_drag_selects():
    print("TEST: parvion message log - character drag selects ... ", end="", flush=True)
    with _session() as s:
        info = _enter_log(s)
        if info is None:
            print("FAIL: message log / seeded lines not found"); return False
        lr, lc, _ = info
        s.drag_path([(lc + 1, lr + 1), (lc + 9, lr + 1)])  # select "log line".
        cells = _selbg_cells(s, lr)
        if not cells:
            print("FAIL: no selection highlight after drag"); return False
        chars, _ = s.screen()
        text = "".join(chars[lr][c] or " " for c in cells)
        if text != "log line":
            print(f"FAIL: highlighted {text!r}, expected 'log line'"); return False
    print("PASS"); return True


def test_log_drag_selection_autoscrolls_vertically():
    print("TEST: parvion message log - selection drag auto-scrolls vertically ... ", end="", flush=True)
    with _session() as s:
        info = _enter_log(s)
        if info is None:
            print("FAIL: message log / seeded lines not found"); return False
        lr, lc, _ = info
        chars, _ = s.screen()
        before = "\n".join(T.row_text(chars, r) for r in range(T.ROWS))
        if "log line 00" in before:
            print("FAIL: top seeded line already visible before upward drag"); return False
        _drag_hold(s, [(lc + 1, lr + 1), (lc + 1, 1)], hold=1.0)
        chars, _ = s.screen()
        after = "\n".join(T.row_text(chars, r) for r in range(T.ROWS))
        if "log line 00" not in after:
            print("FAIL: upward selection drag did not scroll to earlier log lines"); return False
        if not any(_selbg_cells(s, r) for r in range(T.ROWS)):
            print("FAIL: selection highlight disappeared after vertical auto-scroll"); return False
    print("PASS"); return True


def test_log_drag_selection_autoscrolls_horizontally():
    print("TEST: parvion message log - selection drag auto-scrolls horizontally ... ", end="", flush=True)
    old_cols, old_rows = T.COLS, T.ROWS
    T.COLS = 100  # Keep the seeded overflow tail off-screen at hscroll=0.
    try:
        with _session() as s:
            if _enter_log(s) is None:
                print("FAIL: message log / seeded lines not found"); return False
            chars, _ = s.screen()
            pos = T.find_text(chars, "a very long")
            if pos is None:
                print("FAIL: seeded overflow line not found"); return False
            lr, lc = pos
            before = T.row_text(chars, lr)
            if "horizontal scrollbar" in before:
                print("FAIL: overflow tail already visible before rightward drag"); return False
            _drag_hold(s, [(lc + 1, lr + 1), (T.COLS + 24, lr + 1)], hold=1.0)
            chars, _ = s.screen()
            after = T.row_text(chars, lr)
            if "horizontal scrollbar" not in after and "for testing" not in after:
                print("FAIL: rightward selection drag did not expose the overflow tail"); return False
            clip = _copy_via_menu(s, lr, min(T.COLS - 5, max(2, lc + 2)))
            if clip is None:
                print("FAIL: no clipboard sequence after horizontal auto-scroll selection"); return False
            if "horizontal scrollbar" not in clip and "for testing" not in clip:
                print(f"FAIL: copied selection missed overflow text: {clip!r}"); return False
    finally:
        T.COLS, T.ROWS = old_cols, old_rows
    print("PASS"); return True


def test_log_left_click_cancels():
    print("TEST: parvion message log - left-click cancels selection ... ", end="", flush=True)
    with _session() as s:
        info = _enter_log(s)
        if info is None:
            print("FAIL: message log / seeded lines not found"); return False
        lr, lc, _ = info
        s.drag_path([(lc + 1, lr + 1), (lc + 9, lr + 1)])
        if not _selbg_cells(s, lr):
            print("FAIL: drag did not select"); return False
        s.click(lc + 1, lr + 1)  # plain left-click.
        if _selbg_cells(s, lr):
            print("FAIL: selection survived the left-click"); return False
    print("PASS"); return True


def test_log_right_click_copies_selection():
    print("TEST: parvion message log - right-click Copy copies the selection ... ", end="", flush=True)
    with _session() as s:
        info = _enter_log(s)
        if info is None:
            print("FAIL: message log / seeded lines not found"); return False
        lr, lc, _ = info
        chars, _ = s.screen()
        expected = "".join(chars[lr][c] or " " for c in range(lc, lc + 8))  # "log line".
        s.drag_path([(lc + 1, lr + 1), (lc + 9, lr + 1)])
        clip = _copy_via_menu(s, lr, lc + 2)
        if clip is None:
            print("FAIL: no 'Copy' item / no clipboard sequence emitted"); return False
        if clip != expected:
            print(f"FAIL: clipboard {clip!r} != selection {expected!r}"); return False
        if "\n" in clip or "log line 19" in clip:
            print("FAIL: clipboard looks like the whole log dump, not the selection"); return False
    print("PASS"); return True


def test_log_double_click_word():
    print("TEST: parvion message log - double-click selects a word ... ", end="", flush=True)
    with _session() as s:
        info = _enter_log(s)
        if info is None:
            print("FAIL: message log / seeded lines not found"); return False
        lr, lc, _ = info
        _multiclick(s, lc + 2, lr + 1, 2)  # double-click on "log".
        cells = _selbg_cells(s, lr)
        chars, _ = s.screen()
        word = "".join(chars[lr][c] or " " for c in cells)
        if word != "log":
            print(f"FAIL: double-click highlighted {word!r}, expected 'log'"); return False
    print("PASS"); return True


def test_log_triple_click_line():
    print("TEST: parvion message log - triple-click selects the line ... ", end="", flush=True)
    with _session() as s:
        info = _enter_log(s)
        if info is None:
            print("FAIL: message log / seeded lines not found"); return False
        lr, lc, line = info
        _multiclick(s, lc + 2, lr + 1, 2)
        word_cells = _selbg_cells(s, lr)
        time.sleep(0.6)  # let the click chain reset.
        _multiclick(s, lc + 2, lr + 1, 3)  # triple-click.
        line_cells = _selbg_cells(s, lr)
        if not line_cells:
            print("FAIL: triple-click produced no selection"); return False
        if len(line_cells) <= len(word_cells):
            print(f"FAIL: line selection ({len(line_cells)}) not wider than word ({len(word_cells)})"); return False
        # The selection should span the whole visible line: from the left content edge through
        # the end of the seeded body (no trailing padding / scrollbar cells).
        chars, _ = s.screen()
        sel_text = "".join(chars[lr][c] or " " for c in line_cells).rstrip()
        if min(line_cells) > 2:
            print(f"FAIL: line selection starts at col {min(line_cells)}, expected the left edge"); return False
        if "Status: log line" not in sel_text or not sel_text.endswith("log line 10"):
            print(f"FAIL: line selection text {sel_text!r} does not cover the whole line"); return False
    print("PASS"); return True


def test_log_copy_present_disabled_no_selection():
    print("TEST: parvion message log - Copy present but disabled with no selection ... ", end="", flush=True)
    with _session() as s:
        info = _enter_log(s)
        if info is None:
            print("FAIL: message log / seeded lines not found"); return False
        lr, lc, _ = info
        # No selection: the right-click log menu must still offer "Copy" (requirement #2).
        s.click(lc + 2, lr + 1, button=2)
        chars, _ = s.screen()
        cell = _find_copy_cell(chars)
        if cell is None:
            print("FAIL: Copy item missing when there is no selection"); return False
        # Clicking the disabled Copy is inert: it neither copies (no OSC 52) nor closes the menu.
        before = len(s._buf)
        s.click(cell[1] + 1, cell[0] + 1)
        time.sleep(0.3); s.feed(0.6)
        if _OSC52.findall(s._buf[before:]):
            print("FAIL: disabled Copy still wrote to the clipboard"); return False
        chars, _ = s.screen()
        if _find_copy_cell(chars) is None:
            print("FAIL: clicking disabled Copy dismissed the menu (should be inert)"); return False
    print("PASS"); return True


def test_log_select_all_covers_full_log():
    print("TEST: parvion message log - Select all covers the full log ... ", end="", flush=True)
    with _session() as s:
        info = _enter_log(s)
        if info is None:
            print("FAIL: message log / seeded lines not found"); return False
        lr, lc, _ = info
        s.click(lc + 2, lr + 1, button=2)
        select_all = T.find_text(s.screen()[0], "Select all")
        if select_all is None:
            print("FAIL: Select all menu item missing"); return False
        s.click(select_all[1] + 1, select_all[0] + 1)
        clip = _copy_via_menu(s, lr, lc + 2)
        if clip is None or "log line 00" not in clip or "log line 19" not in clip:
            print(f"FAIL: Select all copied incomplete log text: {clip!r}"); return False
    print("PASS"); return True


def test_log_context_menu_grouping():
    print("TEST: parvion message log - context menu grouping ... ", end="", flush=True)
    with _session() as s:
        info = _enter_log(s)
        if info is None:
            print("FAIL: message log / seeded lines not found"); return False
        lr, lc, _ = info
        s.click(lc + 2, lr + 1, button=2)
        chars, _ = s.screen()
        blob = "\n".join(T.row_text(chars, r) for r in range(len(chars)))
        for needle in ("Copy", "Select all", "Clear all"):
            if needle not in blob:
                print(f"FAIL: '{needle}' missing from menu"); return False
        if "Copy to clipboard" in blob or "Show detailed log" in blob or "Log level" in blob:
            print("FAIL: obsolete message-log menu item is still present"); return False
        copy = _find_copy_cell(chars)
        select_all = T.find_text(chars, "Select all")
        clear = T.find_text(chars, "Clear all")
        if not (copy and select_all and clear
                and copy[0] < select_all[0] < clear[0]):
            print("FAIL: message-log menu groups are out of order"); return False
        if not T.menu_has_separator_between(chars, "Copy", "Select all"):
            print("FAIL: Copy is not separated from the remaining log actions"); return False
        if T.menu_has_separator_between(chars, "Select all", "Clear all"):
            print("FAIL: unexpected separator within the log-action group"); return False
    print("PASS"); return True


def test_log_live_appends_arrive():
    print("TEST: parvion message log - live log appends arrive (tick seam) ... ", end="", flush=True)
    with _session({"PARVION_DEMO_LOG_TICK": "4"}) as s:
        if _enter_log(s) is None:
            print("FAIL: message log / seeded lines not found"); return False
        _pump(s, 1.2)  # follow is engaged (no selection): new ticks pin to the bottom.
        chars, _ = s.screen()
        if not T.grid_contains(chars, "tick "):
            print("FAIL: no live-appended 'tick' line appeared"); return False
    print("PASS"); return True


def test_log_selection_survives_log_update():
    print("TEST: parvion message log - selection survives live log updates ... ", end="", flush=True)
    with _session({"PARVION_DEMO_LOG_TICK": "4"}) as s:  # append a line every ~200ms.
        info = _enter_log(s)
        if info is None:
            print("FAIL: message log / seeded lines not found"); return False
        lr, lc, _ = info
        # Drag-select a visible row; drag::start disengages follow and freezes the view.
        s.drag_path([(lc + 1, lr + 1), (lc + 9, lr + 1)])
        before = _selbg_cells(s, lr)
        if not before:
            print("FAIL: drag did not select"); return False
        _pump(s, 1.2)  # several log lines append while the selection is held.
        after = _selbg_cells(s, lr)
        if after != before:
            print(f"FAIL: selection changed across log updates: {before} -> {after}"); return False
    print("PASS"); return True


TESTS = [
    test_log_menu_button_tracks_selection,
    test_log_char_drag_selects,
    test_log_drag_selection_autoscrolls_vertically,
    test_log_drag_selection_autoscrolls_horizontally,
    test_log_left_click_cancels,
    test_log_right_click_copies_selection,
    test_log_double_click_word,
    test_log_triple_click_line,
    test_log_copy_present_disabled_no_selection,
    test_log_select_all_covers_full_log,
    test_log_context_menu_grouping,
    test_log_live_appends_arrive,
    test_log_selection_survives_log_update,
]


def main():
    if not os.path.isfile(T.VTM_TILE_BINARY):
        print(f"ERROR: vtm-tile binary not found at {T.VTM_TILE_BINARY}")
        return 1
    kill_all_vtm()
    passed = failed = 0
    for test in TESTS:
        try:
            if test():
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
            time.sleep(0.4)
    total = passed + failed
    print(f"\n{'=' * 60}")
    print(f"Results: {passed}/{total} passed, {failed} failed")
    print(f"{'=' * 60}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
