#!/usr/bin/env python3
# Copyright (c) Shaobo Song
# Licensed under the MIT license.

"""
End-to-end TUI tests for the Parvion address bar (the editable path field in each pane's
title strip, panes.hpp): clicking the path starts an inline edit (connect-bar field style,
with a block caret), a left-drag scrubs the caret, Enter navigates to the typed path
(absolute or resolved relative to the current dir), Esc reverts, and a nonexistent or
inaccessible path falls back to the previous directory.

Local pane only (no network): the app is launched as `vtm-tile -r parvion` with the child's
cwd set to a fresh temp directory, so the address shown in the Local site header is known.
"""

import os
import sys
import time
import shutil
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_parvion_panes as T  # ParvionSession, row_text, find_text, grid_contains, make_tree, ...

kill_all_vtm = T.kill_all_vtm  # Let run_all_tests.py reuse our between-test cleanup.

ACCENT = (137, 180, 250)  # theme::sel_bg_act — the address edit's block caret bg.


def _local_header(s):
    """(row, path, path_col): the Local site header row, the path it shows and the path's
    0-based start column. The title row paints ' ' + label + '  ' + path (panes.hpp);
    temp-dir paths contain no spaces, so the first token after the label is the path."""
    chars = s.screen()[0]
    pos = T.find_text(chars, "Local site")
    if pos is None:
        return None
    row = T.row_text(chars, pos[0])
    after = row[pos[1] + len("Local site"):]
    path = after.split()[0] if after.split() else ""
    col = row.find(path, pos[1] + len("Local site")) if path else -1
    return (pos[0], path, col)


def _click_path_end(s, hdr):
    """Click one cell past the shown path's end: starts the edit with the caret at the end."""
    r, path, col = hdr
    s.click(col + len(path) + 1, r + 1)  # 1-based SGR coords; +1 past the last path cell.


def _caret_in_header(s, hdr_row):
    """True when some cell in the header row carries the accent bg (the block caret)."""
    bg = s.screen()[1]
    return any(bg[hdr_row][c] == ACCENT for c in range(T.COLS))


def test_address_click_starts_edit():
    """Clicking the path in the title strip starts the edit: a block caret appears."""
    print("TEST: parvion address bar - click starts edit ... ", end="", flush=True)
    d = T.make_tree()
    try:
        with T.ParvionSession(d) as s:
            hdr = _local_header(s)
            if hdr is None or hdr[1] != d:
                print(f"FAIL - Local site header does not show {d}")
                return False
            if _caret_in_header(s, hdr[0]):
                print("FAIL - caret shown before any click")
                return False
            _click_path_end(s, hdr)
            if not _caret_in_header(s, hdr[0]):
                print("FAIL - no caret after clicking the path")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_address_absolute_navigation():
    """Clearing the field and typing an absolute path + Enter lists that directory."""
    print("TEST: parvion address bar - absolute path navigation ... ", end="", flush=True)
    d = T.make_tree()
    b = tempfile.mkdtemp(prefix="parvionaddr_")
    open(os.path.join(b, "marker_b.txt"), "w").close()
    try:
        with T.ParvionSession(d) as s:
            hdr = _local_header(s)
            if hdr is None:
                print("FAIL - Local site header not found")
                return False
            _click_path_end(s, hdr)
            s.write("\x7f" * (len(d) + 8))  # Clear the seeded path.
            s.write(b)
            s.write("\r")
            hdr2 = _local_header(s)
            if hdr2 is None or hdr2[1] != b:
                print(f"FAIL - header shows {hdr2 and hdr2[1]!r}, want {b!r}")
                return False
            if not T.grid_contains(s.screen()[0], "marker_b.txt"):
                print("FAIL - target dir contents not listed")
                return False
            if T.grid_contains(s.screen()[0], "alpha.txt"):
                print("FAIL - old dir contents still listed")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)
        shutil.rmtree(b, ignore_errors=True)


def test_address_relative_navigation():
    """A relative path resolves against the current directory."""
    print("TEST: parvion address bar - relative path navigation ... ", end="", flush=True)
    d = T.make_tree()  # Contains the subdir "gamma".
    try:
        with T.ParvionSession(d) as s:
            hdr = _local_header(s)
            if hdr is None:
                print("FAIL - Local site header not found")
                return False
            _click_path_end(s, hdr)
            s.write("\x7f" * (len(d) + 8))
            s.write("gamma")
            s.write("\r")
            hdr2 = _local_header(s)
            want = os.path.join(d, "gamma")
            if hdr2 is None or hdr2[1] != want:
                print(f"FAIL - header shows {hdr2 and hdr2[1]!r}, want {want!r}")
                return False
            if T.grid_contains(s.screen()[0], "alpha.txt"):
                print("FAIL - parent dir contents still listed")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_address_esc_reverts():
    """Esc cancels the edit: the header path and the listing stay unchanged."""
    print("TEST: parvion address bar - Esc reverts ... ", end="", flush=True)
    d = T.make_tree()
    try:
        with T.ParvionSession(d) as s:
            hdr = _local_header(s)
            if hdr is None:
                print("FAIL - Local site header not found")
                return False
            _click_path_end(s, hdr)
            s.write("zzz")  # Goes into the address editor, not the list's type-ahead.
            s.write("\x1b")  # Esc.
            s.feed(0.6)
            hdr2 = _local_header(s)
            if hdr2 is None or hdr2[1] != d:
                print(f"FAIL - header shows {hdr2 and hdr2[1]!r} after Esc, want {d!r}")
                return False
            if _caret_in_header(s, hdr2[0]):
                print("FAIL - caret still shown after Esc")
                return False
            if not T.grid_contains(s.screen()[0], "alpha.txt"):
                print("FAIL - listing lost after a cancelled edit")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_address_bad_path_falls_back():
    """Enter on a nonexistent path falls back to the previous directory."""
    print("TEST: parvion address bar - bad path falls back ... ", end="", flush=True)
    d = T.make_tree()
    try:
        with T.ParvionSession(d) as s:
            hdr = _local_header(s)
            if hdr is None:
                print("FAIL - Local site header not found")
                return False
            _click_path_end(s, hdr)
            s.write("\x7f" * (len(d) + 8))
            s.write("/nonexistent_parvion_xyz")
            s.write("\r")
            hdr2 = _local_header(s)
            if hdr2 is None or hdr2[1] != d:
                print(f"FAIL - header shows {hdr2 and hdr2[1]!r}, want the previous dir {d!r}")
                return False
            if not T.grid_contains(s.screen()[0], "alpha.txt"):
                print("FAIL - previous dir listing not restored")
                return False
            if T.grid_contains(s.screen()[0], "Not a directory:"):
                print("FAIL - error body shown instead of the previous listing")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_address_drag_scrubs_caret():
    """A left-drag within the field moves the caret with the cursor."""
    print("TEST: parvion address bar - drag scrubs caret ... ", end="", flush=True)
    d = T.make_tree()
    try:
        with T.ParvionSession(d) as s:
            hdr = _local_header(s)
            if hdr is None:
                print("FAIL - Local site header not found")
                return False
            r, path, col = hdr
            # Press at the path's end, pull to its 3rd cell (1-based SGR coords). The short
            # ASCII path is unscrolled, so the caret cell must land exactly under the cursor.
            start = (col + len(path) + 1, r + 1)
            end   = (col + 3, r + 1)
            s.drag_path([start, (col + len(path) // 2, r + 1), end])
            bg = s.screen()[1]
            caret_cols = [c for c in range(T.COLS) if bg[r][c] == ACCENT]
            if caret_cols != [col + 2]:
                print(f"FAIL - caret cells at {caret_cols}, want [{col + 2}]")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


TESTS = [
    test_address_click_starts_edit,
    test_address_absolute_navigation,
    test_address_relative_navigation,
    test_address_esc_reverts,
    test_address_bad_path_falls_back,
    test_address_drag_scrubs_caret,
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
