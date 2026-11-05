// Copyright (c) Shaobo Song
// Licensed under the MIT license.

// Exercise the terminal parser -> DSR reply -> Windows console input chain.
// Plain-text "OK" replies become O (open line) + K (insert text) in Neovim.
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>

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

    struct channel
    {
        handle memory, done;
        char* error;
        explicit channel(std::wstring const& name)
            : memory{ CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 1024, name.c_str()) },
              done{ CreateEventW(nullptr, FALSE, FALSE, (name + L"-done").c_str()) },
              error{ static_cast<char*>(MapViewOfFile(memory, FILE_MAP_ALL_ACCESS, 0, 0, 1024)) }
        {
            check(error != nullptr, "MapViewOfFile");
        }
        ~channel() { UnmapViewOfFile(error); }
    };

    void query(HANDLE in, HANDLE out, std::string_view request, std::string_view expected)
    {
        check(FlushConsoleInputBuffer(in), "FlushConsoleInputBuffer");
        DWORD written;
        check(WriteFile(out, request.data(), static_cast<DWORD>(request.size()), &written, nullptr)
              && written == request.size(), "WriteFile");
        std::string received;
        // Capture for the full interval to also catch duplicate/trailing bytes.
        auto deadline = GetTickCount64() + 750;
        while (GetTickCount64() < deadline)
        {
            DWORD available;
            check(GetNumberOfConsoleInputEvents(in, &available), "GetNumberOfConsoleInputEvents");
            if (!available) { Sleep(5); continue; }
            INPUT_RECORD records[128];
            DWORD count;
            check(ReadConsoleInputW(in, records, static_cast<DWORD>(std::size(records)), &count), "ReadConsoleInputW");
            for (DWORD i = 0; i < count; ++i)
            {
                auto const& r = records[i];
                if (r.EventType == KEY_EVENT && r.Event.KeyEvent.bKeyDown && r.Event.KeyEvent.uChar.UnicodeChar)
                    received += static_cast<char>(r.Event.KeyEvent.uChar.UnicodeChar);
            }
        }
        if (received != expected)
        {
            std::string bytes;
            for (unsigned char c : received)
            {
                char hex[4];
                snprintf(hex, sizeof(hex), "%02x ", c);
                bytes += hex;
            }
            throw std::runtime_error("Unexpected DSR reply bytes: " + bytes);
        }
    }

    int probe(std::wstring const& name)
    {
        channel ipc{ name };
        try
        {
            auto in = GetStdHandle(STD_INPUT_HANDLE);
            auto out = GetStdHandle(STD_OUTPUT_HANDLE);
            DWORD mode;
            check(GetConsoleMode(out, &mode), "GetConsoleMode(output)");
            check(SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING), "SetConsoleMode(output)");
            check(SetConsoleMode(in, ENABLE_EXTENDED_FLAGS | ENABLE_VIRTUAL_TERMINAL_INPUT), "SetConsoleMode(input)");
            query(in, out, "\x1b[5n", "\x1b[0n");
            query(in, out, "\x1b[5n\x1b[5n", "\x1b[0n\x1b[0n");
            query(in, out, "\x1b[3;7H\x1b[6n", "\x1b[3;7R");
            SetEvent(ipc.done);
            return 0;
        }
        catch (std::exception const& e)
        {
            snprintf(ipc.error, 1024, "%s", e.what());
            SetEvent(ipc.done);
            return 1;
        }
    }

    int run(const wchar_t* binary)
    {
        wchar_t self[32768];
        check(GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self))) != 0, "GetModuleFileNameW");
        auto name = L"Local\\vtm-dsr-test-" + std::to_wstring(GetCurrentProcessId());
        channel ipc{ name };
        ipc.error[0] = '\0';
        // Closing this job cleans up only the test's terminal and descendants.
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
        check(WaitForSingleObject(ipc.done, 20000) == WAIT_OBJECT_0, "DSR probe timeout");
        check(!ipc.error[0], ipc.error);
        puts("PASS: status, repeated status, and cursor position replies");
        return 0;
    }
}

int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc == 3 && std::wstring_view{ argv[1] } == L"--probe") return probe(argv[2]);
        if (argc != 2) throw std::runtime_error("Usage: terminal_dsr_test.exe <vtm-tile.exe>");
        return run(argv[1]);
    }
    catch (std::exception const& e)
    {
        fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
