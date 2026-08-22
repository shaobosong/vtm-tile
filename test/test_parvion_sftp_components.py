#!/usr/bin/env python3
# Copyright (c) Shaobo Song
# Licensed under the MIT license.

"""Focused TUI coverage for the retained-component Settings SFTP page."""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_parvion_panes as T
import test_parvion_component_settings as C

kill_all_vtm = T.kill_all_vtm

TEXT_FG = (205, 214, 244)
DISABLED_FG = (108, 112, 134)


def session(cfgdir):
    return T.ParvionSession(os.getcwd(), env={
        "PARVION_DEMO_QUEUE": "0",
        "XDG_CONFIG_HOME": cfgdir,
    })


def page_text(chars):
    return "\n".join(T.row_text(chars, row) for row in range(len(chars)))


def goto_sftp(s):
    chars = s.screen()[0]
    tab = T.find_text(chars, "SFTP")
    if not tab:
        return None
    s.click(tab[1] + 1, tab[0] + 1)
    return s.screen()[0]


def replace_parallel_field(s, label, value):
    chars = s.screen()[0]
    position = T.find_text(chars, label)
    if not position:
        return False
    shared_width = len("Enable parallel transfers for files larger than:")
    column = position[1] + shared_width + 1
    s.click(column + 5, position[0] + 1)
    s.write("\x7f" * 24)
    if value:
        s.write(value)
    return True


def select_dropdown(s, current, target):
    chars = s.screen()[0]
    trigger = T.find_text(chars, current)
    if not trigger:
        return False
    s.click(trigger[1] + 1, trigger[0] + 1)
    chars = s.screen()[0]
    option = T.find_text(chars, target)
    if not option:
        return False
    s.click(option[1] + 1, option[0] + 1)
    return T.find_text(s.screen()[0], target) is not None


def settings_values(cfgdir):
    path = os.path.join(cfgdir, "parvion", "settings")
    values = {}
    if not os.path.isfile(path):
        return values
    with open(path, encoding="utf-8") as stream:
        for line in stream:
            if "\t" in line:
                key, value = line.rstrip("\n").split("\t", 1)
                values[key] = value
    return values


def test_sftp_component_structure():
    print("TEST: component Settings SFTP structure ... ", end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pv_sftp_components_")
    with session(cfg) as s:
        if C.open_dialog(s) is None:
            print("FAIL - dialog did not open")
            return False
        chars = goto_sftp(s)
        if chars is None:
            print("FAIL - SFTP tab was not found")
            return False
        text = page_text(chars)
        expected = (
            "Public Key Authentication", "Private keys:", "Filename", "Comment", "Data",
            "Add key file", "Remove key", "Hash verification",
            "Calculate target file hash during transfers:", "None",
            "Other SFTP options", "Enable compression", "Parallel transfers",
            "Enable parallel transfers for files larger than:", "MiB",
            "Maximum parallel transfer connections:", "Channel allocation:",
            "Strict queue order",
        )
        for needle in expected:
            if needle not in text:
                print(f"FAIL - {needle!r} missing")
                return False
        allocation = T.find_text(chars, "Channel allocation:")
        if not allocation or "└" not in T.row_text(chars, allocation[0] + 1):
            print("FAIL - Parallel transfers reserved an unused scrollbar row")
            return False
        hash_label = T.find_text(chars, "Calculate target file hash during transfers:")
        compression = T.find_text(chars, "Enable compression")
        if (not hash_label or "└" not in T.row_text(chars, hash_label[0] + 1)
                or not compression or "└" not in T.row_text(chars, compression[0] + 1)):
            print("FAIL - SFTP groupbox reserved an unused scrollbar row")
            return False
    print("PASS")
    return True


def test_remove_key_button_tracks_table_selection():
    print("TEST: component Remove key button follows table selection ... ",
          end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pv_sftp_remove_button_")
    settings_dir = os.path.join(cfg, "parvion")
    os.makedirs(settings_dir)
    with open(os.path.join(settings_dir, "settings"), "w", encoding="utf-8") as stream:
        stream.write("SFTP keyfile\t/keys/remove_me.pem\n")

    with session(cfg) as s:
        if C.open_dialog(s) is None or goto_sftp(s) is None:
            print("FAIL - SFTP page did not open")
            return False

        chars, _, fg = s.screen_with_fg()
        remove = T.find_text(chars, "Remove key")
        if not remove:
            print("FAIL - Remove key button missing")
            return False
        if fg[remove[0]][remove[1]] != DISABLED_FG:
            print("FAIL - Remove key was not disabled without a selection")
            return False
        s.click(remove[1] + 1, remove[0] + 1)
        if not T.grid_contains(s.screen()[0], "remove_me.pem"):
            print("FAIL - disabled Remove key removed an unselected key")
            return False

        key = T.find_text(s.screen()[0], "remove_me.pem")
        s.click(key[1] + 1, key[0] + 1)
        chars, _, fg = s.screen_with_fg()
        remove = T.find_text(chars, "Remove key")
        if fg[remove[0]][remove[1]] != TEXT_FG:
            print("FAIL - Remove key did not enable after selecting a row")
            return False

        s.click(key[1] + 1, key[0] + 3)
        chars, _, fg = s.screen_with_fg()
        remove = T.find_text(chars, "Remove key")
        if fg[remove[0]][remove[1]] != DISABLED_FG:
            print("FAIL - Remove key did not disable after clearing the selection")
            return False

        key = T.find_text(chars, "remove_me.pem")
        s.click(key[1] + 1, key[0] + 1)
        remove = T.find_text(s.screen()[0], "Remove key")
        s.click(remove[1] + 1, remove[0] + 1)
        chars, _, fg = s.screen_with_fg()
        remove = T.find_text(chars, "Remove key")
        if T.grid_contains(chars, "remove_me.pem"):
            print("FAIL - enabled Remove key did not remove the selected row")
            return False
        if fg[remove[0]][remove[1]] != DISABLED_FG:
            print("FAIL - Remove key did not disable after clearing the selection")
            return False
    print("PASS")
    return True


def test_sftp_controls_persist_clamp_and_keep_dropdown_width():
    print("TEST: component SFTP controls persist, clamp, and stay fixed-width ... ",
          end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pv_sftp_components_")
    with session(cfg) as s:
        if C.open_dialog(s) is None or goto_sftp(s) is None:
            print("FAIL - SFTP page did not open")
            return False

        chars = s.screen()[0]
        none = T.find_text(chars, "None")
        if not none:
            print("FAIL - hash dropdown missing")
            return False
        hash_arrow_before = T.row_text(chars, none[0]).find("▾", none[1])

        if not replace_parallel_field(s,
                "Enable parallel transfers for files larger than:", "0"):
            print("FAIL - threshold input missing")
            return False
        if not replace_parallel_field(s,
                "Maximum parallel transfer connections:", "99"):
            print("FAIL - maximum-connections input missing")
            return False

        compression = T.find_text(s.screen()[0], "Enable compression")
        if not compression:
            print("FAIL - compression checkbox missing")
            return False
        blank_column = compression[1] + len("Enable compression") + 5
        s.click(blank_column + 1, compression[0] + 1)
        if not T.find_text(s.screen()[0], "□ Enable compression"):
            print("FAIL - blank space after compression label toggled the checkbox")
            return False
        s.click(compression[1] + 1, compression[0] + 1)

        if not select_dropdown(s, "None", "SHA-512"):
            print("FAIL - hash dropdown selection failed")
            return False
        chars = s.screen()[0]
        selected_hash = T.find_text(chars, "SHA-512")
        hash_arrow_after = T.row_text(chars, selected_hash[0]).find("▾", selected_hash[1])
        if hash_arrow_before < 0 or hash_arrow_after != hash_arrow_before:
            print(f"FAIL - hash dropdown width changed ({hash_arrow_before} -> {hash_arrow_after})")
            return False

        if not select_dropdown(s, "MiB", "GiB"):
            print("FAIL - unit dropdown selection failed")
            return False
        if not select_dropdown(s, "Strict queue order", "New file first"):
            print("FAIL - allocation dropdown selection failed")
            return False

        chars = s.screen()[0]
        ok = T.find_text(chars, " OK ")
        if not ok:
            print("FAIL - OK button missing")
            return False
        s.click(ok[1] + 2, ok[0] + 1)
        s.feed(0.8)

    values = settings_values(cfg)
    expected = {
        "SFTP compression": "1",
        "SFTP parallel transfer threshold value": "1",
        "SFTP parallel transfer threshold unit": "3",
        "SFTP parallel max connections": "10",
        "SFTP transfer queue allocation": "1",
        "Hash on transfer": "1",
        "Hash algorithm": "4",
    }
    for key, wanted in expected.items():
        if values.get(key) != wanted:
            print(f"FAIL - {key}={values.get(key)!r}, want {wanted!r}")
            return False
    print("PASS")
    return True


def test_narrow_sftp_groupbox_inner_clipping():
    print("TEST: narrow SFTP groupboxes preserve right padding and border ... ",
          end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pv_sftp_groupbox_clip_")
    old_rows, old_cols = T.ROWS, T.COLS
    T.ROWS, T.COLS = 20, 25

    def intact_right_edge(chars, needle):
        position = T.find_text(chars, needle)
        if not position:
            return False
        row = T.row_text(chars, position[0])
        border = row.find("│", position[1])
        return border > 0 and row[border - 1] == " "

    try:
        with session(cfg) as s:
            if C.open_dialog(s) is None or goto_sftp(s) is None:
                print("FAIL - SFTP page did not open")
                return False
            # Move below the internally bounded private-key table to the two
            # groupboxes under test. Vertical wheel input passes through any
            # nested horizontal-only viewport crossed along the way.
            for _ in range(40):
                os.write(s.master_fd, b"\x1b[<65;20;5M")
            s.feed(0.6)
            chars = s.screen()[0]
            cases = (
                ("Enable compres", "Enable compression"),
                ("Enable parallel", "Enable parallel transfers for files larger than:"),
            )
            for visible, complete in cases:
                if not intact_right_edge(chars, visible) or T.find_text(chars, complete):
                    print(f"FAIL - {visible!r} overwrote its groupbox right edge")
                    return False
    finally:
        T.ROWS, T.COLS = old_rows, old_cols
    print("PASS")
    return True


def test_small_parallel_transfers_horizontal_scrollview():
    print("TEST: small Parallel transfers groupbox scrolls horizontally ... ",
          end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pv_sftp_parallel_scroll_")
    old_rows, old_cols = T.ROWS, T.COLS
    T.ROWS, T.COLS = 20, 25
    try:
        with session(cfg) as s:
            if C.open_dialog(s) is None or goto_sftp(s) is None:
                print("FAIL - SFTP page did not open")
                return False

            # Scroll the outer page until the Parallel transfers groupbox is
            # visible. Keep the pointer above its nested horizontal viewport.
            for _ in range(40):
                os.write(s.master_fd, b"\x1b[<65;20;5M")
            s.feed(0.6)
            before = s.screen()[0]
            title = T.find_text(before, "Parallel transfe")
            leading = T.find_text(before, "Enable parallel")
            if not title or not leading:
                print("FAIL - Parallel transfers groupbox was not revealed")
                return False

            border_row = T.row_text(before, leading[0])
            right_border = border_row.find("│", leading[1])
            scrollbar_row = title[0] + 4
            if (right_border < 1 or border_row[right_border - 1] != " "
                    or "▂" not in T.row_text(before, scrollbar_row)):
                print("FAIL - horizontal scrollbar or groupbox right edge is missing")
                return False

            # A genuine horizontal wheel event exposes the fixed grid's
            # trailing controls.
            wheel_x = max(2, leading[1] + 1)
            wheel_y = leading[0] + 1
            for _ in range(30):
                os.write(s.master_fd,
                         f"\x1b[<67;{wheel_x};{wheel_y}M".encode())
            s.feed(0.7)
            after = s.screen()[0]
            trailing = T.find_text(after, "queue order")
            if not trailing or T.find_text(after, "Enable parallel"):
                print("FAIL - wheel input did not reveal trailing parallel controls")
                return False

            trailing_row = T.row_text(after, trailing[0])
            right_border = trailing_row.find("│", trailing[1])
            if (right_border < 1 or trailing_row[right_border - 1] != " "
                    or "▂" not in T.row_text(after, scrollbar_row)):
                print("FAIL - scrolling damaged the frame or added a vertical scrollbar")
                return False
    finally:
        T.ROWS, T.COLS = old_rows, old_cols
    print("PASS")
    return True


def test_small_hash_and_compression_scrollviews():
    print("TEST: small Hash and compression controls scroll horizontally ... ",
          end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pv_sftp_small_scrollviews_")
    old_rows, old_cols = T.ROWS, T.COLS
    T.ROWS, T.COLS = 20, 25
    try:
        with session(cfg) as s:
            if C.open_dialog(s) is None or goto_sftp(s) is None:
                print("FAIL - SFTP page did not open")
                return False
            for _ in range(40):
                os.write(s.master_fd, b"\x1b[<65;20;5M")
            s.feed(0.5)
            for _ in range(4):
                os.write(s.master_fd, b"\x1b[<64;20;5M")
            s.feed(0.5)
            before = s.screen()[0]
            hash_title = T.find_text(before, "Hash verificatio")
            hash_leading = T.find_text(before, "Calculate target")
            compression_title = T.find_text(before, "Other SFTP optio")
            compression_leading = T.find_text(before, "Enable compres")
            if not all((hash_title, hash_leading, compression_title, compression_leading)):
                print("FAIL - narrow Hash or compression groupbox is incomplete")
                return False
            if ("▂" not in T.row_text(before, hash_title[0] + 2)
                    or "▂" not in T.row_text(before, compression_title[0] + 2)):
                print("FAIL - Hash or compression scrollbar is missing")
                return False

            for _ in range(30):
                os.write(s.master_fd,
                         f"\x1b[<67;{hash_leading[1] + 1};{hash_leading[0] + 1}M".encode())
            s.feed(0.5)
            after_hash = s.screen()[0]
            dropdown = T.find_text(after_hash, "None")
            if not dropdown or T.find_text(after_hash, "Calculate target"):
                print("FAIL - Hash fields did not reveal the trailing dropdown")
                return False

            compression_leading = T.find_text(after_hash, "Enable compres")
            for _ in range(30):
                os.write(s.master_fd,
                         f"\x1b[<67;{compression_leading[1] + 1};{compression_leading[0] + 1}M".encode())
            s.feed(0.5)
            after = s.screen()[0]
            trailing = T.find_text(after, "able compression")
            if not trailing or T.find_text(after, "□ Enable compres"):
                print("FAIL - compression checkbox did not scroll horizontally")
                return False
            for position in (dropdown, trailing):
                row = T.row_text(after, position[0])
                border = row.find("│", position[1])
                if border < 1 or row[border - 1] != " ":
                    print("FAIL - scrolling damaged an SFTP groupbox frame")
                    return False
    finally:
        T.ROWS, T.COLS = old_rows, old_cols
    print("PASS")
    return True


def test_dropdown_open_preserves_scroll_and_hidden_trigger_closes_popup():
    print("TEST: scrolled dropdown stays put and closes when hidden ... ",
          end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pv_sftp_dropdown_scroll_")
    old_rows, old_cols = T.ROWS, T.COLS
    T.ROWS, T.COLS = 20, 80
    try:
        with session(cfg) as s:
            if C.open_dialog(s) is None or goto_sftp(s) is None:
                print("FAIL - SFTP page did not open")
                return False

            # Scroll clear of the private-key table so wheel input belongs to
            # the page viewport, then open the allocation dropdown near the
            # bottom of the retained content.
            for _ in range(24):
                os.write(s.master_fd, b"\x1b[<65;72;15M")
            s.feed(0.7)
            before = s.screen()[0]
            # At this width the fixed Parallel-transfers grid is wider than
            # the groupbox's inner viewport. The trigger is intentionally
            # clipped before its final character; the right padding and border
            # must remain available instead of completing the label by leaking.
            trigger_label = "Strict queue"
            trigger = T.find_text(before, trigger_label)
            if not trigger or T.find_text(before, "Public Key Authentication"):
                print("FAIL - allocation dropdown was not revealed by scrolling")
                return False
            trigger_row = T.row_text(before, trigger[0])
            border = trigger_row.find("│", trigger[1])
            if (border < 1 or trigger_row[border - 1] != " "
                    or "Strict queue order" in trigger_row):
                print("FAIL - parallel controls overwrote the groupbox right padding/border")
                return False
            s.click(trigger[1] + 1, trigger[0] + 1)
            opened = s.screen()[0]
            trigger_after = T.find_text(opened, trigger_label)
            if trigger_after != trigger or not T.find_text(opened, "New file first"):
                print(f"FAIL - opening moved the viewport ({trigger} -> {trigger_after})")
                return False

            # Wheel outside the popup's horizontal range until the trigger is
            # clipped above the viewport. The popup must disappear and clear
            # its open state rather than remain detached from its trigger.
            for _ in range(30):
                # Stay inside the scrollview; row 4 belongs to the page grid's
                # outer top padding and intentionally does not receive scrolling.
                os.write(s.master_fd, b"\x1b[<64;10;6M")
            s.feed(0.8)
            hidden = s.screen()[0]
            if not T.find_text(hidden, "Public Key Authentication"):
                print("FAIL - viewport did not scroll away from the trigger")
                return False
            if T.find_text(hidden, "New file first"):
                print("FAIL - popup remained visible after its trigger was hidden")
                return False

            for _ in range(24):
                os.write(s.master_fd, b"\x1b[<65;72;15M")
            s.feed(0.8)
            returned = s.screen()[0]
            trigger = T.find_text(returned, trigger_label)
            if not trigger:
                print("FAIL - allocation dropdown did not return into view")
                return False
            s.click(trigger[1] + 1, trigger[0] + 1)
            if not T.find_text(s.screen()[0], "New file first"):
                print("FAIL - one click did not reopen the dismissed dropdown")
                return False
    finally:
        T.ROWS, T.COLS = old_rows, old_cols
    print("PASS")
    return True


TESTS = [
    test_sftp_component_structure,
    test_remove_key_button_tracks_table_selection,
    test_sftp_controls_persist_clamp_and_keep_dropdown_width,
    test_narrow_sftp_groupbox_inner_clipping,
    test_small_parallel_transfers_horizontal_scrollview,
    test_small_hash_and_compression_scrollviews,
    test_dropdown_open_preserves_scroll_and_hidden_trigger_closes_popup,
]


def main():
    if not os.path.isfile(T.VTM_TILE_BINARY):
        print(f"ERROR: vtm-tile binary not found at {T.VTM_TILE_BINARY}")
        return 1
    passed = 0
    for test in TESTS:
        try:
            passed += int(test())
        except Exception as error:
            print(f"FAIL - exception: {error}")
        kill_all_vtm()
    print(f"\n{passed}/{len(TESTS)} component SFTP tests passed")
    return 0 if passed == len(TESTS) else 1


if __name__ == "__main__":
    sys.exit(main())
