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
import textwrap

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


def _remote_header(s):
    """(row, path, path_col) for the Remote site header: the title row paints
    ' ' + label + '  ' + cur_path() (panes.hpp); the path is the first token after the label,
    and its 0-based start column is where the address-bar field is clickable."""
    chars = s.screen()[0]
    pos = T.find_text(chars, "Remote site")
    if pos is None:
        return None
    row = T.row_text(chars, pos[0])
    after = row[pos[1] + len("Remote site"):]
    toks = after.split()
    path = toks[0] if toks else ""
    col = row.find(path, pos[1] + len("Remote site")) if path else -1
    return (pos[0], path, col)


def _make_symlink_backend(directory):
    """Create a tiny deterministic parvionsftp protocol peer for symlink activation tests."""
    helper = os.path.join(directory, "fake-parvionsftp.py")
    command_log = os.path.join(directory, "commands.log")
    source = "#!/usr/bin/env python3\n" + textwrap.dedent(r'''
        import os
        import shlex
        import sys

        command_log = os.environ["PARVION_FAKE_COMMAND_LOG"]
        fail_target_list = os.environ.get("PARVION_FAKE_FAIL_TARGET_LIST") == "1"
        fail_rollback = os.environ.get("PARVION_FAKE_FAIL_ROLLBACK") == "1"
        rollback_marker = command_log + ".rollback-failed"
        cwd = "/"

        def event(kind, payload):
            sys.stdout.write(chr(ord("0") + kind) + payload + "\n")
            sys.stdout.flush()

        def listing(longname, name):
            event(8, longname)
            sys.stdout.write("0\n" + name + "\n")
            sys.stdout.flush()

        event(0, "parvionSftp started, protocol_version=12")
        for raw in sys.stdin:
            raw = raw.rstrip("\r\n")
            if not raw:
                continue
            with open(command_log, "a", encoding="utf-8") as log:
                log.write(raw + "\n")
            words = shlex.split(raw)
            cmd = words[0]
            if cmd == "keyfile":
                event(1, "1")
            elif cmd == "open":
                event(0, "Connected")
            elif cmd == "pwd":
                event(0, f'Current directory is: "{cwd}"')
            elif cmd == "ls":
                if cwd == "/dir_link" and fail_target_list:
                    event(2, "Unable to open destination listing")
                    event(1, "0")
                else:
                    if cwd == "/":
                        listing("lrwxrwxrwx 1 user group 0 Jan 1 00:00 dir_link -> real_dir", "dir_link")
                        listing("lrwxrwxrwx 1 user group 7 Jan 1 00:00 file_link -> real_file", "file_link")
                        listing("lrwxrwxrwx 1 user group 0 Jan 1 00:00 broken_link -> missing", "broken_link")
                        listing("drwxr-xr-x 1 user group 0 Jan 1 00:00 real_dir", "real_dir")
                    else:
                        listing("-rw-r--r-- 1 user group 3 Jan 1 00:00 inside.txt", "inside.txt")
                    event(1, "1")
            elif cmd == "cd":
                target = words[1]
                if target == "/dir_link" or target == "/real_dir":
                    cwd = target
                    event(0, f'New directory is: "{cwd}"')
                elif target == "/":
                    if fail_rollback and not os.path.exists(rollback_marker):
                        open(rollback_marker, "w").close()
                        event(2, "Rollback failed")
                        event(1, "0")
                    else:
                        cwd = target
                        event(0, f'New directory is: "{cwd}"')
                else:
                    event(2, f"Directory {target}: not a directory")
                    event(1, "0")
            else:
                event(2, f"Unsupported command: {cmd}")
                event(1, "0")
    ''')
    with open(helper, "w", encoding="utf-8") as f:
        f.write(source)
    os.chmod(helper, 0o755)
    return helper, command_log


def _connect_fake(s):
    if not T.fill_connect_field(s, "Host", "fake.test"):
        return False
    T.fill_connect_field(s, "User", "tester")
    s.write("\r")
    for _ in range(20):
        s.feed(0.5)
        if _find_remote_row(s, "dir_link") and _find_remote_row(s, "file_link"):
            return True
    return False


def _fake_commands(path):
    try:
        with open(path, encoding="utf-8") as f:
            return [line.rstrip("\n") for line in f]
    except FileNotFoundError:
        return []


def _settled_in_pub(s, tries=20):
    """Poll until the remote pane has fully entered /pub: its child 'example' dir is listed AND the
    header path reads exactly /pub (header and listing agree)."""
    for _ in range(tries):
        s.feed(1.0)
        if _find_remote_row(s, "/example") is not None and _remote_header_path(s) == "/pub":
            return True
    return False


# ----------------------------------- tests -----------------------------------

def test_remote_symlink_activation_probes_directory_first():
    """A remote symlink activation uses cd as the referent probe. Directory links navigate;
    failed probes retain the unchanged one-file download path, while explicit Download skips cd."""
    print("TEST: parvion nav - remote symlink activation probes referent with cd ... ", end="", flush=True)
    root = tempfile.mkdtemp(prefix="parvionsymlink_")
    try:
        helper, command_log = _make_symlink_backend(root)
        base_env = {
            "PARVION_SFTP_BIN": helper,
            "PARVION_FAKE_COMMAND_LOG": command_log,
            "PARVION_DEMO_QUEUE": "0",
            "PARVION_DEMO_HASH": "1",  # Hold queued downloads; the fake peer remains control-only.
            "PARVION_KEEPALIVE_SEC": "0",
            "PARVION_TIMEOUT_SEC": "0",
        }

        # Double-clicking a link to a directory uses the same cd/list transaction as a plain dir.
        with T.ParvionSession(root, env=base_env) as s:
            if not _connect_fake(s):
                print("FAIL - fake remote listing did not arrive"); return False
            hit = _find_remote_row(s, "dir_link")
            F.dclick(s, hit[1] + 1, hit[0] + 1, settle=0.8)
            for _ in range(10):
                s.feed(0.3)
                if _remote_header_path(s) == "/dir_link" and _find_remote_row(s, "inside.txt"):
                    break
            if _remote_header_path(s) != "/dir_link" or not _find_remote_row(s, "inside.txt"):
                print(f"FAIL - directory link did not navigate (header={_remote_header_path(s)!r})")
                return False
            commands = _fake_commands(command_log)
            if not any(line == 'cd "/dir_link"' for line in commands):
                print(f"FAIL - directory link did not probe/navigate with cd: {commands!r}")
                return False
            if any(line.startswith("parvion-stat ") for line in commands):
                print(f"FAIL - removed stat protocol command was sent: {commands!r}")
                return False

        # Enter on a link to a file must try cd, then enqueue its original path on failure.
        open(command_log, "w").close()
        with T.ParvionSession(root, env=base_env) as s:
            if not _connect_fake(s):
                print("FAIL - fake remote listing did not arrive for file link"); return False
            hit = _find_remote_row(s, "file_link")
            s.click(hit[1] + 1, hit[0] + 1)
            s.write("\r", settle=0.8)
            for _ in range(10):
                s.feed(0.3)
                if T.grid_contains(s.screen()[0], "Transferring (1)"):
                    break
            commands = _fake_commands(command_log)
            if not any(line == 'cd "/file_link"' for line in commands):
                print(f"FAIL - file link did not probe with cd: {commands!r}")
                return False
            if _remote_header_path(s) != "/" or not T.grid_contains(s.screen()[0], "Transferring (1)"):
                print(f"FAIL - failed probe did not queue one download (header={_remote_header_path(s)!r})")
                return False
            # A broken/inaccessible referent also fails cd and retains the transfer attempt.
            hit = _find_remote_row(s, "broken_link")
            s.click(hit[1] + 1, hit[0] + 1)
            s.write("\r", settle=0.8)
            for _ in range(10):
                s.feed(0.3)
                if T.grid_contains(s.screen()[0], "Transferring (2)"):
                    break
            commands = _fake_commands(command_log)
            if not any(line == 'cd "/broken_link"' for line in commands):
                print(f"FAIL - broken link did not probe with cd: {commands!r}")
                return False
            if _remote_header_path(s) != "/" or not T.grid_contains(s.screen()[0], "Transferring (2)"):
                print("FAIL - broken-link probe did not preserve transfer fallback")
                return False

        # Context-menu Download remains a transfer action and must not perform the cd probe.
        open(command_log, "w").close()
        with T.ParvionSession(root, env=base_env) as s:
            if not _connect_fake(s):
                print("FAIL - fake remote listing did not arrive for explicit download"); return False
            hit = _find_remote_row(s, "dir_link")
            s.click(hit[1] + 1, hit[0] + 1, button=2)
            download = T.find_text(s.screen()[0], "Download")
            if download is None:
                print("FAIL - Download action missing"); return False
            s.click(download[1] + 1, download[0] + 1)
            s.feed(0.5)
            commands = _fake_commands(command_log)
            if any(line.startswith("cd ") for line in commands):
                print(f"FAIL - explicit Download probed the symlink: {commands!r}")
                return False
            if not T.grid_contains(s.screen()[0], "Transferring (1)"):
                print("FAIL - explicit Download no longer queued the symlink")
                return False

        print("PASS")
        return True
    finally:
        shutil.rmtree(root, ignore_errors=True)

def test_remote_navigation_rolls_back_after_listing_failure():
    """If cd succeeds but the destination ls fails, backend cwd rolls back to the still-visible
    source. The retained source listing remains usable for a subsequent navigation."""
    print("TEST: parvion nav - failed destination listing rolls back cwd ... ", end="", flush=True)
    root = tempfile.mkdtemp(prefix="parvionnav_rollback_")
    try:
        helper, command_log = _make_symlink_backend(root)
        base_env = {
            "PARVION_SFTP_BIN": helper,
            "PARVION_FAKE_COMMAND_LOG": command_log,
            "PARVION_FAKE_FAIL_TARGET_LIST": "1",
            "PARVION_DEMO_QUEUE": "0",
            "PARVION_DEMO_HASH": "1",
            "PARVION_KEEPALIVE_SEC": "0",
            "PARVION_TIMEOUT_SEC": "0",
        }
        with T.ParvionSession(root, env=base_env) as s:
            if not _connect_fake(s):
                print("FAIL - fake remote listing did not arrive"); return False
            hit = _find_remote_row(s, "dir_link")
            F.dclick(s, hit[1] + 1, hit[0] + 1, settle=0.8)
            for _ in range(15):
                s.feed(0.3)
                commands = _fake_commands(command_log)
                if 'cd "/"' in commands:
                    break
            commands = _fake_commands(command_log)
            try:
                cd_pos = commands.index('cd "/dir_link"')
                ls_pos = commands.index("ls", cd_pos + 1)
                rollback_pos = commands.index('cd "/"', ls_pos + 1)
            except ValueError:
                print(f"FAIL - rollback command sequence missing/out of order: {commands!r}")
                return False
            if _remote_header_path(s) != "/" or not _find_remote_row(s, "real_dir"):
                print(f"FAIL - source view was not retained (header={_remote_header_path(s)!r})")
                return False
            # Rollback completion must release the transaction for another ordinary navigation.
            hit = _find_remote_row(s, "real_dir")
            F.dclick(s, hit[1] + 1, hit[0] + 1, settle=0.8)
            for _ in range(10):
                s.feed(0.3)
                if _remote_header_path(s) == "/real_dir" and _find_remote_row(s, "inside.txt"):
                    break
            if _remote_header_path(s) != "/real_dir" or not _find_remote_row(s, "inside.txt"):
                print(f"FAIL - navigation stayed busy after rollback (header={_remote_header_path(s)!r})")
                return False
        print("PASS")
        return True
    finally:
        shutil.rmtree(root, ignore_errors=True)


def test_remote_navigation_recovers_after_rollback_failure():
    """If the compensating cd also fails, reconnect must restore the last committed source path."""
    print("TEST: parvion nav - failed cwd rollback reconnects to committed path ... ", end="", flush=True)
    root = tempfile.mkdtemp(prefix="parvionnav_recover_")
    try:
        helper, command_log = _make_symlink_backend(root)
        env = {
            "PARVION_SFTP_BIN": helper,
            "PARVION_FAKE_COMMAND_LOG": command_log,
            "PARVION_FAKE_FAIL_TARGET_LIST": "1",
            "PARVION_FAKE_FAIL_ROLLBACK": "1",
            "PARVION_DEMO_QUEUE": "0",
            "PARVION_DEMO_HASH": "1",
            "PARVION_KEEPALIVE_SEC": "0",
            "PARVION_TIMEOUT_SEC": "0",
        }
        with T.ParvionSession(root, env=env) as s:
            if not _connect_fake(s):
                print("FAIL - fake remote listing did not arrive"); return False
            hit = _find_remote_row(s, "dir_link")
            F.dclick(s, hit[1] + 1, hit[0] + 1, settle=0.8)
            recovered = False
            for _ in range(40):
                s.feed(0.5)
                commands = _fake_commands(command_log)
                if commands.count('cd "/"') >= 2 and _remote_header_path(s) == "/" and _find_remote_row(s, "real_dir"):
                    recovered = True
                    break
            if not recovered:
                print(f"FAIL - committed root was not restored: {_fake_commands(command_log)!r}")
                return False
        print("PASS")
        return True
    finally:
        shutil.rmtree(root, ignore_errors=True)

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


def test_remote_dotdot_normalizes():
    """Address-bar ".." on the remote pane resolves to the parent: chdir_abs normalizes
    "/pub/.." to "/", so the header reads "/" -- never a literal "/pub/..". This pins the
    POSIX path conversion (normalize_posix) on the remote navigation path."""
    print("TEST: parvion nav - remote address-bar .. normalizes to the parent ... ", end="", flush=True)
    if not F._rebex_reachable():
        print("SKIP (test.rebex.net unreachable)")
        return True
    d = tempfile.mkdtemp(prefix="parvionnavdd_")
    try:
        with T.ParvionSession(d, env={"PARVION_KEEPALIVE_SEC": "30"}) as s:
            if not F._connect_rebex(s):
                print("FAIL - could not connect / list the remote root")
                return False
            pub = _find_remote_row(s, "/pub")
            if pub is None:
                print("FAIL - /pub not found in the remote pane")
                return False
            F.dclick(s, pub[1] + 2, pub[0] + 1, settle=1.0)  # Enter /pub.
            if not _settled_in_pub(s):
                print(f"FAIL - did not settle into /pub (header={_remote_header_path(s)!r})")
                return False
            hdr = _remote_header(s)
            if hdr is None or hdr[1] != "/pub":
                print(f"FAIL - remote header not at /pub: {hdr!r}")
                return False
            r, path, col = hdr
            s.click(col + len(path) + 1, r + 1)  # Start the address edit, caret at the end.
            s.write("\x7f" * (len(path) + 8))     # Clear "/pub".
            s.write("..")
            s.write("\r")
            # Poll until we are back at root: header reads exactly "/" and /pub is listed again.
            settled = False
            for _ in range(20):
                s.feed(1.0)
                if _remote_header_path(s) == "/" and _find_remote_row(s, "/pub") is not None:
                    settled = True
                    break
            if not settled:
                print(f"FAIL - .. did not return to root (header={_remote_header_path(s)!r})")
                return False
            if T.grid_contains(s.screen()[0], "/pub/.."):
                print("FAIL - literal '/pub/..' appeared (path was not normalized)")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


TESTS = [
    test_remote_symlink_activation_probes_directory_first,
    test_remote_navigation_rolls_back_after_listing_failure,
    test_remote_navigation_recovers_after_rollback_failure,
    test_remote_enter_reveals_path_with_listing,
    test_double_doubleclick_does_not_overdescend,
    test_remote_dotdot_normalizes,
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
