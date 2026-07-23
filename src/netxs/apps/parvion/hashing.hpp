// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/hashing.hpp: engine-side hash-task plumbing — the checksum counterpart to the
// transfer pipeline in session.hpp. Hash tasks live in their OWN queue and worker pool, so
// they run concurrently with (and independently of) transfers:
//   - hash_item    : one queued/active/finished checksum (mirrors queue_item in model.hpp).
//   - hash_session : a slim spawn + line-reader transport for the `parvionhash` backend
//                    process (a trimmed sftp_session: no shared memory / fzprintf framing).
//   - hash_worker  : drives one `parvionhash` child and accumulates its progress + digest.
// The backend's stdout line protocol (see parvionhash_main in vtm-tile.cpp):
//     S <total-bytes>   (optional, once)   P <bytes-done>   R <hex-digest>   E <message>
// The remote target's password is fed to the child over stdin (kept off argv / `ps`).

#include "model.hpp"
#include "settings.hpp"

#include <thread>
#include <mutex>
#include <atomic>
#include <deque>
#include <array>
#include <functional>
#include <vector>
#include <string>

#if !defined(_WIN32)
    #include <unistd.h>
    #include <spawn.h>
    #include <signal.h>
    #include <sys/wait.h>
    #include <fcntl.h>
    extern char** environ;
#endif

namespace netxs::app::parvion
{
    // One queued/active/finished checksum computation.
    struct hash_item
    {
        enum status_t { queued, hashing, succeeded, failed };

        ui64       id = 0;          // Stable identity (matches a hash_worker to its item across queue edits).
        bool       remote = faux;   // true: stream-hash a remote file (download+discard); false: local file.
        text       path;            // Local path, or absolute remote path, being hashed.
        text       name;            // Basename for the queue panel's Name column.
        si32       algo = 2;        // Algorithm index; see settings.hpp helpers.
        si64       size = -1;       // Total bytes (for the progress %, -1 = unknown).
        si64       done = 0;        // Bytes hashed so far.
        status_t   status = queued;
        text       digest;          // Lowercase hex result (when status == succeeded).
        text       error;           // Failure reason (when status == failed).
        std::time_t started = 0;    // Wall-clock start.
        rate_meter rate;            // Live byte-rate (sliding window), shared with transfers.
        bool       selected = faux; // UI: row is part of the Checksums tab's selection set.

        // Credential snapshot for a remote task, captured at enqueue time so the worker can
        // open its own SFTP connection independently of the live control session.
        text host, user, pass;
        si32 port = 22;
        std::vector<text> keyfiles;

        // Origin shown in the Checksums "Source" column: "user@host:port" for a remote file
        // (the user@ part is dropped when no user is known), or empty for a local file.
        auto source() const -> text
        {
            return remote ? server_source(host, user, port) : text{};
        }
    };

    // Slim transport for a spawned `parvionhash` child: redirects the child's stdout to a pipe
    // and splits it into lines on a reader thread; the remote password is written to the child's
    // stdin. No shared memory and no fzprintf parser (unlike sftp_session) — hashing needs neither.
    struct hash_session
    {
        std::mutex        line_mtx;
        std::deque<text>  lines;
        std::atomic<bool> running{ faux };
        std::thread       reader;
        #if defined(_WIN32)
        HANDLE hproc = nullptr;
        HANDLE wr    = nullptr; // -> child stdin
        HANDLE rd    = nullptr; // <- child stdout
        #else
        pid_t pid = -1;
        int   wr  = -1;
        int   rd  = -1;
        #endif
        std::vector<text> runargs; // Inserted between the executable and the hash args (e.g. {"-r","parvionhash"}).

        ~hash_session() { stop(); }
        hash_session() = default;
        hash_session(hash_session const&) = delete;
        hash_session& operator=(hash_session const&) = delete;

        auto alive() const { return running.load(); }

        auto launch(text const& exe, std::vector<text> const& args) -> bool
        {
            #if !defined(_WIN32)
            int ip[2], op[2];
            if (::pipe(ip)) return faux;
            if (::pipe(op)) { ::close(ip[0]); ::close(ip[1]); return faux; }
            auto fa = posix_spawn_file_actions_t{};
            ::posix_spawn_file_actions_init(&fa);
            ::posix_spawn_file_actions_adddup2(&fa, ip[0], 0);
            ::posix_spawn_file_actions_adddup2(&fa, op[1], 1);
            ::posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
            ::posix_spawn_file_actions_addclose(&fa, ip[1]);
            ::posix_spawn_file_actions_addclose(&fa, op[0]);
            #if defined(__GLIBC__)
            ::posix_spawn_file_actions_addclosefrom_np(&fa, 3); // Don't leak vtm's internal fds into the child.
            #endif
            auto argstore = std::vector<text>{ exe };
            for (auto& a : runargs) argstore.push_back(a);
            for (auto& a : args)    argstore.push_back(a);
            auto argv = std::vector<char*>{};
            for (auto& a : argstore) argv.push_back(a.data());
            argv.push_back(nullptr);
            auto rc = ::posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
            ::posix_spawn_file_actions_destroy(&fa);
            ::close(ip[0]);
            ::close(op[1]);
            if (rc != 0) { ::close(ip[1]); ::close(op[0]); pid = -1; return faux; }
            wr = ip[1];
            rd = op[0];
            running = true;
            start_reader();
            return true;
            #else
            auto sa = SECURITY_ATTRIBUTES{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
            auto inRd = HANDLE{}, inWr = HANDLE{}, outRd = HANDLE{}, outWr = HANDLE{};
            if (!::CreatePipe(&inRd, &inWr, &sa, 0)) return faux;
            if (!::CreatePipe(&outRd, &outWr, &sa, 0)) { ::CloseHandle(inRd); ::CloseHandle(inWr); return faux; }
            ::SetHandleInformation(inWr, HANDLE_FLAG_INHERIT, 0);
            ::SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);
            auto nul = ::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ,
                                     &sa, OPEN_EXISTING, 0, nullptr);
            auto startinf = STARTUPINFOEXW{ sizeof(STARTUPINFOEXW) };
            startinf.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
            startinf.StartupInfo.hStdInput  = inRd;
            startinf.StartupInfo.hStdOutput = outWr;
            startinf.StartupInfo.hStdError  = nul;
            HANDLE inherit[] = { inRd, outWr, nul };
            auto attrbuff = std::vector<byte>{};
            auto attrsize = SIZE_T{ 0 };
            ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attrsize);
            attrbuff.resize(attrsize);
            startinf.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrbuff.data());
            auto procsinf = PROCESS_INFORMATION{};
            auto cmd = "\"" + exe + "\"";
            for (auto& a : runargs) cmd += " " + a;
            for (auto& a : args)    cmd += " \"" + a + "\""; // Quote hash args (paths may contain spaces).
            auto wcmd = utf::to_utf(cmd);
            auto ok = ::InitializeProcThreadAttributeList(startinf.lpAttributeList, 1, 0, &attrsize)
                   && ::UpdateProcThreadAttribute(startinf.lpAttributeList, 0,
                          PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr, nullptr)
                   && ::CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE,
                          CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                          &startinf.StartupInfo, &procsinf);
            if (startinf.lpAttributeList) ::DeleteProcThreadAttributeList(startinf.lpAttributeList);
            ::CloseHandle(inRd);
            ::CloseHandle(outWr);
            if (nul) ::CloseHandle(nul);
            if (!ok) { ::CloseHandle(inWr); ::CloseHandle(outRd); return faux; }
            ::CloseHandle(procsinf.hThread);
            hproc = procsinf.hProcess;
            wr = inWr;
            rd = outRd;
            running = true;
            start_reader();
            return true;
            #endif
        }

        void write_line(view s) // -> child stdin (the remote password)
        {
            auto line = text{ s };
            line.push_back('\n');
            auto p = line.data();
            #if defined(_WIN32)
            if (!wr) return;
            auto left = (DWORD)line.size();
            while (left) { auto n = DWORD{}; if (!::WriteFile(wr, p, left, &n, nullptr) || n == 0) break; p += n; left -= n; }
            #else
            if (wr < 0) return;
            auto left = line.size();
            while (left) { auto n = ::write(wr, p, left); if (n <= 0) break; p += n; left -= (size_t)n; }
            #endif
        }

        auto drain() -> std::vector<text>
        {
            auto out = std::vector<text>{};
            auto lock = std::lock_guard{ line_mtx };
            out.reserve(lines.size());
            while (!lines.empty()) { out.push_back(std::move(lines.front())); lines.pop_front(); }
            return out;
        }

        void stop()
        {
            #if defined(_WIN32)
            if (hproc) ::TerminateProcess(hproc, 1);
            if (wr) { ::CloseHandle(wr); wr = nullptr; }
            if (reader.joinable()) reader.join();
            if (rd) { ::CloseHandle(rd); rd = nullptr; }
            if (hproc) { ::WaitForSingleObject(hproc, INFINITE); ::CloseHandle(hproc); hproc = nullptr; }
            #else
            if (pid > 0) ::kill(pid, SIGKILL);
            if (wr >= 0) { ::close(wr); wr = -1; }
            if (reader.joinable()) reader.join();
            if (rd >= 0) { ::close(rd); rd = -1; }
            if (pid > 0) { auto st = int{}; ::waitpid(pid, &st, 0); pid = -1; }
            #endif
            running = false;
        }

    private:
        void start_reader()
        {
            reader = std::thread{ [this]
            {
                auto buf = text{};
                auto tmp = std::array<char, 4096>{};
                for (;;)
                {
                    #if defined(_WIN32)
                    auto n = DWORD{};
                    if (!::ReadFile(rd, tmp.data(), (DWORD)tmp.size(), &n, nullptr) || n == 0) break;
                    #else
                    auto n = ::read(rd, tmp.data(), tmp.size());
                    if (n <= 0) break;
                    #endif
                    buf.append(tmp.data(), (size_t)n);
                    for (auto pos = buf.find('\n'); pos != text::npos; pos = buf.find('\n'))
                    {
                        auto line = buf.substr(0, pos);
                        if (!line.empty() && line.back() == '\r') line.pop_back();
                        { auto lk = std::lock_guard{ line_mtx }; lines.push_back(std::move(line)); }
                        buf.erase(0, pos + 1);
                    }
                }
                running = false;
            }};
        }
    };

    // Drives one `parvionhash` child for a single hash_item: builds its argv, feeds the remote
    // password over stdin, and folds the child's progress/result lines back into local state.
    struct hash_worker
    {
        hash_session session;
        ui64 item_id = 0;            // hash_item::id this worker serves.
        text exe;                    // Backend executable (the multi-call self).
        std::vector<text> runargs;   // {"-r","parvionhash"} for the multi-call self.
        // Task spec (copied from the hash_item at start):
        bool remote = faux;
        si32 algo = 2;
        text path, host, user, pass;
        si32 port = 22;
        std::vector<text> keyfiles;
        // Live state:
        si64 done = 0;
        si64 total = -1;             // From the optional S line.
        text digest, error;
        enum stt { s_init, s_running, s_ok, s_err } state = s_init;

        auto busy() const { return state == s_running; }
        auto finished() const { return state == s_ok || state == s_err; }

        static auto to_i64(view s) -> si64
        {
            auto v = si64{};
            auto neg = !s.empty() && s.front() == '-';
            for (auto c : s) if (c >= '0' && c <= '9') v = v * 10 + (c - '0');
            return neg ? -v : v;
        }

        void begin()
        {
            done = 0; total = -1; digest.clear(); error.clear();
            state = s_running;
            auto args = std::vector<text>{ text{ hash_algo_name(algo) } };
            if (remote)
            {
                args.push_back("remote");
                args.push_back(host);
                args.push_back(std::to_string(port));
                args.push_back(user);
                args.push_back(path);
                for (auto& k : keyfiles) args.push_back(k);
            }
            else
            {
                args.push_back("local");
                args.push_back(path);
            }
            session.runargs = runargs;
            if (!session.launch(exe, args)) { state = s_err; error = "Failed to launch parvionhash"; return; }
            if (remote) session.write_line(pass); // Account password over stdin (kept off argv / ps).
        }
        void poll()
        {
            if (!busy()) return;
            for (auto& ln : session.drain()) on_line(ln);
            if (state == s_running && !session.alive())
            {
                state = s_err;
                if (error.empty()) error = "parvionhash exited before producing a digest";
            }
        }
        void on_line(view ln)
        {
            if (ln.empty()) return;
            auto tag  = ln.front();
            auto rest = ln.substr(1);
            while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
            switch (tag)
            {
                case 'S': total = to_i64(rest); break;
                case 'P': { auto d = to_i64(rest); if (d >= 0) done = d; } break;
                case 'R': digest = text{ rest }; state = s_ok; break;
                case 'E': error  = text{ rest }; state = s_err; break;
                default: break;
            }
        }
        void stop() { session.stop(); }
    };
}
