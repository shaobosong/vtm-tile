#!/usr/bin/env python3
# Copyright (c) Shaobo Song
# Licensed under the MIT license.

"""Regression coverage for resumed chunks parked by the SSH connect-burst limiter."""

import glob
import hashlib
import os
import select
import shutil
import struct
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_parvion_dest_refresh as R


kill_all_vtm = R.kill_all_vtm

CHANNELS = 10
BURST = 6
FILE_SIZE = 40 << 20
MIN_SAVED = 64 << 10
ENV = {
    "PARVION_THRESHOLD_BYTES": "1",
    "PARVION_MAX_CONN": str(CHANNELS),
    "PARVION_CONNECT_BURST": str(BURST),
}


def make_payload(path):
    """Write a deterministic, non-uniform file without retaining 40 MiB in memory."""
    block = bytes((i * 37 + 11) & 0xff for i in range(1 << 20))
    with open(path, "wb") as f:
        for _ in range(FILE_SIZE // len(block)):
            f.write(block)


def file_hash(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def state_progress(path):
    """Return the PARVIONC2 per-part transferred counters, or None while unavailable."""
    try:
        with open(path, "rb") as f:
            data = f.read()
        if len(data) < 72 or data[:9] != b"PARVIONC2":
            return None
        count = struct.unpack_from("<I", data, 24)[0]
        metadata_size = struct.unpack_from("<I", data, 32)[0]
        base = 72 + metadata_size
        if len(data) < base + count * 24:
            return None
        return [struct.unpack_from("<Q", data, base + i * 24 + 16)[0]
                for i in range(count)]
    except OSError:
        return None


def pump_bounded(s, duration):
    """Drain terminal output for a fixed time; the shared harness extends while output flows."""
    deadline = time.time() + duration
    while time.time() < deadline:
        ready, _, _ = select.select([s.master_fd], [], [], min(0.02, deadline - time.time()))
        if not ready:
            continue
        try:
            block = os.read(s.master_fd, 65536)
        except OSError:
            break
        if not block:
            break
        s._buf += block


def click_bounded(s, col, row, button=0, settle=0.15):
    os.write(s.master_fd, f"\x1b[<{button};{col};{row}M".encode())
    time.sleep(0.03)
    os.write(s.master_fd, f"\x1b[<{button};{col};{row}m".encode())
    pump_bounded(s, settle)


def start_transfer_bounded(s, name, upload):
    pos = R.find_in_pane(s.screen()[0], name, remote=not upload)
    if pos is None:
        return f"{name} not listed in the {'local' if upload else 'remote'} pane"
    s.click(pos[1] + 2, pos[0] + 1, button=2)
    label = "Upload" if upload else "Download"
    action = R.T.find_text(s.screen()[0], label)
    if action is None:
        return f"'{label}' not in the item menu"
    click_bounded(s, action[1] + 1, action[0] + 1)
    return None


def queue_action_bounded(s, label):
    header = R.T.find_text(s.screen()[0], "Local Name")
    if header is None:
        return "queue header not found"
    click_bounded(s, 100, header[0] + 2, button=2)
    action = R.T.find_text(s.screen()[0], label)
    if action is None:
        return f"'{label}' not in the queue menu"
    click_bounded(s, action[1] + 1, action[0] + 1)
    return None


def wait_for_saved_progress(s, state_path, timeout=45.0):
    """Wait until every part, including the four delayed ones, has a useful checkpoint."""
    deadline = time.time() + timeout
    last = None
    while time.time() < deadline:
        pump_bounded(s, 0.15)
        progress = state_progress(state_path)
        if progress is not None:
            last = progress
        if progress and len(progress) == CHANNELS and min(progress) >= MIN_SAVED:
            return progress, last
    return None, last


def verify_resume(direction):
    sroot = tempfile.mkdtemp(prefix=f"parvionresume_{direction}_srv_")
    local = tempfile.mkdtemp(prefix=f"parvionresume_{direction}_local_")
    upload = direction == "upload"
    source = os.path.join(local if upload else sroot, "ten.bin")
    target = os.path.join(sroot if upload else local, "ten.bin")
    make_payload(source)
    state_glob = os.path.join(local, f"ten.bin.parvion-{direction}-state.*")
    srv = R.LocalSftpServer(sroot, delay=0.5)
    try:
        with R.T.ParvionSession(local, env=ENV) as s:
            if not R.connect(s, srv.port):
                return "could not connect to the local SFTP server"
            err = start_transfer_bounded(s, "ten.bin", upload)
            if err:
                return err

            state_path = None
            deadline = time.time() + 15.0
            while time.time() < deadline and state_path is None:
                pump_bounded(s, 0.15)
                paths = glob.glob(state_glob)
                if paths:
                    state_path = paths[0]
            if state_path is None:
                queue_text = " | ".join(
                    R.T.row_text(s.screen()[0], row).strip()
                    for row in range(R.T.ROWS)
                    if any(word in R.T.row_text(s.screen()[0], row)
                           for word in ("ten.bin", "Transferring", "Failed", "Succeeded")))
                return ("resume-state file was not created; "
                        f"local entries={os.listdir(local)}, target_exists={os.path.exists(target)}, "
                        f"queue={queue_text!r}")

            saved, last_progress = wait_for_saved_progress(s, state_path)
            if saved is None:
                queue_text = " | ".join(
                    R.T.row_text(s.screen()[0], row).strip()
                    for row in range(R.T.ROWS)
                    if any(word in R.T.row_text(s.screen()[0], row)
                           for word in ("ten.bin", "Transferring", "Failed", "Succeeded")))
                return ("not all ten channels reached checkpointed progress; "
                        f"last={last_progress}, state_exists={os.path.exists(state_path)}, "
                        f"target_size={os.path.getsize(target) if os.path.exists(target) else None}, "
                        f"queue={queue_text!r}")
            if any(value <= 0 for value in saved[BURST:]):
                return f"channels {BURST + 1}-{CHANNELS} had no saved progress: {saved}"

            err = queue_action_bounded(s, "Pause All")
            if err:
                return err
            paused = state_progress(state_path)
            if paused is None or any(now < before for now, before in zip(paused, saved)):
                return f"pause lost checkpointed progress: before={saved}, after={paused}"

            err = queue_action_bounded(s, "Start All")
            if err:
                return err

            # On the broken path, activating parked resumed workers calls begin()/rearm(),
            # resets their counters, and immediately writes zeros back to parts 7-10.
            # Sample through delayed activation, rejecting any transient regression. Some helper
            # paths emit their next progress delta only with the final Done event, at which point
            # pump_queue removes the completed state file in the same tick, so advancing every
            # on-disk counter is not a reliable precondition for the second pause.
            floor = paused
            completed = False
            deadline = time.time() + 1.5
            while time.time() < deadline:
                pump_bounded(s, 0.05)
                current = state_progress(state_path)
                if current is None:
                    completed = True
                    break  # Transfer completed and removed the state file.
                if any(now < before for now, before in zip(current, floor)):
                    return f"resume regressed a channel counter: floor={floor}, current={current}"

            if not completed:
                err = queue_action_bounded(s, "Pause All")
                if err:
                    return err
                second_pause = state_progress(state_path)
                if second_pause is None or any(now < before for now, before in zip(second_pause, floor)):
                    return f"second pause retained invalid progress: floor={floor}, after={second_pause}"

                err = queue_action_bounded(s, "Start All")
                if err:
                    return err
            if not R.wait_tab_label(s, "Succeeded (1)", timeout=90.0):
                return "transfer did not complete after the second resume"
            if glob.glob(state_glob):
                return "resume-state file remained after success"
            if not os.path.isfile(target) or file_hash(target) != file_hash(source):
                return "completed target is not byte-identical to the source"
            return None
    finally:
        srv.close()
        shutil.rmtree(local, ignore_errors=True)
        shutil.rmtree(sroot, ignore_errors=True)


def test_ten_channel_pause_resume():
    print("TEST: parvion 10-channel pause/resume preserves delayed channel progress ... ",
          end="", flush=True)
    for direction in ("download", "upload"):
        error = verify_resume(direction)
        if error:
            print(f"FAIL - {direction}: {error}")
            return False
    print("PASS")
    return True


TESTS = [test_ten_channel_pause_resume]


def main():
    if R.paramiko is None:
        print("SKIP: paramiko not installed (pip install paramiko)")
        print("Results: 0/0 passed, 0 failed")
        return 0
    if not os.path.isfile(R.T.VTM_TILE_BINARY):
        print(f"ERROR: vtm-tile binary not found at {R.T.VTM_TILE_BINARY}")
        return 1
    kill_all_vtm()
    try:
        ok = test_ten_channel_pause_resume()
    finally:
        kill_all_vtm()
    print("\n" + "=" * 60)
    print(f"Results: {1 if ok else 0}/1 passed, {0 if ok else 1} failed")
    print("=" * 60)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
