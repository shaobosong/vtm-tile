#!/usr/bin/env python3
# Copyright (c) Shaobo Song
# Licensed under the MIT license.

"""End-to-end geometry and interaction checks for Parvion's shared main grid."""

import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
import test_parvion_queue as T  # noqa: E402


def test_main_grid_layout_and_handles():
    print("TEST: parvion shared grid layout + handles ... ", end="", flush=True)
    with T.ParvionSession(T.DEMO_ENV) as s:
        def pos(label):
            return T.find_text(s.screen()[0], label)

        host = pos("Host:")
        local = pos("Local site")
        remote = pos("Remote site")
        header = pos("Server")
        if None in (host, local, remote, header):
            print(f"FAIL - initial landmarks missing: {host}/{local}/{remote}/{header}")
            return False

        # Hidden handles reserve no separator row between the one-row connect
        # bar and the pane titles.
        if local[0] != host[0] + 1 or remote[0] != local[0]:
            print(f"FAIL - hidden outer handle left a gap: {host}/{local}/{remote}")
            return False

        initial_remote_col = remote[1]
        initial_header_row = header[0]

        # Remote title starts one cell inside its pane. The rightmost cell of
        # the two-cell vertical handle is therefore remote_col - 1 in 1-based
        # mouse coordinates.
        vertical_handle = initial_remote_col - 1
        s.drag(vertical_handle, 10, vertical_handle + 10, 10)
        moved_remote = pos("Remote site")
        if moved_remote is None or moved_remote[1] != initial_remote_col + 10:
            print(f"FAIL - vertical handle did not move by 10: {remote} -> {moved_remote}")
            return False
        s.double_click(vertical_handle + 10, 10)
        reset_remote = pos("Remote site")
        if reset_remote is None or reset_remote[1] != initial_remote_col:
            print(f"FAIL - vertical handle did not reset to 1:1: {reset_remote}")
            return False

        # The horizontal handle occupies the screen row immediately above the
        # table header; its 1-based mouse row equals the header's 0-based row.
        s.drag(110, initial_header_row, 110, initial_header_row - 5)
        moved_header = pos("Server")
        if moved_header is None or moved_header[0] != initial_header_row - 5:
            print(f"FAIL - horizontal handle did not move by -5: {header} -> {moved_header}")
            return False
        s.double_click(110, moved_header[0])
        reset_header = pos("Server")
        if reset_header is None or reset_header[0] != initial_header_row:
            print(f"FAIL - horizontal handle did not reset to 3:2: {reset_header}")
            return False

        print("PASS")
        return True


if __name__ == "__main__":
    raise SystemExit(0 if test_main_grid_layout_and_handles() else 1)
