#!/usr/bin/env python3
# Copyright (c) Shaobo Song
# Licensed under the MIT license.

"""
End-to-end TUI tests for the Quick Connect bar's input fields (connectbar.hpp):
a left mouse *press* (not the click release) focuses the make_input child under
the cursor, and a left press-drag scrubs the caret in real time, clamped to the
field's text. Tab chords are consumed without moving focus. The Connect button
still fires on the click.

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


def _open_history(s):
    chars = s.screen()[0]
    connect = T.find_text(chars, " Connect ")
    history = T.find_text_on_row(chars, "▾", connect[0]) if connect else None
    if history is None:
        return None
    s.click(history[1] + 1, history[0] + 1)
    return s.screen()[0]


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


def test_field_drag_scrolls_left():
    """Dragging left of a horizontally scrolled field reveals and reaches earlier text."""
    print("TEST: parvion connect bar - drag beyond left edge scrolls caret ... ", end="", flush=True)
    d = tempfile.mkdtemp(prefix="parvioncb_")
    try:
        with T.ParvionSession(d) as s:
            hf = _host_field(s)
            if hf is None:
                print("FAIL - 'Host:' label not found")
                return False
            r, x0 = hf
            s.click(x0 + 1, r + 1)
            s.write("abcdefghijklmnopqrstuv")  # Wider than the 16-cell Host field.
            # Pull from inside the scrolled field through its left edge to terminal column 1.
            # The signed out-of-bounds coordinates must walk the caret back through hidden text.
            s.drag_path([(x0 + 8, r + 1), (x0 + 1, r + 1), (x0, r + 1), (1, r + 1)])
            s.write("X")
            s.write("\x1b[H")  # Reveal the beginning without changing the inserted text.
            got = _host_text(s, r, x0)
            if not got.startswith("Xabcdef"):
                print(f"FAIL - Host field begins with {got!r}, want marker before 'abcdef'")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_tab_and_shift_tab_are_noops():
    """Tab chords are consumed by the active input and never move focus."""
    print("TEST: parvion connect bar - Tab/Shift+Tab are no-ops ... ", end="", flush=True)
    d = tempfile.mkdtemp(prefix="parvioncb_")
    try:
        with T.ParvionSession(d) as s:
            hf = _host_field(s)
            if hf is None:
                print("FAIL - 'Host:' label not found")
                return False
            r, x0 = hf
            s.click(x0 + 1, r + 1)
            s.write("a")
            s.write("\t")
            s.write("b")
            s.write("\x1b[Z")
            s.write("c")
            got = _host_text(s, r, x0)
            if got != "abc":
                print(f"FAIL - Host field shows {got!r}, want 'abc'")
                return False
            row = T.row_text(s.screen()[0], r)
            user = row[row.find("User:") + len("User:"):row.find("Pass:")].strip()
            if user:
                print(f"FAIL - focus moved to User field, which contains {user!r}")
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


def test_connect_button_visual_states():
    """The shared button owns distinct resting, hover, and held feedback."""
    print("TEST: parvion connect bar - shared button visual states ... ", end="", flush=True)
    d = tempfile.mkdtemp(prefix="parvioncb_")
    try:
        with T.ParvionSession(d) as s:
            pos = T.find_text(s.screen()[0], " Connect ")
            if pos is None:
                print("FAIL - Connect button not found")
                return False
            row = pos[0] + 1
            col = pos[1] + 2
            sample = (pos[0], col - 1)
            resting = s.screen()[1][sample[0]][sample[1]]
            # The first motion packet seeds vtm's pointer position; the second is delivered.
            s.hover(100, row, settle=0.3)
            s.hover(col, row)
            hover = s.screen()[1][sample[0]][sample[1]]
            os.write(s.master_fd, f"\x1b[<0;{col};{row}M".encode())
            s.feed(0.5)
            held = s.screen()[1][sample[0]][sample[1]]
            os.write(s.master_fd, f"\x1b[<0;{col};{row}m".encode())
            s.feed(0.5)
            released = s.screen()[1][sample[0]][sample[1]]
            if resting == hover or hover == held:
                print(f"FAIL - button shades are not distinct ({resting}, {hover}, {held})")
                return False
            if released != hover:
                print(f"FAIL - release did not restore hover shade ({released} vs {hover})")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_connect_drag_off_cancels_click():
    """A press dragged away from Connect must not activate it on release."""
    print("TEST: parvion connect bar - drag off cancels Connect ... ", end="", flush=True)
    d = tempfile.mkdtemp(prefix="parvioncb_")
    try:
        with T.ParvionSession(d) as s:
            pos = T.find_text(s.screen()[0], " Connect ")
            host = _host_field(s)
            if pos is None or host is None:
                print("FAIL - Connect button or Host field not found")
                return False
            row = pos[0] + 1
            s.drag_path([(pos[1] + 2, row), (host[1] + 1, row)])
            if T.grid_contains(s.screen()[0], "Enter a host name."):
                print("FAIL - Connect fired after its press was dragged away")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_responsive_narrow_bar_stays_interactive():
    """The fully compact retained form keeps Connect/dropdown adjacency and input hit boxes."""
    print("TEST: parvion connect bar - narrow retained layout stays interactive ... ", end="", flush=True)
    d = tempfile.mkdtemp(prefix="parvioncb_")
    try:
        with T.ParvionSession(d) as s:
            narrow_cols = 54
            T.set_winsize(s.master_fd, T.ROWS, narrow_cols)
            s.feed(1.0)
            chars = s.screen()[0]
            host = T.find_text(chars, "H:")
            if host is None:
                print("FAIL - compact Host label not found")
                return False
            r, host_label_x = host
            row = T.row_text(chars, r)[:narrow_cols]
            compact = row.find(" » ")
            history = row.find("▾", compact + 3 if compact >= 0 else 0)
            if compact < 0 or history != compact + 4:
                print(f"FAIL - compact Connect/history are not flush: {row!r}")
                return False

            # Empty-host Connect must still activate while compact. Reveal its
            # status after restoring enough width for the status label.
            s.click(compact + 2, r + 1)
            T.set_winsize(s.master_fd, T.ROWS, T.COLS)
            s.feed(1.0)
            if not T.grid_contains(s.screen()[0], "Enter a host name."):
                print("FAIL - compact Connect button did not activate")
                return False

            # Resize compact again and verify the retained Host input owns the
            # field cells immediately after the abbreviated label.
            T.set_winsize(s.master_fd, T.ROWS, narrow_cols)
            s.feed(1.0)
            chars = s.screen()[0]
            host = T.find_text(chars, "H:")
            if host is None:
                print("FAIL - compact Host label disappeared after second resize")
                return False
            r, host_label_x = host
            host_input_x = host_label_x + len("H:") + 1
            s.click(host_input_x + 1, r + 1)
            s.write("hi")

            T.set_winsize(s.master_fd, T.ROWS, T.COLS)
            s.feed(1.0)
            full_host = _host_field(s)
            if full_host is None or _host_text(s, *full_host) != "hi":
                print("FAIL - typing through compact Host hit box did not update the field")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_history_dropdown_follows_trigger_during_resize():
    """An open history popup must re-anchor after the connect bar reflows."""
    print("TEST: parvion connect history follows trigger during resize ... ", end="", flush=True)
    d = tempfile.mkdtemp(prefix="parvioncb_")
    try:
        with T.ParvionSession(d, env={"XDG_CONFIG_HOME": d, "PARVION_DEMO_QUEUE": "0"}) as s:
            narrow_cols = 54
            T.set_winsize(s.master_fd, T.ROWS, narrow_cols)
            s.feed(1.0)
            chars = s.screen()[0]
            compact = T.find_text(chars, " » ")
            history = T.find_text_on_row(chars, "▾", compact[0]) if compact else None
            if history is None:
                print("FAIL - compact Quick Connect history button not found")
                return False
            s.click(history[1] + 1, history[0] + 1)
            chars = s.screen()[0]
            narrow_popup = T.find_text(chars, "Clear history")
            if narrow_popup is None:
                print("FAIL - compact history dropdown did not open")
                return False

            T.set_winsize(s.master_fd, T.ROWS, T.COLS)
            s.feed(1.0)
            chars = s.screen()[0]
            connect = T.find_text(chars, " Connect ")
            wide_history = T.find_text_on_row(chars, "▾", connect[0]) if connect else None
            wide_popup = T.find_text(chars, "Clear history")
            if wide_history is None or wide_popup is None:
                print("FAIL - history trigger or open popup disappeared after widening")
                return False
            if wide_history[1] <= history[1] or wide_popup[1] <= narrow_popup[1]:
                print(f"FAIL - trigger/popup did not move right: "
                      f"trigger {history[1]}->{wide_history[1]}, popup {narrow_popup[1]}->{wide_popup[1]}")
                return False
            # At full width the popup fits at its preferred anchor. Both the
            # arrow-only trigger and popup labels have one cell of left padding,
            # so their visible glyphs align exactly when the live anchor is used.
            if wide_popup[1] != wide_history[1]:
                print(f"FAIL - popup stayed at stale x={wide_popup[1]}, "
                      f"current trigger x={wide_history[1]}")
                return False

            # The trigger's own preview-tier carve-out must move with it too;
            # otherwise this click dismisses in preview and immediately reopens
            # in the trigger handler instead of toggling the chain off.
            s.click(wide_history[1] + 1, wide_history[0] + 1)
            if T.grid_contains(s.screen()[0], "Clear history"):
                print("FAIL - moved history trigger did not toggle the popup off")
                return False

            # Reopen and prove the moved popup retained a live hit target, not
            # merely a correctly repainted label.
            s.click(wide_history[1] + 1, wide_history[0] + 1)
            wide_popup = T.find_text(s.screen()[0], "Clear history")
            if wide_popup is None:
                print("FAIL - moved history trigger did not reopen the popup")
                return False
            s.click(wide_popup[1] + 1, wide_popup[0] + 1)
            if T.grid_contains(s.screen()[0], "Clear history"):
                print("FAIL - moved history row was not interactive")
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


def test_history_dropdown_in_host_outside_click_still_dismisses():
    """Removing mouse-halt dismissal must not affect delivered outside clicks."""
    print("TEST: parvion connect history keeps in-host outside-click dismissal ... ",
          end="", flush=True)
    d = tempfile.mkdtemp(prefix="parvioncb_")
    try:
        with T.ParvionSession(d) as s:
            chars = _open_history(s)
            if chars is None or not T.grid_contains(chars, "Clear history"):
                print("FAIL - Quick Connect history dropdown did not open")
                return False
            host = _host_field(s)
            if host is None:
                print("FAIL - Host field not found")
                return False
            row, field_x = host
            s.click(field_x + 1, row + 1)
            if T.grid_contains(s.screen()[0], "Clear history"):
                print("FAIL - delivered outside click did not dismiss the dropdown")
                return False
            s.write("x")
            if _host_text(s, row, field_x) != "x":
                print("FAIL - outside click did not pass through to the Host field")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_site_connection_empty_submenu_is_first_and_separated():
    print("TEST: parvion connect history - empty Site Connection submenu ... ", end="", flush=True)
    d = tempfile.mkdtemp(prefix="parvioncb_")
    try:
        with T.ParvionSession(d, env={"XDG_CONFIG_HOME": d, "PARVION_DEMO_QUEUE": "0"}) as s:
            chars = _open_history(s)
            if chars is None:
                print("FAIL - Quick Connect history button not found")
                return False
            if not T.menu_items_are_ordered(chars, ["Site Connection", "Clear Quickconnect bar", "Clear history"]):
                print("FAIL - Site Connection is not the first history-menu item")
                return False
            if not T.menu_has_separator_between(chars, "Site Connection", "Clear Quickconnect bar"):
                print("FAIL - Site Connection is not separated from the existing actions")
                return False
            site = T.find_text(chars, "Site Connection")
            s.click(site[1] + 1, site[0] + 1)
            if not T.grid_contains(s.screen()[0], "Empty"):
                print("FAIL - empty Site Connection submenu has no Empty row")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_site_connection_fills_bar_and_connects():
    print("TEST: parvion connect history - saved site fills bar and connects ... ", end="", flush=True)
    d = tempfile.mkdtemp(prefix="parvioncb_")
    try:
        cfg = os.path.join(d, "parvion")
        os.makedirs(cfg, exist_ok=True)
        with open(os.path.join(cfg, "settings"), "w") as f:
            f.write("Site\tMy Site\tsite.test\t2222\talice\tsecret\n")
        with T.ParvionSession(d, env={"XDG_CONFIG_HOME": d, "PARVION_DEMO_QUEUE": "0"}) as s:
            chars = _open_history(s)
            site_menu = T.find_text(chars, "Site Connection")
            if not site_menu:
                print("FAIL - Site Connection menu missing")
                return False
            s.click(site_menu[1] + 1, site_menu[0] + 1)
            chars = s.screen()[0]
            saved = T.find_text(chars, "My Site")
            if not saved:
                print("FAIL - saved site missing from submenu")
                return False
            s.click(saved[1] + 1, saved[0] + 1); s.feed(0.8)
            row = T.row_text(s.screen()[0], _host_field(s)[0])
            for value in ("site.test", "alice", "2222", "******"):
                if value not in row:
                    print(f"FAIL - Quick Connect row missing {value!r}: {row!r}")
                    return False
            recent = os.path.join(cfg, "recent_servers")
            if not os.path.isfile(recent):
                print("FAIL - selecting the saved site did not invoke Connect")
                return False
            with open(recent) as f:
                remembered = f.read()
            if "site.test\talice\t2222\tsecret" not in remembered:
                print(f"FAIL - Connect did not remember the selected site: {remembered!r}")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


TESTS = [
    test_field_press_focuses,
    test_field_drag_scrubs_caret,
    test_field_drag_clamps_to_text,
    test_field_drag_scrolls_left,
    test_tab_and_shift_tab_are_noops,
    test_connect_fires_on_click,
    test_connect_button_visual_states,
    test_connect_drag_off_cancels_click,
    test_responsive_narrow_bar_stays_interactive,
    test_history_dropdown_follows_trigger_during_resize,
    test_site_connection_empty_submenu_is_first_and_separated,
    test_site_connection_fills_bar_and_connects,
    test_history_dropdown_does_not_arm_menubar_hover_switch,
    test_history_dropdown_in_host_outside_click_still_dismisses,
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
