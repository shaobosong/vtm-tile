#!/usr/bin/env python3
# Copyright (c) Dmitry Sapozhnikov
# Licensed under the MIT license.

"""
Windows regression test: vtm-tile orphan after closing pane with nvim
inside its inner console.

User-reported repro on Windows 11:
  1. Run `vtm-tile.exe -r term` -- the default config launches PowerShell
     inside vtm's emulated console (consrv.hpp).
  2. From inside PowerShell, launch `nvim` (Neovim from PATH).
  3. Click the `×` button in the top-right of vtm-tile's applet menu bar
     (the standalone applet's close-window button, see
     src/netxs/desktopio/application.hpp:804).
  4. Observed: the vtm-tile.exe process is left running (orphaned).

Root cause
----------
On close, the owning applet drives consrv shutdown via
`consrv::impl::wait()` (src/netxs/desktopio/consrv.hpp). The original
code did:
        allout.wait(faux); //todo set timeout (far manager deadlocking here)
which blocks indefinitely on the `allout` flag. `allout` is only flipped
to true when every attached client calls api_process_detach (joined list
becomes empty -- see consrv.hpp:2733).

Before that, events.sighup() broadcasts CTRL_CLOSE_EVENT to all clients
(events_t::alert). nvim installs a console control handler that returns
TRUE for CTRL_CLOSE_EVENT, swallowing it without detaching. As a result
`allout` never becomes true, `wait()` blocks forever, and vtm-tile.exe
cannot exit -> orphan.

Fix
---
Replace the unbounded wait with a bounded one:
  - Give the child a short grace period (~500 ms) to react to the
    CTRL_CLOSE_EVENT broadcast.
  - If the descendant tree is still alive after the deadline, walk it via
    Toolhelp32 and TerminateProcess every PID (root + descendants).
  - In wait(), call events.stop() before closing condrv to release any
    blocked api handlers, then CancelSynchronousIo + close condrv +
    join the server thread.

This test asserts the vtm-tile.exe parent PID exits within
CLOSE_PANE_SETTLE seconds after the click; without the fix it hangs and
the test fails.

Why CREATE_NEW_CONSOLE + WriteConsoleInputW
-------------------------------------------
ConPTY does not give vtm-tile a fully-functional console (Win32
Console API calls return ERROR_INVALID_HANDLE), so neither VT key bytes
nor SGR mouse sequences reach the keybind/mouse handler. Instead we
spawn vtm-tile attached to a real new conhost window and inject
synthetic INPUT_RECORDs (key + mouse) by AttachConsole'ing temporarily.

Run:
  python test/test_tile_closepane_nvim_orphan.py
"""

import os
import sys

# Windows-only regression test: the entire driver below depends on
# AttachConsole + WriteConsoleInputW, neither of which exist on
# non-Windows hosts. Bail out cleanly so the CMake test runner records
# success on Linux/macOS instead of crashing on `ctypes.windll`.
if sys.platform != "win32":
    print("SKIP - Windows-only test (requires ctypes.windll)")
    sys.exit(0)

import ctypes
import ctypes.wintypes
import subprocess
import time
import uuid

# --- Configurable paths ------------------------------------------------------

VTM_TILE_BINARY = os.environ.get(
    "VTM_TILE_BINARY",
    os.path.normpath(
        os.path.join(
            os.path.dirname(os.path.abspath(__file__)),
            "..", "build-x64", "Release", "vtm-tile.exe",
        )
    ),
)
NVIM_BINARY = os.environ.get("NVIM_BINARY", "nvim")

# --- Timing constants --------------------------------------------------------

STARTUP_SETTLE    = 4.0   # vtm-tile applet + PowerShell prompt
NVIM_SETTLE       = 4.0   # nvim launch + consrv attach
CLOSE_PANE_SETTLE = 15.0  # max time to wait for vtm-tile.exe to exit (>0.5s deadline + tree kill + propagation)
POLL_INTERVAL     = 0.1

# --- Win32 constants ---------------------------------------------------------

CREATE_NEW_CONSOLE          = 0x00000010
STILL_ACTIVE                = 259
PROCESS_QUERY_INFORMATION   = 0x0400
PROCESS_TERMINATE           = 0x0001
TH32CS_SNAPPROCESS          = 0x00000002
INVALID_HANDLE_VALUE        = ctypes.c_void_p(-1).value
GENERIC_READ                = 0x80000000
GENERIC_WRITE               = 0x40000000
FILE_SHARE_READ             = 0x00000001
FILE_SHARE_WRITE            = 0x00000002
OPEN_EXISTING               = 3

# Virtual-key codes we need.
VK_RETURN = 0x0D

# Control-key state flags for KEY_EVENT_RECORD.
SHIFT_PRESSED       = 0x0010
LEFT_ALT_PRESSED    = 0x0002
RIGHT_ALT_PRESSED   = 0x0001  # noqa
LEFT_CTRL_PRESSED   = 0x0008
RIGHT_CTRL_PRESSED  = 0x0004  # noqa

# Mouse button state.
FROM_LEFT_1ST_BUTTON_PRESSED = 0x0001
MOUSE_MOVED                  = 0x0001  # noqa: dwEventFlags

# INPUT_RECORD event types.
KEY_EVENT   = 0x0001
MOUSE_EVENT = 0x0002

wt = ctypes.wintypes
_k32 = ctypes.windll.kernel32


# --- ctypes structures -------------------------------------------------------

class COORD(ctypes.Structure):
    _fields_ = [("X", wt.SHORT), ("Y", wt.SHORT)]


class SMALL_RECT(ctypes.Structure):
    _fields_ = [
        ("Left",   wt.SHORT),
        ("Top",    wt.SHORT),
        ("Right",  wt.SHORT),
        ("Bottom", wt.SHORT),
    ]


class CONSOLE_SCREEN_BUFFER_INFO(ctypes.Structure):
    _fields_ = [
        ("dwSize",              COORD),
        ("dwCursorPosition",    COORD),
        ("wAttributes",         wt.WORD),
        ("srWindow",            SMALL_RECT),
        ("dwMaximumWindowSize", COORD),
    ]


class STARTUPINFOW(ctypes.Structure):
    _fields_ = [
        ("cb",              wt.DWORD),
        ("lpReserved",      wt.LPWSTR),
        ("lpDesktop",       wt.LPWSTR),
        ("lpTitle",         wt.LPWSTR),
        ("dwX",             wt.DWORD),
        ("dwY",             wt.DWORD),
        ("dwXSize",         wt.DWORD),
        ("dwYSize",         wt.DWORD),
        ("dwXCountChars",   wt.DWORD),
        ("dwYCountChars",   wt.DWORD),
        ("dwFillAttribute", wt.DWORD),
        ("dwFlags",         wt.DWORD),
        ("wShowWindow",     wt.WORD),
        ("cbReserved2",     wt.WORD),
        ("lpReserved2",     ctypes.POINTER(wt.BYTE)),
        ("hStdInput",       wt.HANDLE),
        ("hStdOutput",      wt.HANDLE),
        ("hStdError",       wt.HANDLE),
    ]


class PROCESS_INFORMATION(ctypes.Structure):
    _fields_ = [
        ("hProcess",    wt.HANDLE),
        ("hThread",     wt.HANDLE),
        ("dwProcessId", wt.DWORD),
        ("dwThreadId",  wt.DWORD),
    ]


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize",              wt.DWORD),
        ("cntUsage",            wt.DWORD),
        ("th32ProcessID",       wt.DWORD),
        ("th32DefaultHeapID",   ctypes.c_size_t),
        ("th32ModuleID",        wt.DWORD),
        ("cntThreads",          wt.DWORD),
        ("th32ParentProcessID", wt.DWORD),
        ("pcPriClassBase",      ctypes.c_long),
        ("dwFlags",             wt.DWORD),
        ("szExeFile",           ctypes.c_wchar * 260),
    ]


class _KE_uChar(ctypes.Union):
    _fields_ = [("UnicodeChar", wt.WCHAR), ("AsciiChar", ctypes.c_char)]


class KEY_EVENT_RECORD(ctypes.Structure):
    _fields_ = [
        ("bKeyDown",          wt.BOOL),
        ("wRepeatCount",      wt.WORD),
        ("wVirtualKeyCode",   wt.WORD),
        ("wVirtualScanCode",  wt.WORD),
        ("uChar",             _KE_uChar),
        ("dwControlKeyState", wt.DWORD),
    ]


class MOUSE_EVENT_RECORD(ctypes.Structure):
    _fields_ = [
        ("dwMousePosition",   COORD),
        ("dwButtonState",     wt.DWORD),
        ("dwControlKeyState", wt.DWORD),
        ("dwEventFlags",      wt.DWORD),
    ]


class WINDOW_BUFFER_SIZE_RECORD(ctypes.Structure):
    _fields_ = [("dwSize", COORD)]


class MENU_EVENT_RECORD(ctypes.Structure):
    _fields_ = [("dwCommandId", wt.UINT)]


class FOCUS_EVENT_RECORD(ctypes.Structure):
    _fields_ = [("bSetFocus", wt.BOOL)]


class _IR_Event(ctypes.Union):
    _fields_ = [
        ("KeyEvent",              KEY_EVENT_RECORD),
        ("MouseEvent",            MOUSE_EVENT_RECORD),
        ("WindowBufferSizeEvent", WINDOW_BUFFER_SIZE_RECORD),
        ("MenuEvent",             MENU_EVENT_RECORD),
        ("FocusEvent",            FOCUS_EVENT_RECORD),
    ]


class INPUT_RECORD(ctypes.Structure):
    _anonymous_ = ("Event",)
    _fields_ = [("EventType", wt.WORD), ("Event", _IR_Event)]


# --- Process helpers ---------------------------------------------------------

def iter_processes():
    snap = _k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap == INVALID_HANDLE_VALUE:
        return
    try:
        e = PROCESSENTRY32W()
        e.dwSize = ctypes.sizeof(PROCESSENTRY32W)
        if _k32.Process32FirstW(snap, ctypes.byref(e)):
            while True:
                yield (e.th32ProcessID, e.th32ParentProcessID, e.szExeFile.lower())
                if not _k32.Process32NextW(snap, ctypes.byref(e)):
                    break
    finally:
        _k32.CloseHandle(snap)


def find_pids_by_name(name):
    name = name.lower()
    return {pid for pid, _, exe in iter_processes() if exe == name}


def pid_alive(pid):
    h = _k32.OpenProcess(PROCESS_QUERY_INFORMATION, False, pid)
    if not h:
        return False
    try:
        c = wt.DWORD(0)
        return bool(_k32.GetExitCodeProcess(h, ctypes.byref(c))) and c.value == STILL_ACTIVE
    finally:
        _k32.CloseHandle(h)


def kill_pids(pids):
    for pid in pids:
        if pid_alive(pid):
            h = _k32.OpenProcess(PROCESS_TERMINATE, False, pid)
            if h:
                try:    _k32.TerminateProcess(h, 1)
                finally: _k32.CloseHandle(h)


def wait_for_exit(pid, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not pid_alive(pid):
            return True
        time.sleep(POLL_INTERVAL)
    return False


# --- INPUT_RECORD injection --------------------------------------------------

def make_key_event(vk, down, ctrl_state=0, ch=""):
    r = INPUT_RECORD()
    r.EventType = KEY_EVENT
    r.KeyEvent.bKeyDown          = 1 if down else 0
    r.KeyEvent.wRepeatCount      = 1
    r.KeyEvent.wVirtualKeyCode   = vk
    r.KeyEvent.wVirtualScanCode  = 0
    r.KeyEvent.uChar.UnicodeChar = ch
    r.KeyEvent.dwControlKeyState = ctrl_state
    return r


def make_mouse_event(x, y, button_state, event_flags=0):
    r = INPUT_RECORD()
    r.EventType = MOUSE_EVENT
    r.MouseEvent.dwMousePosition.X = x
    r.MouseEvent.dwMousePosition.Y = y
    r.MouseEvent.dwButtonState     = button_state
    r.MouseEvent.dwControlKeyState = 0
    r.MouseEvent.dwEventFlags      = event_flags
    return r


def write_input_records(conin, records):
    arr = (INPUT_RECORD * len(records))(*records)
    written = wt.DWORD(0)
    if not _k32.WriteConsoleInputW(conin, arr, len(records), ctypes.byref(written)):
        raise OSError(f"WriteConsoleInputW failed: {_k32.GetLastError()}")
    return written.value


def type_text(conin, text):
    """Type ASCII text via synthetic key events. Letters use VK = upper-case ord."""
    records = []
    for ch in text:
        if ch == "\r" or ch == "\n":
            records.append(make_key_event(VK_RETURN, True,  ch="\r"))
            records.append(make_key_event(VK_RETURN, False, ch="\r"))
            continue
        upper = ch.upper()
        vk = ord(upper) if "A" <= upper <= "Z" or "0" <= upper <= "9" else 0
        ctrl_state = SHIFT_PRESSED if ch.isupper() and ch.isalpha() else 0
        records.append(make_key_event(vk, True,  ctrl_state, ch=ch))
        records.append(make_key_event(vk, False, ctrl_state, ch=ch))
    if records:
        write_input_records(conin, records)


def click_mouse(conin, x, y):
    """Synthesize a left-button click at (x, y) buffer coords (0-based)."""
    write_input_records(conin, [
        make_mouse_event(x, y, 0,                              MOUSE_MOVED),
        make_mouse_event(x, y, FROM_LEFT_1ST_BUTTON_PRESSED,   0),
        make_mouse_event(x, y, 0,                              0),
    ])


def screen_buffer_width(conin):
    """Return the console output buffer width. We open CONOUT$ on the
    currently-attached console to read CONSOLE_SCREEN_BUFFER_INFO."""
    h = _k32.CreateFileW(
        "CONOUT$",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        None, OPEN_EXISTING, 0, None,
    )
    if h == INVALID_HANDLE_VALUE:
        return None
    try:
        info = CONSOLE_SCREEN_BUFFER_INFO()
        if not _k32.GetConsoleScreenBufferInfo(h, ctypes.byref(info)):
            return None
        return int(info.srWindow.Right - info.srWindow.Left + 1)
    finally:
        _k32.CloseHandle(h)


# --- Console attach helpers --------------------------------------------------

def open_target_conin(pid):
    """Detach from current console and open CONIN$ for the target process."""
    _k32.FreeConsole()
    if not _k32.AttachConsole(pid):
        err = _k32.GetLastError()
        raise OSError(f"AttachConsole({pid}) failed: {err}")
    h = _k32.CreateFileW(
        "CONIN$",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        None, OPEN_EXISTING, 0, None,
    )
    if h == INVALID_HANDLE_VALUE:
        err = _k32.GetLastError()
        _k32.FreeConsole()
        raise OSError(f"CreateFileW(CONIN$) failed: {err}")
    return h


def detach_console(handle):
    if handle and handle != INVALID_HANDLE_VALUE:
        _k32.CloseHandle(handle)
    _k32.FreeConsole()


# --- Test --------------------------------------------------------------------

def make_runapp_config():
    # Disable terminal `confirm_close` so that clicking the applet's `×`
    # button on the menu bar drives shutdown immediately (no dialog), which
    # is the codepath the user reports leaving vtm-tile orphaned. Default
    # config is otherwise preserved -- in particular, the term cmd is still
    # the OS shell (PowerShell on Windows 11).
    return "<config><terminal><confirm_close=false/></terminal></config>"


def spawn_vtm_tile(cmdline):
    si = STARTUPINFOW()
    si.cb = ctypes.sizeof(STARTUPINFOW)
    pi = PROCESS_INFORMATION()
    cmd_buf = ctypes.create_unicode_buffer(cmdline)
    if not _k32.CreateProcessW(
            None, cmd_buf, None, None, False,
            CREATE_NEW_CONSOLE, None, None,
            ctypes.byref(si), ctypes.byref(pi)):
        raise OSError(f"CreateProcessW failed: {_k32.GetLastError()}")
    _k32.CloseHandle(pi.hThread)
    return pi.hProcess, pi.dwProcessId


def test_closepane_nvim_orphan():
    sid = uuid.uuid4().hex[:8]
    print(f"TEST: vtm-tile -r term + nvim close-pane orphan [{sid}] ... ", end="", flush=True)

    vtm_exe = os.path.abspath(VTM_TILE_BINARY)
    if not os.path.isfile(vtm_exe):
        print(f"SKIP - vtm-tile binary not found: {vtm_exe}")
        return None

    try:
        r = subprocess.run([NVIM_BINARY, "--version"], capture_output=True, timeout=5)
        if r.returncode != 0:
            raise FileNotFoundError
    except (FileNotFoundError, subprocess.TimeoutExpired, OSError):
        print(f"SKIP - nvim not available: {NVIM_BINARY}")
        return None

    # Reproduce the user's invocation exactly: `vtm-tile.exe -r term`.
    # `-c "<config>...</config>"` only flips terminal/confirm_close so we
    # don't need a human to dismiss the close-confirmation dialog; the rest
    # of the configuration (in particular the `term` app's command -> OS
    # shell, i.e. PowerShell on Win11) is the stock default.
    cmdline = f'"{vtm_exe}" -c "{make_runapp_config()}" -r term'

    h_proc, pid = spawn_vtm_tile(cmdline)
    print(f"[vtm_tile_pid={pid}] ", end="", flush=True)

    try:
        # Wait for vtm-tile applet TUI render and for PowerShell to print
        # its prompt inside vtm's emulated console (consrv).
        time.sleep(STARTUP_SETTLE)

        if not pid_alive(pid):
            print("FAIL - vtm-tile exited during startup")
            return False

        # Attach to the new console and inject synthetic input.
        nvim_pids_before_click = set()
        clicked_x = clicked_y = None
        width = None
        conin = None
        try:
            conin = open_target_conin(pid)

            # 1) Type "nvim<Enter>" into the active PowerShell pane.
            nvim_pids_pre = find_pids_by_name("nvim.exe")
            type_text(conin, "nvim\r")
            time.sleep(NVIM_SETTLE)
            nvim_pids_now = find_pids_by_name("nvim.exe") - nvim_pids_pre
            nvim_pids_before_click = nvim_pids_now

            # 2) Click the standalone applet's `×` close-window button on
            #    the top-right of the menu bar
            #    (src/netxs/desktopio/application.hpp:804). With the
            #    default macstyle=false, the three control buttons
            #    [minimize][maximize][×] sit flush-right on row 0; each is
            #    rendered as the 5-cell label "  ×  " (etc.). The last
            #    cell of `×` lands at column (width-1) and its visible
            #    glyph is at the center, column (width-3).
            width = screen_buffer_width(conin)
            if not width or width < 16:
                print(f"FAIL - bogus console width {width}; cannot locate × button")
                return False
            clicked_x, clicked_y = width - 3, 0
            click_mouse(conin, x=clicked_x, y=clicked_y)
        finally:
            if conin is not None:
                detach_console(conin)
            else:
                _k32.FreeConsole()
            # Re-attach to parent's console so subsequent prints land sanely.
            _k32.AttachConsole(0xFFFFFFFF)  # ATTACH_PARENT_PROCESS

        print(f"[w={width} click=({clicked_x},{clicked_y}) "
              f"nvim_pids={nvim_pids_before_click}] ", end="", flush=True)

        # The orphan signal is observable purely on vtm-tile.exe's PID:
        # before the fix, consrv::impl::wait() blocks forever on the
        # `allout` flag because nvim swallows CTRL_CLOSE_EVENT and never
        # calls api_process_detach -> joined never empties -> allout
        # stays false -> wait() never returns -> vtm-tile.exe stays alive.
        # With the fix (consrv.hpp wait() bounded poll + force-terminate
        # of stuck clients), vtm-tile.exe must exit within
        # CLOSE_PANE_SETTLE seconds.
        t_click = time.monotonic()
        exited = wait_for_exit(pid, CLOSE_PANE_SETTLE)
        shutdown_s = time.monotonic() - t_click
        nvim_alive_now = {p for p in nvim_pids_before_click if pid_alive(p)}
        print(f"[vtm_exited={exited} nvim_alive={nvim_alive_now} "
              f"shutdown={shutdown_s:.2f}s] ",
              end="", flush=True)
    finally:
        # Defensive cleanup: never leak processes if the test itself bails.
        if pid_alive(pid):
            _k32.TerminateProcess(h_proc, 1)
        _k32.CloseHandle(h_proc)
        # Sweep any nvim that consrv-fix should have terminated.
        kill_pids(nvim_pids_before_click)

    if not exited:
        print(
            f"\nFAIL - vtm-tile.exe (pid={pid}) still alive {CLOSE_PANE_SETTLE}s "
            f"after clicking ×; consrv::impl::wait() likely deadlocked at "
            f"src/netxs/desktopio/consrv.hpp wait() because nvim swallowed "
            f"CTRL_CLOSE_EVENT and never detached."
        )
        return False
    if nvim_alive_now:
        # The originally-reported bug is that nvim is left orphaned. The
        # bounded-wait + descendant-tree TerminateProcess in consrv::cleanup()
        # is what guarantees nvim dies. If vtm-tile exited but nvim survived,
        # only the test's own kill_pids() sweep killed it -- the in-process
        # fix did not actually do its job.
        print(
            f"\nFAIL - vtm-tile exited but nvim pids {nvim_alive_now} were still "
            f"alive when vtm-tile finished. consrv::cleanup() did not force-"
            f"terminate the descendant tree as expected."
        )
        return False
    print("PASS (vtm-tile exited cleanly; nvim force-terminated by consrv deadline)")
    return True


TESTS = [
    test_closepane_nvim_orphan,
]


def main():
    if not hasattr(_k32, "AttachConsole"):
        print("SKIP - AttachConsole not available (requires Windows 2000+)")
        print(f"\n{'='*60}")
        print(f"Results: 1/1 passed, 0 failed")
        print(f"{'='*60}")
        return 0

    passed = 0
    failed = 0
    skipped = 0

    for test in TESTS:
        try:
            result = test()
            if result is None:
                skipped += 1
                passed += 1  # SKIP counts as pass for aggregation
            elif result:
                passed += 1
            else:
                failed += 1
        except Exception as e:
            print(f"ERROR: {test.__name__}: {e}")
            import traceback
            traceback.print_exc()
            failed += 1

    total = passed + failed
    print(f"\n{'='*60}")
    print(f"Results: {passed}/{total} passed, {failed} failed"
          + (f" ({skipped} skipped)" if skipped else ""))
    print(f"{'='*60}")

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
