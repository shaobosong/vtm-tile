#!/usr/bin/env python3
# Copyright (c) Shaobo Song
# Licensed under the MIT license.

"""
End-to-end TUI tests for the Quick Connect bar's input fields (connectbar.hpp):
a left mouse *press* (not the click release) focuses the bar and activates the
field under the cursor, and a left press-drag scrubs the caret in real time,
clamped to the field's text. The Connect button still fires on the click.

The caret position is asserted behaviorally — by typing after the gesture and
checking where the character lands — because the harness's replay() does not
model the inverse-video / colon-form-underline SGRs vtm may use for the caret
cell. Local-only: connecting is exercised just far enough to see the
"Enter a host name." hint (no network).
"""

import os
import sys
import time
import shutil
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_parvion_panes as T  # ParvionSession, row_text, find_text, drag_path, ...

kill_all_vtm = T.kill_all_vtm  # Let run_all_tests.py reuse our between-test cleanup.


def _host_field(s):
    """(row, x0): the connect bar row and the Host field's 0-based start column
    (the field box begins one cell after the 'Host:' label)."""
    pos = T.find_text(s.screen()[0], "Host:")
    if pos is None:
        return None
    return (pos[0], pos[1] + len("Host:") + 1)


def _host_text(s, r, x0):
    """The Host field's visible text (up to the gap before the 'User:' label)."""
    row = T.row_text(s.screen()[0], r)
    return row[x0:row.find("User:")].strip()


def test_field_press_focuses():
    """Typing right after the press (before any release) lands in the field."""
    print("TEST: parvion connect bar - press focuses field ... ", end="", flush=True)
    d = tempfile.mkdtemp(prefix="parvioncb_")
    try:
        with T.ParvionSession(d) as s:
            hf = _host_field(s)
            if hf is None:
                print("FAIL - 'Host:' label not found")
                return False
            r, x0 = hf
            os.write(s.master_fd, f"\x1b[<0;{x0 + 1};{r + 1}M".encode())  # Press only.
            s.feed(0.6)
            s.write("hi")  # Button still held: focus must already be on the field.
            os.write(s.master_fd, f"\x1b[<0;{x0 + 1};{r + 1}m".encode())  # Release.
            s.feed(0.4)
            got = _host_text(s, r, x0)
            if got != "hi":
                print(f"FAIL - Host field shows {got!r} after press+type, want 'hi'")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_field_drag_scrubs_caret():
    """A left-drag moves the caret with the cursor: the next typed char lands there."""
    print("TEST: parvion connect bar - drag scrubs caret ... ", end="", flush=True)
    d = tempfile.mkdtemp(prefix="parvioncb_")
    try:
        with T.ParvionSession(d) as s:
            hf = _host_field(s)
            if hf is None:
                print("FAIL - 'Host:' label not found")
                return False
            r, x0 = hf
            s.click(x0 + 1, r + 1)
            s.write("abcdef")
            # Press at in-field cell 5, pull left, release at cell 2 (1-based SGR coords).
            # Only drag::pull moves the caret after the press, so 'X' landing between
            # 'b' and 'c' proves the pulls tracked the cursor.
            s.drag_path([(x0 + 6, r + 1), (x0 + 4, r + 1), (x0 + 3, r + 1)])
            s.write("X")
            got = _host_text(s, r, x0)
            if got != "abXcdef":
                print(f"FAIL - Host field shows {got!r}, want 'abXcdef'")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_field_drag_clamps_to_text():
    """Dragging past the field keeps scrubbing the armed field, clamped to its text end."""
    print("TEST: parvion connect bar - drag clamps to text ... ", end="", flush=True)
    d = tempfile.mkdtemp(prefix="parvioncb_")
    try:
        with T.ParvionSession(d) as s:
            hf = _host_field(s)
            if hf is None:
                print("FAIL - 'Host:' label not found")
                return False
            r, x0 = hf
            s.click(x0 + 1, r + 1)
            s.write("abc")
            # Press at in-field cell 1, pull far right over the User label and beyond.
            s.drag_path([(x0 + 2, r + 1), (x0 + 12, r + 1), (x0 + 24, r + 1)])
            s.write("Z")
            got = _host_text(s, r, x0)
            if got != "abcZ":  # Caret clamped to the end of "abc", not stuck at cell 1.
                print(f"FAIL - Host field shows {got!r}, want 'abcZ'")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_connect_fires_on_click():
    """The Connect button still acts on the click: empty host shows the hint."""
    print("TEST: parvion connect bar - Connect fires on click ... ", end="", flush=True)
    d = tempfile.mkdtemp(prefix="parvioncb_")
    try:
        with T.ParvionSession(d) as s:
            pos = T.find_text(s.screen()[0], " Connect ")
            if pos is None:
                print("FAIL - Connect button not found")
                return False
            s.click(pos[1] + 2, pos[0] + 1)
            if not T.grid_contains(s.screen()[0], "Enter a host name."):
                print("FAIL - no 'Enter a host name.' hint after clicking Connect")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_history_dropdown_does_not_arm_menubar_hover_switch():
    """A control dropdown must not start a menu-bar hover-switch session."""
    print("TEST: parvion connect history does not arm menu-bar hover ... ", end="", flush=True)
    d = tempfile.mkdtemp(prefix="parvioncb_")
    try:
        with T.ParvionSession(d) as s:
            chars = s.screen()[0]
            connect = T.find_text(chars, " Connect ")
            history = T.find_text_on_row(chars, "▾", connect[0]) if connect else None
            if history is None:
                print("FAIL - Quick Connect history button not found")
                return False
            s.click(history[1] + 1, history[0] + 1)
            chars = s.screen()[0]
            if not T.grid_contains(chars, "Clear history"):
                print("FAIL - Quick Connect history dropdown did not open")
                return False
            edit = T.find_text(chars, "Edit")
            if edit is None:
                print("FAIL - Edit menu-bar trigger not found")
                return False

            s.hover(edit[1] + 1, edit[0] + 1)
            chars = s.screen()[0]
            if T.grid_contains(chars, "Settings"):
                print("FAIL - control dropdown armed menu-bar hover switching")
                return False
            if not T.grid_contains(chars, "Clear history"):
                print("FAIL - hovering Edit dismissed the history dropdown")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


TESTS = [
    test_field_press_focuses,
    test_field_drag_scrubs_caret,
    test_field_drag_clamps_to_text,
    test_connect_fires_on_click,
    test_history_dropdown_does_not_arm_menubar_hover_switch,
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
