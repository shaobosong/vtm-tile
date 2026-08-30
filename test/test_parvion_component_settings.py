#!/usr/bin/env python3
# Copyright (c) Shaobo Song
# Licensed under the MIT license.

"""Focused TUI coverage for the component-based Settings Connection page."""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_parvion_panes as T

kill_all_vtm = T.kill_all_vtm


def session(cfgdir):
    return T.ParvionSession(os.getcwd(), env={
        "PARVION_DEMO_QUEUE": "0",
        "XDG_CONFIG_HOME": cfgdir,
    })


def blob(chars):
    return "\n".join(T.row_text(chars, row) for row in range(len(chars)))


def open_dialog(s):
    edit = T.find_text(s.screen()[0], "Edit")
    if not edit:
        return None
    s.click(edit[1] + 2, edit[0] + 1)
    settings = T.find_text(s.screen()[0], "Settings")
    if not settings:
        return None
    s.click(settings[1] + 1, settings[0] + 1)
    return s.screen()[0]


def settings_file(cfgdir):
    path = os.path.join(cfgdir, "parvion", "settings")
    if not os.path.isfile(path):
        return {}
    result = {}
    with open(path) as stream:
        for line in stream:
            if "\t" in line:
                key, value = line.rstrip("\n").split("\t", 1)
                result[key] = value
    return result


def replace_field(s, label, shared_label_width, value):
    chars = s.screen()[0]
    pos = T.find_text(chars, label)
    if not pos:
        return False
    field_col = pos[1] + shared_label_width + 1
    s.click(field_col + 5, pos[0] + 1)
    s.write("\x7f" * 16)
    if value:
        s.write(value)
    return True


def test_structure_and_tabs():
    print("TEST: component Settings structure and ported tabs ... ", end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pv_component_settings_")
    with session(cfg) as s:
        chars = open_dialog(s)
        if chars is None:
            print("FAIL - dialog did not open")
            return False
        text = blob(chars)
        for needle in ("Settings", "Connection", "SFTP", "Site", "Debug", "OK", "Cancel",
                       "Timeout", "Timeout in seconds", "(10-9999, 0 to disable)",
                       "Reconnection settings", "Maximum number of retries",
                       "Delay between failed attempts", "(0-999 seconds)",
                       "Please note that some servers might ban you"):
            if needle not in text:
                print(f"FAIL - '{needle}' missing")
                return False
        delay = T.find_text(chars, "Delay between failed attempts:")
        if not delay or "Parvion will retry" not in T.row_text(chars, delay[0] + 1):
            print("FAIL - Reconnection settings reserved an unused scrollbar row")
            return False
        timeout = T.find_text(chars, "Timeout in seconds:")
        if not timeout or "If no data" not in T.row_text(chars, timeout[0] + 1):
            print("FAIL - Timeout reserved an unused scrollbar row")
            return False
        sftp = T.find_text(chars, "SFTP")
        s.click(sftp[1] + 1, sftp[0] + 1)
        if "Public Key Authentication" not in blob(s.screen()[0]):
            print("FAIL - ported SFTP page did not appear")
            return False
        s.click(1, 1)
        if "Public Key Authentication" in blob(s.screen()[0]):
            print("FAIL - backdrop click did not dismiss the dialog")
            return False
    print("PASS")
    return True


def test_connection_edits_persist_and_clamp():
    print("TEST: component Connection inputs persist and clamp ... ", end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pv_component_settings_")
    with session(cfg) as s:
        chars = open_dialog(s)
        if chars is None:
            print("FAIL - dialog did not open")
            return False
        if not replace_field(s, "Timeout in seconds:", len("Timeout in seconds:"), "1"):
            print("FAIL - Timeout field missing")
            return False
        if not replace_field(s, "Maximum number of retries:",
                             len("Delay between failed attempts:"), "120"):
            print("FAIL - retry field missing")
            return False
        if not replace_field(s, "Delay between failed attempts:",
                             len("Delay between failed attempts:"), ""):
            print("FAIL - delay field missing")
            return False
        chars = s.screen()[0]
        ok = T.find_text(chars, " OK ")
        if not ok:
            print("FAIL - OK button missing")
            return False
        s.click(ok[1] + 2, ok[0] + 1)
        s.feed(0.8)
    values = settings_file(cfg)
    expected = {"Timeout": "10", "Reconnect count": "99", "Reconnect delay": "0"}
    for key, value in expected.items():
        if values.get(key) != value:
            print(f"FAIL - {key} persisted as {values.get(key)!r}, want {value!r}")
            return False
    print("PASS")
    return True


def test_cancel_and_escape_do_not_persist():
    print("TEST: component Connection cancellation discards edits ... ", end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pv_component_settings_")
    settings_dir = os.path.join(cfg, "parvion")
    os.makedirs(settings_dir)
    settings_path = os.path.join(settings_dir, "settings")
    with open(settings_path, "w") as stream:
        stream.write("Timeout\t40\nReconnect count\t3\nReconnect delay\t8\n")
    with session(cfg) as s:
        if open_dialog(s) is None or not replace_field(
                s, "Timeout in seconds:", len("Timeout in seconds:"), "90"):
            print("FAIL - dialog or Timeout field missing")
            return False
        chars = s.screen()[0]
        cancel = T.find_text(chars, " Cancel ")
        s.click(cancel[1] + 2, cancel[0] + 1)
        s.feed(0.6)
        if open_dialog(s) is None or not replace_field(
                s, "Timeout in seconds:", len("Timeout in seconds:"), "91"):
            print("FAIL - dialog did not reopen")
            return False
        s.write("\x1b")
        s.feed(0.6)
    values = settings_file(cfg)
    if values.get("Timeout") != "40":
        print(f"FAIL - cancelled Timeout persisted as {values.get('Timeout')!r}")
        return False
    print("PASS")
    return True


def test_constrained_connection_scrollview():
    print("TEST: constrained Connection page wraps and scrolls ... ", end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pv_component_settings_")
    old_rows, old_cols = T.ROWS, T.COLS
    T.ROWS, T.COLS = 12, 70
    try:
        with session(cfg) as s:
            chars = open_dialog(s)
            if chars is None:
                print("FAIL - dialog did not open")
                return False
            page = blob(chars)
            if "Timeout in seconds" not in page:
                print("FAIL - first groupbox is not visible")
                return False
            found = False
            for _ in range(30):
                os.write(s.master_fd, b"\x1b[<65;35;6M")
                s.feed(0.08)
                page = blob(s.screen()[0])
                if "Connection" in page and "Delay between failed attempts" in page:
                    found = True
                    break
            if not found:
                print("FAIL - wheel input did not reveal the reconnection fields")
                return False
    finally:
        T.ROWS, T.COLS = old_rows, old_cols
    print("PASS")
    return True


def test_small_reconnection_fields_scroll_but_wrapped_hint_stays_put():
    print("TEST: small Reconnection fields scroll without moving wrapped hint ... ",
          end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pv_component_reconnect_scroll_")
    old_rows, old_cols = T.ROWS, T.COLS
    T.ROWS, T.COLS = 20, 25
    try:
        with session(cfg) as s:
            if open_dialog(s) is None:
                print("FAIL - dialog did not open")
                return False

            # Reveal the Reconnection settings fields using the outer page viewport.
            # The expanded task-03 help now needs more rows than this deliberately
            # narrow screen, so the fields and complete hint are not expected to fit
            # at once.
            for _ in range(8):
                os.write(s.master_fd, b"\x1b[<65;20;5M")
            s.feed(0.7)
            before = s.screen()[0]
            if not T.find_text(before, "Reconnection set") or not T.find_text(before, "Maximum number"):
                print("FAIL - narrow Reconnection settings fields are unavailable")
                return False
            title = T.find_text(before, "Reconnection set")
            leading = T.find_text(before, "Maximum number")

            scrollbar_row = title[0] + 3
            field_row = T.row_text(before, leading[0])
            right_border = field_row.find("│", leading[1])
            if (right_border < 1 or field_row[right_border - 1] != " "
                    or "▂" not in T.row_text(before, scrollbar_row)):
                print("FAIL - horizontal scrollbar or groupbox right edge is missing")
                return False
            # A genuine horizontal wheel event follows the nested X axis without
            # moving the outer page vertically.
            for _ in range(30):
                os.write(s.master_fd,
                         f"\x1b[<67;{leading[1] + 1};{leading[0] + 1}M".encode())
            s.feed(0.7)
            after = s.screen()[0]
            retry_range = T.find_text(after, "0 for unlimited)")
            delay_range = T.find_text(after, "seconds)")
            if (not retry_range or not delay_range
                    or T.find_text(after, "Maximum number")):
                print("FAIL - narrow Reconnection fields did not scroll horizontally")
                return False

            range_row = T.row_text(after, delay_range[0])
            right_border = range_row.find("│", delay_range[1])
            if (right_border < 1 or range_row[right_border - 1] != " "
                    or "▂" not in T.row_text(after, scrollbar_row)):
                print("FAIL - horizontal scrolling damaged the groupbox frame")
                return False
    finally:
        T.ROWS, T.COLS = old_rows, old_cols
    print("PASS")
    return True


def test_small_timeout_fields_scroll_but_wrapped_hint_stays_put():
    print("TEST: small Timeout fields scroll without moving wrapped hint ... ",
          end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pv_component_timeout_scroll_")
    old_rows, old_cols = T.ROWS, T.COLS
    T.ROWS, T.COLS = 20, 25
    try:
        with session(cfg) as s:
            before = open_dialog(s)
            if before is None:
                print("FAIL - dialog did not open")
                return False
            title = T.find_text(before, "Timeout")
            leading = T.find_text(before, "Timeout in secon")
            hint_start = T.find_text(before, "If no data is")
            if not title or not leading or not hint_start:
                print("FAIL - narrow Timeout content is incomplete")
                return False
            scrollbar_row = title[0] + 2
            wrapped_before = [T.row_text(before, row)
                              for row in range(hint_start[0], 17)]
            if "▂" not in T.row_text(before, scrollbar_row):
                print("FAIL - Timeout horizontal scrollbar is missing")
                return False

            for _ in range(30):
                os.write(s.master_fd,
                         f"\x1b[<67;{leading[1] + 1};{leading[0] + 1}M".encode())
            s.feed(0.7)
            after = s.screen()[0]
            trailing = T.find_text(after, "9, 0 to disable)")
            wrapped_after = [T.row_text(after, row)
                             for row in range(hint_start[0], 17)]
            if (not trailing or T.find_text(after, "Timeout in secon")
                    or T.find_text(after, "If no data is") != hint_start
                    or wrapped_after != wrapped_before):
                print("FAIL - Timeout fields did not scroll independently of the hint")
                return False
            row = T.row_text(after, trailing[0])
            border = row.find("│", trailing[1])
            if border < 1 or row[border - 1] != " ":
                print("FAIL - Timeout scrolling damaged the groupbox frame")
                return False
    finally:
        T.ROWS, T.COLS = old_rows, old_cols
    print("PASS")
    return True


TESTS = [
    test_structure_and_tabs,
    test_connection_edits_persist_and_clamp,
    test_cancel_and_escape_do_not_persist,
    test_constrained_connection_scrollview,
    test_small_reconnection_fields_scroll_but_wrapped_hint_stays_put,
    test_small_timeout_fields_scroll_but_wrapped_hint_stays_put,
]


def main():
    if not os.path.isfile(T.VTM_TILE_BINARY):
        print(f"ERROR: vtm-tile binary not found at {T.VTM_TILE_BINARY}")
        return 1
    passed = 0
    for test in TESTS:
        try:
            ok = test()
        except Exception as error:
            print(f"FAIL - exception: {error}")
            ok = False
        passed += int(ok)
        kill_all_vtm()
    print(f"\n{passed}/{len(TESTS)} component Settings tests passed")
    return 0 if passed == len(TESTS) else 1


if __name__ == "__main__":
    sys.exit(main())
