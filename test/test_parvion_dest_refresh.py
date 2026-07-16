#!/usr/bin/env python3
# Copyright (c) Shaobo Song
# Licensed under the MIT license.

"""
End-to-end tests for the pane auto-refresh added in parvion/session.hpp (refresh_panes):
every path that stops an in-flight transfer must re-list each displayed directory the
transfer touched, without a manual Refresh.

Destination side (the partial/complete target file):
  1. Success     - a completed download shows up in the local pane (regression for the
                   refresh_panes refactor of the existing success-path behavior).
  2. Pause       - pausing an in-flight download re-lists the local pane, so the partial
                   target file becomes visible.
  3. Failure     - a transfer that fails mid-flight (server rejects reads past an offset)
                   re-lists the local pane.
  4. Remove      - removing an in-flight download from the queue re-lists the local pane.

Source side (an upload's local dir holds its PARVIONC2 resume-state file, kept on
pause/failure/removal for a later resume and deleted on success; parallel striping is
forced so the state file exists). Each interrupted upload must re-list BOTH panes:
the remote destination (partial target) and the local source (state file):
  5. Pause (up)  - pausing an in-flight upload shows the partial remote file and the
                   local state file.
  6. Remove (up) - same after removing the in-flight upload from the queue.
  7. Failure (up)- same after the server rejects writes past an offset.
  8. Success (up)- a paused-then-resumed upload that completes deletes the state file
                   and the local pane drops the stale state-file row.

Transfers run against a local in-process paramiko SFTP server with throttled file I/O,
so a multi-megabyte transfer reliably stays in flight long enough to pause/remove it,
and failures can be injected deterministically (reads/writes past N bytes error out).
The app side reuses the pty harness from test_parvion_panes.py; download cases run
single-stream (PARVION_NO_PARALLEL=1) for determinism, upload cases run parallel
(2 chunks) so the resume-state file machinery engages.
"""

import os
import sys
import glob
import time
import socket
import shutil
import logging
import tempfile
import threading

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_parvion_panes as T  # ParvionSession, row_text, find_text, kill_all_vtm, ...

kill_all_vtm = T.kill_all_vtm  # Re-exported for run_all_tests.py's between-test cleanup.

try:
    import paramiko
    from paramiko import SFTPServer, SFTPServerInterface, SFTPHandle, SFTPAttributes
    from paramiko import SFTP_OK, SFTP_FAILURE, AUTH_SUCCESSFUL, OPEN_SUCCEEDED
except ImportError:
    paramiko = None

USER, PASS = "test", "secret"
PANE_SPLIT = 56          # Local pane text lives left of this column, remote pane right of it.
FILE_SIZE = 16 << 20     # Big enough that a throttled transfer cannot finish mid-test.
ENV = {"PARVION_NO_PARALLEL": "1"}
# Upload cases force parallel striping (2 chunks) so the local resume-state file exists.
PENV = {"PARVION_THRESHOLD_BYTES": "4096", "PARVION_MAX_CONN": "2"}
# The pane's name column truncates long names, so match the state file by this prefix
# ("up.bin.parvion-upload-state.<hash>" on disk).
STATE_PREFIX = "up.bin.parvion"

if paramiko:
    logging.getLogger("paramiko").setLevel(logging.CRITICAL)  # Worker kills are expected; mute resets.
    _HOST_KEY = paramiko.RSAKey.generate(2048)  # One key for every per-test server instance.


# ------------------------------ local SFTP server ------------------------------

class _ThrottledHandle(SFTPHandle):
    """A real-file handle with a per-request delay (keeps transfers in flight) and an
    optional read/write-error injection past a byte offset (forces a mid-flight failure)."""

    def __init__(self, flags, fileobj, delay, fail_read_after, fail_write_after):
        super().__init__(flags)
        self.readfile = fileobj
        self.writefile = fileobj
        self._delay = delay
        self._fail_read_after = fail_read_after
        self._fail_write_after = fail_write_after

    def read(self, offset, length):
        if self._fail_read_after is not None and offset >= self._fail_read_after:
            return SFTP_FAILURE
        if self._delay:
            time.sleep(self._delay)
        return super().read(offset, length)

    def write(self, offset, data):
        if self._fail_write_after is not None and offset >= self._fail_write_after:
            return SFTP_FAILURE
        if self._delay:
            time.sleep(self._delay)
        return super().write(offset, data)

    def stat(self):
        try:
            return SFTPAttributes.from_stat(os.fstat(self.readfile.fileno()))
        except OSError as e:
            return SFTPServer.convert_errno(e.errno)

    def chattr(self, attr):
        return SFTP_OK  # Accept (ignore) fsetstat so timestamp/mode writes never fail a transfer.


class _StubSftp(SFTPServerInterface):
    """Minimal real-filesystem SFTP backend rooted at a temp dir."""

    def __init__(self, server, root="", delay=0.0, fail_read_after=None, fail_write_after=None, **kwargs):
        super().__init__(server, **kwargs)
        self.root = root
        self.delay = delay
        self.fail_read_after = fail_read_after
        self.fail_write_after = fail_write_after

    def _real(self, path):
        return os.path.join(self.root, self.canonicalize(path).lstrip("/"))

    def list_folder(self, path):
        try:
            out = []
            rp = self._real(path)
            for name in os.listdir(rp):
                attr = SFTPAttributes.from_stat(os.stat(os.path.join(rp, name)))
                attr.filename = name
                out.append(attr)
            return out
        except OSError as e:
            return SFTPServer.convert_errno(e.errno)

    def stat(self, path):
        try:
            return SFTPAttributes.from_stat(os.stat(self._real(path)))
        except OSError as e:
            return SFTPServer.convert_errno(e.errno)

    lstat = stat  # No symlinks in the test trees.

    def open(self, path, flags, attr):
        try:
            fd = os.open(self._real(path), flags | getattr(os, "O_BINARY", 0), 0o666)
            if flags & os.O_WRONLY:
                fstr = "ab" if flags & os.O_APPEND else "wb"
            elif flags & os.O_RDWR:
                fstr = "a+b" if flags & os.O_APPEND else "r+b"
            else:
                fstr = "rb"
            fileobj = os.fdopen(fd, fstr)
        except OSError as e:
            return SFTPServer.convert_errno(e.errno)
        return _ThrottledHandle(flags, fileobj, self.delay, self.fail_read_after, self.fail_write_after)

    def remove(self, path):
        try:
            os.remove(self._real(path))
            return SFTP_OK
        except OSError as e:
            return SFTPServer.convert_errno(e.errno)

    def rename(self, oldpath, newpath):
        try:
            os.rename(self._real(oldpath), self._real(newpath))
            return SFTP_OK
        except OSError as e:
            return SFTPServer.convert_errno(e.errno)

    def mkdir(self, path, attr):
        try:
            os.mkdir(self._real(path))
            return SFTP_OK
        except OSError as e:
            return SFTPServer.convert_errno(e.errno)

    def rmdir(self, path):
        try:
            os.rmdir(self._real(path))
            return SFTP_OK
        except OSError as e:
            return SFTPServer.convert_errno(e.errno)

    def chattr(self, path, attr):
        return SFTP_OK


class _AllowAll(paramiko.ServerInterface):
    def check_auth_password(self, username, password):
        return AUTH_SUCCESSFUL

    def get_allowed_auths(self, username):
        return "password"

    def check_channel_request(self, kind, chanid):
        return OPEN_SUCCEEDED


class LocalSftpServer:
    """A loopback SFTP server; each incoming connection (control + transfer workers)
    gets its own Transport with the shared throttle/failure configuration."""

    def __init__(self, root, delay=0.0, fail_read_after=None, fail_write_after=None):
        self.root = root
        self._transports = []
        self._closing = False
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._sock.bind(("127.0.0.1", 0))
        self._sock.listen(8)
        self.port = self._sock.getsockname()[1]
        self._cfg = {"root": root, "delay": delay,
                     "fail_read_after": fail_read_after, "fail_write_after": fail_write_after}
        threading.Thread(target=self._accept_loop, daemon=True).start()

    def _accept_loop(self):
        while not self._closing:
            try:
                conn, _ = self._sock.accept()
            except OSError:
                break
            try:
                t = paramiko.Transport(conn)
                t.add_server_key(_HOST_KEY)
                t.set_subsystem_handler("sftp", SFTPServer, _StubSftp, **self._cfg)
                t.start_server(event=threading.Event(), server=_AllowAll())
                self._transports.append(t)
            except Exception:
                pass

    def close(self):
        self._closing = True
        try:
            self._sock.close()
        except OSError:
            pass
        for t in self._transports:
            try:
                t.close()
            except Exception:
                pass


# --------------------------------- helpers ---------------------------------

def connect(s, port):
    """Fill the Quick Connect bar (host/user/pass/port) and wait for the remote listing."""
    rt = T.row_text(s.screen()[0], 1)
    hpos = rt.find("Host")
    if hpos < 0:
        return False
    s.click(hpos + 7, 2, button=0)                    # Focus the Host field.
    s.write("127.0.0.1"); s.write("\t")               # Tab -> User.
    s.write(USER); s.write("\t")                      # Tab -> Pass.
    s.write(PASS); s.write("\t")                      # Tab -> Port.
    s.write("\x7f\x7f"); s.write(str(port))           # Clear the seeded "22", set ours.
    s.write("\r")                                     # Enter -> Connect.
    for _ in range(30):
        s.feed(1.0)
        if find_in_pane(s.screen()[0], "/..", remote=True):
            return True
    return False


def find_in_pane(chars, needle, remote):
    """Locate *needle* inside one file pane only: rows above the queue panel, columns on
    the local (left) or remote (right) side of the split. The queue panel - whose item
    rows repeat the transfer's file names - starts at its "Local Name" column header
    (its tab strip sits below the items), so everything from that row down is excluded."""
    top, bot = 2, T.ROWS
    for marker in ("Local Name", "Transferring ("):
        pos = T.find_text(chars, marker)
        if pos and pos[0] < bot:
            bot = pos[0]
    lo, hi = (PANE_SPLIT, T.COLS) if remote else (0, PANE_SPLIT)
    for r in range(top, bot):
        c = T.row_text(chars, r).find(needle, lo, hi)
        if c >= 0:
            return (r, c)
    return None


def start_transfer(s, name, remote, action):
    """Right-click *name* in the local/remote pane and pick *action* (Download/Upload)."""
    pos = find_in_pane(s.screen()[0], name, remote=remote)
    if pos is None:
        return f"{name} not listed in the {'remote' if remote else 'local'} pane"
    s.click(pos[1] + 2, pos[0] + 1, button=2)
    act = T.find_text(s.screen()[0], action)
    if act is None:
        return f"'{action}' not in the item menu"
    s.click(act[1] + 1, act[0] + 1, button=0)
    return None


def queue_all_action(s, label):
    """Right-click the queue body's blank area and pick a 'Start/Pause/Remove All' item."""
    hdr = T.find_text(s.screen()[0], "Local Name")
    if hdr is None:
        return "queue header not found"
    s.click(100, hdr[0] + 2, button=2)  # Right of the last column: blank-area menu.
    it = T.find_text(s.screen()[0], label)
    if it is None:
        return f"'{label}' not in the queue menu"
    s.click(it[1] + 1, it[0] + 1, button=0)
    if label == "Remove All":  # Destructive: confirm the dialog (Enter -> Confirm, the default).
        if not T.grid_contains(s.screen()[0], "Remove all transfers on this tab?"):
            return "Remove All confirmation dialog not shown"
        s.write("\r")
    return None


def wait_partial(path, timeout, s):
    """Pump the UI until *path* exists with >0 bytes (the transfer is really in flight)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        s.feed(0.3)
        try:
            if os.path.getsize(path) > 0:
                return True
        except OSError:
            pass
    return False


def wait_in_pane(s, needle, remote, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        s.feed(0.5)
        if find_in_pane(s.screen()[0], needle, remote=remote):
            return True
    return False


def wait_gone_in_pane(s, needle, remote, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        s.feed(0.5)
        if not find_in_pane(s.screen()[0], needle, remote=remote):
            return True
    return False


def wait_tab_label(s, label, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        s.feed(0.5)
        if T.find_text(s.screen()[0], label):
            return True
    return False


def make_file(path, size):
    with open(path, "wb") as f:
        f.write(os.urandom(size))


def _interrupted_download_case(tag, action_label):
    """Shared body for the pause/remove cases: start a throttled download, interrupt it
    mid-flight via the queue menu, and require the partial target to appear in the local
    pane with no manual refresh."""
    print(f"TEST: parvion refresh - {tag} an in-flight download re-lists the local pane ... ",
          end="", flush=True)
    sroot = tempfile.mkdtemp(prefix="parvionsrv_")
    d = tempfile.mkdtemp(prefix="parvionref_")
    make_file(os.path.join(sroot, "big.bin"), FILE_SIZE)
    srv = LocalSftpServer(sroot, delay=0.05)  # ~640 KB/s: a 16 MB pull stays in flight for ~25 s.
    try:
        with T.ParvionSession(d, env=ENV) as s:
            if not connect(s, srv.port):
                print("FAIL - could not connect to the local SFTP server")
                return False
            err = start_transfer(s, "big.bin", remote=True, action="Download")
            if err:
                print(f"FAIL - {err}")
                return False
            if not wait_partial(os.path.join(d, "big.bin"), 30.0, s):
                print("FAIL - download never started writing the local target")
                return False
            # The download is in flight and the partial target is on disk, but the local
            # pane was listed before it existed - it must not be visible yet (otherwise
            # this test cannot prove the interrupt triggered the refresh).
            if find_in_pane(s.screen()[0], "big.bin", remote=False):
                print("FAIL - partial file already listed mid-flight (stale precondition)")
                return False
            err = queue_all_action(s, action_label)
            if err:
                print(f"FAIL - {err}")
                return False
            if not wait_in_pane(s, "big.bin", remote=False, timeout=10.0):
                print(f"FAIL - local pane not refreshed after {action_label} "
                      "(partial file only appears after a manual refresh)")
                return False
            print("PASS")
            return True
    finally:
        srv.close()
        shutil.rmtree(d, ignore_errors=True)
        shutil.rmtree(sroot, ignore_errors=True)


# ---------------------------------- tests ----------------------------------

def test_success_refreshes_local_pane():
    """A download that completes shows up in the local pane (success path regression)."""
    print("TEST: parvion refresh - a completed download re-lists the local pane ... ",
          end="", flush=True)
    sroot = tempfile.mkdtemp(prefix="parvionsrv_")
    d = tempfile.mkdtemp(prefix="parvionref_")
    make_file(os.path.join(sroot, "small.bin"), 64 << 10)
    srv = LocalSftpServer(sroot)  # No throttle: completes immediately.
    try:
        with T.ParvionSession(d, env=ENV) as s:
            if not connect(s, srv.port):
                print("FAIL - could not connect to the local SFTP server")
                return False
            err = start_transfer(s, "small.bin", remote=True, action="Download")
            if err:
                print(f"FAIL - {err}")
                return False
            if not wait_in_pane(s, "small.bin", remote=False, timeout=15.0):
                print("FAIL - local pane not refreshed after a successful download")
                return False
            lp = os.path.join(d, "small.bin")
            if not (os.path.isfile(lp) and os.path.getsize(lp) == 64 << 10):
                print("FAIL - downloaded file missing or truncated on disk")
                return False
            print("PASS")
            return True
    finally:
        srv.close()
        shutil.rmtree(d, ignore_errors=True)
        shutil.rmtree(sroot, ignore_errors=True)


def test_pause_refreshes_local_pane():
    return _interrupted_download_case("pausing", "Pause All")


def test_remove_refreshes_local_pane():
    return _interrupted_download_case("removing", "Remove All")


def test_failed_refreshes_local_pane():
    """A download that fails mid-flight (server errors reads past 256 KB) re-lists the
    local pane, so the partial target is visible without a manual refresh."""
    print("TEST: parvion refresh - a failed download re-lists the local pane ... ",
          end="", flush=True)
    sroot = tempfile.mkdtemp(prefix="parvionsrv_")
    d = tempfile.mkdtemp(prefix="parvionref_")
    make_file(os.path.join(sroot, "flaky.bin"), FILE_SIZE)
    srv = LocalSftpServer(sroot, delay=0.005, fail_read_after=256 << 10)
    try:
        with T.ParvionSession(d, env=ENV) as s:
            if not connect(s, srv.port):
                print("FAIL - could not connect to the local SFTP server")
                return False
            err = start_transfer(s, "flaky.bin", remote=True, action="Download")
            if err:
                print(f"FAIL - {err}")
                return False
            failed = False
            for _ in range(40):  # The first 256 KB land fast; then the read error fails the item.
                s.feed(0.5)
                if T.find_text(s.screen()[0], "Failed (1)"):
                    failed = True
                    break
            if not failed:
                print("FAIL - transfer never reached the Failed state")
                return False
            try:
                partial = os.path.getsize(os.path.join(d, "flaky.bin"))
            except OSError:
                partial = 0
            if partial <= 0:
                print("FAIL - no partial file on disk after the failure")
                return False
            if not wait_in_pane(s, "flaky.bin", remote=False, timeout=10.0):
                print("FAIL - local pane not refreshed after the failure "
                      "(partial file only appears after a manual refresh)")
                return False
            print("PASS")
            return True
    finally:
        srv.close()
        shutil.rmtree(d, ignore_errors=True)
        shutil.rmtree(sroot, ignore_errors=True)


def _assert_upload_panes_refreshed(s, d, after_what):
    """After an interrupted parallel upload: the remote pane must show the partial
    target and the local source pane must show the kept resume-state file."""
    if not glob.glob(os.path.join(d, "up.bin.parvion-upload-state.*")):
        return "no resume-state file on disk (parallel striping not engaged?)"
    if not wait_in_pane(s, "up.bin", remote=True, timeout=10.0):
        return (f"remote pane not refreshed after {after_what} "
                "(partial file only appears after a manual refresh)")
    if not wait_in_pane(s, STATE_PREFIX, remote=False, timeout=10.0):
        return (f"local source pane not refreshed after {after_what} "
                "(state file only appears after a manual refresh)")
    return None


def _interrupted_upload_case(tag, action_label):
    """Shared body for the upload pause/remove cases: start a throttled parallel upload,
    interrupt it mid-flight via the queue menu, and require both panes to re-list with
    no manual refresh: the remote destination (partial target) and the local source
    (resume-state file)."""
    print(f"TEST: parvion refresh - {tag} an in-flight upload re-lists both panes ... ",
          end="", flush=True)
    sroot = tempfile.mkdtemp(prefix="parvionsrv_")
    d = tempfile.mkdtemp(prefix="parvionref_")
    make_file(os.path.join(d, "up.bin"), FILE_SIZE)
    srv = LocalSftpServer(sroot, delay=0.05)  # Throttled writes keep the upload in flight.
    try:
        with T.ParvionSession(d, env=PENV) as s:
            if not connect(s, srv.port):
                print("FAIL - could not connect to the local SFTP server")
                return False
            err = start_transfer(s, "up.bin", remote=False, action="Upload")
            if err:
                print(f"FAIL - {err}")
                return False
            if not wait_partial(os.path.join(sroot, "up.bin"), 30.0, s):
                print("FAIL - upload never started writing the remote target")
                return False
            if find_in_pane(s.screen()[0], "up.bin", remote=True):
                print("FAIL - partial file already listed mid-flight (stale precondition)")
                return False
            if find_in_pane(s.screen()[0], STATE_PREFIX, remote=False):
                print("FAIL - state file already listed mid-flight (stale precondition)")
                return False
            err = queue_all_action(s, action_label)
            if err:
                print(f"FAIL - {err}")
                return False
            err = _assert_upload_panes_refreshed(s, d, action_label)
            if err:
                print(f"FAIL - {err}")
                return False
            print("PASS")
            return True
    finally:
        srv.close()
        shutil.rmtree(d, ignore_errors=True)
        shutil.rmtree(sroot, ignore_errors=True)


def test_pause_upload_refreshes_both_panes():
    return _interrupted_upload_case("pausing", "Pause All")


def test_remove_upload_refreshes_both_panes():
    return _interrupted_upload_case("removing", "Remove All")


def test_failed_upload_refreshes_both_panes():
    """An upload that fails mid-flight (server errors writes past 256 KB) re-lists the
    remote pane (partial target) and the local source pane (kept resume-state file)."""
    print("TEST: parvion refresh - a failed upload re-lists both panes ... ",
          end="", flush=True)
    sroot = tempfile.mkdtemp(prefix="parvionsrv_")
    d = tempfile.mkdtemp(prefix="parvionref_")
    make_file(os.path.join(d, "up.bin"), FILE_SIZE)
    srv = LocalSftpServer(sroot, delay=0.005, fail_write_after=256 << 10)
    try:
        with T.ParvionSession(d, env=PENV) as s:
            if not connect(s, srv.port):
                print("FAIL - could not connect to the local SFTP server")
                return False
            err = start_transfer(s, "up.bin", remote=False, action="Upload")
            if err:
                print(f"FAIL - {err}")
                return False
            if not wait_tab_label(s, "Failed (1)", timeout=20.0):
                print("FAIL - transfer never reached the Failed state")
                return False
            err = _assert_upload_panes_refreshed(s, d, "the failure")
            if err:
                print(f"FAIL - {err}")
                return False
            print("PASS")
            return True
    finally:
        srv.close()
        shutil.rmtree(d, ignore_errors=True)
        shutil.rmtree(sroot, ignore_errors=True)


def test_upload_success_clears_state_row():
    """A paused-then-resumed upload that completes deletes the resume-state file, and
    the local source pane drops the stale state-file row without a manual refresh."""
    print("TEST: parvion refresh - a completed upload clears the state row from the local pane ... ",
          end="", flush=True)
    sroot = tempfile.mkdtemp(prefix="parvionsrv_")
    d = tempfile.mkdtemp(prefix="parvionref_")
    size = 8 << 20
    make_file(os.path.join(d, "up.bin"), size)
    srv = LocalSftpServer(sroot, delay=0.04)  # ~1.6 MB/s on 2 chunks: ~5 s in flight.
    try:
        with T.ParvionSession(d, env=PENV) as s:
            if not connect(s, srv.port):
                print("FAIL - could not connect to the local SFTP server")
                return False
            err = start_transfer(s, "up.bin", remote=False, action="Upload")
            if err:
                print(f"FAIL - {err}")
                return False
            if not wait_partial(os.path.join(sroot, "up.bin"), 30.0, s):
                print("FAIL - upload never started writing the remote target")
                return False
            err = queue_all_action(s, "Pause All")
            if err:
                print(f"FAIL - {err}")
                return False
            # The pause refresh makes the state-file row visible: the stale row whose
            # disappearance the success path must then trigger.
            if not wait_in_pane(s, STATE_PREFIX, remote=False, timeout=10.0):
                print("FAIL - state file not listed after Pause All")
                return False
            err = queue_all_action(s, "Start All")
            if err:
                print(f"FAIL - {err}")
                return False
            if not wait_tab_label(s, "Succeeded (1)", timeout=60.0):
                print("FAIL - resumed upload never completed")
                return False
            if glob.glob(os.path.join(d, "up.bin.parvion-upload-state.*")):
                print("FAIL - resume-state file still on disk after success")
                return False
            if not wait_gone_in_pane(s, STATE_PREFIX, remote=False, timeout=10.0):
                print("FAIL - local pane still lists the deleted state file "
                      "(stale row only clears after a manual refresh)")
                return False
            print("PASS")
            return True
    finally:
        srv.close()
        shutil.rmtree(d, ignore_errors=True)
        shutil.rmtree(sroot, ignore_errors=True)


def test_remote_create_and_inline_rename():
    """The writable SFTP pane creates a unique default, enters inline Rename after refresh,
    commits the new name, and refuses a duplicate without letting server rename overwrite it."""
    print("TEST: parvion remote pane - Create Folder / inline Rename ... ", end="", flush=True)
    sroot = tempfile.mkdtemp(prefix="parvionsrv_remote_name_")
    d = tempfile.mkdtemp(prefix="parvionremote_name_")
    os.mkdir(os.path.join(sroot, "New folder"))
    os.mkdir(os.path.join(sroot, "taken"))
    srv = LocalSftpServer(sroot)
    try:
        with T.ParvionSession(d, env=ENV) as s:
            if not connect(s, srv.port):
                print("FAIL - could not connect to the local SFTP server")
                return False
            parent = find_in_pane(s.screen()[0], "/..", remote=True)
            if parent is None:
                print("FAIL - remote parent row not found")
                return False
            s.click(T.COLS - 5, parent[0] + 2, button=2)
            create = T.find_text(s.screen()[0], "Create Folder")
            if create is None:
                print("FAIL - remote Create Folder menu item not found")
                return False
            s.click(create[1] + 1, create[0] + 1)
            if not wait_in_pane(s, "New folder (2)", remote=True, timeout=10.0):
                print("FAIL - remote default was not created/refreshed")
                return False
            if not os.path.isdir(os.path.join(sroot, "New folder (2)")):
                print("FAIL - generated remote directory is missing on the server")
                return False
            s.write("\x7f" * len("New folder (2)"))
            s.write("remote_named")
            s.write("\r")
            if not wait_in_pane(s, "remote_named", remote=True, timeout=10.0):
                print("FAIL - committed remote rename was not refreshed")
                return False
            if not os.path.isdir(os.path.join(sroot, "remote_named")):
                print("FAIL - remote directory was not renamed on the server")
                return False

            renamed = find_in_pane(s.screen()[0], "remote_named", remote=True)
            s.click(renamed[1] + 1, renamed[0] + 1, button=2)
            rename = T.find_text(s.screen()[0], "Rename")
            s.click(rename[1] + 1, rename[0] + 1)
            s.write("\x7f" * len("remote_named"))
            s.write("taken")
            s.write("\r")
            if not os.path.isdir(os.path.join(sroot, "remote_named")) \
                    or not os.path.isdir(os.path.join(sroot, "taken")):
                print("FAIL - duplicate remote rename overwrote or removed an entry")
                return False
            print("PASS")
            return True
    finally:
        srv.close()
        shutil.rmtree(d, ignore_errors=True)
        shutil.rmtree(sroot, ignore_errors=True)


TESTS = [
    test_success_refreshes_local_pane,
    test_pause_refreshes_local_pane,
    test_failed_refreshes_local_pane,
    test_remove_refreshes_local_pane,
    test_pause_upload_refreshes_both_panes,
    test_remove_upload_refreshes_both_panes,
    test_failed_upload_refreshes_both_panes,
    test_upload_success_clears_state_row,
    test_remote_create_and_inline_rename,
]


def main():
    if paramiko is None:
        print("SKIP: paramiko not installed (pip install paramiko)")
        print("Results: 0/0 passed, 0 failed")
        return 0
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
