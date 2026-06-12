#!/usr/bin/env python3
# Copyright (c) Shaobo Song
# Licensed under the MIT license.

"""
End-to-end TUI regression test for the menu item type="check" feature.

Verifies the contract added in application.hpp menu::check_resolve_state /
_attach_popup_overlay / load_item:

  1. The XML <item type="check" ...> schema is parsed by menu::load_item: the
     row renders inside a dropdown popup with a checkbox in the left gutter —
     □ (U+25A1) when unchecked, ▣ (U+25A3) when checked.

  2. checked="true" is honoured as the static fallback when the item carries
     no event-updater script.

  3. The check gutter is reserved popup-wide (same precedent as the submenu
     chevron column): plain rows in the same popup have their labels aligned
     with the check rows' labels.

  4. Clicking a check row dispatches its script and tears the chain down
     (re-clicking the trigger re-opens the popup).

  5. Dynamic (the headline ExclusiveKeyboard use case): a check row whose
     <script> updater publishes vtm.item.Check(vtm.terminal.ExclusiveKeyboardMode())
     renders the LIVE mode at popup-open, does NOT toggle the mode merely by
     opening the popup (the OnLeftClick script must be skipped during state
     resolution), and the box flips after the row is clicked.

The harness is shared with test_radiomenu.py: vtm-tile is driven via a pty
with SGR mouse + raw input, and the cumulative paint stream is scanned for
markers and glyphs.
"""

import os
import sys
import time
import tempfile

from test_radiomenu import (
    VtmTileSession,
    find_marker_position,
    kill_all_vtm,
    fail,
    VTM_TILE_BINARY,
)

# Checkbox glyphs painted in the popup's left gutter.
CHECK_FILLED = "▣"  # U+25A3  checked row
CHECK_HOLLOW = "□"  # U+25A1  unchecked row
CHECK_FILLED_B = CHECK_FILLED.encode()
CHECK_HOLLOW_B = CHECK_HOLLOW.encode()

# Distinctive markers so screen scans are unambiguous.
TRIGGER_LABEL = "  [CHECKMENU]  "
CHK_OFF = "ChkRowOff-MARK"   # no checked attr: renders □.
CHK_ON = "ChkRowOn-MARK"     # checked="true": renders ▣.
PLAIN = "PlainRow-MARK"      # plain leaf sharing the popup (gutter alignment).

# Self-contained tile config: a dropdown trigger holding two static check
# rows (one checked) and one plain row. The check rows carry only an empty
# click script — no event-updater — so check_resolve_state must fall through
# to the static checked= value. The dtvt child clears its own menu (-c) so
# no native glyphs collide with markers.
CHECK_TILE_CONFIG = (
    "<config>"
        "<tile>"
            "<confirm_close=0/>"
            '<app selected="term">'
                "<item*/>"
                '<item id="term" label="term" type="dtvt"'
                ' cmd="$0 -c \'<config><terminal><menu item*></menu></terminal></config>\' -r term"/>'
            "</app>"
            "<menu item*>"
                f'<item type="dropdown" label="{TRIGGER_LABEL}" tooltip=" chk " item*>'
                    f'<item type="check" label="{CHK_OFF}" tooltip=" off " script=\'OnLeftClick|\'/>'
                    f'<item type="check" label="{CHK_ON}" checked="true" tooltip=" on " script=\'OnLeftClick|\'/>'
                    f'<item label="{PLAIN}" tooltip=" plain " script=\'OnLeftClick|\'/>'
                "</item>"
            "</menu>"
        "</tile>"
    "</config>"
)
CHECK_TILE_ARGS = []  # CHECK_TILE_CONFIG is shipped via $VTM_CONFIG.


# ---------------------------------------------------------------------------
# Dynamic check row wired to live terminal state (the headline use case).
# ---------------------------------------------------------------------------
# Mirrors /Menu/Buttons/ExclusiveKeyboardCheck in vtm.xml: the row's <script>
# updater publishes the live exclusive-keyboard mode via vtm.item.Check() and
# its OnLeftClick script toggles the mode. For vtm.terminal to resolve, the
# menu must live in the TERMINAL process, so this config is loaded by the
# dtvt child via `-c <file>` (a temp file, to avoid inline-quote escaping).
DYN_TRIGGER = "  [TERMMENU]  "
DYN_CHECK_ROW = "ExclKbd-MARK"


def _write_dyn_term_menu():
    """Write the terminal-process menu config (dropdown > check row) to a
    temp file and return its path. Caller is responsible for deleting it."""
    cfg = (
        "<config><terminal><menu item*>"
        f'<item type="dropdown" label="{DYN_TRIGGER}" item*>'
        f'<item type="check" label="{DYN_CHECK_ROW}">'
        "<script>"
        '<on="release: e2::form::upon::started"  source="applet"/>'
        '<on="release: terminal::events::rawkbd" source="terminal"/>'
        "vtm.item.Check(vtm.terminal.ExclusiveKeyboardMode())"
        "</script>"
        "<script=OnLeftClick|ExclusiveKeyboardMode/>"
        "</item>"
        "</item>"
        "</menu></terminal></config>"
    )
    fd, path = tempfile.mkstemp(prefix="vtm_checkmenu_", suffix=".xml")
    with os.fdopen(fd, "w") as fh:
        fh.write(cfg)
    return path


def _open_check_popup(s, trigger_marker):
    """Locate + click the dropdown trigger; return (trigger_row, trigger_col)
    or None on failure. Resets the buffer first so the cumulative stream
    holds only the fresh popup paint."""
    coords = find_marker_position(s._screen_buf, trigger_marker)
    if coords is None:
        return None
    trigger_row, trigger_col = coords
    s.reset_buffer()
    s.click(trigger_col + 2, trigger_row)
    s.snapshot(timeout=1.5)
    return (trigger_row, trigger_col)


def test_check_rows_render_boxes_and_align():
    """Open the popup: the unchecked row must carry □, the checked='true' row
    must carry ▣ (static fallback), each box must sit in the left gutter
    before its label, and the plain row's label must align with the check
    rows' labels (popup-wide gutter reservation)."""
    print("TEST: check: ▣/□ boxes rendered, static checked, gutter aligned ... ",
          end="", flush=True)
    with VtmTileSession(CHECK_TILE_ARGS, vtm_config=CHECK_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.snapshot(timeout=2.0)

        if _open_check_popup(s, "[CHECKMENU]") is None:
            return fail(
                "dropdown trigger '[CHECKMENU]' not rendered — the "
                "<item type='check'> XML may not have been parsed"
            )
        rendered = s.snapshot(timeout=1.0)
        for marker in (CHK_OFF, CHK_ON, PLAIN):
            if marker not in rendered:
                return fail(f"popup did not paint row '{marker}'")

        raw = s._screen_buf
        if raw.count(CHECK_FILLED_B) < 1:
            return fail("no ▣ emitted — the checked='true' row was not rendered filled")
        if raw.count(CHECK_HOLLOW_B) < 1:
            return fail("no □ emitted — the unchecked row was not rendered hollow")

        off_pos = find_marker_position(raw, CHK_OFF)
        on_pos = find_marker_position(raw, CHK_ON)
        plain_pos = find_marker_position(raw, PLAIN)
        hollow_pos = find_marker_position(raw, CHECK_HOLLOW_B)
        filled_pos = find_marker_position(raw, CHECK_FILLED_B)
        if None in (off_pos, on_pos, plain_pos, hollow_pos, filled_pos):
            return fail("could not locate all rows / box glyphs")

        if hollow_pos[0] != off_pos[0]:
            return fail(
                f"□ painted on row {hollow_pos[0]} but the unchecked option "
                f"'{CHK_OFF}' is on row {off_pos[0]}"
            )
        if filled_pos[0] != on_pos[0]:
            return fail(
                f"▣ painted on row {filled_pos[0]} but the checked option "
                f"'{CHK_ON}' is on row {on_pos[0]} — the static "
                f"checked='true' fallback was not honoured"
            )
        if hollow_pos[1] >= off_pos[1] or filled_pos[1] >= on_pos[1]:
            return fail(
                f"box not in the left gutter before the label "
                f"(□ col {hollow_pos[1]} vs label col {off_pos[1]}, "
                f"▣ col {filled_pos[1]} vs label col {on_pos[1]})"
            )
        if plain_pos[1] != off_pos[1]:
            return fail(
                f"plain row label at col {plain_pos[1]} but check row labels "
                f"at col {off_pos[1]} — the check gutter is not reserved "
                f"popup-wide"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during check-render test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print(f"PASS (□ {hollow_pos} ▣ {filled_pos}, labels col {off_pos[1]})")
        return True


def test_check_click_dismisses_chain():
    """Clicking a check row must dispatch its script and tear the chain
    down. Verified by re-clicking the trigger and confirming the popup
    re-opens (only possible once menu.dropdown.open was cleared)."""
    print("TEST: check: clicking a row dismisses the chain ... ",
          end="", flush=True)
    with VtmTileSession(CHECK_TILE_ARGS, vtm_config=CHECK_TILE_CONFIG) as s:
        if not s.is_alive():
            return fail("vtm-tile did not start")
        s.snapshot(timeout=2.0)

        opened = _open_check_popup(s, "[CHECKMENU]")
        if opened is None:
            return fail("dropdown trigger '[CHECKMENU]' not found")
        trigger_row, trigger_col = opened
        rendered = s.snapshot(timeout=1.0)
        if CHK_OFF not in rendered:
            return fail("popup did not open on first click")

        off_pos = find_marker_position(s._screen_buf, CHK_OFF)
        if off_pos is None:
            return fail("could not locate the check row")

        # Click the check row.
        s.reset_buffer()
        s.click(off_pos[1] + 2, off_pos[0])
        time.sleep(0.4)
        s.snapshot(timeout=1.5)

        # Re-click the trigger: must re-open if the chain dismissed.
        s.reset_buffer()
        s.click(trigger_col + 2, trigger_row)
        reopened = s.snapshot(timeout=1.5)
        if CHK_ON not in reopened:
            return fail(
                "after row click + re-trigger, popup did not re-open — "
                "the check row click likely failed to dismiss the chain"
            )

        if not s.is_alive():
            return fail("vtm-tile crashed during row-click test")
        if not s.normal_exit():
            return fail("vtm-tile did not exit cleanly")
        print("PASS")
        return True


def test_check_dynamic_tracks_exclusive_keyboard_mode():
    """End-to-end ExclusiveKeyboard contract: the box must render the LIVE
    mode at popup-open (□ initially), must NOT flip merely from opening and
    re-opening the popup (the OnLeftClick toggle is skipped during state
    resolution), and must flip to ▣ after the row is clicked — then back to
    □ on a second click."""
    print("TEST: check: dynamic box tracks live ExclusiveKeyboardMode ... ",
          end="", flush=True)
    cfg_path = _write_dyn_term_menu()
    tile_config = (
        "<config><tile><confirm_close=0/>"
        '<app selected="term"><item*/>'
        f'<item id="term" label="term" type="dtvt" cmd="$0 -c {cfg_path} -r term"/>'
        "</app><menu item*></menu></tile></config>"
    )

    def open_and_read_box(s):
        """Open the dropdown; return 'checked', 'unchecked', or None."""
        if _open_check_popup(s, "[TERMMENU]") is None:
            return None
        raw = s._screen_buf
        row = find_marker_position(raw, DYN_CHECK_ROW)
        if row is None:
            return None
        filled = find_marker_position(raw, CHECK_FILLED_B)
        hollow = find_marker_position(raw, CHECK_HOLLOW_B)
        if filled is not None and filled[0] == row[0]:
            return "checked"
        if hollow is not None and hollow[0] == row[0]:
            return "unchecked"
        return None

    def dismiss(s):
        s.write(b"\x1b")
        time.sleep(0.3)
        s.snapshot(timeout=0.5)

    try:
        with VtmTileSession([], vtm_config=tile_config) as s:
            if not s.is_alive():
                return fail("vtm-tile did not start")
            # The dtvt child loads its menu from the -c <file>; give it a beat
            # longer than an inline-config session to come up and paint.
            time.sleep(1.0)
            s.snapshot(timeout=3.0)

            state1 = open_and_read_box(s)
            if state1 is None:
                return fail(
                    "dropdown '[TERMMENU]' or its check row did not render — "
                    "the dynamic check config was not loaded"
                )
            if state1 != "unchecked":
                return fail(
                    f"fresh session: expected □ (mode off), saw {state1} — "
                    f"opening the popup may have toggled the mode"
                )

            # Open/close cycle must not change the state (the OnLeftClick
            # toggle script must not run during check_resolve_state).
            dismiss(s)
            state2 = open_and_read_box(s)
            if state2 != "unchecked":
                return fail(
                    f"after a plain open/close cycle the box became "
                    f"{state2} — popup-open resolution ran the toggle script"
                )

            # Click the row: toggles the mode and dismisses the chain.
            row = find_marker_position(s._screen_buf, DYN_CHECK_ROW)
            if row is None:
                return fail("could not locate the check row to click")
            s.click(row[1] + 2, row[0])
            time.sleep(0.4)
            s.snapshot(timeout=1.5)

            state3 = open_and_read_box(s)
            if state3 != "checked":
                return fail(
                    f"after clicking the row, expected ▣ (mode on), saw "
                    f"{state3} — vtm.item.Check did not publish the new "
                    f"live state at popup-open"
                )

            # Click again: back to unchecked.
            row = find_marker_position(s._screen_buf, DYN_CHECK_ROW)
            if row is None:
                return fail("could not locate the check row for the second click")
            s.click(row[1] + 2, row[0])
            time.sleep(0.4)
            s.snapshot(timeout=1.5)

            state4 = open_and_read_box(s)
            if state4 != "unchecked":
                return fail(
                    f"after the second click, expected □ (mode off again), "
                    f"saw {state4}"
                )

            if not s.is_alive():
                return fail("vtm-tile crashed during dynamic check test")
            if not s.normal_exit():
                return fail("vtm-tile did not exit cleanly")
            print("PASS (□ -> □ -> ▣ -> □)")
            return True
    finally:
        try:
            os.unlink(cfg_path)
        except OSError:
            pass


TESTS = [
    test_check_rows_render_boxes_and_align,
    test_check_click_dismisses_chain,
    test_check_dynamic_tracks_exclusive_keyboard_mode,
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
