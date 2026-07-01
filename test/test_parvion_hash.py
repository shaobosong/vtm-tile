#!/usr/bin/env python3
# Copyright (c) Shaobo Song
# Licensed under the MIT license.

"""
End-to-end TUI smoke tests for the Parvion checksum/hash feature (parvion/queue.hpp
Checksums tab + hashing.hpp). Driven via the same pty + SGR-mouse harness as
test_parvion_queue.py, whose helpers are imported here.

The app is launched as `vtm-tile -r parvion` with PARVION_DEMO_HASH=1, which seeds the
hash queue with four synthetic tasks (succeeded / hashing / queued / failed) and sets
no_autostart so no real parvionhash backend is spawned. We verify:

  1. The bottom tab strip shows "Checksums (4)"; clicking it lists every seeded task,
     its algorithm, a final digest, and a failure reason.
  2. Right-clicking a finished task opens the Copy digest / Remove / Clear finished menu.
"""

import os
import sys
import time
import tempfile
import subprocess

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_parvion_queue import (  # noqa: E402
    ParvionSession, kill_all_vtm, find_text, grid_contains, row_text, ROWS, COLS, VTM_TILE_BINARY,
)
import test_parvion_panes as P  # noqa: E402  (ParvionSession that sets the launch cwd)

DEMO_ENV = {"PARVION_DEMO_HASH": "1"}


def _open_checksums_tab(s):
    chars, _ = s.screen()
    assert grid_contains(chars, "Checksums (4)"), "Checksums tab label missing from the tab strip"
    pos = find_text(chars, "Checksums (")
    assert pos, "could not locate the Checksums tab"
    r, c = pos
    s.click(c + 1, r + 1)  # mouse coords are 1-based; grid indices are 0-based.
    return s.screen()[0]


def test_checksums_tab_lists_tasks():
    with ParvionSession(DEMO_ENV) as s:
        chars = _open_checksums_tab(s)
        wanted = [
            "Source",             # new origin column header
            "Path",               # path column header
            "/home/user/report.pdf",  # full path (local, succeeded)
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
        wanted = ["Copy digest", "Remove", "Clear finished"]
        missing = [w for w in wanted if not grid_contains(chars, w)]
        if missing:
            print(f"FAIL test_checksums_context_menu: missing menu items {missing}")
            return False
        print("OK test_checksums_context_menu")
        return True


def test_local_file_hash_end_to_end():
    """Real pipeline (no demo seam): right-click a local file -> Calculate Checksum ->
    SHA-256 spawns the parvionhash backend; the computed digest lands in the Checksums
    tab and matches coreutils' sha256sum."""
    d = tempfile.mkdtemp(prefix="pvhash_")
    fpath = os.path.join(d, "payload.bin")
    with open(fpath, "wb") as f:
        f.write(b"parvion checksum end-to-end test\n" * 5000)  # ~165 KiB
    want = subprocess.run(["sha256sum", fpath], capture_output=True, text=True).stdout.split()[0]
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
    argv would otherwise mangle the name) so the file opens. Digest must match coreutils."""
    d = tempfile.mkdtemp(prefix="pvhash_")
    fpath = os.path.join(d, "测试-café-Ω.bin")  # 测试-café-Ω.bin
    with open(fpath, "wb") as f:
        f.write(b"parvion non-ascii checksum test\n" * 5000)
    want = subprocess.run(["sha256sum", fpath], capture_output=True, text=True).stdout.split()[0]
    out = subprocess.run([VTM_TILE_BINARY, "-r", "parvionhash", "sha256", "local", fpath],
                         capture_output=True, text=True)
    digest = ""
    for line in out.stdout.splitlines():
        if line.startswith("R "):
            digest = line[2:].strip()
    if digest != want:
        print(f"FAIL test_backend_hash_non_ascii_path: got {digest!r} want {want!r} "
              f"(stdout={out.stdout!r})")
        return False
    print("OK test_backend_hash_non_ascii_path")
    return True


def test_no_checksum_on_folder():
    """Right-clicking a folder must NOT offer 'Calculate Checksum' (checksums are for files);
    a regular file in the same dir still offers it."""
    d = tempfile.mkdtemp(prefix="pvhashdir_")
    os.mkdir(os.path.join(d, "subdir"))
    with open(os.path.join(d, "plain.txt"), "w") as f:
        f.write("hi")
    with P.ParvionSession(d, env={"PARVION_DEMO_QUEUE": "0"}) as s:
        # Right-click the folder row: the item menu opens (Rename present) but no checksum entry.
        fd = find_text(s.screen()[0], "subdir")
        if not fd:
            print("FAIL test_no_checksum_on_folder: subdir not listed"); return False
        s.click(fd[1] + 2, fd[0] + 1, button=2); s.feed(0.7)
        chars = s.screen()[0]
        if not grid_contains(chars, "Rename"):
            print("FAIL test_no_checksum_on_folder: folder context menu did not open"); return False
        if grid_contains(chars, "Calculate Checksum"):
            print("FAIL test_no_checksum_on_folder: 'Calculate Checksum' shown for a folder"); return False
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
        for item in ("None", "MD5", "SHA-1", "SHA-256", "SHA-512"):
            if not find_text(chars, item):
                print(f"FAIL test_hash_settings_single_dropdown: dropdown missing {item}"); return False
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
    test_local_file_hash_end_to_end,
    test_backend_hash_non_ascii_path,
    test_no_checksum_on_folder,
    test_checksums_column_resize,
    test_checksums_row_selection,
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
