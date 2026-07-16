#!/usr/bin/env python3
# Copyright (c) Shaobo Song
# Licensed under the MIT license.

"""
End-to-end TUI tests for the Parvion Edit -> Settings dialog (settings_dialog.hpp):
the SFTP subset of FileZilla's Connection, Connection/SFTP, and Debug option pages, ported
into a top-tabbed modal dialog that persists to <XDG_CONFIG_HOME>/parvion/settings
and feeds the live SFTP engine.

Driven through the shared ParvionSession harness (SGR mouse + replay() screen
capture). Each test runs against an isolated XDG_CONFIG_HOME so it neither reads nor
clobbers real user settings. The public-key test exercises the pvputtygen applet by
adding /root/.ssh/id_rsa and asserting its parsed Comment/Data land in the table and
the key path is persisted.
"""

import os
import sys
import time
import tempfile
import shutil
import subprocess

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_parvion_panes as T  # ParvionSession, row_text, find_text

kill_all_vtm = T.kill_all_vtm  # Let run_all_tests.py reuse our between-test cleanup.

ID_RSA = "/root/.ssh/id_rsa"


def _session(cfgdir):
    # No demo queue; isolate settings under cfgdir. HOME is inherited (the picker
    # browses it), so the key test needs the harness launched with HOME=/root.
    return T.ParvionSession(os.getcwd(), env={"PARVION_DEMO_QUEUE": "0",
                                              "XDG_CONFIG_HOME": cfgdir})


def _open_dialog(s):
    chars = s.screen()[0]
    e = T.find_text(chars, "Edit")
    s.click(e[1] + 2, e[0] + 1); s.feed(0.7)
    chars = s.screen()[0]
    p = T.find_text(chars, "Settings")
    s.click(p[1] + 1, p[0] + 1); s.feed(0.9)
    return s.screen()[0]


def _goto_sftp(s):
    chars = s.screen()[0]
    sftp = T.find_text(chars, "SFTP")
    s.click(sftp[1] + 1, sftp[0] + 1); s.feed(0.7)
    return s.screen()[0]


def _goto_debug(s):
    chars = s.screen()[0]
    dbg = T.find_text(chars, "Debug")
    s.click(dbg[1] + 1, dbg[0] + 1); s.feed(0.7)
    return s.screen()[0]


def _click_label(s, needle, dx=1):
    chars = s.screen()[0]
    pos = T.find_text(chars, needle)
    assert pos, f"'{needle}' not on screen"
    s.click(pos[1] + dx, pos[0] + 1); s.feed(0.6)
    return s.screen()[0]


def _settings_file(cfgdir):
    path = os.path.join(cfgdir, "parvion", "settings")
    if not os.path.isfile(path):
        return {}
    out = {}
    with open(path) as f:
        for line in f:
            if "\t" in line:
                k, v = line.rstrip("\n").split("\t", 1)
                out.setdefault(k, []).append(v)
    return out


def test_dialog_opens():
    print("TEST: settings dialog opens (Connection tab) ... ", end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pvset_")
    with _session(cfg) as s:
        chars = _open_dialog(s)
        blob = "\n".join(T.row_text(chars, r) for r in range(len(chars)))
        for needle in ("Settings", "Connection", "SFTP", "Debug", "Timeout",
                       "Reconnection settings", "OK", "Cancel"):
            if needle not in blob:
                print(f"FAIL - '{needle}' missing"); return False
    print("PASS"); return True


def test_debug_tab_persists():
    print("TEST: settings dialog Debug tab persists log controls ... ", end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pvset_")
    with _session(cfg) as s:
        _open_dialog(s)
        chars = _goto_debug(s)
        blob = "\n".join(T.row_text(chars, r) for r in range(len(chars)))
        for needle in ("Debugging settings", "Debug information in message log",
                       "0 - None", "Show raw directory listing"):
            if needle not in blob:
                print(f"FAIL - '{needle}' missing"); return False
        level = T.find_text(chars, "0 - None")
        s.click(level[1] + 1, level[0] + 1); s.feed(0.7)
        verbose = T.find_text(s.screen()[0], "3 - Verbose")
        if not verbose:
            print("FAIL - debug-level dropdown did not open"); return False
        s.click(verbose[1] + 1, verbose[0] + 1); s.feed(0.7)
        _click_label(s, "Show raw directory listing", dx=1)
        chars = s.screen()[0]
        ok = T.find_text(chars, " OK ")
        s.click(ok[1] + 2, ok[0] + 1); s.feed(1.0)
    vals = _settings_file(cfg)
    if vals.get("Logging Debug Level") != ["3"]:
        print(f"FAIL - debug level not persisted: {vals.get('Logging Debug Level')}"); return False
    if vals.get("Logging Raw Listing") != ["1"]:
        print(f"FAIL - raw listing not persisted: {vals.get('Logging Raw Listing')}"); return False
    print("PASS"); return True


def test_sftp_tab():
    print("TEST: settings dialog SFTP tab ... ", end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pvset_")
    with _session(cfg) as s:
        _open_dialog(s)
        chars = _goto_sftp(s)
        blob = "\n".join(T.row_text(chars, r) for r in range(len(chars)))
        for needle in ("Public Key Authentication", "Private keys:", "Filename",
                       "Comment", "Data", "Add key file", "Remove key",
                       "Enable compression", "Parallel transfers",
                       "Maximum parallel connections"):
            if needle not in blob:
                print(f"FAIL - '{needle}' missing"); return False
    print("PASS"); return True


def test_compression_persists():
    print("TEST: toggle compression + OK persists ... ", end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pvset_")
    with _session(cfg) as s:
        _open_dialog(s)
        _goto_sftp(s)
        _click_label(s, "Enable compression", dx=1)  # Toggle the checkbox.
        chars = s.screen()[0]
        ok = T.find_text(chars, " OK ")
        s.click(ok[1] + 2, ok[0] + 1); s.feed(1.0)
    vals = _settings_file(cfg)
    if vals.get("SFTP compression") != ["1"]:
        print(f"FAIL - compression not persisted: {vals.get('SFTP compression')}"); return False
    print("PASS"); return True


def test_unit_dropdown_selects_and_persists():
    print("TEST: threshold unit dropdown selects + persists ... ", end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pvset_")
    with _session(cfg) as s:
        _open_dialog(s)
        _goto_sftp(s)
        # Default unit is MiB (2). Click the "MiB ▾" dropdown, then pick GiB (3) from the menu.
        chars = s.screen()[0]
        unit = T.find_text(chars, "MiB")
        if not unit:
            print("FAIL - MiB unit dropdown not found"); return False
        s.click(unit[1] + 1, unit[0] + 1); s.feed(0.7)  # Open the dropdown.
        chars = s.screen()[0]
        gib = T.find_text(chars, "GiB")
        if not gib:
            print("FAIL - dropdown did not open (no GiB row)"); return False
        s.click(gib[1] + 1, gib[0] + 1); s.feed(0.7)     # Select GiB.
        chars = s.screen()[0]
        if "GiB ▾" not in "\n".join(T.row_text(chars, r) for r in range(len(chars))):
            print("FAIL - unit did not change to GiB"); return False
        ok = T.find_text(chars, " OK ")
        s.click(ok[1] + 2, ok[0] + 1); s.feed(1.0)
    vals = _settings_file(cfg)
    if vals.get("SFTP parallel transfer threshold unit") != ["3"]:
        print(f"FAIL - unit not persisted as GiB(3): {vals.get('SFTP parallel transfer threshold unit')}"); return False
    print("PASS"); return True


def test_add_key_pubkey_parse_and_persist():
    print("TEST: add private key (pvputtygen parse) + persist ... ", end="", flush=True)
    if not os.path.isfile(ID_RSA):
        print("SKIP - /root/.ssh/id_rsa absent"); return True
    cfg = tempfile.mkdtemp(prefix="pvset_")
    with _session(cfg) as s:
        _open_dialog(s)
        _goto_sftp(s)
        chars = s.screen()[0]
        add = T.find_text(chars, "Add key file")
        s.click(add[1] + 1, add[0] + 1); s.feed(1.0)
        # Picker: enter /.ssh, then activate id_rsa.
        chars = s.screen()[0]
        ssh = T.find_text(chars, "/.ssh")
        if not ssh:
            print("FAIL - /.ssh not in picker (HOME=/root?)"); return False
        s.click(ssh[1] + 2, ssh[0] + 1); s.feed(0.5)
        s.write("\r"); s.feed(1.0)
        chars = s.screen()[0]
        idr = T.find_text(chars, "id_rsa")
        if not idr:
            print("FAIL - id_rsa not listed in /.ssh"); return False
        s.click(idr[1] + 2, idr[0] + 1); s.feed(0.5)
        s.write("\r"); s.feed(1.5)
        # Back in the dialog: the key table shows the parsed fingerprint.
        chars = s.screen()[0]
        blob = "\n".join(T.row_text(chars, r) for r in range(len(chars)))
        if "SHA256:" not in blob:
            print("FAIL - parsed fingerprint (Data) not shown in key table"); return False
        ok = T.find_text(chars, " OK ")
        s.click(ok[1] + 2, ok[0] + 1); s.feed(1.0)
    vals = _settings_file(cfg)
    if ID_RSA not in vals.get("SFTP keyfile", []):
        print(f"FAIL - key not persisted: {vals.get('SFTP keyfile')}"); return False
    print("PASS"); return True


def _write_settings_with_key(cfgdir, keyfile):
    d = os.path.join(cfgdir, "parvion")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "settings"), "w") as f:
        f.write("Timeout\t20\nReconnect count\t0\nReconnect delay\t5\n")
        f.write("SFTP compression\t0\n")
        f.write("SFTP parallel transfer threshold value\t64\n")
        f.write("SFTP parallel transfer threshold unit\t2\n")
        f.write("SFTP parallel max connections\t4\n")
        f.write(f"SFTP keyfile\t{keyfile}\n")


def test_reopen_repopulates_key_metadata():
    # Regression: a key persisted in the settings file (i.e. "already added") must show
    # its Comment/Data in real time whenever the SFTP page is (re)opened — the dialog
    # re-parses each key via pvputtygen on open, rather than showing blank columns.
    print("TEST: reopen Settings repopulates key Comment/Data ... ", end="", flush=True)
    if not os.path.isfile(ID_RSA):
        print("SKIP - /root/.ssh/id_rsa absent"); return True
    cfg = tempfile.mkdtemp(prefix="pvset_")
    _write_settings_with_key(cfg, ID_RSA)
    with _session(cfg) as s:
        _open_dialog(s)
        chars = _goto_sftp(s)
        s.feed(1.2)  # Allow the synchronous pvputtygen re-parse to populate the table.
        chars = s.screen()[0]
        blob = "\n".join(T.row_text(chars, r) for r in range(len(chars)))
        if "id_rsa" not in blob:
            print("FAIL - persisted key not listed"); return False
        if "SHA256:" not in blob:
            print("FAIL - Data (fingerprint) not repopulated on open"); return False
        if "@" not in blob:  # The key Comment (e.g. root@host) carries an '@'.
            print("FAIL - Comment not repopulated on open"); return False
    print("PASS"); return True


def _write_settings_with_keys(cfgdir, keyfiles):
    d = os.path.join(cfgdir, "parvion")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "settings"), "w") as f:
        f.write("Timeout\t20\nReconnect count\t0\nReconnect delay\t5\n")
        f.write("SFTP compression\t0\nSFTP parallel transfer threshold value\t64\n")
        f.write("SFTP parallel transfer threshold unit\t2\nSFTP parallel max connections\t4\n")
        for k in keyfiles:
            f.write(f"SFTP keyfile\t{k}\n")


def test_key_table_sort_configuration():
    """Filename/Comment/Data headers expose three-state shared-table sorting."""
    print("TEST: key table shared sort configuration ... ", end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pvset_")
    names = ("zeta.pem", "alpha.pem", "middle.pem")
    _write_settings_with_keys(cfg, [f"/keys/{name}" for name in names])
    with _session(cfg) as s:
        _open_dialog(s)
        _goto_sftp(s)
        chars = s.screen()[0]
        filename = T.table_header_field(chars, "Filename")
        if filename is None:
            print("FAIL - Filename header not found"); return False
        hr = filename[0]
        for title in ("Filename", "Comment", "Data"):
            field = T.table_header_field(chars, title, hr)
            if field is None or field[5] != "↕":
                print(f"FAIL - {title} header is not sortable: {field}"); return False
        if T.named_row_order(chars, names) != list(names):
            print(f"FAIL - unexpected source order: {T.named_row_order(chars, names)}"); return False

        T.click_table_header(s, "Filename", hr)
        chars = s.screen()[0]
        if T.table_header_field(chars, "Filename", hr)[5] != "↑":
            print("FAIL - Filename did not enter ascending mode"); return False
        if T.named_row_order(chars, names) != ["alpha.pem", "middle.pem", "zeta.pem"]:
            print(f"FAIL - ascending Filename order: {T.named_row_order(chars, names)}"); return False

        T.click_table_header(s, "Filename", hr)
        chars = s.screen()[0]
        if T.table_header_field(chars, "Filename", hr)[5] != "↓":
            print("FAIL - Filename did not enter descending mode"); return False
        if T.named_row_order(chars, names) != ["zeta.pem", "middle.pem", "alpha.pem"]:
            print(f"FAIL - descending Filename order: {T.named_row_order(chars, names)}"); return False

        T.click_table_header(s, "Filename", hr)
        if T.named_row_order(s.screen()[0], names) != list(names):
            print("FAIL - third Filename click did not restore source order"); return False

        # The metadata columns use the displayed Comment/Data strings as their sort values.
        for title in ("Comment", "Data"):
            T.click_table_header(s, title, hr)
            field = T.table_header_field(s.screen()[0], title, hr)
            if field is None or field[5] != "↑":
                print(f"FAIL - {title} did not enter ascending mode: {field}"); return False
    print("PASS"); return True


def test_empty_key_table_autofit_preserves_sort_header():
    """Header-derived auto-fit retains the full title and glyph when the table has no rows."""
    print("TEST: empty key table auto-fit preserves sortable header ... ", end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pvset_")
    try:
        _write_settings_with_keys(cfg, [])
        with _session(cfg) as s:
            _open_dialog(s)
            _goto_sftp(s)
            filename = T.table_header_field(s.screen()[0], "Filename")
            if filename is None:
                print("FAIL - Filename header not found"); return False
            hr = filename[0]
            s.double_click(filename[3] + 1, hr + 1)
            filename = T.table_header_field(s.screen()[0], "Filename", hr)
            if filename is None or filename[5] != "↕":
                print(f"FAIL - Filename auto-fit clipped its sortable header: {filename}"); return False
        print("PASS"); return True
    finally:
        shutil.rmtree(cfg, ignore_errors=True)


def _find_column_menu_item(chars, name):
    """Return the menu row and label column for a shared-table column toggle."""
    for r in range(len(chars)):
        line = T.row_text(chars, r)
        for marker in ("▣ ", "□ "):
            c = line.find(marker + name)
            if c >= 0:
                return r, c + len(marker)
    return None


def test_key_table_vertical_scrollbar():
    # Item 6: the key table strictly matches the file-browser table — a long key list
    # raises a vertical scrollbar (thumb/track) on the table's right edge.
    print("TEST: key table shows a vertical scrollbar ... ", end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pvset_")
    _write_settings_with_keys(cfg, [f"/keys/k_{i:02d}.pem" for i in range(40)])
    with _session(cfg) as s:
        _open_dialog(s)
        _goto_sftp(s)
        s.feed(0.8)
        chars = s.screen()[0]
        # The VSB renders as ▐ (track) / █ (thumb) glyphs inside the Public Key box body.
        found = any(("▐" in T.row_text(chars, r)) or ("█" in T.row_text(chars, r)) for r in range(6, 32))
        if not found:
            print("FAIL - no vertical scrollbar glyph in the key table"); return False
    print("PASS"); return True


def test_key_table_vertical_scrollbar_drag_reaches_last_key():
    """Dragging the shared table's vertical thumb to the bottom reveals the final key."""
    print("TEST: key table vertical scrollbar drags to the final key ... ", end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pvset_")
    _write_settings_with_keys(cfg, [f"/keys/scroll_key_{i:02d}.pem" for i in range(40)])
    with _session(cfg) as s:
        _open_dialog(s)
        _goto_sftp(s)
        s.feed(0.8)
        chars = s.screen()[0]
        header = T.find_text(chars, "Filename")
        add = T.find_text(chars, "Add key file")
        if not header or not add:
            print("FAIL - key table bounds not found"); return False
        # Find the repeated VSB half-block column only within the key-table body. This avoids
        # depending on the settings card's negotiated origin or width.
        glyphs = {}
        for r in range(header[0] + 1, add[0]):
            for c, ch in enumerate(chars[r]):
                if ch in ("▐", "█") and c > header[1]:
                    glyphs.setdefault(c, []).append(r)
        if not glyphs:
            print("FAIL - no vertical scrollbar track found"); return False
        col, track_rows = max(glyphs.items(), key=lambda item: len(item[1]))
        top, bottom = min(track_rows), max(track_rows)
        if bottom <= top:
            print("FAIL - vertical scrollbar track is too short to drag"); return False
        s.drag_path([(col + 1, top + 1), (col + 1, bottom + 1)], settle=0.8)
        if not T.grid_contains(s.screen()[0], "scroll_key_39.pem"):
            print("FAIL - dragging the scrollbar to the bottom did not reveal the final key"); return False
    print("PASS"); return True


def test_key_table_column_menu_hides_comment():
    """The Settings adapter exposes the shared table's intrinsic show/hide column menu."""
    print("TEST: key table shared column menu hides Comment ... ", end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pvset_")
    _write_settings_with_keys(cfg, ["/keys/menu_key.pem"])
    with _session(cfg) as s:
        _open_dialog(s)
        _goto_sftp(s)
        chars = s.screen()[0]
        filename = T.find_text(chars, "Filename")
        comment = T.find_text(chars, "Comment")
        data = T.find_text(chars, "Data")
        if not filename or not comment or not data or not (filename[0] == comment[0] == data[0]):
            print("FAIL - key table headers not found on one row"); return False
        header_row = filename[0]
        data_before = data[1]
        s.click(filename[1] + 1, header_row + 1, button=2)
        if _find_column_menu_item(s.screen()[0], "Comment") is None:
            print("FAIL - Comment toggle missing from the header menu"); return False
        s.write("c")  # Auto-assigned &Comment shortcut from the shared table.
        line = T.row_text(s.screen()[0], header_row)
        if "Comment" in line:
            print("FAIL - Comment header remained visible after toggling it off"); return False
        data_after = line.find("Data")
        if data_after < 0 or data_after >= data_before:
            print(f"FAIL - Data column did not close the hidden-column gap ({data_before} -> {data_after})"); return False
    print("PASS"); return True


def test_key_table_ctrl_multiselect_remove_and_reindex():
    """Ctrl-selected key rows are removed together while the unselected row survives and persists."""
    print("TEST: key table Ctrl multi-select removes only selected keys ... ", end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pvset_")
    paths = ["/keys/remove_alpha.pem", "/keys/keep_beta.pem", "/keys/remove_gamma.pem"]
    _write_settings_with_keys(cfg, paths)
    with _session(cfg) as s:
        _open_dialog(s)
        _goto_sftp(s)
        chars = s.screen()[0]
        alpha = T.find_text(chars, "remove_alpha.pem")
        gamma = T.find_text(chars, "remove_gamma.pem")
        if not alpha or not gamma:
            print("FAIL - key rows not found"); return False
        s.click(alpha[1] + 1, alpha[0] + 1)
        s.click(gamma[1] + 1, gamma[0] + 1, button=16)  # Ctrl+click adds a non-contiguous row.
        remove = T.find_text(s.screen()[0], "Remove key")
        if not remove:
            print("FAIL - Remove key button not found"); return False
        s.click(remove[1] + 1, remove[0] + 1)
        chars = s.screen()[0]
        if T.grid_contains(chars, "remove_alpha.pem") or T.grid_contains(chars, "remove_gamma.pem"):
            print("FAIL - a selected key remained after Remove key"); return False
        if not T.grid_contains(chars, "keep_beta.pem"):
            print("FAIL - the unselected middle key was removed or mis-reindexed"); return False
        ok = T.find_text(chars, " OK ")
        s.click(ok[1] + 2, ok[0] + 1); s.feed(0.8)
    saved = _settings_file(cfg).get("SFTP keyfile", [])
    if saved != [paths[1]]:
        print(f"FAIL - wrong key rows persisted after removal: {saved}"); return False
    print("PASS"); return True


def test_key_table_keyboard_remove_selected_and_noop():
    """Delete removes selected key rows and does nothing when no rows are selected."""
    print("TEST: key table keyboard remove selected/no-selection noop ... ", end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pvset_")
    paths = ["/keys/remove_alpha.pem", "/keys/keep_beta.pem", "/keys/remove_gamma.pem"]
    _write_settings_with_keys(cfg, paths)
    with _session(cfg) as s:
        _open_dialog(s)
        _goto_sftp(s)
        chars = s.screen()[0]
        alpha = T.find_text(chars, "remove_alpha.pem")
        gamma = T.find_text(chars, "remove_gamma.pem")
        if not alpha or not gamma:
            print("FAIL - key rows not found"); return False
        s.click(alpha[1] + 1, alpha[0] + 1)
        s.click(gamma[1] + 1, gamma[0] + 1, button=16)
        s.write("\x1b[3~")  # Delete -> remove selected key rows.
        chars = s.screen()[0]
        if T.grid_contains(chars, "remove_alpha.pem") or T.grid_contains(chars, "remove_gamma.pem"):
            print("FAIL - selected key rows survived Delete"); return False
        if not T.grid_contains(chars, "keep_beta.pem"):
            print("FAIL - unselected key row was removed by Delete"); return False

        beta = T.find_text(chars, "keep_beta.pem")
        if not beta:
            print("FAIL - surviving key row not found"); return False
        s.click(beta[1] + 1, beta[0] + 1)
        s.click(beta[1] + 1, beta[0] + 3)  # Blank table space clears selection without closing Settings.
        s.write("\x1b[3~")  # Delete with no selection: no-op.
        chars = s.screen()[0]
        if not T.grid_contains(chars, "keep_beta.pem"):
            print("FAIL - no-selection Delete removed the surviving key"); return False
        ok = T.find_text(chars, " OK ")
        s.click(ok[1] + 2, ok[0] + 1); s.feed(0.8)
    saved = _settings_file(cfg).get("SFTP keyfile", [])
    if saved != [paths[1]]:
        print(f"FAIL - wrong key rows persisted after keyboard removal: {saved}"); return False
    print("PASS"); return True


def test_key_table_column_resize():
    # Item 6: dragging a column border (resize handle) widens that column — the next
    # column's header shifts right, exactly like the Local Site browser.
    print("TEST: key table column resize (drag handle) ... ", end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pvset_")
    _write_settings_with_keys(cfg, ["/keys/alpha.pem", "/keys/beta.pem"])
    with _session(cfg) as s:
        _open_dialog(s)
        _goto_sftp(s)
        s.feed(0.6)
        chars = s.screen()[0]
        before = T.find_text(chars, "Comment")
        hdr = T.find_text(chars, "Filename")
        if not before or not hdr:
            print("FAIL - table header not found"); return False
        # Border between Filename and Comment sits just left of the Comment header.
        bx, by = before[1] - 1, before[0]
        # Drag the border 8 cells to the right (1-based SGR coords).
        s.drag_path([(bx + 1, by + 1), (bx + 9, by + 1)])
        s.feed(0.6)
        chars = s.screen()[0]
        after = T.find_text(chars, "Comment")
        if not after or after[1] <= before[1]:
            print(f"FAIL - Comment header did not move right ({before[1]} -> {after[1] if after else None})"); return False
    print("PASS"); return True


def test_picker_buttons_right_aligned():
    # Item 5: the file picker's Open/Cancel buttons are both on the right, Open left of Cancel.
    print("TEST: picker Open/Cancel right-aligned (Open left of Cancel) ... ", end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pvset_")
    with _session(cfg) as s:
        _open_dialog(s)
        _goto_sftp(s)
        chars = s.screen()[0]
        add = T.find_text(chars, "Add key file")
        s.click(add[1] + 1, add[0] + 1); s.feed(1.0)
        chars = s.screen()[0]
        row = None
        for r in range(len(chars)):
            line = T.row_text(chars, r)
            if "Open" in line and "Cancel" in line:
                row = line; op = line.find("Open"); cn = line.find("Cancel"); break
        if row is None:
            print("FAIL - Open/Cancel row not found"); return False
        if not (op < cn):
            print("FAIL - Open is not left of Cancel"); return False
        if op <= 50:
            print(f"FAIL - buttons not right-aligned (Open at {op})"); return False
    print("PASS"); return True


def _dialog_open(chars):
    blob = "\n".join(T.row_text(chars, r) for r in range(len(chars)))
    return "Timeout" in blob and "Reconnection settings" in blob


def test_esc_closes_dialog_on_open():
    # The dialog grabs keyboard focus when it opens, so Esc closes it immediately —
    # without needing a click to focus it first.
    print("TEST: Esc closes the dialog right after opening ... ", end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pvset_")
    with _session(cfg) as s:
        chars = _open_dialog(s)
        if not _dialog_open(chars):
            print("FAIL - dialog did not open"); return False
        s.write("\x1b"); s.feed(0.8)  # Esc, no click first.
        chars = s.screen()[0]
        if _dialog_open(chars):
            print("FAIL - Esc did not close the dialog"); return False
    print("PASS"); return True


def test_esc_closes_key_picker():
    # The "Add key file..." picker grabs focus on open, and Esc closes it (returning to
    # the SFTP tab) without dismissing the whole settings dialog.
    print("TEST: Esc closes the key-file picker ... ", end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pvset_")
    with _session(cfg) as s:
        _open_dialog(s)
        _goto_sftp(s)
        chars = s.screen()[0]
        add = T.find_text(chars, "Add key file")
        s.click(add[1] + 1, add[0] + 1); s.feed(1.0)
        chars = s.screen()[0]
        blob = "\n".join(T.row_text(chars, r) for r in range(len(chars)))
        if "Open" not in blob or "Cancel" not in blob:
            print("FAIL - picker did not open"); return False
        s.write("\x1b"); s.feed(0.9)  # Esc, no click first.
        chars = s.screen()[0]
        blob = "\n".join(T.row_text(chars, r) for r in range(len(chars)))
        # Back on the SFTP tab; the picker's Open/Cancel row is gone.
        if "Public Key Authentication" not in blob:
            print("FAIL - did not return to the settings dialog"); return False
        if "Open" in blob and "Cancel" in blob:
            print("FAIL - picker still open after Esc"); return False
    print("PASS"); return True


ENC_PASSPHRASE = "secret"


def _gen_encrypted_key(keydir):
    # Generate an encrypted OpenSSH ed25519 key (-> pvputtygen "convertible"); None if no ssh-keygen.
    if not shutil.which("ssh-keygen"):
        return None
    key = os.path.join(keydir, "enc_key")
    r = subprocess.run(["ssh-keygen", "-t", "ed25519", "-N", ENC_PASSPHRASE, "-C", "parvion@test",
                        "-f", key, "-q"], capture_output=True)
    return key if r.returncode == 0 and os.path.isfile(key) else None


def _session_home(cfgdir, home):
    # Like _session, but also point HOME at `home` so the key picker opens straight into it.
    return T.ParvionSession(os.getcwd(), env={"PARVION_DEMO_QUEUE": "0",
                                              "XDG_CONFIG_HOME": cfgdir, "HOME": home})


def _pick_key(s, name):
    # In the open "Add key file" picker (rooted at HOME), activate the file `name`.
    chars = s.screen()[0]
    f = T.find_text(chars, name)
    if not f:
        return False
    s.click(f[1] + 2, f[0] + 1); s.feed(0.5)
    s.write("\r"); s.feed(1.2)
    return True


def test_add_encrypted_key_converts_to_ppk():
    # FileZilla parity: adding an encrypted non-ppk key prompts for its passphrase, converts it to a
    # PuTTY .ppk (via pvputtygen write), asks where to save it, and persists the .ppk path.
    print("TEST: encrypted key -> passphrase -> convert to .ppk + persist ... ", end="", flush=True)
    keydir = tempfile.mkdtemp(prefix="pvkey_")
    key = _gen_encrypted_key(keydir)
    if not key:
        print("SKIP - ssh-keygen unavailable"); return True
    cfg = tempfile.mkdtemp(prefix="pvset_")
    with _session_home(cfg, keydir) as s:
        _open_dialog(s)
        _goto_sftp(s)
        chars = s.screen()[0]
        add = T.find_text(chars, "Add key file")
        s.click(add[1] + 1, add[0] + 1); s.feed(1.0)
        if not _pick_key(s, "enc_key"):
            print("FAIL - enc_key not listed in picker (HOME=keydir?)"); return False
        # Passphrase modal.
        blob = "\n".join(T.row_text(s.screen()[0], r) for r in range(len(s.screen()[0])))
        if "Convert private key" not in blob and "passphrase" not in blob.lower():
            print("FAIL - passphrase modal did not appear"); return False
        s.write(ENC_PASSPHRASE); s.feed(0.4); s.write("\r"); s.feed(1.3)
        # Save-as modal (prefilled .ppk path): accept the default with Enter.
        blob = "\n".join(T.row_text(s.screen()[0], r) for r in range(len(s.screen()[0])))
        if "Save converted key" not in blob and ".ppk" not in blob:
            print("FAIL - save-as modal did not appear"); return False
        s.write("\r"); s.feed(1.6)
        # Back in the dialog: the converted .ppk shows a fingerprint.
        blob = "\n".join(T.row_text(s.screen()[0], r) for r in range(len(s.screen()[0])))
        if "SHA256:" not in blob:
            print("FAIL - converted key fingerprint not shown in table"); return False
        chars = s.screen()[0]
        ok = T.find_text(chars, " OK ")
        s.click(ok[1] + 2, ok[0] + 1); s.feed(1.0)
    ppk = key + ".ppk"
    if not os.path.isfile(ppk):
        print(f"FAIL - converted .ppk not written: {ppk}"); return False
    vals = _settings_file(cfg)
    if ppk not in vals.get("SFTP keyfile", []):
        print(f"FAIL - .ppk path not persisted: {vals.get('SFTP keyfile')}"); return False
    print("PASS"); return True


def _multiclick(s, col, row, n, gap=0.05):
    # Send n rapid press/release pairs at one cell (a double/triple click), 1-based SGR coords.
    for _ in range(n):
        os.write(s.master_fd, f"\x1b[<0;{col};{row}M".encode()); time.sleep(0.02)
        os.write(s.master_fd, f"\x1b[<0;{col};{row}m".encode()); time.sleep(gap)
    s.feed(0.6)


def test_key_table_autofit_creates_and_pages_horizontal_scrollbar():
    """Auto-fitting a long Filename grows it and the resulting horizontal rail pages right."""
    print("TEST: key table auto-fit creates and pages horizontal scrollbar ... ", end="", flush=True)
    cfg = tempfile.mkdtemp(prefix="pvset_")
    long_name = "key_" + "x" * 110 + ".pem"
    _write_settings_with_keys(cfg, ["/keys/" + long_name])
    with _session(cfg) as s:
        _open_dialog(s)
        _goto_sftp(s)
        chars = s.screen()[0]
        filename = T.find_text(chars, "Filename")
        comment = T.find_text(chars, "Comment")
        add = T.find_text(chars, "Add key file")
        if not filename or not comment or not add or filename[0] != comment[0]:
            print("FAIL - key table geometry not found"); return False
        header_row = filename[0]
        # The divider immediately left of Comment is Filename's resize/auto-fit handle.
        divider_col = comment[1] - 1
        if chars[header_row][divider_col] != "│":
            print("FAIL - Filename divider not found next to Comment"); return False
        _multiclick(s, divider_col + 1, header_row + 1, 2)
        chars = s.screen()[0]
        hbars = []
        for r in range(header_row + 1, add[0]):
            cols = [c for c, ch in enumerate(chars[r]) if ch in ("▂", "▄")]
            if cols:
                hbars.append((len(cols), r, min(cols), max(cols)))
        if not hbars:
            print("FAIL - auto-fit did not create a horizontal scrollbar"); return False
        _, hrow, _, hright = max(hbars)
        # At scroll-left the widened Filename pushes Comment out of view. Clicking the rail's
        # right end pages to the other columns without assuming a card width.
        if "Comment" in T.row_text(chars, header_row):
            print("FAIL - long auto-fitted Filename did not move Comment out of view"); return False
        s.click(hright + 1, hrow + 1)
        if "Comment" not in T.row_text(s.screen()[0], header_row):
            print("FAIL - horizontal scrollbar did not page to the Comment column"); return False
    print("PASS"); return True


def test_save_picker_double_click_overwrite():
    # Requirement: double-clicking a FILE in the Save picker triggers Save; an existing target raises
    # an overwrite confirmation (show_close_confirmation). Confirming writes the converted .ppk over it.
    print("TEST: save picker double-click file -> overwrite confirm -> save ... ", end="", flush=True)
    keydir = tempfile.mkdtemp(prefix="pvkey_")
    key = _gen_encrypted_key(keydir)
    if not key:
        print("SKIP - ssh-keygen unavailable"); return True
    target = os.path.join(keydir, "preexist.ppk")
    with open(target, "w") as f:
        f.write("old contents that must be overwritten\n")
    cfg = tempfile.mkdtemp(prefix="pvset_")
    with _session_home(cfg, keydir) as s:
        _open_dialog(s); _goto_sftp(s)
        chars = s.screen()[0]
        add = T.find_text(chars, "Add key file")
        s.click(add[1] + 1, add[0] + 1); s.feed(1.0)
        if not _pick_key(s, "enc_key"):
            print("FAIL - enc_key not listed"); return False
        s.write("secret"); s.feed(0.4); s.write("\r"); s.feed(1.3)  # passphrase -> save picker
        pe = T.find_text(s.screen()[0], "preexist")
        if not pe:
            print("FAIL - preexist.ppk not listed in picker"); return False
        _multiclick(s, pe[1] + 3, pe[0] + 1, 2)  # double-click the existing file -> Save.
        chars = s.screen()[0]
        # The overwrite confirmation shows an Overwrite/Cancel button row (its message wraps across rows).
        row = next((r for r in range(len(chars))
                    if "Overwrite" in T.row_text(chars, r) and "Cancel" in T.row_text(chars, r)), None)
        if row is None:
            print("FAIL - overwrite confirmation did not appear"); return False
        ov = T.row_text(chars, row).find("Overwrite")
        s.click(ov + 2, row + 1); s.feed(1.6)
        blob = "\n".join(T.row_text(s.screen()[0], r) for r in range(len(s.screen()[0])))
        if "SHA256:" not in blob:
            print("FAIL - conversion did not complete after the overwrite confirm"); return False
        ok = T.find_text(s.screen()[0], " OK ")
        s.click(ok[1] + 2, ok[0] + 1); s.feed(1.0)
    if target not in _settings_file(cfg).get("SFTP keyfile", []):
        print("FAIL - the overwritten target was not persisted"); return False
    with open(target) as f:
        head = f.read(64)
    if "old contents" in head or "PuTTY-User-Key-File" not in head:
        print(f"FAIL - target not overwritten with a .ppk: {head!r}"); return False
    print("PASS"); return True


def test_save_picker_click_updates_name():
    # Requirement: in the Save picker, single-clicking a file copies its name into the Name field
    # (overwrite target), using the shared input_field component.
    print("TEST: save picker single-click a file updates the Name field ... ", end="", flush=True)
    keydir = tempfile.mkdtemp(prefix="pvkey_")
    key = _gen_encrypted_key(keydir)
    if not key:
        print("SKIP - ssh-keygen unavailable"); return True
    with open(os.path.join(keydir, "decoy_target.txt"), "w") as f:
        f.write("x")  # A decoy file to click in the picker.
    cfg = tempfile.mkdtemp(prefix="pvset_")
    with _session_home(cfg, keydir) as s:
        _open_dialog(s)
        _goto_sftp(s)
        chars = s.screen()[0]
        add = T.find_text(chars, "Add key file")
        s.click(add[1] + 1, add[0] + 1); s.feed(1.0)
        if not _pick_key(s, "enc_key"):
            print("FAIL - enc_key not listed in picker"); return False
        s.write("secret"); s.feed(0.4); s.write("\r"); s.feed(1.3)  # passphrase -> save picker
        blob = "\n".join(T.row_text(s.screen()[0], r) for r in range(len(s.screen()[0])))
        if "Save converted key" not in blob or "enc_key.ppk" not in blob:
            print("FAIL - save picker with default name not shown"); return False
        # Single-click (not double) the decoy file; the Name field should switch to it.
        dc = T.find_text(s.screen()[0], "decoy_target")
        if not dc:
            print("FAIL - decoy file not listed in picker"); return False
        s.click(dc[1] + 2, dc[0] + 1); s.feed(0.8)
        blob = "\n".join(T.row_text(s.screen()[0], r) for r in range(len(s.screen()[0])))
        name_rows = [l for l in blob.splitlines() if "Name:" in l]
        joined = "\n".join(name_rows)
        if "decoy_target.txt" not in joined:
            print(f"FAIL - Name not updated to the clicked file: {name_rows}"); return False
        if "enc_key.ppk" in joined:
            print("FAIL - Name still shows the old default after clicking another file"); return False
    print("PASS"); return True


def test_encrypted_key_wrong_passphrase_retries():
    # New flow: decryption + conversion happen ONCE up front (behind a "Converting key..." spinner),
    # which also verifies the passphrase. A wrong one is caught there and re-prompts (with the retry
    # note) BEFORE the "Save converted key" picker ever appears; Esc then aborts without adding the key.
    print("TEST: encrypted key wrong passphrase re-prompts before save, Esc aborts ... ", end="", flush=True)
    keydir = tempfile.mkdtemp(prefix="pvkey_")
    key = _gen_encrypted_key(keydir)
    if not key:
        print("SKIP - ssh-keygen unavailable"); return True
    cfg = tempfile.mkdtemp(prefix="pvset_")
    with _session_home(cfg, keydir) as s:
        _open_dialog(s)
        _goto_sftp(s)
        chars = s.screen()[0]
        add = T.find_text(chars, "Add key file")
        s.click(add[1] + 1, add[0] + 1); s.feed(1.0)
        if not _pick_key(s, "enc_key"):
            print("FAIL - enc_key not listed in picker"); return False
        # Wrong passphrase + Enter -> (async verify) -> re-prompt with the retry note, NO save picker.
        s.write("nopenope"); s.feed(0.4); s.write("\r"); s.feed(2.0)
        blob = "\n".join(T.row_text(s.screen()[0], r) for r in range(len(s.screen()[0])))
        if "Incorrect passphrase" not in blob:
            print("FAIL - wrong passphrase did not re-prompt early with the retry note"); return False
        if "Save converted key" in blob:
            print("FAIL - the save picker appeared for a wrong passphrase (validation should precede it)"); return False
        s.write("\x1b"); s.feed(0.9)  # Esc aborts the conversion.
        blob = "\n".join(T.row_text(s.screen()[0], r) for r in range(len(s.screen()[0])))
        if "Public Key Authentication" not in blob:
            print("FAIL - did not return to the SFTP settings tab after Esc"); return False
        if "Incorrect passphrase" in blob or "Convert private key" in blob:
            print("FAIL - passphrase modal still open after Esc"); return False
    if os.path.isfile(key + ".ppk"):
        print("FAIL - .ppk written despite cancellation"); return False
    vals = _settings_file(cfg)
    if vals.get("SFTP keyfile"):
        print(f"FAIL - key persisted despite cancellation: {vals.get('SFTP keyfile')}"); return False
    print("PASS"); return True


TESTS = [
    test_dialog_opens,
    test_sftp_tab,
    test_debug_tab_persists,
    test_add_encrypted_key_converts_to_ppk,
    test_save_picker_double_click_overwrite,
    test_save_picker_click_updates_name,
    test_encrypted_key_wrong_passphrase_retries,
    test_key_table_sort_configuration,
    test_empty_key_table_autofit_preserves_sort_header,
    test_key_table_vertical_scrollbar,
    test_key_table_vertical_scrollbar_drag_reaches_last_key,
    test_key_table_column_resize,
    test_key_table_autofit_creates_and_pages_horizontal_scrollbar,
    test_key_table_column_menu_hides_comment,
    test_key_table_ctrl_multiselect_remove_and_reindex,
    test_key_table_keyboard_remove_selected_and_noop,
    test_picker_buttons_right_aligned,
    test_esc_closes_dialog_on_open,
    test_esc_closes_key_picker,
    test_compression_persists,
    test_unit_dropdown_selects_and_persists,
    test_add_key_pubkey_parse_and_persist,
    test_reopen_repopulates_key_metadata,
]


def main():
    if not os.path.isfile(T.VTM_TILE_BINARY):
        print(f"ERROR: vtm-tile binary not found at {T.VTM_TILE_BINARY}")
        return 1
    passed = 0
    failed = 0
    for t in TESTS:
        try:
            ok = t()
        except Exception as e:
            print(f"FAIL - exception: {e}")
            ok = False
        finally:
            kill_all_vtm()
        passed += 1 if ok else 0
        failed += 0 if ok else 1
    print("\n" + "=" * 60)
    print(f"Results: {passed}/{passed + failed} passed, {failed} failed")
    print("=" * 60)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
