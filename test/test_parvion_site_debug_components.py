#!/usr/bin/env python3
# Copyright (c) Shaobo Song
# Licensed under the MIT license.

"""Focused TUI coverage for the retained-component Settings Site and Debug pages."""

import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_parvion_panes as T
import test_parvion_component_settings as C

kill_all_vtm = T.kill_all_vtm

TEXT_FG = (205, 214, 244)
DISABLED_FG = (108, 112, 134)
SITE_FIELD_LABELS = {
    "Site Name": "Site Name:",
    "Host": "Host *:",
    "Port": "Port:",
    "User": "User:",
    "Password": "Password:",
}
SITE_LABEL_WIDTH = max(map(len, SITE_FIELD_LABELS.values()))


def session(cfgdir):
    return T.ParvionSession(os.getcwd(), env={
        "PARVION_DEMO_QUEUE": "0",
        "XDG_CONFIG_HOME": cfgdir,
    })


def page_text(chars):
    return "\n".join(T.row_text(chars, row) for row in range(len(chars)))


def goto_tab(s, label):
    chars = s.screen()[0]
    tab = T.find_text(chars, label)
    if not tab:
        return None
    s.click(tab[1] + 1, tab[0] + 1)
    return s.screen()[0]


def find_lowest(chars, needle):
    matches = []
    for row in range(len(chars)):
        column = T.row_text(chars, row).find(needle)
        if column >= 0:
            matches.append((row, column))
    return max(matches) if matches else None


def settings_values(cfgdir):
    path = os.path.join(cfgdir, "parvion", "settings")
    values = {}
    if not os.path.isfile(path):
        return values
    with open(path, encoding="utf-8") as stream:
        for line in stream:
            if "\t" in line:
                key, value = line.rstrip("\n").split("\t", 1)
                values.setdefault(key, []).append(value)
    return values


def replace_site_field(s, label, value, clear=False):
    chars = s.screen()[0]
    first = T.find_text(chars, SITE_FIELD_LABELS["Site Name"])
    if not first:
        return False
    display_label = SITE_FIELD_LABELS[label]
    position = None
    for row in range(first[0], min(len(chars), first[0] + 6)):
        column = T.row_text(chars, row).find(display_label)
        if column >= 0:
            position = (row, column)
            break
    if not position:
        return False
    field_column = first[1] + SITE_LABEL_WIDTH + 1
    s.click(field_column + 1, position[0] + 1)
    if clear:
        s.write("\x1b[F" + "\x7f" * 96)
    if value:
        s.write(value)
    return True


def click_outer_button(s, label):
    position = find_lowest(s.screen()[0], label)
    if not position:
        return False
    s.click(position[1] + 2, position[0] + 1)
    return True


def test_site_structure_add_edit_and_persist():
    print("TEST: component Site page adds, edits, and persists a site ... ",
          end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pv_site_components_")
    with session(cfg) as s:
        if C.open_dialog(s) is None:
            print("FAIL - dialog did not open")
            return False
        chars = goto_tab(s, "Site")
        if chars is None:
            print("FAIL - Site tab was not found")
            return False
        text = page_text(chars)
        expected = (
            "Site Manager", "Save SFTP connection details", "Saved SFTP sites:",
            "Site Name", "Host", "Port", "User", "No sites configured.",
            " Add ", " Edit ", " Remove ",
        )
        for needle in expected:
            if needle not in text:
                print(f"FAIL - {needle!r} missing")
                return False

        if not click_outer_button(s, " Add "):
            print("FAIL - Add button missing")
            return False
        editor_text = page_text(s.screen()[0])
        for needle in ("Add SFTP Site", *SITE_FIELD_LABELS.values()):
            if needle not in editor_text:
                print(f"FAIL - {needle!r} missing from editor")
                return False
        if "(optional)" in editor_text:
            print("FAIL - optional field annotation is still present")
            return False
        if not replace_site_field(s, "Site Name", "Production"):
            print("FAIL - Site Name field missing")
            return False
        if not replace_site_field(s, "Host", "sftp.example.test"):
            print("FAIL - Host field missing")
            return False
        if not replace_site_field(s, "Port", "", clear=True):
            print("FAIL - Port field missing")
            return False
        if not replace_site_field(s, "User", "alice"):
            print("FAIL - User field missing")
            return False
        if not replace_site_field(s, "Password", "secretpw"):
            print("FAIL - Password field missing")
            return False
        if "secretpw" in page_text(s.screen()[0]):
            print("FAIL - password rendered as plaintext")
            return False
        ok = T.find_text(s.screen()[0], " OK ")
        s.click(ok[1] + 2, ok[0] + 1)

        chars = s.screen()[0]
        for needle in ("Production", "sftp.example.test", "22", "alice"):
            if needle not in page_text(chars):
                print(f"FAIL - table row is missing {needle!r}")
                return False
        edit = find_lowest(chars, " Edit ")
        s.click(edit[1] + 2, edit[0] + 1)
        if not T.grid_contains(s.screen()[0], "Edit SFTP Site"):
            print("FAIL - selected site did not open for editing")
            return False
        if not replace_site_field(s, "Host", "edited.example.test", clear=True):
            print("FAIL - editor Host field missing")
            return False
        ok = T.find_text(s.screen()[0], " OK ")
        s.click(ok[1] + 2, ok[0] + 1)
        if not T.grid_contains(s.screen()[0], "edited.example.test"):
            print("FAIL - edited host did not refresh in the table")
            return False
        if not click_outer_button(s, " OK "):
            print("FAIL - outer OK button missing")
            return False
        s.feed(0.8)

    wanted = "Production\tedited.example.test\t22\talice\tsecretpw"
    actual = settings_values(cfg).get("Site")
    if actual != [wanted]:
        print(f"FAIL - Site persisted as {actual!r}, want {[wanted]!r}")
        return False
    print("PASS")
    return True


def test_empty_site_name_is_generated_and_stays_unique():
    print("TEST: empty Site Name generates a unique User@Host:Port name ... ",
          end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pv_site_generated_name_")
    with session(cfg) as s:
        if C.open_dialog(s) is None or goto_tab(s, "Site") is None:
            print("FAIL - Site page did not open")
            return False

        click_outer_button(s, " Add ")
        replace_site_field(s, "Host", "auto.example.test")
        replace_site_field(s, "User", "alice")
        ok = T.find_text(s.screen()[0], " OK ")
        s.click(ok[1] + 2, ok[0] + 1)
        if T.grid_contains(s.screen()[0], "Add SFTP Site"):
            print("FAIL - valid blank Site Name was not accepted")
            return False

        edit = find_lowest(s.screen()[0], " Edit ")
        s.click(edit[1] + 2, edit[0] + 1)
        replace_site_field(s, "Site Name", "", clear=True)
        replace_site_field(s, "Port", "2200", clear=True)
        ok = T.find_text(s.screen()[0], " OK ")
        s.click(ok[1] + 2, ok[0] + 1)
        if T.grid_contains(s.screen()[0], "Edit SFTP Site"):
            print("FAIL - blank Site Name was not regenerated while editing")
            return False

        # A second blank name with the same connection details resolves to the
        # same generated name and must pass through normal uniqueness checking.
        click_outer_button(s, " Add ")
        replace_site_field(s, "Host", "auto.example.test")
        replace_site_field(s, "User", "alice")
        replace_site_field(s, "Port", "2200", clear=True)
        ok = T.find_text(s.screen()[0], " OK ")
        s.click(ok[1] + 2, ok[0] + 1)
        if not T.grid_contains(s.screen()[0], "Site Name must be unique."):
            print("FAIL - duplicate generated Site Name was accepted")
            return False

        # The format is intentionally literal: keep '@' for an empty User and
        # do not bracket an IPv6 Host.
        replace_site_field(s, "Host", "2001:db8::1", clear=True)
        replace_site_field(s, "Port", "2222", clear=True)
        replace_site_field(s, "User", "", clear=True)
        ok = T.find_text(s.screen()[0], " OK ")
        s.click(ok[1] + 2, ok[0] + 1)
        if T.grid_contains(s.screen()[0], "Add SFTP Site"):
            print("FAIL - generated IPv6 Site Name was not accepted")
            return False
        click_outer_button(s, " OK ")
        s.feed(0.8)

    wanted = [
        "alice@auto.example.test:2200\tauto.example.test\t2200\talice\t",
        "@2001:db8::1:2222\t2001:db8::1\t2222\t\t",
    ]
    actual = settings_values(cfg).get("Site")
    if actual != wanted:
        print(f"FAIL - generated sites persisted as {actual!r}, want {wanted!r}")
        return False
    print("PASS")
    return True


def test_remove_site_button_tracks_table_selection():
    print("TEST: component Site Remove button follows table selection ... ",
          end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pv_site_remove_button_")
    settings_dir = os.path.join(cfg, "parvion")
    os.makedirs(settings_dir)
    with open(os.path.join(settings_dir, "settings"), "w", encoding="utf-8") as stream:
        stream.write("Site\tExisting\texisting.example.test\t22\tbob\tpassword\n")

    with session(cfg) as s:
        if C.open_dialog(s) is None or goto_tab(s, "Site") is None:
            print("FAIL - Site page did not open")
            return False

        chars, _, fg = s.screen_with_fg()
        remove = find_lowest(chars, " Remove ")
        if not remove:
            print("FAIL - Remove button missing")
            return False
        if fg[remove[0]][remove[1] + 1] != DISABLED_FG:
            print("FAIL - Remove was not disabled without a selection")
            return False
        s.click(remove[1] + 2, remove[0] + 1)
        if not T.grid_contains(s.screen()[0], "Existing"):
            print("FAIL - disabled Remove removed an unselected site")
            return False

        site = T.find_text(s.screen()[0], "Existing")
        s.click(site[1] + 1, site[0] + 1)
        chars, _, fg = s.screen_with_fg()
        remove = find_lowest(chars, " Remove ")
        if fg[remove[0]][remove[1] + 1] != TEXT_FG:
            print("FAIL - Remove did not enable after selecting a row")
            return False

        s.click(site[1] + 1, site[0] + 3)
        chars, _, fg = s.screen_with_fg()
        remove = find_lowest(chars, " Remove ")
        if fg[remove[0]][remove[1] + 1] != DISABLED_FG:
            print("FAIL - Remove did not disable after clearing the selection")
            return False

        site = T.find_text(chars, "Existing")
        s.click(site[1] + 1, site[0] + 1)
        remove = find_lowest(s.screen()[0], " Remove ")
        s.click(remove[1] + 2, remove[0] + 1)
        chars, _, fg = s.screen_with_fg()
        remove = find_lowest(chars, " Remove ")
        if T.grid_contains(chars, "Existing"):
            print("FAIL - enabled Remove did not remove the selected row")
            return False
        if fg[remove[0]][remove[1] + 1] != DISABLED_FG:
            print("FAIL - Remove did not disable after clearing the selection")
            return False
    print("PASS")
    return True


def test_site_remove_is_draft_until_outer_accept():
    print("TEST: component Site removal obeys outer Cancel and OK ... ",
          end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pv_site_draft_")
    settings_dir = os.path.join(cfg, "parvion")
    os.makedirs(settings_dir)
    settings_path = os.path.join(settings_dir, "settings")
    saved = "Existing\texisting.example.test\t22\tbob\tpassword"
    with open(settings_path, "w", encoding="utf-8") as stream:
        stream.write("Site\t" + saved + "\n")

    with session(cfg) as s:
        if C.open_dialog(s) is None or goto_tab(s, "Site") is None:
            print("FAIL - Site page did not open")
            return False
        site = T.find_text(s.screen()[0], "Existing")
        s.click(site[1] + 1, site[0] + 1)
        if not click_outer_button(s, " Remove "):
            print("FAIL - Remove button missing")
            return False
        if T.grid_contains(s.screen()[0], "Existing"):
            print("FAIL - selected site remained in the draft table")
            return False
        if not click_outer_button(s, " Cancel "):
            print("FAIL - Cancel button missing")
            return False
        s.feed(0.6)
    if settings_values(cfg).get("Site") != [saved]:
        print("FAIL - outer Cancel persisted the removal")
        return False

    with session(cfg) as s:
        if C.open_dialog(s) is None or goto_tab(s, "Site") is None:
            print("FAIL - Site page did not reopen")
            return False
        site = T.find_text(s.screen()[0], "Existing")
        s.click(site[1] + 1, site[0] + 1)
        click_outer_button(s, " Remove ")
        click_outer_button(s, " OK ")
        s.feed(0.8)
    if settings_values(cfg).get("Site"):
        print("FAIL - accepted removal was not persisted")
        return False
    print("PASS")
    return True


def test_site_editor_validation():
    print("TEST: component Site editor validation remains intact ... ",
          end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pv_site_validation_")
    with session(cfg) as s:
        if C.open_dialog(s) is None or goto_tab(s, "Site") is None:
            print("FAIL - Site page did not open")
            return False
        click_outer_button(s, " Add ")
        ok = T.find_text(s.screen()[0], " OK ")
        s.click(ok[1] + 2, ok[0] + 1)
        if not T.grid_contains(s.screen()[0], "Host is required."):
            print("FAIL - blank Host was accepted")
            return False
        replace_site_field(s, "Host", "bad host")
        ok = T.find_text(s.screen()[0], " OK ")
        s.click(ok[1] + 2, ok[0] + 1)
        if not T.grid_contains(s.screen()[0], "Host must be a valid"):
            print("FAIL - malformed Host was accepted")
            return False
        replace_site_field(s, "Host", "validation.example.test", clear=True)
        replace_site_field(s, "Port", "invalid", clear=True)
        ok = T.find_text(s.screen()[0], " OK ")
        s.click(ok[1] + 2, ok[0] + 1)
        if not T.grid_contains(s.screen()[0], "Port must be a number from 1 to 65535."):
            print("FAIL - invalid Port was accepted")
            return False
        s.write("\x1b")
        if T.grid_contains(s.screen()[0], "Add SFTP Site"):
            print("FAIL - Escape did not cancel the editor")
            return False
        click_outer_button(s, " OK ")
        s.feed(0.6)
    if settings_values(cfg).get("Site"):
        print("FAIL - cancelled invalid site was persisted")
        return False
    print("PASS")
    return True


def test_site_editor_error_row_is_dynamic():
    print("TEST: component Site editor dynamically renders its error row ... ",
          end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pv_site_dynamic_error_")
    with session(cfg) as s:
        if C.open_dialog(s) is None or goto_tab(s, "Site") is None:
            print("FAIL - Site page did not open")
            return False
        if not click_outer_button(s, " Add "):
            print("FAIL - Add button missing")
            return False

        chars = s.screen()[0]
        title = T.find_text(chars, "Add SFTP Site")
        ok = T.find_text(chars, " OK ")
        if not title or not ok:
            print("FAIL - compact editor title or buttons missing")
            return False
        compact_span = ok[0] - title[0]

        s.click(ok[1] + 2, ok[0] + 1)
        chars = s.screen()[0]
        title = T.find_text(chars, "Add SFTP Site")
        error = T.find_text(chars, "Host is required.")
        ok = T.find_text(chars, " OK ")
        if not title or not error or not ok:
            print("FAIL - validation did not render the error row")
            return False
        if ok[0] - title[0] != compact_span + 2:
            print("FAIL - error row and separator did not grow the editor by two rows")
            return False

        # Site Name has initial focus, so edit it directly. Any field change
        # clears the current validation report.
        s.write("x")
        chars = s.screen()[0]
        title = T.find_text(chars, "Add SFTP Site")
        ok = T.find_text(chars, " OK ")
        if T.grid_contains(chars, "Host is required."):
            print("FAIL - field edit did not clear the error")
            return False
        if not title or not ok or ok[0] - title[0] != compact_span:
            print("FAIL - clearing the error did not restore the compact editor height")
            return False
        s.write("\x1b")
    print("PASS")
    return True


def test_site_multiselect_edit_gate_and_batch_delete():
    print("TEST: component Site table gates Edit and batch-deletes selections ... ",
          end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pv_site_multiselect_")
    settings_dir = os.path.join(cfg, "parvion")
    os.makedirs(settings_dir)
    with open(os.path.join(settings_dir, "settings"), "w", encoding="utf-8") as stream:
        stream.write("Site\tAlpha\talpha.example.test\t22\talice\t\n")
        stream.write("Site\tBeta\tbeta.example.test\t22\tbob\t\n")
        stream.write("Site\tGamma\tgamma.example.test\t22\tgrace\t\n")

    with session(cfg) as s:
        if C.open_dialog(s) is None or goto_tab(s, "Site") is None:
            print("FAIL - Site page did not open")
            return False
        chars = s.screen()[0]
        alpha = T.find_text(chars, "Alpha")
        gamma = T.find_text(chars, "Gamma")
        if not alpha or not gamma:
            print("FAIL - seeded rows were not rendered")
            return False
        s.click(alpha[1] + 1, alpha[0] + 1)
        s.click(gamma[1] + 1, gamma[0] + 1, button=16)
        click_outer_button(s, " Edit ")
        if T.grid_contains(s.screen()[0], "Edit SFTP Site"):
            print("FAIL - Edit opened for a multi-selection")
            return False
        s.write("\x1b[3~")
        chars = s.screen()[0]
        if T.grid_contains(chars, "Alpha") or T.grid_contains(chars, "Gamma"):
            print("FAIL - a selected site survived Delete")
            return False
        if not T.grid_contains(chars, "Beta"):
            print("FAIL - Delete removed an unselected site")
            return False
        click_outer_button(s, " OK ")
        s.feed(0.8)
    wanted = "Beta\tbeta.example.test\t22\tbob\t"
    if settings_values(cfg).get("Site") != [wanted]:
        print(f"FAIL - batch deletion persisted {settings_values(cfg).get('Site')!r}")
        return False
    print("PASS")
    return True


def test_debug_structure_persistence_and_cancel():
    print("TEST: component Debug controls persist and cancel cleanly ... ",
          end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pv_debug_components_")
    with session(cfg) as s:
        if C.open_dialog(s) is None:
            print("FAIL - dialog did not open")
            return False
        chars = goto_tab(s, "Debug")
        if chars is None:
            print("FAIL - Debug tab was not found")
            return False
        text = page_text(chars)
        for needle in ("Debugging settings", "Debug information in message log:",
                       "0 - None", "Directory listing", "Show raw directory listing",
                       "negative impact on performance"):
            if needle not in text:
                print(f"FAIL - {needle!r} missing")
                return False

        current = T.find_text(chars, "0 - None")
        s.click(current[1] + 1, current[0] + 1)
        verbose = T.find_text(s.screen()[0], "3 - Verbose")
        if not verbose:
            print("FAIL - debug-level dropdown did not open")
            return False
        s.click(verbose[1] + 1, verbose[0] + 1)
        raw = T.find_text(s.screen()[0], "Show raw directory listing")
        s.click(raw[1] + 1, raw[0] + 1)
        click_outer_button(s, " OK ")
        s.feed(0.8)
    values = settings_values(cfg)
    if values.get("Logging Debug Level") != ["3"]:
        print(f"FAIL - debug level persisted as {values.get('Logging Debug Level')!r}")
        return False
    if values.get("Logging Raw Listing") != ["1"]:
        print(f"FAIL - raw-listing flag persisted as {values.get('Logging Raw Listing')!r}")
        return False

    with session(cfg) as s:
        if C.open_dialog(s) is None or goto_tab(s, "Debug") is None:
            print("FAIL - Debug page did not reopen")
            return False
        current = T.find_text(s.screen()[0], "3 - Verbose")
        s.click(current[1] + 1, current[0] + 1)
        debug = T.find_text(s.screen()[0], "4 - Debug")
        s.click(debug[1] + 1, debug[0] + 1)
        raw = T.find_text(s.screen()[0], "Show raw directory listing")
        s.click(raw[1] + 1, raw[0] + 1)
        click_outer_button(s, " Cancel ")
        s.feed(0.6)
    values = settings_values(cfg)
    if (values.get("Logging Debug Level") != ["3"]
     or values.get("Logging Raw Listing") != ["1"]):
        print("FAIL - outer Cancel persisted Debug edits")
        return False
    print("PASS")
    return True


def test_debug_narrow_groupbox_inner_clipping():
    print("TEST: narrow Debug groupboxes preserve right padding and border ... ",
          end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pv_debug_groupbox_clip_")
    old_rows, old_cols = T.ROWS, T.COLS
    T.ROWS, T.COLS = 20, 35

    def intact_right_edge(chars, needle):
        position = T.find_text(chars, needle)
        if not position:
            return False
        row = T.row_text(chars, position[0])
        border = row.find("│", position[1])
        return border > 0 and row[border - 1] == " "

    try:
        with session(cfg) as s:
            if C.open_dialog(s) is None or goto_tab(s, "Debug") is None:
                print("FAIL - Debug page did not open")
                return False

            # Debugging settings uses a flex-wrapped grid. Its long fixed row
            # is clipped, but the wrapper already kept this edge intact before
            # the direct-child groupbox fix.
            chars = s.screen()[0]
            if (not intact_right_edge(chars, "Debug information")
                    or T.find_text(chars, "Debug information in message log:")):
                print("FAIL - Debugging settings overwrote its right edge")
                return False

            # Directory listing is the direct fixed-width checkbox regression:
            # at this width it exceeds the inner viewport by exactly the right
            # padding and border cells.
            for _ in range(20):
                os.write(s.master_fd, b"\x1b[<65;30;15M")
            s.feed(0.6)
            chars = s.screen()[0]
            if (not intact_right_edge(chars, "Show raw directory")
                    or T.find_text(chars, "Show raw directory listing")):
                print("FAIL - Directory listing overwrote its right edge")
                return False
    finally:
        T.ROWS, T.COLS = old_rows, old_cols
    print("PASS")
    return True


TESTS = [
    test_site_structure_add_edit_and_persist,
    test_empty_site_name_is_generated_and_stays_unique,
    test_remove_site_button_tracks_table_selection,
    test_site_remove_is_draft_until_outer_accept,
    test_site_editor_validation,
    test_site_editor_error_row_is_dynamic,
    test_site_multiselect_edit_gate_and_batch_delete,
    test_debug_structure_persistence_and_cancel,
    test_debug_narrow_groupbox_inner_clipping,
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
    print(f"\n{passed}/{len(TESTS)} component Site/Debug tests passed")
    return 0 if passed == len(TESTS) else 1


if __name__ == "__main__":
    sys.exit(main())
