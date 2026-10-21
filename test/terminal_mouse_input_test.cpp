// Copyright (c) Shaobo Song
// Licensed under the MIT license.

// End-to-end Windows regression: native input readers must receive MOUSE_EVENTs
// even after DECSET 1006. Run against an existing vtm-tile binary to reproduce
// the failure. The same executable runs as a console probe inside vtm.
#define NOMINMAX
#include <windows.h>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <regex>

namespace
{
    void check(bool ok, const char* message)
    {
        if (!ok) throw std::runtime_error(std::string{ message } + " (Win32 " + std::to_string(GetLastError()) + ")");
    }

    struct handle
    {
        HANDLE value;
        explicit handle(HANDLE h) : value{ h }
        {
            check(h && h != INVALID_HANDLE_VALUE, "Invalid handle");
        }
        ~handle() { CloseHandle(value); }
        handle(handle const&) = delete;
        operator HANDLE() const { return value; }
    };

    struct result
    {
        DWORD count;
        INPUT_RECORD records[2048];
        char error[512];
    };

    struct channel
    {
        handle memory, ready, done, ack;
        result* data;
        explicit channel(std::wstring const& name)
            : memory{ CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                         sizeof(result), (name + L"-memory").c_str()) },
              ready{ CreateEventW(nullptr, FALSE, FALSE, (name + L"-ready").c_str()) },
              done{ CreateEventW(nullptr, FALSE, FALSE, (name + L"-done").c_str()) },
              ack{ CreateEventW(nullptr, FALSE, FALSE, (name + L"-ack").c_str()) },
              data{ static_cast<result*>(MapViewOfFile(memory, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(result))) }
        {
            check(data != nullptr, "MapViewOfFile");
        }
        ~channel() { UnmapViewOfFile(data); }
    };

    struct scenario
    {
        const char* name;
        bool vt;
        const char* sequence;
        bool sequence_first;
        enum { native, sgr, x11, vtim } expected;
    };
    constexpr scenario scenarios[] = {
        { "native + SGR (Codex)", false, "\x1b[?1006h", false, scenario::native },
        { "VT + SGR", true, "\x1b[?1003h\x1b[?1006h", false, scenario::sgr },
        { "back to native + SGR", false, "\x1b[?1006h", false, scenario::native },
        { "SGR before console mode", false, "\x1b[?1006h", true, scenario::native },
        // vtm's DECRST 1006 also disables motion tracking; explicitly re-enable it.
        { "native + X11 reset", false, "\x1b[?1006l\x1b[?1003h", false, scenario::native },
        { "VT + X11", true, "\x1b[?1006l\x1b[?1003h", false, scenario::x11 },
        { "explicit vt-input-mode", false, "\x1b_lua: terminal.EventReporting(\"mouse\")\x1b\\", false, scenario::vtim },
    };

    void output(HANDLE out, std::string_view text)
    {
        DWORD written;
        check(WriteFile(out, text.data(), static_cast<DWORD>(text.size()), &written, nullptr)
              && written == text.size(), "WriteFile");
    }

    int probe(std::wstring const& name)
    {
        channel ipc{ name };
        try
        {
            auto in = GetStdHandle(STD_INPUT_HANDLE);
            auto out = GetStdHandle(STD_OUTPUT_HANDLE);
            DWORD original_in, original_out;
            check(GetConsoleMode(in, &original_in), "GetConsoleMode(input)");
            check(GetConsoleMode(out, &original_out), "GetConsoleMode(output)");
            check(SetConsoleMode(out, original_out | ENABLE_VIRTUAL_TERMINAL_PROCESSING), "SetConsoleMode(output)");
            output(out, "\x1b[?1049h\x1b[2J\x1b[HMouse input regression probe");
            for (auto const& s : scenarios)
            {
                // SetConsoleMode also sets vtm's mouse encoder to w32. Exercise
                // both orderings of the console API and terminal sequence.
                if (s.sequence_first)
                {
                    output(out, s.sequence);
                    Sleep(200);
                }
                check(SetConsoleMode(in, ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT
                                         | (s.vt ? ENABLE_VIRTUAL_TERMINAL_INPUT : 0)), "SetConsoleMode(input)");
                if (!s.sequence_first) output(out, s.sequence);
                Sleep(200); // Allow queued output/Lua mode changes to reach the terminal.
                check(FlushConsoleInputBuffer(in), "FlushConsoleInputBuffer");
                ipc.data->count = 0;
                check(SetEvent(ipc.ready), "SetEvent(ready)");
                auto deadline = GetTickCount64() + 900;
                while (GetTickCount64() < deadline)
                {
                    DWORD available;
                    check(GetNumberOfConsoleInputEvents(in, &available), "GetNumberOfConsoleInputEvents");
                    if (available)
                    {
                        auto room = static_cast<DWORD>(std::size(ipc.data->records)) - ipc.data->count;
                        check(room != 0, "Probe record buffer full");
                        DWORD count;
                        check(ReadConsoleInputW(in, ipc.data->records + ipc.data->count,
                                                available < room ? available : room, &count), "ReadConsoleInputW");
                        ipc.data->count += count;
                    }
                    else Sleep(5);
                }
                check(SetEvent(ipc.done), "SetEvent(done)");
                // Parent acknowledges before the next scenario overwrites the results.
                check(WaitForSingleObject(ipc.ack, 10000) == WAIT_OBJECT_0, "Probe acknowledgement timeout");
            }
            output(out, "\x1b_lua: terminal.EventReporting(\"\")\x1b\\\x1b[?1003l\x1b[?1006l\x1b[?1049l");
            check(SetConsoleMode(in, original_in), "Restore input mode");
            check(SetConsoleMode(out, original_out), "Restore output mode");
            return 0;
        }
        catch (std::exception const& e)
        {
            snprintf(ipc.data->error, sizeof(ipc.data->error), "%s", e.what());
            SetEvent(ipc.ready);
            SetEvent(ipc.done);
            return 1;
        }
    }

    void inject(HANDLE in, INPUT_RECORD record)
    {
        DWORD written;
        check(WriteConsoleInputW(in, &record, 1, &written) && written == 1, "WriteConsoleInputW");
        Sleep(50); // Keep button transitions separate from movement coalescing.
    }

    bool verify(scenario const& s, result const& r)
    {
        std::string text;
        bool moved = false, pressed = false, released = false, wheel = false;
        COORD last_move{};
        bool position_changed = false;
        unsigned native_count = 0;
        for (DWORD i = 0; i < r.count; ++i)
        {
            auto const& event = r.records[i];
            if (event.EventType == KEY_EVENT && event.Event.KeyEvent.bKeyDown
                && event.Event.KeyEvent.uChar.UnicodeChar)
                text += static_cast<char>(event.Event.KeyEvent.uChar.UnicodeChar);
            if (event.EventType != MOUSE_EVENT) continue;
            ++native_count;
            auto const& m = event.Event.MouseEvent;
            if (m.dwEventFlags & MOUSE_MOVED)
            {
                position_changed |= moved && (m.dwMousePosition.X != last_move.X || m.dwMousePosition.Y != last_move.Y);
                last_move = m.dwMousePosition;
                moved = true;
            }
            if (m.dwEventFlags & MOUSE_WHEELED)
                wheel |= static_cast<SHORT>(HIWORD(m.dwButtonState)) == WHEEL_DELTA;
            else if (!(m.dwEventFlags & MOUSE_MOVED))
            {
                pressed |= (m.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
                released |= pressed && m.dwButtonState == 0;
            }
        }
        bool ok = false;
        switch (s.expected)
        {
            case scenario::native:
                ok = moved && position_changed && pressed && released && wheel && text == "q";
                break;
            case scenario::sgr:
                ok = native_count == 0
                  && std::regex_match(text, std::regex{ "(\x1b\\[<[0-9]+;[0-9]+;[0-9]+[Mm])+q" })
                  && text.find("\x1b[<35;") != std::string::npos
                  && text.find("\x1b[<0;") != std::string::npos
                  && text.find("m") != std::string::npos
                  && text.find("\x1b[<64;") != std::string::npos;
                break;
            case scenario::x11:
                ok = native_count == 0 && text.ends_with('q') && text.size() >= 7;
                for (size_t i = 0; ok && i < text.size() - 1; i += 6)
                    ok = i + 6 <= text.size() - 1 && text.compare(i, 3, "\x1b[M") == 0;
                break;
            case scenario::vtim:
                ok = native_count == 0 && text.find("event=mouse") != std::string::npos
                  && text.find("\x1b[<") == std::string::npos && text.ends_with('q');
                break;
        }
        printf("%s: %s (%u native mouse records, %zu text bytes)\n", s.name, ok ? "PASS" : "FAIL", native_count, text.size());
        if (!ok)
        {
            printf("  input bytes:");
            for (unsigned char c : text) printf(" %02x", c);
            printf("\n");
        }
        return ok;
    }

    int run(const wchar_t* binary)
    {
        wchar_t self[32768];
        check(GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self))) != 0, "GetModuleFileNameW");
        auto name = L"Local\\vtm-mouse-test-" + std::to_wstring(GetCurrentProcessId());
        channel ipc{ name };
        *ipc.data = {};
        // The job contains only this test's vtm and descendants; closing it also
        // cleans up on failure/timeout without touching the user's sessions.
        handle job{ CreateJobObjectW(nullptr, nullptr) };
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        check(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)), "SetInformationJobObject");
        auto command = L"\"" + std::wstring{ binary } + L"\" -r term \"" + self + L"\" --probe " + name;
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        check(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                             CREATE_NEW_CONSOLE | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi), "CreateProcessW");
        handle process{ pi.hProcess }, thread{ pi.hThread };
        if (!AssignProcessToJobObject(job, process))
        {
            TerminateProcess(process, 1);
            check(false, "AssignProcessToJobObject");
        }
        check(ResumeThread(thread) != DWORD(-1), "ResumeThread");
        FreeConsole();
        // CREATE_NEW_CONSOLE initialization may still be pending after ResumeThread.
        auto attach_deadline = GetTickCount64() + 5000;
        while (!AttachConsole(pi.dwProcessId))
        {
            check(GetTickCount64() < attach_deadline, "AttachConsole");
            Sleep(10);
        }
        handle input{ CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr) };
        bool ok = true;
        for (auto const& s : scenarios)
        {
            check(WaitForSingleObject(ipc.ready, 15000) == WAIT_OBJECT_0, "Probe startup/scenario timeout");
            check(!ipc.data->error[0], ipc.data->error);
            for (auto mouse : std::array<MOUSE_EVENT_RECORD, 5>{
                MOUSE_EVENT_RECORD{ { 12, 8 }, 0, 0, MOUSE_MOVED },
                MOUSE_EVENT_RECORD{ { 14, 9 }, 0, 0, MOUSE_MOVED },
                MOUSE_EVENT_RECORD{ { 14, 9 }, FROM_LEFT_1ST_BUTTON_PRESSED, 0, 0 },
                MOUSE_EVENT_RECORD{ { 14, 9 }, 0, 0, 0 },
                MOUSE_EVENT_RECORD{ { 14, 9 }, DWORD(WHEEL_DELTA) << 16, 0, MOUSE_WHEELED },
            })
            {
                INPUT_RECORD event{};
                event.EventType = MOUSE_EVENT;
                event.Event.MouseEvent = mouse;
                inject(input, event);
            }
            INPUT_RECORD key{};
            key.EventType = KEY_EVENT;
            key.Event.KeyEvent = { TRUE, 1, 'Q', 0, { L'q' }, 0 };
            inject(input, key);
            key.Event.KeyEvent.bKeyDown = FALSE;
            inject(input, key);
            check(WaitForSingleObject(ipc.done, 10000) == WAIT_OBJECT_0, "Probe capture timeout");
            check(!ipc.data->error[0], ipc.data->error);
            ok &= verify(s, *ipc.data);
            check(SetEvent(ipc.ack), "Acknowledge probe");
        }
        FreeConsole();
        return ok ? 0 : 1;
    }
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc == 3 && std::wstring_view{ argv[1] } == L"--probe") return probe(argv[2]);
        if (argc != 2) throw std::runtime_error("Usage: terminal_mouse_input_test.exe <vtm-tile.exe>");
        return run(argv[1]);
    }
    catch (std::exception const& e)
    {
        fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
