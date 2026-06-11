#!/usr/bin/env python3
# Copyright (c) Shaobo Song
# Licensed under the MIT license.

"""
End-to-end regression tests for Parvion remote (SFTP) directory *navigation* timing, on top of
parvion/panes.hpp + parvion/session.hpp. Entering a remote dir is a two-step backend exchange (cd, then ls);
two bugs that lived in the gap between those steps are pinned down here:

  1. Premature path switch. The pane must reveal the destination only once its listing is in -- the
     header path and the listing switch together, never "new path shown over the previous dir's rows".
     (Bug: the path used to be committed at cd-done, before the ls reply.)

  2. Double double-click over-descends. Under high latency a second double-click issued while the
     first cd/ls was still in flight used to resolve the still-selected row against the half-updated
     path -- e.g. entering "test", then "test/test". It must now be ignored / harmless.

Both are remote-only (the local pane is synchronous and commits path + listing together). Exercised
against the public read-only test.rebex.net (demo/password); skipped automatically when offline.

The race in (2) only opens for ~1 round-trip -- shorter than the 500 ms double-click timeout that has
to separate two *distinct* double-clicks -- so it can't be hit by wall-clock timing on a fast link.
The session exposes a test-only seam, PARVION_DEBUG_LS_DELAY_MS, that holds the post-cd `ls` to widen
that window deterministically (it has no effect unless set).

Driven via a pty using the SGR mouse protocol, reusing the harness from test_parvion_panes.py and the
rebex connect / double-click helpers from test_parvion_folder_ops.py.
"""

import os
import sys
import time
import shutil
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_parvion_panes as T       # ParvionSession, replay, row_text, find_text, grid_contains, ROWS, ...
import test_parvion_folder_ops as F  # _connect_rebex, _rebex_reachable, dclick, REBEX_*

kill_all_vtm = T.kill_all_vtm  # Let run_all_tests.py reuse our between-test cleanup.


def _find_remote_row(s, needle, start_col=50):
    """Locate `needle` in the remote (right) pane; return its 0-based (row, col) or None."""
    chars = s.screen()[0]
    for r in range(T.ROWS):
        c = T.row_text(chars, r).find(needle, start_col)
        if c >= 0:
            return (r, c)
    return None


def _remote_header_path(s):
    """The path shown in the Remote site pane header (the text after the 'Remote site' label).
    Mirrors panes.hpp's title row: ' ' + label + '  ' + cur_path(); for the remote pane that path
    is committed only when the listing lands (c_ls), so it is what the user sees as 'the current dir'."""
    chars = s.screen()[0]
    pos = T.find_text(chars, "Remote site")
    if pos is None:
        return None
    after = T.row_text(chars, pos[0])[pos[1] + len("Remote site"):].strip()
    return after.split()[0] if after else ""


def _settled_in_pub(s, tries=20):
    """Poll until the remote pane has fully entered /pub: its child 'example' dir is listed AND the
    header path reads exactly /pub (header and listing agree)."""
    for _ in range(tries):
        s.feed(1.0)
        if _find_remote_row(s, "/example") is not None and _remote_header_path(s) == "/pub":
            return True
    return False


# ----------------------------------- tests -----------------------------------

def test_remote_enter_reveals_path_with_listing():
    """Entering a remote dir reveals the new path together with its listing (the deferred-commit
    endpoint): after double-clicking /pub, the header reads /pub *and* the /pub listing is shown."""
    print("TEST: parvion nav - entering a remote dir reveals path with its listing ... ", end="", flush=True)
    if not F._rebex_reachable():
        print("SKIP (test.rebex.net unreachable)")
        return True
    d = tempfile.mkdtemp(prefix="parvionnav_")
    try:
        with T.ParvionSession(d, env={"PARVION_KEEPALIVE_SEC": "30"}) as s:
            if not F._connect_rebex(s):
                print("FAIL - could not connect / list the remote root")
                return False
            pub = _find_remote_row(s, "/pub")
            if pub is None:
                print("FAIL - /pub not found in the remote pane")
                return False
            F.dclick(s, pub[1] + 2, pub[0] + 1, settle=1.0)  # Double-click /pub to enter it.
            if not _settled_in_pub(s):
                print(f"FAIL - did not settle into /pub (header={_remote_header_path(s)!r}, "
                      f"example_listed={_find_remote_row(s, '/example') is not None})")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_double_doubleclick_does_not_overdescend():
    """A second double-click fired while the first cd/ls is still in flight must not over-descend
    (enter <dir>/<dir>). PARVION_DEBUG_LS_DELAY_MS holds the post-cd `ls` ~4 s, so a clean second
    double-click (>500 ms after the first, hence a distinct double-click) lands well inside the
    in-flight window. With the in-flight guard + deferred path commit, that click is ignored and we
    land cleanly in /pub -- never /pub/pub."""
    print("TEST: parvion nav - double double-click does not over-descend ... ", end="", flush=True)
    if not F._rebex_reachable():
        print("SKIP (test.rebex.net unreachable)")
        return True
    d = tempfile.mkdtemp(prefix="parvionnav2_")
    try:
        with T.ParvionSession(d, env={"PARVION_KEEPALIVE_SEC": "30", "PARVION_DEBUG_LS_DELAY_MS": "4000"}) as s:
            if not F._connect_rebex(s):
                print("FAIL - could not connect / list the remote root")
                return False
            pub = _find_remote_row(s, "/pub")
            if pub is None:
                print("FAIL - /pub not found in the remote pane")
                return False
            col, row = pub[1] + 2, pub[0] + 1
            F.dclick(s, col, row, settle=1.0)  # 1st double-click: cd /pub issued; its ls is held ~4 s.
            F.dclick(s, col, row, settle=1.0)  # 2nd double-click, inside the held window: must be ignored.
            # Let the held ls fire and everything settle, then assert we are cleanly in /pub.
            settled = _settled_in_pub(s, tries=15)
            hp = _remote_header_path(s)
            if hp and hp.count("/pub") > 1:  # e.g. "/pub/pub": the second click over-descended.
                print(f"FAIL - over-descended into {hp!r} (second double-click was not ignored)")
                return False
            if T.grid_contains(s.screen()[0], "/pub/pub"):
                print("FAIL - a /pub/pub path appeared (over-descend / failed cd into a nonexistent dir)")
                return False
            if not settled:
                print(f"FAIL - did not settle into /pub (header={hp!r}, "
                      f"example_listed={_find_remote_row(s, '/example') is not None})")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


TESTS = [
    test_remote_enter_reveals_path_with_listing,
    test_double_doubleclick_does_not_overdescend,
]


def main():
    if not os.path.isfile(T.VTM_TILE_BINARY):
        print(f"ERROR: vtm-tile binary not found at {T.VTM_TILE_BINARY}")
        return 1
    T.kill_all_vtm()
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
            T.kill_all_vtm()
            time.sleep(0.4)
    total = passed + failed
    print(f"\n{'=' * 60}")
    print(f"Results: {passed}/{total} passed, {failed} failed")
    print(f"{'=' * 60}")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
