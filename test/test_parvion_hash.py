#!/usr/bin/env python3
# Copyright (c) Shaobo Song
# Licensed under the MIT license.

"""
End-to-end TUI smoke tests for the Parvion checksum/hash feature (parvion/queue.hpp
Checksums tab + hashing.hpp). Driven via the same pty + SGR-mouse harness as
test_parvion_queue.py, whose helpers are imported here.

The app is launched as `vtm-tile -r parvion` with PARVION_DEMO_HASH=1, which seeds the
hash queue with five synthetic tasks (succeeded / hashing / queued / failed) and sets
no_autostart so no real parvionhash backend is spawned. We verify:

  1. The bottom tab strip shows "Checksums (5)"; clicking it lists every seeded task,
     its algorithm, a final digest, and a failure reason.
  2. Every context menu exposes Copy -> Path / Digest, Remove, and Select All.
  3. Copy on a multi-selection writes newline-separated paths or successful digests.
"""

import os
import sys
import time
import base64
import hashlib
import re
import tempfile
import subprocess

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_parvion_queue import (  # noqa: E402
    ParvionSession, kill_all_vtm, find_text, grid_contains, row_text,
    find_menu_item, menu_has_separator_between, header_field, click_header, named_row_order,
    ROWS, COLS, VTM_TILE_BINARY,
)
import test_parvion_panes as P  # noqa: E402  (ParvionSession that sets the launch cwd)

DEMO_ENV = {"PARVION_DEMO_HASH": "1"}
DEMO_REPORT_DIGEST = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
DEMO_NOTES_DIGEST = "d41d8cd98f00b204e9800998ecf8427e"
_OSC52 = re.compile(rb"\x1b\]52;[^;]*;([A-Za-z0-9+/=]+)(?:\x07|\x1b\\)")


def _find_text_on_row(chars, needle, row):
    col = row_text(chars, row).find(needle)
    return (row, col) if col >= 0 else None


def _digest_file(path, algo):
    h = hashlib.new(algo)
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def _backend_digest(algo, path):
    out = subprocess.run([VTM_TILE_BINARY, "-r", "parvionhash", algo, "local", path],
                         capture_output=True, text=True)
    digest = ""
    for line in out.stdout.splitlines():
        if line.startswith("R "):
            digest = line[2:].strip()
    return digest, out


def _open_checksums_tab(s):
    chars, _ = s.screen()
    assert grid_contains(chars, "Checksums (5)"), "Checksums tab label missing from the tab strip"
    pos = find_text(chars, "Checksums (")
    assert pos, "could not locate the Checksums tab"
    r, c = pos
    s.click(c + 1, r + 1)  # mouse coords are 1-based; grid indices are 0-based.
    return s.screen()[0]


def _hide_checksums_column(s, name):
    chars = s.screen()[0]
    hdr = find_text(chars, "Source")
    if not hdr:
        return False
    s.click(hdr[1] + 1, hdr[0] + 1, button=2)
    item = find_menu_item(s.screen()[0], name)
    if not item:
        return False
    s.click(item[1] + 1, item[0] + 1)
    return True


def test_checksums_tab_lists_tasks():
    with ParvionSession(DEMO_ENV) as s:
        chars = _open_checksums_tab(s)
        wanted = [
            "Source",             # new origin column header
            "Path",               # path column header
            "/home/user/report.pdf",  # full path (local, succeeded)
            "notes.txt",          # second finished checksum for multi-copy coverage
            "backup.tar.gz",      # remote, hashing
            "image.iso",          # local, queued
            "missing.bin",        # remote, failed
            "deploy@192.168.0.5:22",        # Source = user@ip:port (backup.tar.gz)
            "admin@files.example.com:2222", # Source = user@domain:port (missing.bin)
            "SHA-256",            # algorithm label
            "e3b0c44298fc",       # digest prefix (succeeded row; short, stays on-screen)
            "No such file",       # failure reason
        ]
        missing = [w for w in wanted if not grid_contains(chars, w)]
        if missing:
            print(f"FAIL test_checksums_tab_lists_tasks: missing {missing}")
            return False
        print("OK test_checksums_tab_lists_tasks")
        return True


def test_checksums_context_menu():
    with ParvionSession(DEMO_ENV) as s:
        chars = _open_checksums_tab(s)
        rp = find_text(chars, "report.pdf")
        if not rp:
            print("FAIL test_checksums_context_menu: report.pdf row not found")
            return False
        s.click(rp[1] + 1, rp[0] + 1, button=2)  # right-click the finished row
        chars, _ = s.screen()
        wanted = ["Copy", "Remove", "Select All"]
        missing = [w for w in wanted if not grid_contains(chars, w)]
        if missing:
            print(f"FAIL test_checksums_context_menu: missing menu items {missing}")
            return False
        if grid_contains(chars, "Clear finished") or grid_contains(chars, "Remove all"):
            print("FAIL test_checksums_context_menu: obsolete bulk action shown")
            return False
        if not menu_has_separator_between(chars, "Remove", "Select All"):
            print("FAIL test_checksums_context_menu: action groups are not separated")
            return False
        rm = find_text(chars, "Remove")
        s.click(rm[1] + 1, rm[0] + 1)
        if not grid_contains(s.screen()[0], "Remove this checksum?"):
            print("FAIL test_checksums_context_menu: Remove confirmation not shown")
            return False
        s.write("\x1b")
        if not grid_contains(s.screen()[0], "report.pdf"):
            print("FAIL test_checksums_context_menu: cancelled Remove deleted the row")
            return False

        missing_row = find_text(s.screen()[0], "missing.bin")
        s.click(missing_row[1] + 1, missing_row[0] + 3, button=2)
        chars = s.screen()[0]
        select_all = find_text(chars, "Select All")
        if not select_all or grid_contains(chars, "Remove all") or not grid_contains(chars, "Copy"):
            print("FAIL test_checksums_context_menu: blank menu is not unified")
            return False
        copy = find_text(chars, "Copy")
        s.click(copy[1] + 1, copy[0] + 1)
        if not grid_contains(s.screen()[0], "Select All"):
            print("FAIL test_checksums_context_menu: disabled Copy submenu was interactive")
            return False
        chars = s.screen()[0]
        select_all = find_text(chars, "Select All")
        s.click(select_all[1] + 1, select_all[0] + 1)
        report = find_text(s.screen()[0], "report.pdf")
        s.click(report[1] + 1, report[0] + 1, button=2)
        rm = find_text(s.screen()[0], "Remove")
        s.click(rm[1] + 1, rm[0] + 1)
        if not grid_contains(s.screen()[0], "Remove 5 selected checksums?"):
            print("FAIL test_checksums_context_menu: Select All did not select every checksum")
            return False
        s.write("\x1b")
        print("OK test_checksums_context_menu")
        return True


def test_checksums_multiselect_copy_fields():
    with ParvionSession(DEMO_ENV) as s:
        chars = _open_checksums_tab(s)
        report = find_text(chars, "report.pdf")
        notes = find_text(chars, "notes.txt")
        backup = find_text(chars, "backup.tar.gz")
        if not report or not notes or not backup:
            print("FAIL test_checksums_multiselect_copy_fields: rows not found")
            return False
        s.click(report[1] + 1, report[0] + 1)
        s.click(backup[1] + 1, backup[0] + 1, button=16)  # Include a non-finished row; it has no digest.
        s.click(notes[1] + 1, notes[0] + 1, button=16)
        cases = (
            ("Path", 0, "/home/user/report.pdf\n/home/user/notes.txt\n/srv/backup/backup.tar.gz"),
            ("Digest", 1, DEMO_REPORT_DIGEST + "\n" + DEMO_NOTES_DIGEST),
        )
        for leaf, row_offset, want in cases:
            s.click(report[1] + 1, report[0] + 1, button=2)  # Keep the existing multi-selection.
            cp = find_text(s.screen()[0], "Copy")
            if not cp:
                print("FAIL test_checksums_multiselect_copy_fields: Copy submenu missing")
                return False
            s.click(cp[1] + 1, cp[0] + 1)
            item = _find_text_on_row(s.screen()[0], leaf, cp[0] + row_offset)
            if not item:
                print(f"FAIL test_checksums_multiselect_copy_fields: {leaf} item missing")
                return False
            before = len(s._buf)
            s.click(item[1] + 1, item[0] + 1)
            s.feed(0.6)
            hits = _OSC52.findall(s._buf[before:])
            if not hits:
                print(f"FAIL test_checksums_multiselect_copy_fields: {leaf} emitted no clipboard sequence")
                return False
            got = base64.b64decode(hits[-1]).decode("utf-8", "replace")
            if got != want:
                print(f"FAIL test_checksums_multiselect_copy_fields: {leaf} clipboard {got!r} != {want!r}")
                return False
        print("OK test_checksums_multiselect_copy_fields")
        return True


def test_local_file_hash_end_to_end():
    """Real pipeline (no demo seam): right-click a local file -> Calculate Checksum ->
    SHA-256 spawns the parvionhash backend; the computed digest lands in the Checksums
    tab and matches hashlib's SHA-256."""
    d = tempfile.mkdtemp(prefix="pvhash_")
    fpath = os.path.join(d, "payload.bin")
    with open(fpath, "wb") as f:
        f.write(b"parvion checksum end-to-end test\n" * 5000)  # ~165 KiB
    want = _digest_file(fpath, "sha256")
    with P.ParvionSession(d, env={"PARVION_DEMO_QUEUE": "0"}) as s:
        pos = find_text(s.screen()[0], "payload.bin")
        if not pos:
            print("FAIL test_local_file_hash_end_to_end: file not listed")
            return False
        s.click(pos[1] + 2, pos[0] + 1, button=2); s.feed(0.7)        # right-click the file row
        cc = find_text(s.screen()[0], "Calculate Checksum")
        if not cc:
            print("FAIL test_local_file_hash_end_to_end: 'Calculate Checksum' submenu missing")
            return False
        s.click(cc[1] + 1, cc[0] + 1); s.feed(0.7)                    # open the algorithm submenu
        algo = find_text(s.screen()[0], "SHA-256")
        if not algo:
            print("FAIL test_local_file_hash_end_to_end: 'SHA-256' item missing")
            return False
        s.click(algo[1] + 1, algo[0] + 1); s.feed(0.8)                # enqueue the SHA-256 task
        tab = find_text(s.screen()[0], "Checksums (")
        s.click(tab[1] + 1, tab[0] + 1); s.feed(0.6)                  # show the Checksums tab
        for _ in range(25):                                           # poll until the digest computes
            blob = "\n".join(row_text(s.screen()[0], r) for r in range(ROWS))
            if want[:32] in blob:
                print("OK test_local_file_hash_end_to_end")
                return True
            s.feed(0.4)
        print(f"FAIL test_local_file_hash_end_to_end: digest {want[:16]}... never appeared")
        return False


def test_backend_hash_non_ascii_path():
    """Regression: the `parvionhash local <path>` backend must hash a file whose name contains
    non-ASCII characters. The path reaches the backend via argv; the multi-call dispatch in
    main() rebuilds argv as UTF-8 (on Windows from the wide command line, where the CRT's ANSI
    argv would otherwise mangle the name) so the file opens. Digest must match hashlib."""
    d = tempfile.mkdtemp(prefix="pvhash_")
    fpath = os.path.join(d, "测试-café-Ω.bin")  # 测试-café-Ω.bin
    with open(fpath, "wb") as f:
        f.write(b"parvion non-ascii checksum test\n" * 5000)
    want = _digest_file(fpath, "sha256")
    digest, out = _backend_digest("sha256", fpath)
    if digest != want:
        print(f"FAIL test_backend_hash_non_ascii_path: got {digest!r} want {want!r} "
              f"(stdout={out.stdout!r})")
        return False
    print("OK test_backend_hash_non_ascii_path")
    return True


def test_backend_hash_all_algorithms():
    """The backend maps every UI-exposed algorithm to the expected lowercase digest."""
    d = tempfile.mkdtemp(prefix="pvhashalgo_")
    fpath = os.path.join(d, "payload.bin")
    payload = bytes(range(256)) * 4096 + b"parvion hash algorithms\n"
    with open(fpath, "wb") as f:
        f.write(payload)
    for algo in ("md5", "sha1", "sha256", "sha384", "sha512"):
        want = hashlib.new(algo, payload).hexdigest()
        digest, out = _backend_digest(algo, fpath)
        if digest != want:
            print(f"FAIL test_backend_hash_all_algorithms[{algo}]: got {digest!r} "
                  f"want {want!r} (stdout={out.stdout!r}, stderr={out.stderr!r})")
            return False
    print("OK test_backend_hash_all_algorithms")
    return True


def test_no_checksum_on_folder():
    """A folder keeps Calculate Checksum visible but disabled; a file enables it."""
    d = tempfile.mkdtemp(prefix="pvhashdir_")
    os.mkdir(os.path.join(d, "subdir"))
    with open(os.path.join(d, "plain.txt"), "w") as f:
        f.write("hi")
    with P.ParvionSession(d, env={"PARVION_DEMO_QUEUE": "0"}) as s:
        # Right-click the folder row: the checksum submenu is visible but inert.
        fd = find_text(s.screen()[0], "subdir")
        if not fd:
            print("FAIL test_no_checksum_on_folder: subdir not listed"); return False
        s.click(fd[1] + 2, fd[0] + 1, button=2); s.feed(0.7)
        chars = s.screen()[0]
        if not grid_contains(chars, "Rename"):
            print("FAIL test_no_checksum_on_folder: folder context menu did not open"); return False
        checksum = find_text(chars, "Calculate Checksum")
        if not checksum:
            print("FAIL test_no_checksum_on_folder: 'Calculate Checksum' omitted for a folder"); return False
        s.click(checksum[1] + 1, checksum[0] + 1); s.feed(0.4)
        if not grid_contains(s.screen()[0], "Calculate Checksum") or grid_contains(s.screen()[0], "SHA-256"):
            print("FAIL test_no_checksum_on_folder: folder checksum action was interactive"); return False
        # Dismiss, then right-click the file: checksum entry must be present.
        s.click(1, 1); s.feed(0.3)
        ff = find_text(s.screen()[0], "plain.txt")
        s.click(ff[1] + 2, ff[0] + 1, button=2); s.feed(0.7)
        if not grid_contains(s.screen()[0], "Calculate Checksum"):
            print("FAIL test_no_checksum_on_folder: 'Calculate Checksum' missing for a file"); return False
        print("OK test_no_checksum_on_folder")
        return True


def test_checksums_column_resize():
    """The Checksums table's columns are resizable via the divider handles (the shared table
    component): dragging the Source column's right border widens it."""
    with ParvionSession(DEMO_ENV) as s:
        chars = _open_checksums_tab(s)
        hdr = find_text(chars, "Source")
        if not hdr:
            print("FAIL test_checksums_column_resize: Source header not found"); return False
        hr = hdr[0]
        borders = [c for c in range(COLS) if chars[hr][c] == "│"]
        if not borders:
            print("FAIL test_checksums_column_resize: no column dividers on the Checksums tab"); return False
        b0 = borders[0]                       # Source's right-edge resize handle.
        s.drag(b0 + 1, hr + 1, b0 + 11, hr + 1); s.feed(0.5)  # drag it ~10 cells right
        chars, _ = s.screen()
        borders2 = [c for c in range(COLS) if chars[hr][c] == "│"]
        b1 = borders2[0] if borders2 else -1
        if not (b1 > b0 + 3):
            print(f"FAIL test_checksums_column_resize: border did not widen (b0={b0}, b1={b1})"); return False
        print("OK test_checksums_column_resize")
        return True


def test_checksums_row_selection():
    """Checksums rows are selectable via the shared table component: a left-click highlights the
    row, and a Ctrl-click adds a second row to the selection (multi-select)."""
    with ParvionSession(DEMO_ENV) as s:
        chars = _open_checksums_tab(s)
        a = find_text(chars, "report.pdf")     # row in the Path column
        b = find_text(chars, "missing.bin")
        if not a or not b:
            print("FAIL test_checksums_row_selection: rows not found"); return False
        bg0 = s.screen()[1]
        a_before, b_before = bg0[a[0]][a[1]], bg0[b[0]][b[1]]
        s.click(a[1] + 1, a[0] + 1)            # select row A
        bg1 = s.screen()[1]
        if bg1[a[0]][a[1]] is None or bg1[a[0]][a[1]] == a_before:
            print("FAIL test_checksums_row_selection: row A not highlighted"); return False
        if bg1[b[0]][b[1]] != b_before:
            print("FAIL test_checksums_row_selection: a plain click also selected row B"); return False
        # Ctrl-click row B adds it (both highlighted, row A kept).
        s.click(b[1] + 1, b[0] + 1, button=16)  # SGR Ctrl modifier (16)
        bg2 = s.screen()[1]
        if bg2[a[0]][a[1]] == a_before:
            print("FAIL test_checksums_row_selection: Ctrl-click dropped row A"); return False
        if bg2[b[0]][b[1]] is None or bg2[b[0]][b[1]] == b_before:
            print("FAIL test_checksums_row_selection: Ctrl-click did not add row B"); return False
        print("OK test_checksums_row_selection")
        return True


def test_checksums_blank_area_matches_transfer_table():
    """The row-right blank area is not part of a checksum row, matching transfer tables."""
    with ParvionSession(DEMO_ENV) as s:
        _open_checksums_tab(s)
        if not _hide_checksums_column(s, "Result"):
            print("FAIL test_checksums_blank_area_matches_transfer_table: Result column toggle missing")
            return False
        chars, bg0 = s.screen()
        pos = find_text(chars, "notes.txt")
        if not pos:
            print("FAIL test_checksums_blank_area_matches_transfer_table: notes.txt row not found")
            return False
        r, c = pos
        blank_c = 100
        if row_text(chars, r)[blank_c] != " ":
            print("FAIL test_checksums_blank_area_matches_transfer_table: test point is not blank")
            return False

        name_before = bg0[r][c]
        s.click(c + 1, r + 1)
        bg1 = s.screen()[1]
        selected = bg1[r][c]
        if selected is None or selected == name_before:
            print("FAIL test_checksums_blank_area_matches_transfer_table: row was not selected")
            return False

        s.click(blank_c + 1, r + 1)
        if s.screen()[1][r][c] == selected:
            print("FAIL test_checksums_blank_area_matches_transfer_table: left-click blank kept row selected")
            return False

        s.click(c + 1, r + 1)
        selected = s.screen()[1][r][c]
        s.click(blank_c + 1, r + 1, button=2)
        chars = s.screen()[0]
        if not grid_contains(chars, "Select All") or grid_contains(chars, "Remove all") or not grid_contains(chars, "Copy"):
            print("FAIL test_checksums_blank_area_matches_transfer_table: row-right blank did not open unified menu")
            return False
        s.write("\x1b")
        if s.screen()[1][r][c] == selected:
            print("FAIL test_checksums_blank_area_matches_transfer_table: right-click blank kept row selected")
            return False
        print("OK test_checksums_blank_area_matches_transfer_table")
        return True


def test_checksums_arrow_key_selection():
    """Checksums inherits shared-table keyboard navigation: Up/Down moves the single selected row."""
    with ParvionSession(DEMO_ENV) as s:
        chars = _open_checksums_tab(s)
        report = find_text(chars, "report.pdf")
        notes = find_text(chars, "notes.txt")
        if not report or not notes:
            print("FAIL test_checksums_arrow_key_selection: rows not found"); return False
        bg0 = s.screen()[1]
        report_before, notes_before = bg0[report[0]][report[1]], bg0[notes[0]][notes[1]]
        s.click(report[1] + 1, report[0] + 1)
        bg1 = s.screen()[1]
        selected_bg = bg1[report[0]][report[1]]
        if selected_bg is None or selected_bg == report_before:
            print("FAIL test_checksums_arrow_key_selection: initial row not selected"); return False

        s.write("\x1b[B")  # Down Arrow -> notes.txt
        bg2 = s.screen()[1]
        if bg2[notes[0]][notes[1]] != selected_bg:
            print("FAIL test_checksums_arrow_key_selection: Down Arrow did not select next row"); return False
        if bg2[report[0]][report[1]] == selected_bg:
            print("FAIL test_checksums_arrow_key_selection: Down Arrow left previous row selected"); return False

        s.write("\x1b[A")  # Up Arrow -> report.pdf
        bg3 = s.screen()[1]
        if bg3[report[0]][report[1]] != selected_bg:
            print("FAIL test_checksums_arrow_key_selection: Up Arrow did not select previous row"); return False
        if bg3[notes[0]][notes[1]] != notes_before:
            print("FAIL test_checksums_arrow_key_selection: Up Arrow left next row selected"); return False
        print("OK test_checksums_arrow_key_selection")
        return True


def test_checksums_keyboard_remove_selected_and_noop():
    """Delete removes selected checksum rows and does nothing when no rows are selected."""
    with ParvionSession(DEMO_ENV) as s:
        chars = _open_checksums_tab(s)
        report = find_text(chars, "report.pdf")
        notes = find_text(chars, "notes.txt")
        backup = find_text(chars, "backup.tar.gz")
        if not report or not notes or not backup:
            print("FAIL test_checksums_keyboard_remove_selected_and_noop: rows not found"); return False
        s.click(report[1] + 1, report[0] + 1)
        s.write("\x7f")  # Backspace is not table row deletion.
        if not grid_contains(s.screen()[0], "report.pdf"):
            print("FAIL test_checksums_keyboard_remove_selected_and_noop: Backspace removed a selected row"); return False
        s.click(notes[1] + 1, notes[0] + 1, button=16)
        s.write("\x1b[3~")  # Delete -> selected-row confirmation.
        if not grid_contains(s.screen()[0], "Remove 2 selected checksums?"):
            print("FAIL test_checksums_keyboard_remove_selected_and_noop: confirmation not shown"); return False
        s.write("\x1b")  # Cancel preserves both rows.
        if not grid_contains(s.screen()[0], "report.pdf") or not grid_contains(s.screen()[0], "notes.txt"):
            print("FAIL test_checksums_keyboard_remove_selected_and_noop: cancellation removed rows"); return False
        s.write("\x1b[3~")
        s.write("\r")  # Confirm removal.
        chars = s.screen()[0]
        if grid_contains(chars, "report.pdf") or grid_contains(chars, "notes.txt"):
            print("FAIL test_checksums_keyboard_remove_selected_and_noop: selected rows survived Delete"); return False
        if not grid_contains(chars, "backup.tar.gz") or not grid_contains(chars, "Checksums (3)"):
            print("FAIL test_checksums_keyboard_remove_selected_and_noop: wrong rows removed after Delete"); return False

        backup = find_text(chars, "backup.tar.gz")
        if not backup:
            print("FAIL test_checksums_keyboard_remove_selected_and_noop: backup row disappeared"); return False
        s.click(backup[1] + 1, backup[0] + 1)  # Focus/select a remaining row.
        s.write("\x1b")                       # Clear selection.
        s.write("\x1b[3~")                    # Delete with no selection: no-op.
        chars = s.screen()[0]
        for name in ("backup.tar.gz", "image.iso", "missing.bin", "Checksums (3)"):
            if not grid_contains(chars, name):
                print(f"FAIL test_checksums_keyboard_remove_selected_and_noop: no-selection Delete removed {name}"); return False
        print("OK test_checksums_keyboard_remove_selected_and_noop")
        return True


def test_checksums_sort_cycle_is_typed_and_stable():
    """Checksum headers expose the shared sort UI; Size uses raw bytes, default restores source
    order, and equal Algorithm values retain their original relative order."""
    with ParvionSession(DEMO_ENV) as s:
        chars = _open_checksums_tab(s)
        source = header_field(chars, "Source")
        if source is None:
            print("FAIL test_checksums_sort_cycle_is_typed_and_stable: Source header missing")
            return False
        hr = source[0]
        bad = []
        for title in ("Source", "Path", "Algorithm", "Size", "Progress"):
            field = header_field(chars, title, hr)
            if field is None or field[5] != "↕":
                bad.append((title, None if field is None else field[5]))
        if bad:
            print(f"FAIL test_checksums_sort_cycle_is_typed_and_stable: initial glyphs {bad}")
            return False

        names = ("report.pdf", "notes.txt", "backup.tar.gz", "image.iso", "missing.bin")
        if not click_header(s, "Size", hr):
            print("FAIL test_checksums_sort_cycle_is_typed_and_stable: Size header missing")
            return False
        chars = s.screen()[0]
        known_asc = named_row_order(chars, names[:-1])
        want_asc = ["notes.txt", "report.pdf", "backup.tar.gz", "image.iso"]
        size = header_field(chars, "Size", hr)
        if size is None or size[5] != "↑" or known_asc != want_asc:
            print(f"FAIL test_checksums_sort_cycle_is_typed_and_stable: ascending Size "
                  f"glyph/order {None if size is None else size[5]!r}/{known_asc}, expected ↑/{want_asc}")
            return False

        click_header(s, "Size", hr)
        chars = s.screen()[0]
        known_desc = named_row_order(chars, names[:-1])
        size = header_field(chars, "Size", hr)
        if size is None or size[5] != "↓" or known_desc != list(reversed(want_asc)):
            print(f"FAIL test_checksums_sort_cycle_is_typed_and_stable: descending Size "
                  f"glyph/order {None if size is None else size[5]!r}/{known_desc}")
            return False

        click_header(s, "Size", hr)
        chars = s.screen()[0]
        restored = named_row_order(chars, names)
        size = header_field(chars, "Size", hr)
        if size is None or size[5] != "↕" or restored != list(names):
            print(f"FAIL test_checksums_sort_cycle_is_typed_and_stable: default restore "
                  f"glyph/order {None if size is None else size[5]!r}/{restored}")
            return False

        # Two MD5 rows and two SHA-256 rows exercise stable ties.
        if not click_header(s, "Algorithm", hr):
            print("FAIL test_checksums_sort_cycle_is_typed_and_stable: Algorithm header missing")
            return False
        chars = s.screen()[0]
        got = named_row_order(chars, names)
        want = ["notes.txt", "image.iso", "report.pdf", "backup.tar.gz", "missing.bin"]
        algo = header_field(chars, "Algorithm", hr)
        size = header_field(chars, "Size", hr)
        if algo is None or algo[5] != "↑" or size is None or size[5] != "↕" or got != want:
            print(f"FAIL test_checksums_sort_cycle_is_typed_and_stable: Algorithm sort "
                  f"algo={algo}, size={size}, order={got}, expected={want}")
            return False
        print("OK test_checksums_sort_cycle_is_typed_and_stable")
        return True


def _open_settings_sftp(s):
    """Open Edit -> Settings and switch to the SFTP tab; return the rendered screen."""
    chars = s.screen()[0]
    e = find_text(chars, "Edit"); s.click(e[1] + 2, e[0] + 1); s.feed(0.7)
    chars = s.screen()[0]
    p = find_text(chars, "Settings"); s.click(p[1] + 1, p[0] + 1); s.feed(0.9)
    chars = s.screen()[0]
    f = find_text(chars, "SFTP"); s.click(f[1] + 1, f[0] + 1); s.feed(0.7)
    return s.screen()[0]


def _hash_settings(cfgdir):
    path = os.path.join(cfgdir, "parvion", "settings")
    out = {}
    if os.path.isfile(path):
        for line in open(path):
            if "\t" in line:
                k, v = line.rstrip("\n").split("\t", 1)
                out[k] = v
    return out


def test_hash_settings_single_dropdown():
    """The Hash verification groupbox is a single dropdown defaulting to 'None' (no separate
    checkbox); 'None' disables transfer hashing, and picking an algorithm enables + persists it."""
    cfg = tempfile.mkdtemp(prefix="pvhashcfg_")
    work = tempfile.mkdtemp(prefix="pvhashwork_")  # empty cwd so the file list can't match "Settings"
    with P.ParvionSession(work, env={"PARVION_DEMO_QUEUE": "0", "XDG_CONFIG_HOME": cfg}) as s:
        chars = _open_settings_sftp(s)
        if not grid_contains(chars, "Hash verification"):
            print("FAIL test_hash_settings_single_dropdown: groupbox missing"); return False
        if not grid_contains(chars, "None"):
            print("FAIL test_hash_settings_single_dropdown: dropdown does not default to None"); return False
        nb = find_text(chars, "None")
        s.click(nb[1] + 1, nb[0] + 1); s.feed(0.6)            # open the dropdown
        chars = s.screen()[0]
        algos = ("MD5", "SHA-1", "SHA-256", "SHA-384", "SHA-512")
        for item in ("None",) + algos:
            if not find_text(chars, item):
                print(f"FAIL test_hash_settings_single_dropdown: dropdown missing {item}"); return False
        positions = [find_text(chars, item) for item in algos]
        if positions != sorted(positions):
            print(f"FAIL test_hash_settings_single_dropdown: wrong algorithm order {positions}"); return False
        a = find_text(chars, "SHA-256")
        s.click(a[1] + 1, a[0] + 1); s.feed(0.6)              # select SHA-256 (enables hashing)
        chars = s.screen()[0]
        ok = find_text(chars, " OK ")
        s.click(ok[1] + 2, ok[0] + 1); s.feed(0.8)            # OK persists
    cfgvals = _hash_settings(cfg)
    if cfgvals.get("Hash on transfer") != "1" or cfgvals.get("Hash algorithm") != "2":
        print(f"FAIL test_hash_settings_single_dropdown: not persisted (got {cfgvals})"); return False
    print("OK test_hash_settings_single_dropdown")
    return True


TESTS = [
    test_checksums_tab_lists_tasks,
    test_checksums_context_menu,
    test_checksums_multiselect_copy_fields,
    test_local_file_hash_end_to_end,
    test_backend_hash_non_ascii_path,
    test_backend_hash_all_algorithms,
    test_no_checksum_on_folder,
    test_checksums_column_resize,
    test_checksums_row_selection,
    test_checksums_blank_area_matches_transfer_table,
    test_checksums_arrow_key_selection,
    test_checksums_keyboard_remove_selected_and_noop,
    test_checksums_sort_cycle_is_typed_and_stable,
    test_hash_settings_single_dropdown,
]


def main():
    if not os.path.isfile(VTM_TILE_BINARY):
        print(f"ERROR: vtm-tile binary not found at {VTM_TILE_BINARY}")
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
