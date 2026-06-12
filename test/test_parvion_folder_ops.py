#!/usr/bin/env python3
# Copyright (c) Shaobo Song
# Licensed under the MIT license.

"""
End-to-end tests for the Parvion file-preview pane *folder* operations added on top of
parvion/panes.hpp + parvion/session.hpp — the Download / Upload / Delete right-click actions now recurse
into directories (modeled on FileZilla's CRecursiveOperation):

  1. Delete on a local folder removes the whole subtree from disk (recursive, no network).
  1b. Local Delete is asynchronous: with the worker stalled via PARVION_DEBUG_LOCAL_DEL_DELAY_MS the
     click returns immediately, the UI keeps responding, and disk/pane/status catch up on completion.
  2. Download on a remote folder mirrors the remote subtree locally and enqueues one transfer per
     file, then the transfers run. Exercised against the public read-only SFTP test server
     test.rebex.net (demo/password): downloading /pub recurses through /pub/example and pulls every
     file into <local>/pub/example/. Skipped automatically when that server is unreachable.

Upload's recursion mirrors Download's on the local side, but a read-only test server can't accept
uploads, so it is covered by the shared recop engine that Download exercises here rather than by a
dedicated upload case.

Driven via a pty using the SGR mouse protocol, reusing the harness from test_parvion_panes.py.
"""

import os
import sys
import time
import socket
import shutil
import hashlib
import tempfile
import subprocess

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_parvion_panes as T  # ParvionSession, replay, row_text, find_text, grid_contains, kill_all_vtm, ...

# Optional writable SFTP server for the upload-pooling test, as "host:port:user:pass" (rebex is
# read-only, so uploads need a server you provide). Example: PARVION_TEST_SFTP=127.0.0.1:2022:v1:1
PARVION_TEST_SFTP = os.environ.get("PARVION_TEST_SFTP", "")

REBEX_HOST, REBEX_USER, REBEX_PASS = "test.rebex.net", "demo", "password"
# /pub/example on the rebex test server (a stable, public, read-only listing). readme.txt is tiny
# (379 bytes) so it is a fast, byte-exact end-to-end check; the rest are small images.
REBEX_README_SIZE = 379
REBEX_EXAMPLE_MIN_FILES = 14  # The dir holds 16 files; tolerate a couple of throttled/slow transfers.


def dclick(s, col, row, settle=0.6):
    """A double-click (two rapid press/release pairs) — the pane navigates into a dir on it."""
    for _ in range(2):
        os.write(s.master_fd, f"\x1b[<0;{col};{row}M".encode()); time.sleep(0.02)
        os.write(s.master_fd, f"\x1b[<0;{col};{row}m".encode()); time.sleep(0.02)
    s.feed(settle)


def make_nested_tree():
    """A local dir with a non-empty sub-directory, to prove Delete recurses."""
    d = tempfile.mkdtemp(prefix="parvionfold_")
    sub = os.path.join(d, "topdir")
    os.mkdir(sub)
    os.mkdir(os.path.join(sub, "inner"))
    open(os.path.join(sub, "a.txt"), "w").close()
    open(os.path.join(sub, "inner", "b.txt"), "w").close()
    open(os.path.join(d, "keep.txt"), "w").close()  # A sibling that must survive.
    return d


# ----------------------------------- tests -----------------------------------

def test_local_delete_folder():
    """Right-click a local folder -> Delete removes the entire subtree (recursive) from disk."""
    print("TEST: parvion folder - local Delete removes a whole subtree ... ", end="", flush=True)
    d = make_nested_tree()
    try:
        with T.ParvionSession(d) as s:
            pos = T.find_text(s.screen()[0], "/topdir")  # Folders are shown with a leading "/".
            if pos is None:
                print("FAIL - topdir not listed")
                return False
            r, c = pos
            s.click(c + 1, r + 1, button=2)              # Right-click the folder -> item menu (selects it).
            de = T.find_text(s.screen()[0], "Delete")
            if de is None:
                print("FAIL - 'Delete' not in menu")
                return False
            s.click(de[1] + 1, de[0] + 1, button=0)
            s.write("\r")  # Confirm the delete dialog (Enter -> Confirm, the default selection).
            s.feed(0.8)
            if os.path.exists(os.path.join(d, "topdir")):
                print("FAIL - folder still on disk after Delete")
                return False
            if not os.path.exists(os.path.join(d, "keep.txt")):
                print("FAIL - sibling file was wrongly deleted")
                return False
            if T.grid_contains(s.screen()[0], "/topdir"):
                print("FAIL - folder still shown after Delete")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_local_delete_async():
    """A local Delete runs off the UI thread: with the worker stalled (test seam), the click
    returns at once, the in-flight status shows, the UI keeps responding, and disk + pane
    catch up once the worker lands."""
    print("TEST: parvion folder - local Delete is asynchronous (UI live while in flight) ... ", end="", flush=True)
    d = make_nested_tree()
    try:
        env = {"PARVION_DEMO_QUEUE": "1", "PARVION_DEBUG_LOCAL_DEL_DELAY_MS": "2000"}
        with T.ParvionSession(d, env=env) as s:
            pos = T.find_text(s.screen()[0], "/topdir")
            if pos is None:
                print("FAIL - topdir not listed")
                return False
            r, c = pos
            s.click(c + 1, r + 1, button=2)              # Right-click the folder -> item menu (selects it).
            de = T.find_text(s.screen()[0], "Delete")
            if de is None:
                print("FAIL - 'Delete' not in menu")
                return False
            s.click(de[1] + 1, de[0] + 1, button=0, settle=0.3)
            # Delete now asks for confirmation; the stalled worker starts on Confirm.
            t0 = time.time()
            s.write("\r", settle=0.3)
            # Well inside the 2 s stall: nothing removed yet, but the delete is reported in flight
            # (the disconnected remote pane mirrors the controller's status line).
            if not os.path.exists(os.path.join(d, "topdir")):
                print("FAIL - folder already gone (delete ran synchronously?)")
                return False
            if not T.grid_contains(s.screen()[0], "Deleting 1 local item"):
                print("FAIL - in-flight 'Deleting...' status not shown")
                return False
            # The UI must still respond mid-delete: a right-click opens the item menu again.
            kp = T.find_text(s.screen()[0], "keep.txt")
            if kp is None:
                print("FAIL - keep.txt not listed mid-delete")
                return False
            s.click(kp[1] + 1, kp[0] + 1, button=2, settle=0.3)
            if not T.grid_contains(s.screen()[0], "Rename"):
                print("FAIL - UI unresponsive mid-delete (no context menu)")
                return False
            s.click(T.COLS - 20, kp[0] + 1, settle=0.3)  # Click far away to dismiss the menu.
            # Wait out the stall: the worker lands, the pane re-lists, the status flips.
            remaining = t0 + 2.0 - time.time()
            if remaining > 0:
                time.sleep(remaining)
            s.feed(1.5)
            if os.path.exists(os.path.join(d, "topdir")):
                print("FAIL - folder still on disk after the stall elapsed")
                return False
            if not os.path.exists(os.path.join(d, "keep.txt")):
                print("FAIL - sibling file was wrongly deleted")
                return False
            if T.grid_contains(s.screen()[0], "/topdir"):
                print("FAIL - folder still shown after Delete")
                return False
            if not T.grid_contains(s.screen()[0], "Delete finished."):
                print("FAIL - completion status not shown")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def _rebex_reachable():
    try:
        socket.create_connection((REBEX_HOST, 22), timeout=10).close()
        return True
    except OSError:
        return False


def _connect_rebex(s):
    """Type the rebex credentials into the Quick Connect bar and connect; wait for the / listing."""
    rt = T.row_text(s.screen()[0], 1)
    hpos = rt.find("Host")
    if hpos < 0:
        return False
    s.click(hpos + 7, 2, button=0)                       # Focus the Host field.
    s.write(REBEX_HOST); s.write("\t")                   # Tab -> User.
    s.write(REBEX_USER); s.write("\t")                   # Tab -> Pass.
    s.write(REBEX_PASS); s.write("\r")                   # Enter -> Connect.
    for _ in range(45):
        s.feed(1.0)
        if T.grid_contains(s.screen()[0], "/pub"):       # The remote / listing has arrived.
            return True
    return False


def test_download_folder_recursive():
    """Download on a remote folder recreates its subtree locally and transfers every file.

    Connects to the public read-only test.rebex.net, downloads /pub (which contains /pub/example
    with ~16 files), and checks the recreated local tree + a byte-exact small file."""
    print("TEST: parvion folder - recursive Download from test.rebex.net ... ", end="", flush=True)
    if not _rebex_reachable():
        print("SKIP (test.rebex.net unreachable)")
        return True
    d = tempfile.mkdtemp(prefix="parviondl_")
    try:
        # A non-empty env without PARVION_DEMO_QUEUE: a real connection whose queue auto-starts transfers
        # (PARVION_DEMO_QUEUE would set no_autostart and the downloads would never run).
        with T.ParvionSession(d, env={"PARVION_KEEPALIVE_SEC": "30"}) as s:
            if not _connect_rebex(s):
                print("FAIL - could not connect / list the remote root")
                return False
            # Right-click the /pub folder in the remote (right) pane and choose Download.
            chars = s.screen()[0]
            hit = None
            for r in range(T.ROWS):
                c = T.row_text(chars, r).find("/pub", 50)  # col >= 50 -> the remote pane.
                if c >= 0:
                    hit = (r, c); break
            if hit is None:
                print("FAIL - /pub not found in the remote pane")
                return False
            s.click(hit[1] + 2, hit[0] + 1, button=2)
            dl = T.find_text(s.screen()[0], "Download")
            if dl is None:
                print("FAIL - 'Download' not in menu")
                return False
            s.click(dl[1] + 1, dl[0] + 1, button=0)

            example = os.path.join(d, "pub", "example")
            readme = os.path.join(example, "readme.txt")

            # 1) Recursion (fast, deterministic): the walk recreates <local>/pub/example/.
            ok_struct = False
            for _ in range(20):
                s.feed(1.0)
                if os.path.isdir(example):
                    ok_struct = True; break
            if not ok_struct:
                print("FAIL - local pub/example/ not recreated (recursion)")
                return False

            # 2) Transfers (end-to-end): wait for the tiny readme.txt to land byte-exact and for a
            #    healthy majority of the folder's files to arrive (lenient on the server throttling
            #    the odd large image).
            ok = False
            for _ in range(150):
                s.feed(1.0)
                files = [f for f in os.listdir(example) if os.path.getsize(os.path.join(example, f)) > 0]
                if os.path.isfile(readme) and os.path.getsize(readme) == REBEX_README_SIZE \
                   and len(files) >= REBEX_EXAMPLE_MIN_FILES:
                    ok = True; break
            if not ok:
                got = sorted(os.listdir(example)) if os.path.isdir(example) else []
                rsz = os.path.getsize(readme) if os.path.isfile(readme) else None
                print(f"FAIL - incomplete download (readme={rsz}, files={len(got)}: {got})")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_download_pooling_reuses_connection():
    """Transfers reuse a pooled, authenticated backend instead of respawning one per file: after a few
    files of a folder download have landed, the message log shows the connection being reused."""
    print("TEST: parvion folder - downloads reuse a pooled connection ... ", end="", flush=True)
    if not _rebex_reachable():
        print("SKIP (test.rebex.net unreachable)")
        return True
    d = tempfile.mkdtemp(prefix="parvionpool_")
    try:
        with T.ParvionSession(d, env={"PARVION_KEEPALIVE_SEC": "30"}) as s:
            if not _connect_rebex(s):
                print("FAIL - could not connect / list the remote root")
                return False
            chars = s.screen()[0]
            hit = None
            for r in range(T.ROWS):
                c = T.row_text(chars, r).find("/pub", 50)
                if c >= 0:
                    hit = (r, c); break
            if hit is None:
                print("FAIL - /pub not found in the remote pane")
                return False
            s.click(hit[1] + 2, hit[0] + 1, button=2)
            dl = T.find_text(s.screen()[0], "Download")
            if dl is None:
                print("FAIL - 'Download' not in menu")
                return False
            s.click(dl[1] + 1, dl[0] + 1, button=0)
            example = os.path.join(d, "pub", "example")
            # Wait until several files have transferred (so at least the 2nd..Nth ran on the reused
            # connection), then check the message log for the reuse marker.
            got = 0
            for _ in range(120):
                s.feed(1.0)
                got = len([f for f in os.listdir(example) if os.path.getsize(os.path.join(example, f)) > 0]) if os.path.isdir(example) else 0
                if got >= 4:
                    break
            if got < 4:
                print(f"FAIL - too few files transferred to observe reuse ({got})")
                return False
            ml = T.find_text(s.screen()[0], "Message log")
            if ml:
                s.click(ml[1] + 1, ml[0] + 1, button=0)
                s.feed(1.5)
            if not T.grid_contains(s.screen()[0], "Reusing connection"):
                print("FAIL - no 'Reusing connection' in the message log (per-file respawn?)")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_download_parallel_pooling():
    """Parallel (chunked) transfers also reuse pooled connections. With a tiny chunk threshold the
    folder's bigger files download in several chunks each; the chunk connections are reused across
    files (and with the single-stream small ones), and the multi-chunk files reassemble byte-correct."""
    print("TEST: parvion folder - parallel chunked download reuses the pool ... ", end="", flush=True)
    if not _rebex_reachable():
        print("SKIP (test.rebex.net unreachable)")
        return True
    d = tempfile.mkdtemp(prefix="parvionpar_")
    # 8 KB per-chunk gate + up to 4 connections per file: files >8 KB go parallel, smaller ones stay
    # single-stream, so both paths share the pool.
    env = {"PARVION_THRESHOLD_BYTES": "8192", "PARVION_MAX_CONN": "4", "PARVION_KEEPALIVE_SEC": "30"}
    try:
        with T.ParvionSession(d, env=env) as s:
            if not _connect_rebex(s):
                print("FAIL - could not connect / list the remote root")
                return False
            chars = s.screen()[0]
            hit = None
            for r in range(T.ROWS):
                c = T.row_text(chars, r).find("/pub", 50)
                if c >= 0:
                    hit = (r, c); break
            if hit is None:
                print("FAIL - /pub not found in the remote pane")
                return False
            s.click(hit[1] + 2, hit[0] + 1, button=2)
            dl = T.find_text(s.screen()[0], "Download")
            if dl is None:
                print("FAIL - 'Download' not in menu")
                return False
            s.click(dl[1] + 1, dl[0] + 1, button=0)
            example = os.path.join(d, "pub", "example")
            readme = os.path.join(example, "readme.txt")
            winform = os.path.join(example, "WinFormClient.png")  # ~80 KB -> several chunks at the 8 KB gate.
            # Wait until the byte total is stable for a few polls (the whole tree finished downloading).
            prev, stable = -1, 0
            for _ in range(180):
                s.feed(1.0)
                if not os.path.isdir(example):
                    continue
                files = os.listdir(example)
                tot = sum(os.path.getsize(os.path.join(example, f)) for f in files)
                stable = stable + 1 if (tot == prev and len(files) >= REBEX_EXAMPLE_MIN_FILES) else 0
                prev = tot
                if stable >= 4:
                    break
            files = os.listdir(example) if os.path.isdir(example) else []
            if not (os.path.isfile(readme) and os.path.getsize(readme) == REBEX_README_SIZE):
                print(f"FAIL - readme.txt size {os.path.getsize(readme) if os.path.isfile(readme) else None}")
                return False
            # The largest file is reassembled from multiple chunks: a correct size proves chunked
            # transfers over reused connections don't corrupt/truncate it.
            if not (os.path.isfile(winform) and os.path.getsize(winform) >= 79000):
                print(f"FAIL - WinFormClient.png not fully reassembled ({os.path.getsize(winform) if os.path.isfile(winform) else None})")
                return False
            if len(files) < REBEX_EXAMPLE_MIN_FILES:
                print(f"FAIL - too few files transferred ({len(files)})")
                return False
            ml = T.find_text(s.screen()[0], "Message log")
            if ml:
                s.click(ml[1] + 1, ml[0] + 1, button=0)
                s.feed(1.5)
            if not T.grid_contains(s.screen()[0], "Reusing connection"):
                print("FAIL - no 'Reusing connection' in the message log (chunks respawned per file?)")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def _pgrep_parvionsftp():
    r = subprocess.run(["pgrep", "-f", "parvionsftp"], capture_output=True, text=True)
    return set(x for x in r.stdout.split() if x)


def test_pause_terminates_connections():
    """FileZilla parity (QueueView::StopItem -> CSftpControlSocket::Cancel -> DoClose -> process kill):
    pausing an in-flight transfer terminates its backend connections outright — no soft-cancel, no
    keep-alive. Resume then reconnects from scratch and completes (chunk offsets come from the partial
    file / state file). Driven against the slow rebex links so a mid-transfer pause is easy to catch."""
    print("TEST: parvion folder - pause terminates in-flight connections ... ", end="", flush=True)
    if not _rebex_reachable():
        print("SKIP (test.rebex.net unreachable)")
        return True
    d = tempfile.mkdtemp(prefix="parvionpause_")
    # Force parallel chunks so a paused transfer holds several connections at once.
    env = {"PARVION_THRESHOLD_BYTES": "4096", "PARVION_MAX_CONN": "3", "PARVION_KEEPALIVE_SEC": "30"}
    try:
        with T.ParvionSession(d, env=env) as s:
            if not _connect_rebex(s):
                print("FAIL - could not connect / list the remote root")
                return False
            chars = s.screen()[0]
            hit = None
            for r in range(T.ROWS):
                c = T.row_text(chars, r).find("/pub", 50)
                if c >= 0:
                    hit = (r, c); break
            if hit is None:
                print("FAIL - /pub not found in the remote pane")
                return False
            s.click(hit[1] + 2, hit[0] + 1, button=2)
            dl = T.find_text(s.screen()[0], "Download")
            if dl is None:
                print("FAIL - 'Download' not in menu")
                return False
            s.click(dl[1] + 1, dl[0] + 1, button=0)
            # Wait until a multi-chunk download is actually in flight (control + >=2 worker backends).
            before = 0
            for _ in range(60):
                s.feed(0.5)
                if len(_pgrep_parvionsftp()) >= 3:
                    before = len(_pgrep_parvionsftp()); break
            if before < 2:
                print("FAIL - never caught an active transfer to pause")
                return False
            hdr = T.find_text(s.screen()[0], "Local Name")
            if hdr is None:
                print("FAIL - queue header not found")
                return False
            s.click(100, hdr[0] + 2, button=2)            # right-click an item's blank area
            pa = T.find_text(s.screen()[0], "Pause All")
            if pa is None:
                print("FAIL - 'Pause All' not in menu")
                return False
            s.click(pa[1] + 1, pa[0] + 1, button=0)
            # The transfer's worker backends must be killed promptly; only the control session
            # (and nothing else) stays connected.
            after = before
            for _ in range(12):
                s.feed(0.5); after = min(after, len(_pgrep_parvionsftp()))
                if after <= before - 2:
                    break
            if after > before - 2:
                print(f"FAIL - pause left transfer backends running (pids {before} -> {after}; expected them terminated)")
                return False
            # Resume and confirm fresh connections finish the job correctly.
            s.click(100, hdr[0] + 2, button=2)
            sa = T.find_text(s.screen()[0], "Start All")
            if sa is None:
                print("FAIL - 'Start All' not in menu")
                return False
            s.click(sa[1] + 1, sa[0] + 1, button=0)
            example = os.path.join(d, "pub", "example")
            readme = os.path.join(example, "readme.txt")
            prev, stable = -1, 0
            for _ in range(180):
                s.feed(1.0)
                if not os.path.isdir(example):
                    continue
                files = os.listdir(example)
                tot = sum(os.path.getsize(os.path.join(example, f)) for f in files)
                stable = stable + 1 if (tot == prev and len(files) >= REBEX_EXAMPLE_MIN_FILES) else 0
                prev = tot
                if stable >= 4:
                    break
            files = os.listdir(example) if os.path.isdir(example) else []
            if not (os.path.isfile(readme) and os.path.getsize(readme) == REBEX_README_SIZE and len(files) >= REBEX_EXAMPLE_MIN_FILES):
                print(f"FAIL - resume did not complete the download ({len(files)} files)")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_remove_terminates_connections():
    """FileZilla parity: removing an in-flight transfer (Remove / Remove All) terminates its backend
    connections outright, exactly like Pause — only successfully completed transfers pool theirs.
    Driven against rebex so a mid-transfer Remove is easy to catch."""
    print("TEST: parvion folder - Remove terminates in-flight connections ... ", end="", flush=True)
    if not _rebex_reachable():
        print("SKIP (test.rebex.net unreachable)")
        return True
    d = tempfile.mkdtemp(prefix="parvionrm_")
    env = {"PARVION_THRESHOLD_BYTES": "4096", "PARVION_MAX_CONN": "3", "PARVION_KEEPALIVE_SEC": "30"}
    try:
        with T.ParvionSession(d, env=env) as s:
            if not _connect_rebex(s):
                print("FAIL - could not connect / list the remote root")
                return False
            chars = s.screen()[0]
            hit = None
            for r in range(T.ROWS):
                c = T.row_text(chars, r).find("/pub", 50)
                if c >= 0:
                    hit = (r, c); break
            if hit is None:
                print("FAIL - /pub not found in the remote pane")
                return False
            s.click(hit[1] + 2, hit[0] + 1, button=2)
            dl = T.find_text(s.screen()[0], "Download")
            if dl is None:
                print("FAIL - 'Download' not in menu")
                return False
            s.click(dl[1] + 1, dl[0] + 1, button=0)
            before = 0
            for _ in range(60):
                s.feed(0.5)
                if len(_pgrep_parvionsftp()) >= 3:
                    before = len(_pgrep_parvionsftp()); break
            if before < 2:
                print("FAIL - never caught an active transfer to remove")
                return False
            hdr = T.find_text(s.screen()[0], "Local Name")
            if hdr is None:
                print("FAIL - queue header not found")
                return False
            s.click(100, hdr[0] + 2, button=2)
            ra = T.find_text(s.screen()[0], "Remove All")
            if ra is None:
                print("FAIL - 'Remove All' not in menu")
                return False
            s.click(ra[1] + 1, ra[0] + 1, button=0)
            s.write("\r")  # Confirm the Remove All dialog (Enter -> Confirm, the default selection).
            after = before
            for _ in range(12):
                s.feed(0.5); after = min(after, len(_pgrep_parvionsftp()))
                if after <= before - 2:
                    break
            if after > before - 2:
                print(f"FAIL - Remove left transfer backends running (pids {before} -> {after}; expected them terminated)")
                return False
            if not T.grid_contains(s.screen()[0], "Transferring (0)"):
                print("FAIL - queue not emptied by Remove All")
                return False
            print("PASS")
            return True
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_upload_parallel_pooling():
    """Uploads reuse pooled connections too — including the parked follower chunks of a fresh parallel
    upload (regression: those used to respawn a backend per file). Needs a writable SFTP server given
    via PARVION_TEST_SFTP; verifies byte-exact upload (paramiko round-trip) AND that the whole multi-file,
    multi-chunk upload runs on a small, bounded set of backend processes."""
    print("TEST: parvion folder - parallel upload reuses pooled connections ... ", end="", flush=True)
    if not PARVION_TEST_SFTP:
        print("SKIP (set PARVION_TEST_SFTP=host:port:user:pass)")
        return True
    try:
        import paramiko
    except ImportError:
        print("SKIP (paramiko not installed)")
        return True
    try:
        host, port_s, user, pw = PARVION_TEST_SFTP.split(":", 3)
        port = int(port_s)
    except ValueError:
        print("FAIL - PARVION_TEST_SFTP must be host:port:user:pass")
        return False
    try:
        socket.create_connection((host, port), timeout=8).close()
    except OSError:
        print(f"SKIP ({host}:{port} unreachable)")
        return True

    def connect_sftp():
        c = paramiko.SSHClient(); c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        c.connect(host, port=port, username=user, password=pw, look_for_keys=False, allow_agent=False)
        return c, c.open_sftp()

    remote_dir = "/parvionpool_up_%d" % os.getpid()  # unique so parallel runs don't collide; cleaned up below.

    def remote_cleanup():
        try:
            c, sf = connect_sftp()
            try:
                for f in sf.listdir(remote_dir): sf.remove(remote_dir + "/" + f)
                sf.rmdir(remote_dir)
            except IOError:
                pass
            c.close()
        except Exception:
            pass

    remote_cleanup()
    d = tempfile.mkdtemp(prefix="parvionupl_")
    up = os.path.join(d, os.path.basename(remote_dir))  # local folder name == remote folder name on upload.
    os.mkdir(up)
    want = {}
    for i in range(5):
        b = os.urandom(40960)  # 40 KB -> several chunks at the 4 KB gate (a fresh parallel upload).
        want["file_%d.dat" % i] = hashlib.sha256(b).hexdigest()
        with open(os.path.join(up, "file_%d.dat" % i), "wb") as f:
            f.write(b)
    try:
        max_conn = 3
        with T.ParvionSession(d, env={"PARVION_THRESHOLD_BYTES": "4096", "PARVION_MAX_CONN": str(max_conn)}) as s:
            rt = T.row_text(s.screen()[0], 1)
            s.click(rt.find("Host") + 7, 2, button=0)
            s.write(host); s.write("\t"); s.write(user); s.write("\t"); s.write(pw)
            s.write("\t"); s.write("\x7f\x7f"); s.write(str(port)); s.write("\r")  # Port field (clear "22").
            ok_conn = False
            for _ in range(30):
                s.feed(1.0)
                if T.row_text(s.screen()[0], 4).find("/..", 40) >= 0:
                    ok_conn = True; break
            if not ok_conn:
                print("FAIL - could not connect to the SFTP server")
                return False
            pos = T.find_text(s.screen()[0], os.path.basename(remote_dir))
            if pos is None:
                print("FAIL - upload folder not shown in the local pane")
                return False
            s.click(pos[1] + 1, pos[0] + 1, button=2)
            ul = T.find_text(s.screen()[0], "Upload")
            if ul is None:
                print("FAIL - 'Upload' not in menu")
                return False
            s.click(ul[1] + 1, ul[0] + 1, button=0)
            # Track distinct backend PIDs while uploading, and read the bytes back once all land.
            pids = _pgrep_parvionsftp()
            got = {}
            for _ in range(60):
                s.feed(0.5)
                pids |= _pgrep_parvionsftp()
                try:
                    c, sf = connect_sftp()
                    try:
                        names = sf.listdir(remote_dir)
                    except IOError:
                        names = []
                    if len(names) >= 5:
                        for n in names:
                            with sf.open(remote_dir + "/" + n, "rb") as rf:
                                got[n] = hashlib.sha256(rf.read()).hexdigest()
                    c.close()
                except Exception:
                    pass
                if len(got) >= 5:
                    break
            # Correctness: every uploaded file is byte-exact.
            if not (len(got) == 5 and all(want[n] == got.get(n) for n in want)):
                print(f"FAIL - upload not byte-exact (got {len(got)}/5 files)")
                return False
            # Reuse: a 5-file, 3-chunk-each upload must run on a bounded set of backends (1 control +
            # up to max_conn pooled workers). Before the fix the parked followers respawned per file.
            if len(pids) > max_conn + 2:
                print(f"FAIL - {len(pids)} backend processes spawned (no follower reuse; expected <= {max_conn + 2})")
                return False
            print("PASS")
            return True
    finally:
        remote_cleanup()
        shutil.rmtree(d, ignore_errors=True)


TESTS = [
    test_local_delete_folder,
    test_local_delete_async,
    test_download_folder_recursive,
    test_download_pooling_reuses_connection,
    test_download_parallel_pooling,
    test_pause_terminates_connections,
    test_remove_terminates_connections,
    test_upload_parallel_pooling,
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
