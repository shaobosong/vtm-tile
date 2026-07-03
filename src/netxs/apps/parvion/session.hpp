// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/session.hpp: parvionsftp transport + connect/list controller.
//   - sftp_session : spawns the parvionsftp helper, pumps its stdio over pipes on a
//                    reader thread, answers bandwidth-quota requests immediately
//                    (-0-/-1-), and queues all other messages for the UI thread.
//   - sftp_remote  : the connect/navigate state machine; holds the remote
//                    listing for the remote pane to render. poll() (UI thread)
//                    drains the session and steps the machine.
// The handshake/quota/Listentry protocol here was validated against a live SSH
// server using the native parvionsftp built from the vendored FileZilla source.

#include "model.hpp"
#include "settings.hpp"
#include "reorder.hpp"
#include "proto.hpp"
#include "hashing.hpp"

#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <array>
#include <memory>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <map>
#include <set>

#if !defined(_WIN32)
    #include <unistd.h>
    #include <spawn.h>
    #include <signal.h>
    #include <sys/wait.h>
    #include <fcntl.h>
    #include <sys/mman.h>
    #include <sys/syscall.h>
    extern char** environ;
#else
    #include <io.h>       // _open/_close/_fdopen for io_open_chunk
    #include <fcntl.h>    // _O_CREAT/_O_RDWR/_O_BINARY
    #include <sys/stat.h> // _S_IREAD/_S_IWRITE
#endif
#include <cstdio>

namespace netxs::app::parvion
{
    // Parse one Listentry (longname / mtime / name) into a direntry. `longname`
    // is the server's unix `ls -l`-style line: "<perms> <links> <owner> <group>
    // <size> <date...> <name>".
    inline auto to_direntry(sftp_msg const& m) -> direntry
    {
        auto e = direntry{};
        e.name  = m.list_name;
        e.mtime = (time_t)m.list_mtime;
        auto toks = std::vector<text>{};
        for (auto i = size_t{}; i < m.list_text.size();)
        {
            while (i < m.list_text.size() && (m.list_text[i] == ' ' || m.list_text[i] == '\t')) ++i;
            auto b = i;
            while (i < m.list_text.size() && m.list_text[i] != ' ' && m.list_text[i] != '\t') ++i;
            if (i > b) toks.emplace_back(m.list_text.substr(b, i - b));
        }
        if (!toks.empty())
        {
            e.perms   = toks[0];
            e.is_dir  = toks[0][0] == 'd';
            e.is_link = toks[0][0] == 'l';
        }
        if (toks.size() >= 5)
        {
            e.owner = toks[2] + ":" + toks[3];
            auto& sz = toks[4];
            if (!sz.empty() && std::all_of(sz.begin(), sz.end(), [](unsigned char c){ return std::isdigit(c); }))
                e.size = (si64)std::strtoll(sz.c_str(), nullptr, 10);
        }
        if (e.is_dir) e.size = -1;
        return e;
    }

    // Transport for a spawned parvionsftp child. Quota requests are answered on the
    // reader thread (low latency, no UI involvement); everything else is queued.
    struct sftp_session
    {
        std::mutex           inbox_mtx;
        std::deque<sftp_msg> inbox;
        std::mutex           write_mtx;
        std::atomic<bool>    running{ faux };
        std::thread          reader;
        #if defined(_WIN32)
        HANDLE hproc = nullptr;
        HANDLE wr    = nullptr; // -> child stdin
        HANDLE rd    = nullptr; // <- child stdout
        HANDLE shm_map = nullptr;
        #else
        pid_t pid = -1;
        int   wr = -1; // -> child stdin
        int   rd = -1; // <- child stdout
        int   shm_fd = -1;
        #endif
        // Zero-copy shared-memory transfer path (the helper's native io_* protocol).
        // The helper mmaps this region; the applet exchanges file data through a ring
        // of buffers, pipelined by a per-transfer I/O thread (disk overlaps network).
        // Ring slot size = the granularity of the helper<->applet io_* IPC round-trip, NOT the
        // SFTP request size (which stays 32 KiB for sftpgo). The helper drains/fills a whole slot
        // across several 32 KiB FXP_READ/WRITE ops before doing one io_nextbuf, so a 256 KiB slot
        // means one IPC per 8 SFTP ops instead of one per op — the synchronous per-32 KiB IPC was
        // the dominant single-channel overhead vs an in-process client on loopback. Must be a
        // multiple of FXP_READ_REQUEST_SIZE (32 KiB) so reads pack without mid-read truncation.
        static constexpr size_t buf_bytes = 256u * 1024; // per-buffer (8 x 32 KiB SFTP ops per IPC)
        size_t ring_count = 32;            // buffers in the ring (env PARVION_SHM_BUFS; 32 x 256 KiB = 8 MiB)
        size_t shm_size = 0;               // = ring_count * buf_bytes (set in create_shm)
        void*  shm_base = nullptr;
        // Per-transfer I/O context, set by the worker before issuing get/put.
        bool       io_download = faux;     // local file is the write target (download).
        text       io_local_path;          // local file the io_* requests operate on.
        si64       io_length = -1;          // upload prefill byte budget (chunk size; -1 = whole file).
        std::FILE* io_file = nullptr;       // opened on io_open, closed on finalize/stop.
        // Streaming-hash tap (the `parvionhash` remote path, set before the transfer begins):
        // when io_hash_only is set, a download never opens io_file — instead each flushed buffer
        // is fed to io_download_tap and discarded, so a remote file can be hashed while it streams
        // in without ever touching the disk. The credit-IO ring/grant flow is unaffected.
        bool                                io_hash_only = faux;
        std::function<void(char const*, size_t)> io_download_tap;
        // Ring pipeline: the I/O thread produces (upload) / consumes (download) buffers;
        // the io_* handlers on the reader thread hand them to / take them from the helper.
        std::thread             io_thread;
        std::mutex              io_mtx;
        std::condition_variable io_cv;
        std::deque<int>                    free_slots;  // buffer indices ready to (re)use
        std::deque<std::pair<int, size_t>> ready_slots; // (idx, valid bytes) filled buffers
        int  io_cur  = -1;                  // buffer currently lent to the helper
        bool io_eof  = faux;                // upload: source exhausted (chunk end / EOF)
        bool io_done = faux;                // download: finalize requested (drain + stop)
        bool io_err  = faux;
        bool io_stop = faux;                // abort the I/O thread

        // Argv tokens inserted between the executable and "-v". When the backend is
        // vtm-tile itself (multi-call binary) this is {"-r","parvionsftp"}; when a real
        // standalone parvionsftp is used (e.g. via $PARVION_SFTP_BIN) it stays empty.
        std::vector<text> runargs;

        ~sftp_session() { stop(); }
        sftp_session() = default;
        sftp_session(sftp_session const&) = delete;
        sftp_session& operator=(sftp_session const&) = delete;

        auto launch(text const& exe) -> bool
        {
            #if !defined(_WIN32)
            create_shm(); // best-effort; the helper falls back to errors if absent
            int ip[2], op[2];
            if (::pipe(ip)) return faux;
            if (::pipe(op)) { ::close(ip[0]); ::close(ip[1]); return faux; }
            auto fa = posix_spawn_file_actions_t{};
            ::posix_spawn_file_actions_init(&fa);
            ::posix_spawn_file_actions_adddup2(&fa, ip[0], 0);
            ::posix_spawn_file_actions_adddup2(&fa, op[1], 1);
            // Keep the helper's verbose -v stderr off our terminal / protocol pipe.
            ::posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
            // Hand the shared-memory region to the helper as inherited fd 3 (the
            // io_open reply tells it to mmap fd 3).
            if (shm_fd >= 0) ::posix_spawn_file_actions_adddup2(&fa, shm_fd, 3);
            ::posix_spawn_file_actions_addclose(&fa, ip[1]);
            ::posix_spawn_file_actions_addclose(&fa, op[0]);
            // Do not let the helper inherit vtm's internal descriptors (event
            // pipes, etc.) — that wedges the host's event loop. Keep only 0/1/2/3.
            #if defined(__GLIBC__)
            ::posix_spawn_file_actions_addclosefrom_np(&fa, shm_fd >= 0 ? 4 : 3);
            #endif
            auto argstore = std::vector<text>{ exe };
            for (auto& a : runargs) argstore.push_back(a); // e.g. -r parvionsftp (multi-call self).
            argstore.push_back("-v");
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
            create_shm(); // mapping handle is DuplicateHandle'd to the helper at io_open
            // Inheritable pipes: child stdin = inRd, child stdout = outWr.
            auto sa = SECURITY_ATTRIBUTES{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
            auto inRd = HANDLE{}, inWr = HANDLE{}, outRd = HANDLE{}, outWr = HANDLE{};
            if (!::CreatePipe(&inRd, &inWr, &sa, 0)) return faux;
            if (!::CreatePipe(&outRd, &outWr, &sa, 0)) { ::CloseHandle(inRd); ::CloseHandle(inWr); return faux; }
            // Our ends must not be inherited by the child.
            ::SetHandleInformation(inWr, HANDLE_FLAG_INHERIT, 0);
            ::SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);
            // Keep the helper's verbose -v stderr off our protocol pipe.
            auto nul = ::CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ,
                                     &sa, OPEN_EXISTING, 0, nullptr);
            auto startinf = STARTUPINFOEXW{ sizeof(STARTUPINFOEXW) };
            startinf.StartupInfo.dwFlags    = STARTF_USESTDHANDLES;
            startinf.StartupInfo.hStdInput  = inRd;
            startinf.StartupInfo.hStdOutput = outWr;
            startinf.StartupInfo.hStdError  = nul;
            // Inherit ONLY these three handles (the Win32 equivalent of closefrom):
            // never leak vtm's internal handles into the helper.
            HANDLE inherit[] = { inRd, outWr, nul };
            auto attrbuff = std::vector<byte>{};
            auto attrsize = SIZE_T{ 0 };
            ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attrsize);
            attrbuff.resize(attrsize);
            startinf.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrbuff.data());
            auto procsinf = PROCESS_INFORMATION{};
            auto cmd = "\"" + exe + "\"";
            for (auto& a : runargs) cmd += " " + a; // Simple tokens (e.g. -r parvionsftp); no quoting needed.
            cmd += " -v";
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

        void write_line(view s)
        {
            auto lock = std::lock_guard{ write_mtx };
            auto line = text{ s };
            line.push_back('\n');
            auto p = line.data();
            #if defined(_WIN32)
            if (!wr) return;
            auto left = (DWORD)line.size();
            while (left)
            {
                auto n = DWORD{};
                if (!::WriteFile(wr, p, left, &n, nullptr) || n == 0) break;
                p += n;
                left -= n;
            }
            #else
            if (wr < 0) return;
            auto left = line.size();
            while (left)
            {
                auto n = ::write(wr, p, left);
                if (n <= 0) break;
                p += n;
                left -= (size_t)n;
            }
            #endif
        }

        auto drain() -> std::vector<sftp_msg>
        {
            auto out = std::vector<sftp_msg>{};
            auto lock = std::lock_guard{ inbox_mtx };
            out.reserve(inbox.size());
            while (!inbox.empty()) { out.push_back(std::move(inbox.front())); inbox.pop_front(); }
            return out;
        }

        auto alive() const { return running.load(); }

        void stop()
        {
            // Hard-kill the helper first (FileZilla DoClose parity: process_->kill()). A helper
            // sitting at its prompt would exit on the stdin EOF below anyway, but one mid-transfer
            // reads its command pipe only after aborting and draining its pipelined requests —
            // link-speed-bound seconds during which the joins/waits below would block the UI
            // thread (Pause/Remove must take effect immediately). After the kill, the reader
            // sees EOF and the wait reaps a zombie, so every step here completes instantly.
            #if defined(_WIN32)
            if (hproc) ::TerminateProcess(hproc, 1);
            #else
            if (pid > 0) ::kill(pid, SIGKILL);
            #endif
            // Wake any io_* handler parked on the ring before we join the reader thread.
            { auto lk = std::lock_guard{ io_mtx }; io_stop = true; io_cv.notify_all(); }
            #if defined(_WIN32)
            { auto lock = std::lock_guard{ write_mtx }; if (wr) { ::CloseHandle(wr); wr = nullptr; } }
            if (reader.joinable()) reader.join();
            if (rd) { ::CloseHandle(rd); rd = nullptr; }
            if (hproc) { ::WaitForSingleObject(hproc, INFINITE); ::CloseHandle(hproc); hproc = nullptr; }
            #else
            { auto lock = std::lock_guard{ write_mtx }; if (wr >= 0) { ::close(wr); wr = -1; } }
            if (reader.joinable()) reader.join();
            if (rd >= 0) { ::close(rd); rd = -1; }
            if (pid > 0) { auto st = int{}; ::waitpid(pid, &st, 0); pid = -1; }
            #endif
            destroy_shm();
            running = false;
        }

        // Bind the local file an upcoming transfer's io_* requests operate on.
        // Must be set before issuing get/put so the reader thread can serve the
        // helper's shared-memory I/O. (Harmless for the direct parallel paths.)
        void set_io_context(bool download, text path, si64 length = -1)
        {
            stop_io_thread();
            if (io_file) { std::fclose(io_file); io_file = nullptr; }
            // Publish the io context under io_mtx. on_io_open reads these on the reader thread;
            // the helper's io_open only arrives after this call (via the command round-trip), so
            // pairing this lock with on_io_open's lock gives a happens-before edge. Without it the
            // reader thread could see a stale io_download=faux and route a download chunk into the
            // upload branch (which sends no credit grants) -> the helper blocks in next_grant
            // forever -> a parallel-download chunk stuck at progress 0.
            auto lk = std::lock_guard{ io_mtx };
            io_download = download;
            io_local_path = std::move(path);
            io_length = length;
        }

        auto create_shm() -> bool
        {
            ring_count = 32;
            if (auto e = std::getenv("PARVION_SHM_BUFS")) { if (auto n = std::atoi(e); n > 0) ring_count = (size_t)n; }
            if (ring_count < 1)   ring_count = 1;
            if (ring_count > 256) ring_count = 256;
            shm_size = ring_count * buf_bytes;
            #if defined(_WIN32)
            shm_map = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)shm_size, nullptr);
            if (!shm_map) return faux;
            shm_base = ::MapViewOfFile(shm_map, FILE_MAP_ALL_ACCESS, 0, 0, shm_size);
            if (!shm_base) { ::CloseHandle(shm_map); shm_map = nullptr; return faux; }
            #else
            shm_fd = (int)::syscall(SYS_memfd_create, "parvion-shm", 0u);
            if (shm_fd < 0) return faux;
            if (::ftruncate(shm_fd, (off_t)shm_size) != 0) { ::close(shm_fd); shm_fd = -1; return faux; }
            shm_base = ::mmap(nullptr, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
            if (shm_base == MAP_FAILED) { shm_base = nullptr; ::close(shm_fd); shm_fd = -1; return faux; }
            #endif
            return true;
        }
        void destroy_shm()
        {
            stop_io_thread();
            if (io_file) { std::fclose(io_file); io_file = nullptr; }
            #if defined(_WIN32)
            if (shm_base) { ::UnmapViewOfFile(shm_base); shm_base = nullptr; }
            if (shm_map)  { ::CloseHandle(shm_map); shm_map = nullptr; }
            #else
            if (shm_base) { ::munmap(shm_base, shm_size); shm_base = nullptr; }
            if (shm_fd >= 0) { ::close(shm_fd); shm_fd = -1; }
            #endif
        }

    private:
        static auto parse_u64(view s) -> ui64
        {
            auto v = ui64{};
            for (auto c : s) if (c >= '0' && c <= '9') v = v * 10 + (ui64)(c - '0');
            return v;
        }
        // Parse two space-separated unsigned integers "<a> <b>" (credit-IO io_nextbuf /
        // io_finalize payloads). parse_u64 can't be reused — it would concatenate the
        // two numbers across the separator.
        static void parse_two(view s, ui64& a, ui64& b)
        {
            a = 0; b = 0;
            auto i = size_t{ 0 };
            while (i < s.size() && (s[i] < '0' || s[i] > '9')) ++i;
            while (i < s.size() &&  s[i] >= '0' && s[i] <= '9') a = a * 10 + (ui64)(s[i++] - '0');
            while (i < s.size() && (s[i] < '0' || s[i] > '9')) ++i;
            while (i < s.size() &&  s[i] >= '0' && s[i] <= '9') b = b * 10 + (ui64)(s[i++] - '0');
        }
        // Proactive-grant download flow control (default on; PARVION_CREDIT_IO=0 disables).
        // The helper inherits this env via posix_spawn, so both ends agree on the mode.
        static auto credit_io_enabled() -> bool
        {
            static auto v = []{ auto e = std::getenv("PARVION_CREDIT_IO"); return !(e && e[0] == '0'); }();
            return v;
        }
        auto slot_off(int idx) const -> size_t { return (size_t)idx * buf_bytes; }
        // 64-bit file seek (std::fseek's `long` offset is 32-bit on Win32, so a chunk start
        // past 2 GiB would truncate; use _fseeki64 there).
        static void io_seek64(std::FILE* f, ui64 off)
        {
            #if defined(_WIN32)
            ::_fseeki64(f, (long long)off, SEEK_SET);
            #else
            std::fseek(f, (long)off, SEEK_SET);
            #endif
        }
        // Open a parallel-download chunk's shared target for positioned writes: create if
        // absent, NEVER truncate, read/write. O_CREAT (no O_TRUNC/O_EXCL) is idempotent, so
        // the chunks can open the same file concurrently without a pre-creation step and
        // without clobbering each other (a resumed chunk keeps its partial data).
        static auto io_open_chunk(text const& path) -> std::FILE*
        {
            #if defined(_WIN32)
            auto fd = ::_open(path.c_str(), _O_CREAT | _O_RDWR | _O_BINARY, _S_IREAD | _S_IWRITE);
            if (fd < 0) return nullptr;
            auto f = ::_fdopen(fd, "r+b");
            if (!f) ::_close(fd);
            return f;
            #else
            auto fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0666);
            if (fd < 0) return nullptr;
            auto f = ::fdopen(fd, "r+b");
            if (!f) ::close(fd);
            return f;
            #endif
        }
        // Download write-back pacing (PARVION_WB_PACE; default on, Linux only) — see uxsftp.c.
        // Spread a single-channel download's page-cache write-back across the transfer and bound
        // the dirty footprint, so the kernel doesn't burst-write the whole file at completion
        // (which saturates the disk and makes the machine feel sluggish right after the transfer).
        #if defined(__linux__)
        static auto wb_pace_enabled() -> bool
        {
            static auto v = []{ auto e = std::getenv("PARVION_WB_PACE"); return !(e && e[0] == '0'); }();
            return v;
        }
        static void io_pace(std::FILE* file, ui64& synced, ui64& paced, ui64 wr, bool final)
        {
            if (!file || !wb_pace_enabled()) return;
            auto fd = ::fileno(file);
            if (fd < 0) return;
            constexpr ui64 STEP = 4u << 20, LAG = 16u << 20; // drop in 4 MiB units; keep ~16 MiB cached
            while (wr - synced >= STEP || (final && wr > synced))
            {
                auto len = final ? (wr - synced) : std::min<ui64>(wr - synced, STEP);
                if (!len) break;
                #if defined(SYS_sync_file_range) && (defined(__x86_64__) || defined(__aarch64__))
                ::syscall(SYS_sync_file_range, fd, (long long)synced, (long long)len,
                          final ? (1u | 2u | 4u) : 2u); // WAIT_BEFORE|WRITE|WAIT_AFTER : WRITE
                #endif
                synced += len;
            }
            while (wr - paced >= LAG + STEP || (final && synced > paced))
            {
                auto len = final ? (synced - paced) : std::min<ui64>(synced - paced, STEP);
                if (!len) break;
                ::posix_fadvise(fd, (off_t)paced, (off_t)len, POSIX_FADV_DONTNEED);
                paced += len;
            }
        }
        #else
        static auto wb_pace_enabled() -> bool { return false; }
        static void io_pace(std::FILE*, ui64&, ui64&, ui64, bool) {}
        #endif
        void reset_ring()
        {
            auto lk = std::lock_guard{ io_mtx };
            free_slots.clear(); ready_slots.clear();
            for (auto i = 0; i < (int)ring_count; ++i) free_slots.push_back(i);
            io_cur = -1; io_eof = faux; io_done = faux; io_err = faux; io_stop = faux;
        }
        void start_io_thread()
        {
            if (io_thread.joinable()) stop_io_thread();
            reset_ring();
            io_thread = std::thread{ [this]{ io_loop(); } };
        }
        void stop_io_thread()
        {
            { auto lk = std::lock_guard{ io_mtx }; io_stop = true; io_cv.notify_all(); }
            if (io_thread.joinable()) io_thread.join();
        }
        // Pipeline the local file against the helper's network I/O: for an upload,
        // prefill ring buffers from disk ahead of the helper (bounded to the chunk);
        // for a download, flush the helper-filled buffers to disk behind it.
        void io_loop()
        {
            if (io_download)
            {
                auto credit = credit_io_enabled();
                // Write-back pacing state (positioned at the file's start offset = chunk/resume start).
                auto wpos = (ui64)(io_file ? std::ftell(io_file) : 0);
                auto synced = wpos, paced = wpos;
                for (;;)
                {
                    auto item = std::pair<int, size_t>{ -1, 0 };
                    auto finish = faux;
                    {
                        auto lk = std::unique_lock{ io_mtx };
                        io_cv.wait(lk, [&]{ return io_stop || io_err || !ready_slots.empty() || io_done; });
                        if (io_stop || io_err) return;        // abort: leave partial data, no final flush
                        if (ready_slots.empty()) finish = true; // finalize requested + drained
                        else { item = ready_slots.front(); ready_slots.pop_front(); }
                    }
                    if (finish)
                    {
                        // Clean completion: flush stdio + force/drop this file's trailing dirty range
                        // so there is no write-back burst when the transfer finishes.
                        if (io_file) std::fflush(io_file);
                        io_pace(io_file, synced, paced, wpos, true);
                        return;
                    }
                    // Hash-only download: feed the buffer to the tap and discard it (no io_file).
                    if (io_hash_only)
                    {
                        if (item.second && io_download_tap) io_download_tap((char*)shm_base + slot_off(item.first), item.second);
                        wpos += item.second;
                    }
                    auto ok = io_hash_only || !io_file || !item.second
                           || std::fwrite((char*)shm_base + slot_off(item.first), 1, item.second, io_file) == item.second;
                    if (ok && !io_hash_only && item.second) { wpos += item.second; io_pace(io_file, synced, paced, wpos, faux); }
                    {
                        auto lk = std::lock_guard{ io_mtx };
                        if (!ok) io_err = true;
                        if (!credit) free_slots.push_back(item.first); // legacy: on_io_nextbuf hands it back
                        io_cv.notify_all();
                    }
                    // Credit mode: re-grant the just-flushed slot to the helper as fresh
                    // credit (or signal error). write_line takes write_mtx only, never
                    // under io_mtx — released above to preserve io_mtx -> write_mtx order.
                    if (credit) write_line(ok ? "-G" + std::to_string(slot_off(item.first)) + " " + std::to_string(buf_bytes)
                                              : "--1");
                    if (!ok) return;
                }
            }
            else
            {
                auto remaining = io_length; // -1 = whole file
                for (;;)
                {
                    auto slot = int{};
                    {
                        auto lk = std::unique_lock{ io_mtx };
                        io_cv.wait(lk, [&]{ return io_stop || !free_slots.empty(); });
                        if (io_stop) return;
                        slot = free_slots.front(); free_slots.pop_front();
                    }
                    auto want = buf_bytes;
                    if (remaining >= 0 && (si64)want > remaining) want = (size_t)remaining;
                    auto n = (want && io_file) ? std::fread((char*)shm_base + slot_off(slot), 1, want, io_file) : size_t{};
                    if (remaining >= 0) remaining -= (si64)n;
                    {
                        auto lk = std::lock_guard{ io_mtx };
                        if (n) ready_slots.push_back({ slot, n });
                        else   free_slots.push_back(slot);
                        if (n < want || remaining == 0) io_eof = true;
                        io_cv.notify_all();
                    }
                    if (io_eof) return;
                }
            }
        }
        // The helper requested a buffer mapping for the file it just opened. Open
        // the local file (read source for upload, write target for download) and
        // reply with the shared-memory handle/fd, its size, and the file offset.
        void on_io_open(view arg)
        {
            auto off = parse_u64(arg);
            // Read the io context published by set_io_context (poll thread) UNDER io_mtx — this
            // pairs with set_io_context's lock for a happens-before edge, so we never observe a
            // stale io_download/io_local_path/io_length. (A stale io_download=faux on a download
            // chunk would take the upload branch below, which sends no credit grants, and the
            // helper would then block forever in next_grant — a chunk stuck at progress 0.)
            auto dl = bool{}; auto lpath = text{}; auto ilen = si64{};
            { auto lk = std::lock_guard{ io_mtx }; dl = io_download; lpath = io_local_path; ilen = io_length; }
            stop_io_thread();
            if (io_file) { std::fclose(io_file); io_file = nullptr; }
            if (dl && io_hash_only)
            {
                // Hash-only download (parvionhash remote): no local target. io_file stays null;
                // io_loop feeds each flushed buffer to io_download_tap and discards it.
            }
            else if (dl)
            {
                if (off == (ui64)-1) { io_file = std::fopen(lpath.c_str(), "r+b"); if (io_file) std::fseek(io_file, 0, SEEK_END); } // resume
                else if (ilen >= 0)
                {
                    // Parallel-download chunk: open the shared target create-if-absent / no-truncate
                    // and seek to this chunk's start. The io_thread then writes the chunk's bytes
                    // sequentially from there; several chunks open the same file concurrently
                    // without clobbering, and a resumed chunk keeps its partial data.
                    io_file = io_open_chunk(lpath);
                    if (io_file) io_seek64(io_file, off);
                }
                else { io_file = std::fopen(lpath.c_str(), "wb"); } // fresh whole-file (truncate)
            }
            else
            {
                io_file = std::fopen(lpath.c_str(), "rb");
                if (io_file && off) io_seek64(io_file, off); // chunk/resume start (64-bit)
            }
            if (!io_file && !io_hash_only) { write_line("--"); return; }
            auto cur = io_file ? (ui64)std::ftell(io_file) : (ui64)0;
            start_io_thread(); // begins prefill (upload) / awaits flush queue (download)
            #if defined(_WIN32)
            auto target = HANDLE{};
            if (hproc && ::DuplicateHandle(::GetCurrentProcess(), shm_map, hproc, &target, 0, FALSE, DUPLICATE_SAME_ACCESS))
                 write_line("-" + std::to_string((uintptr_t)target) + " " + std::to_string(shm_size) + " " + std::to_string(cur));
            else write_line("--");
            #else
            write_line("-3 " + std::to_string(shm_size) + " " + std::to_string(cur)); // child mmaps its inherited fd 3
            #endif
            // Credit-IO download: pre-grant every free ring slot now (after the open
            // reply, so the helper reads the mapping first). Each grant is the same
            // "-<offset> <buf_bytes>" line the legacy on_io_nextbuf used to reply with;
            // the helper's existing priority_read() consumes them and the OS pipe buffer
            // acts as the credit FIFO. The io_thread then replenishes one grant per slot
            // it flushes. The io_thread can't grant yet (download ready_slots is empty),
            // so no interleaving with this batch.
            if (dl && credit_io_enabled())
            {
                auto grants = std::vector<size_t>{};
                { auto lk = std::lock_guard{ io_mtx };
                  for (auto idx : free_slots) grants.push_back(slot_off(idx));
                  free_slots.clear(); }
                for (auto goff : grants)
                    write_line("-G" + std::to_string(goff) + " " + std::to_string(buf_bytes));
            }
        }
        void on_io_size()
        {
            auto sz = ui64{};
            auto ec = std::error_code{};
            if (!io_local_path.empty()) { sz = (ui64)fs::file_size(fs::path{ io_local_path }, ec); if (ec) sz = 0; }
            write_line("-" + std::to_string(sz));
        }
        // The helper finished with the current buffer and wants the next. Hand it the
        // next ring buffer the I/O thread has prepared (upload) / a free buffer to fill
        // while the I/O thread flushes the one it just finished (download). The reply
        // is "-<buffer-offset> <bytes>" (or "-0" at upload EOF).
        void on_io_nextbuf(view arg)
        {
            if (!shm_base) { write_line("--1"); return; }
            if (io_download && credit_io_enabled())
            {
                // One-way completion: "<off> <bytes>" -> queue the filled slot for disk
                // flush. No io_cv.wait, no reply — the reader thread never blocks, and the
                // next grant was pre-sent / comes from io_loop. bytes==0 is the helper's
                // first/empty notify (no prior slot) -> ignore.
                auto off = ui64{}, bytes = ui64{};
                parse_two(arg, off, bytes);
                if (bytes)
                {
                    auto lk = std::lock_guard{ io_mtx };
                    ready_slots.push_back({ (int)(off / buf_bytes), (size_t)bytes });
                    io_cv.notify_all();
                }
                return;
            }
            auto reply = text{};
            if (io_download)
            {
                auto processed = (size_t)parse_u64(arg);
                auto lk = std::unique_lock{ io_mtx };
                if (io_cur >= 0) // the helper filled io_cur with `processed` bytes -> queue for flush
                {
                    if (processed) ready_slots.push_back({ io_cur, processed });
                    else           free_slots.push_back(io_cur);
                    io_cur = -1; io_cv.notify_all();
                }
                io_cv.wait(lk, [&]{ return io_err || io_stop || !free_slots.empty(); });
                if (io_err || io_stop) reply = "--1";
                else { io_cur = free_slots.front(); free_slots.pop_front();
                       reply = "-" + std::to_string(slot_off(io_cur)) + " " + std::to_string(buf_bytes); }
            }
            else
            {
                auto lk = std::unique_lock{ io_mtx };
                if (io_cur >= 0) { free_slots.push_back(io_cur); io_cur = -1; io_cv.notify_all(); } // helper done with it
                io_cv.wait(lk, [&]{ return io_err || io_stop || !ready_slots.empty() || io_eof; });
                if (io_err || io_stop) reply = "--1";
                else if (ready_slots.empty()) reply = "-0"; // EOF
                else { auto it = ready_slots.front(); ready_slots.pop_front(); io_cur = it.first;
                       reply = "-" + std::to_string(slot_off(it.first)) + " " + std::to_string(it.second); }
            }
            write_line(reply);
        }
        void on_io_finalize(view arg)
        {
            if (io_download && credit_io_enabled())
            {
                // Trailing partial slot carried as "<off> <bytes>" (io_cur is unused on
                // credit-IO download). Queue it, then drain the flush queue via join.
                auto off = ui64{}, last = ui64{};
                parse_two(arg, off, last);
                { auto lk = std::lock_guard{ io_mtx };
                  if (last) ready_slots.push_back({ (int)(off / buf_bytes), (size_t)last });
                  io_done = true; io_cv.notify_all(); }
            }
            else
            {
                auto last = (size_t)parse_u64(arg);
                { auto lk = std::lock_guard{ io_mtx };
                  if (io_cur >= 0) { if (last) ready_slots.push_back({ io_cur, last }); io_cur = -1; }
                  io_done = true; io_cv.notify_all(); }
            }
            if (io_thread.joinable()) io_thread.join(); // drain the flush queue to disk
            if (io_file) { std::fclose(io_file); io_file = nullptr; }
            write_line(io_err ? "-0" : "-1");
        }
        void on_parsed(sftp_msg&& m)
        {
            switch (m.type)
            {
                case sftp_evt::used_quota_recv: write_line("-0-"); return; // grant unlimited recv
                case sftp_evt::used_quota_send: write_line("-1-"); return; // grant unlimited send
                case sftp_evt::io_open:     on_io_open(m.first());    return;
                case sftp_evt::io_size:     on_io_size();             return;
                case sftp_evt::io_nextbuf:  on_io_nextbuf(m.first()); return;
                case sftp_evt::io_finalize: on_io_finalize(m.first());return;
                default: break;
            }
            auto lock = std::lock_guard{ inbox_mtx };
            inbox.push_back(std::move(m));
        }
        void start_reader()
        {
            reader = std::thread{ [this]
            {
                auto parser = sftp_parser{};
                parser.on_msg = [this](sftp_msg&& m){ on_parsed(std::move(m)); };
                auto buf = std::array<char, 16384>{};
                for (;;)
                {
                    #if defined(_WIN32)
                    auto n = DWORD{};
                    if (!::ReadFile(rd, buf.data(), (DWORD)buf.size(), &n, nullptr) || n == 0) break;
                    parser.feed(view{ buf.data(), (size_t)n });
                    #else
                    auto n = ::read(rd, buf.data(), buf.size());
                    if (n > 0) parser.feed(view{ buf.data(), (size_t)n });
                    else break;
                    #endif
                }
                running = false;
            }};
        }
    };

    // Message-log entry classification (FileZilla logmsg::type). Namespace-scoped
    // so transfer workers (below) and the controller (sftp_remote) share it and
    // funnel into the same log — mirroring FileZilla, where every control socket
    // (browsing or transfer) routes through one StatusView.
    enum class logtype : si32 { status, error, command, response, trace, listing };
    // Trace sub-levels, mirroring FileZilla's debug_* logmsg types: a Trace line
    // shows only when the selected debug level reaches its sub-level.
    enum dbg : si32 { dbg_warning = 1, dbg_info = 2, dbg_verbose = 3, dbg_debug = 4 };

    // Extract the key file path from a backend "SSH key passphrase" prompt, whose text is
    // 'Passphrase for key "<comment>" in key file "<path>"' (ssh2userauth.c). The comment can
    // itself contain quotes, so anchor on the LAST `in key file "` and read up to the final quote.
    inline auto sftp_keyfile_from_prompt(view prompt) -> text
    {
        auto marker = view{ "in key file \"" };
        auto p = prompt.rfind(marker);
        if (p == view::npos) return {};
        auto start = p + marker.size();
        auto end = prompt.rfind('"');
        if (end == view::npos || end <= start) return {};
        return text{ prompt.substr(start, end - start) };
    }

    // One transfer worker: a dedicated parvionsftp connection performing a single
    // get/put (optionally a byte range, for parallel chunks). Mirrors the connect
    // handshake, then issues the transfer and accumulates Transfer deltas. Like a
    // FileZilla transfer control socket, it logs the command it sends and the
    // server's replies/errors into the shared message log via `logsink`.
    struct xfer_worker
    {
        sftp_session session;
        text exe, host, user, pass;
        std::vector<text> runargs;  // Backend run-prefix (copied from sftp_remote; gains -C when compression is on).
        std::vector<text> keyfiles; // Private keys to register before auth (copied from sftp_remote::cfg.keyfiles).
        si32 port = 22;
        bool download = true;
        bool parallel = faux;          // Use parvion-get/-put (chunked).
        bool initialize = faux;        // Parallel upload leader (chunk 0): truncate+create the target.
        text remote_path, local_path;
        si64 offset = 0;               // Chunk start (parallel).
        si64 length = -1;              // Chunk length (parallel); -1 = whole file.
        si64 done = 0;                 // Bytes transferred (accumulated deltas + resumed base).
        si32 chunk_index = 0;          // Index into the resumable state file's part table.
        si64 persisted = -1;           // Last `done` written to the state file (skip no-op writes).
        bool opened = faux;            // Helper has opened the remote file (Info line during xfer).
        enum stt { s_init, s_connecting, s_running, s_ok, s_err } state = s_init;
        text error;
        enum awt { a_none, a_open, a_xfer, a_keyfile } await = a_none;
        size_t keyfile_i = 0; // Cursor into `keyfiles` during the pre-auth a_keyfile phase.
        std::function<void(logtype, text, si32)> logsink; // -> sftp_remote::log_line (set by cfg_worker).
        // Passphrase plumbing (set by cfg_worker): the provider queries sftp_remote's session-scoped
        // passphrase cache. Workers never prompt the user — the control connection authenticates first
        // and populates the cache; pass_asked guards against an infinite re-ask if a cached value is wrong.
        std::function<bool(text const&, text&)> passphrase_provider;
        text last_preamble, last_instruction;
        std::set<text> pass_asked;

        // Log into the shared message log (FileZilla routes every control socket
        // through one StatusView). lg() no-ops if no sink was attached.
        void lg(logtype t, text s, si32 level = 0) { if (logsink) logsink(t, std::move(s), level); }
        // Send a command on this connection, logging it first (like SendCommand's
        // log_raw(logmsg::command, ...)). Secrets (host-key answer, password) are
        // sent raw and never logged.
        void wr_cmd(view c) { lg(logtype::command, text{ c }); session.write_line(c); }

        static auto to_i64(view s) -> si64
        {
            auto v = si64{};
            auto neg = !s.empty() && s.front() == '-';
            for (auto c : s) if (c >= '0' && c <= '9') v = v * 10 + (c - '0');
            return neg ? -v : v;
        }
        auto busy() const { return state == s_connecting || state == s_running; }
        auto finished() const { return state == s_ok || state == s_err; }
        // Idle-pool bookkeeping: a worker that finished cleanly and whose backend is still alive can be
        // returned to sftp_remote::idle_pool and reused (see rearm) instead of respawned. idle_since
        // timestamps when it entered the pool, for the idle-disconnect reaper.
        std::chrono::steady_clock::time_point idle_since{};
        auto reusable() const { return state == s_ok && session.alive(); }
        // Parallel-upload leader: once the helper has opened the remote target
        // (Info line emitted right after the truncate+create), or is already
        // writing/done, the follower chunks can safely write their absolute
        // offsets without the leader's TRUNC wiping their data.
        auto leader_ready() const { return opened || (state == s_running && done > 0) || state == s_ok; }

        void begin()
        {
            session.stop();
            done = 0; opened = faux; error.clear(); await = a_none; state = s_connecting;
            pass_asked.clear(); last_preamble.clear(); last_instruction.clear(); // Fresh auth session.
            auto label = text{ download ? "download" : "upload" } + " of " + (download ? remote_path : local_path);
            lg(logtype::status, "Starting " + label + "..."); // FileZilla logs each transfer's start.
            session.runargs = runargs;
            if (!session.launch(exe)) { lg(logtype::error, "Failed to launch parvionsftp: " + exe); state = s_err; error = "Failed to launch parvionsftp"; }
        }
        void stop() { session.stop(); }
        // Reuse this already-connected, authenticated session for a new transfer (single-stream or a
        // parallel chunk): the per-transfer fields (download/remote_path/local_path/parallel/offset/
        // length/initialize) are refreshed by cfg_worker first, then we issue the get/put directly,
        // skipping launch + open + host-key + auth (the costly part). Only progress fields reset here.
        void rearm()
        {
            done = 0; persisted = -1; opened = faux; error.clear();
            state = s_running; await = a_xfer;
            lg(logtype::status, text{ "Reusing connection for " } + (download ? "download" : "upload")
                              + " of " + (download ? remote_path : local_path) + "...");
            send_xfer();
        }
        void poll()
        {
            if (!busy()) return;
            for (auto& m : session.drain()) on(m);
            if (!session.alive() && busy()) { state = s_err; if (error.empty()) error = "Connection closed"; }
        }
        void on(sftp_msg const& m)
        {
            switch (m.type)
            {
                // FileZilla CSftpControlSocket::OnSftpEvent: Error is a log line
                // ONLY; success/failure is decided solely by the Done result code
                // (below). We still remember the text so a subsequent failing Done
                // can surface the real server reason in the queue's Reason column.
                case sftp_evt::error: lg(logtype::error, text{ m.first() }); if (error.empty()) error = text{ m.first() }; break;
                case sftp_evt::ask_hostkey:
                case sftp_evt::ask_hostkey_changed:
                case sftp_evt::ask_hostkey_betteralg: session.write_line("y"); break; // Secret-ish; sent raw, unlogged.
                case sftp_evt::request_preamble:    last_preamble    = m.line.empty() ? text{} : text{ m.first() }; break;
                case sftp_evt::request_instruction: last_instruction = m.line.empty() ? text{} : text{ m.first() }; break;
                case sftp_evt::ask_password: // Key passphrase from the controller's cache, else the account password.
                    if (last_preamble == "SSH key passphrase")
                    {
                        auto kf = sftp_keyfile_from_prompt(m.first());
                        auto pp = text{};
                        if (!pass_asked.count(kf) && passphrase_provider && passphrase_provider(kf, pp)) { pass_asked.insert(kf); session.write_line(pp); }
                        else { state = s_err; if (error.empty()) error = "Key passphrase unavailable"; session.stop(); } // Workers never prompt.
                    }
                    else session.write_line(pass); // Account password (unchanged; from the Quick Connect bar).
                    last_preamble.clear(); last_instruction.clear();
                    break;
                case sftp_evt::transfer: { auto d = to_i64(m.first()); if (d > 0) done += d; } break; // Progress: status bar, not the log.
                case sftp_evt::verbose: if (!m.line.empty()) lg(logtype::trace, text{ m.first() }, dbg_info); break;
                case sftp_evt::info:
                case sftp_evt::status:
                    if (!m.line.empty()) lg(logtype::status, text{ m.first() });
                    if (await == a_xfer) opened = true; // Remote file opened.
                    break;
                case sftp_evt::reply:
                    if (!m.line.empty()) lg(logtype::response, text{ m.first() });
                    if (await == a_none) { keyfile_i = 0; send_next_keyfile_or_open(); } // Greeting: register keys, then open.
                    else complete();
                    break;
                // The Done payload is the command result code (FileZilla
                // OnSftpEvent: "1" = OK, "2" = critical error, anything else =
                // error). It is authoritative: a read-only server rejecting an
                // upload emits Error then Done("0"), so honouring the code is what
                // keeps the item in the Failed queue instead of Succeeded.
                case sftp_evt::done:
                    if (await == a_keyfile) send_next_keyfile_or_open(); // Key registered; next key or open.
                    else if (m.first() == "1") complete();
                    else { state = s_err; if (error.empty()) error = "Transfer failed"; }
                    break;
                default: break;
            }
        }
        // Register the next existing key file (await a_keyfile), or send `open` once all are done.
        // Each `keyfile` command's Done must be consumed before the next command (see the control
        // session's send_next_keyfile_or_open for why they can't be pipelined ahead of `open`).
        void send_next_keyfile_or_open()
        {
            while (keyfile_i < keyfiles.size())
            {
                auto& kf = keyfiles[keyfile_i++];
                if (kf.empty()) continue;
                auto ec = std::error_code{};
                if (!fs::is_regular_file(fs::path{ kf }, ec)) continue;
                await = a_keyfile;
                wr_cmd("keyfile " + quote_name(kf));
                return;
            }
            await = a_open;
            wr_cmd("open " + quote_name(user + "@" + host) + " " + std::to_string(port));
        }
        void complete()
        {
            if (await == a_open) { state = s_running; await = a_xfer; send_xfer(); }
            else if (await == a_xfer) state = s_ok;
        }
        void send_xfer()
        {
            // Bind the local file (and the chunk byte budget for upload prefill) the
            // helper's shared-memory io_* will operate on.
            session.set_io_context(download, local_path, length);
            auto rq = quote_name(remote_path);
            auto lq = quote_name(local_path);
            if (parallel)
            {
                if (download) wr_cmd("parvion-get " + rq + " " + lq + " " + std::to_string(offset) + " " + std::to_string(length));
                else          wr_cmd("parvion-put " + lq + " " + rq + " " + std::to_string(offset) + " " + std::to_string(length) + (initialize ? " 1" : " 0"));
            }
            else
            {
                if (download) wr_cmd("get " + rq + " " + lq);
                else          wr_cmd("put " + lq + " " + rq);
            }
        }
    };

    // The Quick Connect history file: <config>/parvion/recent_servers (see parvion_config_dir
    // in settings.hpp for how <config> is resolved). The directory is created on demand.
    inline auto parvion_recent_path() -> fs::path { return parvion_config_dir() / "recent_servers"; }

    // Connect/navigate state machine. Owns the session and the current remote
    // listing. The remote pane renders from `path`/`items`/`status`; navigation
    // calls chdir()/cdup(). poll() must be called on the UI thread.
    struct sftp_remote
    {
        enum stage_t { s_idle, s_greeting, s_opening, s_connected, s_failed };
        enum cmd_t   { c_none, c_open, c_pwd, c_ls, c_cd, c_op, // c_op: mkdir/rm/rmdir/mv, then re-list.
                       c_rls,   // Recursive-walk listing (ls <path>) for a folder download/delete; result drives recop.
                       c_recop, // A recop one-shot command (mkdir for upload, rm/rmdir for delete); advance regardless of result.
                       c_keyfile }; // Pre-auth `keyfile <path>` registration; its Done advances to the next key or `open`.

        sftp_session          session;
        text                  exe;
        std::vector<text>     runargs; // Backend run-prefix: {"-r","parvionsftp"} for the multi-call self, else empty.
        stage_t               stage = s_idle;
        cmd_t                 await = c_none;
        text                  path = "/";
        text                  pending_path;
        bool                  path_pending = faux; // A cd staged `pending_path`; reveal it (commit to `path`)
                                                   // only when its listing arrives (c_ls), so the header never
                                                   // shows the destination over the previous dir's contents and
                                                   // a second double-click still resolves against the dir on screen.
        text                  last_reply;
        std::vector<direntry> items;
        std::vector<direntry> pending;
        text                  status = "Not connected.";
        text                  host, user, pass;
        si32                  port = 22;
        bool                  dirty = faux;

        // --- Passphrase / password interaction (FileZilla CInteractiveLoginNotification) ---------
        // The backend asks for an SSH key passphrase ("SSH key passphrase" preamble) or the account
        // password (ask_password). We answer from a session-scoped cache, or raise a UI modal via
        // on_prompt_secret (set by parvion.hpp). The cache lets reconnects + parallel transfer workers
        // authenticate without re-prompting (the control connection populates it on the first auth).
        struct secret_req_t { bool is_passphrase = true; text prompt; text keyfile; bool is_retry = faux; };
        std::map<text, text>  key_passphrases;      // keyfile path -> validated passphrase (in-memory only).
        std::set<text>        pass_asked;           // keyfiles asked during the current backend auth.
        bool                  account_asked = faux; // The account password was already offered this auth.
        text                  last_preamble, last_instruction; // Last request_preamble / request_instruction.
        enum sec_state_t { sec_idle, sec_pending, sec_shown } sec = sec_idle; // Single-shot modal latch.
        secret_req_t          sec_req;              // The outstanding prompt (valid when sec != sec_idle).
        std::function<void(secret_req_t const&)> on_prompt_secret; // Raise the UI modal (set by parvion.hpp).

        // Message log (FileZilla-style typed protocol log). The queue panel's
        // "Message log" tab renders the tail of `logbuf`; entries are color-coded
        // and prefixed by `type`. `status` (above) remains the short connect-bar
        // hint; every status line is also mirrored into the log. logtype/dbg are
        // namespace-scoped (above) so transfer workers feed the same log.
        struct logline { logtype type; text body; text stamp; si32 level = 0; }; // level: Trace sub-level (dbg).
        std::deque<logline>   logbuf;
        static constexpr auto log_cap = size_t{ 1000 }; // FileZilla MAX_LINECOUNT.
        si32                  debug_level   = 0;    // OPTION_LOGGING_DEBUGLEVEL (0=None .. 4=Debug).
        bool                  show_detailed = faux; // OPTION_LOGGING_SHOW_DETAILED_LOGS.
        bool                  show_stamps   = true; // OPTION_MESSAGELOG_TIMESTAMP.
        // Transfer-table column visibility (Local Name, Remote Name, Size, Progress, Speed, Reason),
        // toggled from the table-header right-click menu; all shown by default. Size == parvion q_ncol+1.
        std::array<bool, 6>   col_shown{ true, true, true, true, true, true };
        // Checksums-table column visibility (Source, Path, Algorithm, Size, Progress, Result), toggled
        // from that tab's header right-click menu; all shown by default. Size == parvion hash_headers.
        std::array<bool, 6>   hash_col_shown{ true, true, true, true, true, true };
        ui64                  gen = 0; // Bumped on each new listing (pane resets selection).
        std::vector<recent_server> recent;          // Quick Connect history (most-recent-first), persisted to disk.
        static constexpr auto recent_cap = size_t{ 16 };

        // Transfer queue (Phase 3/4).
        std::vector<queue_item>                   queue;
        std::vector<std::unique_ptr<xfer_worker>> workers; // Active item's chunk(s).
        si32  active = -1;            // Index in `queue` being transferred (-1 = none).
        bool  active_reused = faux;   // The active single-stream transfer is running on a pooled connection.
        // Idle connection pool (FileZilla CQueueView parity): finished single-stream workers stay
        // connected here so the next transfer skips the open+auth handshake. Each is dropped when it
        // has been idle longer than xfer_idle_sec (env PARVION_XFER_IDLE_SEC; 0 = never pool).
        std::vector<std::unique_ptr<xfer_worker>> idle_pool;
        si32  xfer_idle_sec = 60;    // FileZilla uses a 60s idle-disconnect timer for queue engines.
        bool  no_autostart = faux;   // Demo/test seam: hold the queue (never auto-start a backend).
        // Hash/checksum tasks (Calculate Checksum + auto-hash-on-transfer). A SEPARATE queue and
        // worker pool from transfers, so checksums run concurrently and never share a slot with a
        // transfer (requirement 5). hash_exe/hash_runargs point at the `parvionhash` backend (the
        // multi-call self); configure_backend (parvion.hpp) sets them.
        std::vector<hash_item>                    hash_queue;
        std::vector<std::unique_ptr<hash_worker>> hash_workers;
        text  hash_exe;                                  // parvionhash backend executable (the self binary).
        std::vector<text> hash_runargs;                  // {"-r","parvionhash"} for the multi-call self.
        si32  max_hash_jobs = 2;     // Concurrent hash workers (the rest stay `queued`).
        ui64  hash_id_seq = 0;       // Monotonic hash_item id allocator.
        bool  holding_followers = faux; // Parallel upload: leader started, followers parked.
        text  active_state_path;      // PARVIONC2 state file for the active parallel upload.
        ui32  active_md_size = 0;     // Cached metadata size (for per-chunk transferred offsets).
        bool  active_resume = faux;   // Active upload is resuming a prior attempt.
        text  local_dir = "/";        // Download target (the local pane's directory).
        // Recursive folder operations on the control session (FileZilla CRecursiveOperation parity).
        //   download : walk the remote subtree (`ls <path>`), mirror its directories locally and
        //              enqueue one download per file (the transfers run on their own connections).
        //   delete   : walk the subtree to collect every file and directory, then `rm` each file
        //              and `rmdir` each directory deepest-first (plain `rmdir` cannot recurse).
        //   upload   : a synchronous local tree-walk produces a parent-first `mkdir` list plus the
        //              per-file uploads; the mkdirs run first, then the uploads are enqueued.
        enum recop_t { rec_none, rec_download, rec_delete, rec_upload };
        struct rec_dir { text remote; text local; }; // local is unused for delete/upload.
        recop_t                recop = rec_none;
        std::vector<rec_dir>   rec_stack;       // Remote dirs still to list (download/delete), DFS order.
        rec_dir                rec_cur;         // The dir whose `ls` result is currently being processed.
        std::vector<text>      rec_delfiles;    // delete: absolute remote file paths to `rm`.
        std::vector<text>      rec_deldirs;     // delete: absolute remote dir paths (preorder; rmdir'd reversed).
        std::vector<text>      rec_cmds;        // FIFO of one-shot commands (mkdir/rm/rmdir) for the command phase.
        size_t                 rec_cmd_i = 0;   // Index of the next command in rec_cmds to run.
        bool                   rec_cmds_built = faux; // delete: rec_cmds assembled once the walk finished.
        std::vector<queue_item> rec_uploads;    // upload: per-file uploads to enqueue once the mkdirs complete.
        bool  remote_refresh_pending = faux; // A completed upload landed in the displayed remote dir; re-list when idle.
        ui64  local_gen = 0;          // Bumped when a download completes into the displayed local dir (pane re-lists).
        // Async local delete (delete_local_async): the detached worker touches only this
        // shared block of atomics, never the pane or this object's non-atomic fields, so it
        // can outlive both. poll() folds `done` ticks into local_gen on the event context.
        struct local_del_state
        {
            std::atomic<ui64> done{ 0 };    // remove_all calls completed (success or not).
            std::atomic<si32> fails{ 0 };   // remove_all calls that reported an error.
            std::atomic<si32> running{ 0 }; // In-flight delete batches.
        };
        std::shared_ptr<local_del_state> ldel = std::make_shared<local_del_state>();
        ui64  ldel_seen = 0;          // Last ldel->done seen by poll() (event context only).
        si32  dbg_local_del_delay_ms = 0; // Test seam (env PARVION_DEBUG_LOCAL_DEL_DELAY_MS): sleep this long before
                                          // each remove_all so a test can observe the UI staying live mid-delete.
        bool  use_parallel = true;    // Split large files across parallel connections.
        si64  parallel_threshold = 4ll << 20; // "Larger than" gate / per-chunk target (4 MiB).
        ui32  max_connections = 6;    // Cap on parallel connections (chunks) per transfer.
        ui32  parallel_connect_burst = 6; // Max fresh SSH handshakes started at once.

        // Persisted user settings (Edit -> Settings dialog): the SFTP subset of FileZilla's
        // Connection / Connection-SFTP option pages. load()ed in the constructor and applied onto
        // the live fields below (apply_settings); the dialog edits a copy and calls update_settings.
        parvion_settings cfg;
        size_t connect_keyfile_i = 0; // Cursor into cfg.keyfiles during the pre-auth c_keyfile phase.

        // Control-connection liveness (FileZilla parity). The control session handles
        // browsing/keepalive only; transfers run on their own connections, so the control
        // link sits idle during transfers and long pauses and the server may time it out
        // -- which strands navigation, downloads, and the post-upload remote refresh. We
        // keep it warm with a periodic cheap `pwd` (FileZilla CFtpControlSocket keep-alive)
        // and, if it drops anyway, transparently reconnect and restore the current dir
        // (mirrors the engine's reconnect-on-broken-connection).
        using steady_clock = std::chrono::steady_clock;
        steady_clock::time_point last_activity = steady_clock::now(); // Last control-session traffic (send or recv).
        steady_clock::time_point retry_at{};        // Earliest time for the next reconnect attempt.
        si32  keep_skip = 0;          // Outstanding keepalive replies to swallow (kept off the nav state machine).
        bool  recovering = faux;      // Restoring a previously-established control link that dropped.
        bool  restoring  = faux;      // After a reconnect's auth, cd back to resume_path instead of pwd.
        text  resume_path = "/";      // Remote dir to restore after a reconnect.
        si32  reconnect_tries = 0;    // Consecutive reconnect attempts since the link dropped.
        si32  keepalive_sec = 30;     // Idle seconds before a keepalive (env PARVION_KEEPALIVE_SEC; 0 = off). FileZilla uses 30s.
        si32  reconnect_delay_sec = 5;// Delay between reconnect attempts (FileZilla OPTION_RECONNECTDELAY default).
        si32  max_reconnect_tries = 10;// Cap on consecutive reconnect attempts (env PARVION_RECONNECT_TRIES; 0 = unlimited).
        si32  response_timeout_sec = 20;// Max seconds to wait for ANY control traffic while a command or keepalive is
                                      // outstanding before declaring the link dead (env PARVION_TIMEOUT_SEC). FileZilla
                                      // OPTION_TIMEOUT: default 20, min 10; 0 = off. The real exposure is the inter-frame
                                      // gap, not total op time, since last_activity resets on every inbound batch.
        si32  dbg_ls_delay_ms = 0;    // Test seam (env PARVION_DEBUG_LS_DELAY_MS): hold the post-cd `ls` this long to
                                      // widen the cd->ls window so navigation races are reproducible. 0 = off (prod).
        bool  ls_deferred = faux;     // A post-cd `ls` is currently being held by dbg_ls_delay_ms.
        steady_clock::time_point ls_due{}; // When the held `ls` becomes due.

        // Copy the persisted settings onto the live engine fields so they take effect.
        // Called from the constructor (before the env test-seams, which still win) and
        // whenever the Settings dialog commits a change (update_settings).
        void apply_settings()
        {
            response_timeout_sec = cfg.timeout;                       // OPTION_TIMEOUT (0 = off).
            max_reconnect_tries  = cfg.reconnect_count;               // OPTION_RECONNECTCOUNT (0 = unlimited).
            reconnect_delay_sec  = cfg.reconnect_delay;              // OPTION_RECONNECTDELAY.
            parallel_threshold   = std::max<si64>(1, cfg.threshold_bytes());
            max_connections      = (ui32)std::clamp(cfg.max_connections, 1, 16);
            // Compression (cfg.compression) and key files (cfg.keyfiles) are read straight
            // from cfg at backend-launch / auth time; no separate live copy is kept.
        }
        // Commit an edited settings copy: store, apply onto the engine, and persist to disk.
        void update_settings(parvion_settings const& s)
        {
            cfg = s;
            cfg.clamp();
            apply_settings();
            cfg.save();
            log_line(logtype::status, "Settings saved.");
        }

        sftp_remote()
        {
            cfg.load();        // Restore persisted Edit -> Settings values...
            apply_settings();  // ...and apply them before the env seams (which override for tests).
            // Escape hatches (also handy for A/B verification):
            //   PARVION_NO_PARALLEL=1    force single-stream transfers
            //   PARVION_MAX_CONN=<n>     cap on parallel connections (PARVION_CHUNKS is an alias)
            //   PARVION_THRESHOLD_MB=<n> files larger than this (and the per-chunk target) go parallel
            if (auto e = std::getenv("PARVION_NO_PARALLEL")) { if (*e && *e != '0') use_parallel = faux; }
            if (auto e = std::getenv("PARVION_MAX_CONN")) { if (auto n = std::atoi(e); n > 0) max_connections = (ui32)n; }
            if (auto e = std::getenv("PARVION_CHUNKS"))   { if (auto n = std::atoi(e); n > 0) max_connections = (ui32)n; }
            if (auto e = std::getenv("PARVION_CONNECT_BURST")) { if (auto n = std::atoi(e); n > 0) parallel_connect_burst = (ui32)n; }
            if (auto e = std::getenv("PARVION_THRESHOLD_MB")) { if (auto n = std::atoll(e); n > 0) parallel_threshold = (si64)n << 20; }
            //   PARVION_THRESHOLD_BYTES=<n> same gate in bytes (test seam: forces small files to chunk)
            if (auto e = std::getenv("PARVION_THRESHOLD_BYTES")) { if (auto n = std::atoll(e); n > 0) parallel_threshold = (si64)n; }
            if (max_connections > 16) max_connections = 16; // sane ceiling
            parallel_connect_burst = std::clamp(parallel_connect_burst, ui32{ 1 }, ui32{ 16 });
            //   PARVION_KEEPALIVE_SEC=<n> idle seconds before a control keepalive (0 disables)
            //   PARVION_RECONNECT_TRIES=<n> cap on auto-reconnect attempts after a drop (0 = unlimited)
            if (auto e = std::getenv("PARVION_KEEPALIVE_SEC"))   { if (auto n = std::atoi(e); n >= 0) keepalive_sec = n; }
            if (auto e = std::getenv("PARVION_RECONNECT_TRIES")) { if (auto n = std::atoi(e); n >= 0) max_reconnect_tries = n; }
            //   PARVION_TIMEOUT_SEC=<n> seconds to wait for a control reply before declaring the link dead (0 = off)
            if (auto e = std::getenv("PARVION_TIMEOUT_SEC")) { if (auto n = std::atoi(e); n >= 0) response_timeout_sec = n == 0 ? 0 : std::max(10, n); }
            //   PARVION_XFER_IDLE_SEC=<n> idle seconds before a pooled transfer connection is closed (0 = don't pool)
            if (auto e = std::getenv("PARVION_XFER_IDLE_SEC"))   { if (auto n = std::atoi(e); n >= 0) xfer_idle_sec = n; }
            //   PARVION_DEBUG_LS_DELAY_MS=<n> test seam: hold the post-cd directory listing to widen the cd->ls window
            if (auto e = std::getenv("PARVION_DEBUG_LS_DELAY_MS")) { if (auto n = std::atoi(e); n > 0) dbg_ls_delay_ms = n; }
            //   PARVION_DEBUG_LOCAL_DEL_DELAY_MS=<n> test seam: stall each local remove_all to keep a delete in flight
            if (auto e = std::getenv("PARVION_DEBUG_LOCAL_DEL_DELAY_MS")) { if (auto n = std::atoi(e); n > 0) dbg_local_del_delay_ms = n; }
            load_recent(); // Restore the persisted Quick Connect history.
        }

        // Number of parallel connections (chunks) for a file of `size` bytes.
        // Mirrors CQueueView::GetParallelSftpPartCount: single-stream at or below
        // the threshold, otherwise ceil(size/threshold) clamped to [2, max].
        auto part_count(si64 size) const -> ui32
        {
            if (!use_parallel || max_connections <= 1 || parallel_threshold <= 0 || size <= parallel_threshold) return 1;
            auto count = (ui64)((size + parallel_threshold - 1) / parallel_threshold);
            if (count < 2) count = 2;
            if (count > max_connections) count = max_connections;
            return (ui32)count;
        }

        // Connected AND the control link is actually up: callers (navigate, enqueue,
        // refresh) must not push commands into a backend that has already exited, or they
        // stall on a reply that never comes. A drop flips this false within one poll, and
        // poll() then reconnects.
        auto connected() const { return stage == s_connected && session.alive(); }

        // After a transfer completes, is the destination directory still the one shown on
        // the destination side? Upload -> remote `path`; download -> local `local_dir`.
        // dest_dir was captured at enqueue time from the same source, so this is byte-exact.
        auto dest_in_view(queue_item const& it) const -> bool
        {
            return it.download ? it.dest_dir == local_dir : it.dest_dir == path;
        }
        // Directory of the item's local file: the target dir for a download, the source dir
        // for an upload (mirrors setup_parallel_state's split; POSIX or Windows separators).
        static auto local_dir_of(queue_item const& it) -> text
        {
            auto ls = it.local_path.find_last_of("/\\");
            return ls == text::npos ? text{ "." } : (ls == 0 ? text{ "/" } : it.local_path.substr(0, ls));
        }
        // Signal a re-list for every displayed dir the stopped transfer touched: the destination
        // dir where the (possibly partial) target landed (upload -> remote, download -> local),
        // and an upload's local source dir, which gains its PARVIONC2 resume-state file on
        // pause/failure/removal (kept for a later resume) and loses it on success. A download
        // keeps both artifacts in its destination dir, so the first check covers it alone.
        void refresh_panes(queue_item const& it)
        {
            if (dest_in_view(it)) { if (it.download) ++local_gen; else remote_refresh_pending = true; }
            if (!it.download && local_dir_of(it) == local_dir) ++local_gen;
        }

        // Local "HH:MM:SS" timestamp for a log entry (FileZilla uses %H:%M:%S).
        static auto make_stamp() -> text
        {
            auto t = std::time(nullptr);
            auto bt = std::tm{};
            #if defined(_WIN32)
            ::localtime_s(&bt, &t);
            #else
            ::localtime_r(&t, &bt);
            #endif
            auto buf = std::array<char, 16>{};
            auto n = std::strftime(buf.data(), buf.size(), "%H:%M:%S", &bt);
            return text{ buf.data(), n };
        }
        // Append one typed line to the message log (drop-oldest past log_cap).
        void log_line(logtype t, text s, si32 level = 0)
        {
            logbuf.push_back({ t, std::move(s), make_stamp(), level });
            while (logbuf.size() > log_cap) logbuf.pop_front();
            dirty = true;
        }
        // Append a Trace line at debug sub-level `level` (dbg_warning .. dbg_debug).
        void trace(si32 level, text s) { log_line(logtype::trace, std::move(s), level); }
        // Short connect-bar hint; mirrored into the log as a Status line.
        void mark(text s) { status = s; log_line(logtype::status, std::move(s)); dirty = true; }
        // Error: short hint on the bar + an Error line in the log.
        void fail(text e) { status = "Error: " + e; log_line(logtype::error, std::move(e)); dirty = true; }

        // Session-scoped passphrase cache lookup (used by the control session and, via a provider,
        // by transfer workers). Returns true and sets `out` when a validated passphrase is known.
        bool lookup_passphrase(text const& keyfile, text& out) const
        {
            auto it = key_passphrases.find(keyfile);
            if (it == key_passphrases.end()) return faux;
            out = it->second;
            return true;
        }
        // The user answered the outstanding prompt: cache a passphrase (so reconnects/workers reuse
        // it) or adopt the account password, send it to the waiting backend, and clear the latch.
        void provide_secret(text v)
        {
            if (sec_req.is_passphrase) key_passphrases[sec_req.keyfile] = v;
            else                       pass = v; // Remember the working account password for this session.
            session.write_line(v);
            sec = sec_idle;
        }
        // The user dismissed the prompt: never send an empty line (the backend would just re-ask in a
        // loop) — tear the connection down. s_failed does not auto-reconnect (only an s_connected drop does).
        void cancel_secret()
        {
            session.stop();
            sec = sec_idle;
            fail("Authentication cancelled.");
            stage = s_failed;
        }
        // Issue a user-visible SFTP command on the control session, logging it as
        // a Command line first (mirrors FileZilla logging the command it sends).
        void send_cmd(view c) { last_activity = steady_clock::now(); log_line(logtype::command, text{ c }); session.write_line(c); }
        // Per-transfer outcome summary, mirroring CControlSocket::LogTransferResultMessage:
        // a Status "File transfer successful[, transferred X in Y]" or an Error
        // "File transfer failed[ after transferring X in Y]".
        void log_transfer_result(queue_item const& item, bool ok, si64 bytes)
        {
            auto secs = (si64)std::max<std::time_t>(1, std::time(nullptr) - item.started);
            auto when = std::to_string(secs) + (secs == 1 ? " second" : " seconds");
            if (ok)
            {
                if (bytes > 0) log_line(logtype::status, "File transfer successful, transferred " + human_size(bytes) + " in " + when);
                else           log_line(logtype::status, "File transfer successful");
            }
            else
            {
                if (bytes > 0) log_line(logtype::error, "File transfer failed after transferring " + human_size(bytes) + " in " + when);
                else           log_line(logtype::error, "File transfer failed");
            }
        }

        // --- Quick Connect history (most-recent-first, persisted to disk) --------------
        void load_recent()
        {
            recent.clear();
            auto f = std::fopen(parvion_recent_path().string().c_str(), "rb");
            if (!f) return;
            auto buf = text{};
            auto tmp = std::array<char, 4096>{};
            for (auto n = size_t{}; (n = std::fread(tmp.data(), 1, tmp.size(), f)) > 0; ) buf.append(tmp.data(), n);
            std::fclose(f);
            for (auto pos = size_t{}; pos < buf.size() && recent.size() < recent_cap; )
            {
                auto eol  = buf.find('\n', pos);
                auto line = buf.substr(pos, eol == text::npos ? text::npos : eol - pos);
                pos = eol == text::npos ? buf.size() : eol + 1;
                auto t1 = line.find('\t'); if (t1 == text::npos) continue;
                auto t2 = line.find('\t', t1 + 1); if (t2 == text::npos) continue;
                auto t3 = line.find('\t', t2 + 1); if (t3 == text::npos) continue;
                auto r = recent_server{};
                r.host = line.substr(0, t1);
                r.user = line.substr(t1 + 1, t2 - t1 - 1);
                r.port = std::atoi(line.substr(t2 + 1, t3 - t2 - 1).c_str());
                r.pass = line.substr(t3 + 1);
                if (r.port <= 0 || r.port > 65535) r.port = 22;
                if (!r.host.empty()) recent.push_back(std::move(r));
            }
        }
        void save_recent()
        {
            auto fpath = parvion_recent_path();
            auto f = std::fopen(fpath.string().c_str(), "wb");
            if (!f) return;
            for (auto& r : recent)
            {
                auto line = r.host + '\t' + r.user + '\t' + std::to_string(r.port) + '\t' + r.pass + '\n';
                std::fwrite(line.data(), 1, line.size(), f);
            }
            std::fclose(f);
            #if !defined(_WIN32)
            auto ec = std::error_code{};
            fs::permissions(fpath, fs::perms::owner_read | fs::perms::owner_write, ec); // 0600: the file stores saved passwords.
            #endif
        }
        void remember(text const& h, text const& u, text const& pw, si32 p)
        {
            if (h.empty()) return;
            auto r = recent_server{ h, u, pw, p };
            std::erase_if(recent, [&](recent_server const& e){ return e.same_target(r); });
            recent.insert(recent.begin(), std::move(r));
            if (recent.size() > recent_cap) recent.resize(recent_cap);
            save_recent();
        }
        void clear_recent() { recent.clear(); save_recent(); }

        void connect(text h, si32 p, text u, text pw)
        {
            session.stop();
            items.clear();
            pending.clear();
            host = std::move(h);
            port = p;
            user = u.empty() ? text{ "anonymous" } : std::move(u);
            pass = std::move(pw);
            path = "/";
            await = c_none;
            remote_refresh_pending = faux;
            reset_recop(); // Drop any half-finished folder walk from a previous session.
            idle_pool.clear(); // Drop pooled transfer connections to the previous server.
            recovering = faux; restoring = faux; reconnect_tries = 0; keep_skip = 0; // Fresh user-initiated connect, not a recovery.
            key_passphrases.clear(); pass_asked.clear(); account_asked = faux; // Fresh credentials: drop any cached passphrases.
            sec = sec_idle; last_preamble.clear(); last_instruction.clear();
            last_activity = steady_clock::now();
            if (host.empty()) { mark("Enter a host name."); stage = s_failed; return; }
            mark("Connecting to " + host + "...");
            trace(dbg_debug, "Target: " + user + "@" + host + ":" + std::to_string(port)); // Wire-level detail.
            trace(dbg_verbose, "Going to execute " + exe);                                  // FileZilla connect.cpp parity.
            // Surface the applied SFTP settings (Edit -> Settings) for this connection.
            if (!cfg.keyfiles.empty()) log_line(logtype::status, "Public-key authentication: " + std::to_string(cfg.keyfiles.size()) + " key file(s) configured.");
            if (cfg.compression)       log_line(logtype::status, "SFTP compression enabled.");
            trace(dbg_debug, "Settings: timeout=" + std::to_string(response_timeout_sec) + "s, retries=" + std::to_string(max_reconnect_tries)
                + ", delay=" + std::to_string(reconnect_delay_sec) + "s, parallel>=" + std::to_string(parallel_threshold) + "B x" + std::to_string(max_connections));
            remember(host, user, pass, port); // Record this target in the Quick Connect history.
            session.runargs = runargs;
            if (cfg.compression) session.runargs.push_back("-C"); // SFTP compression (Settings -> SFTP).
            if (!session.launch(exe)) { fail("Failed to launch parvionsftp: " + exe); stage = s_failed; return; }
            stage = s_greeting;
        }

        void disconnect()
        {
            session.stop();
            stage = s_idle;
            await = c_none;
            remote_refresh_pending = faux;
            reset_recop(); // Drop any half-finished folder walk.
            idle_pool.clear(); // Close pooled transfer connections.
            recovering = faux; restoring = faux; reconnect_tries = 0; keep_skip = 0; // User asked to disconnect: don't auto-reconnect.
            key_passphrases.clear(); pass_asked.clear(); account_asked = faux;
            sec = sec_idle; last_preamble.clear(); last_instruction.clear();
            items.clear();
            pending.clear();
            mark("Not connected.");
        }

        void poll()
        {
            if (stage != s_idle && stage != s_failed)
            {
                auto msgs = session.drain();
                if (!msgs.empty()) last_activity = steady_clock::now(); // Inbound traffic counts as activity.
                for (auto& m : msgs) process(m);
                // A passphrase/password prompt came due: raise the UI modal exactly once (the backend
                // re-emits the sequence on a wrong answer, which re-arms sec_pending for a fresh modal).
                if (sec == sec_pending && on_prompt_secret) { sec = sec_shown; on_prompt_secret(sec_req); }
                if (!session.alive() && (stage == s_greeting || stage == s_opening))
                {
                    trace(dbg_warning, "Backend exited during " + text{ stage == s_greeting ? "greeting" : "authentication" } + ".");
                    stage = s_failed;
                    // A failed reconnect attempt: keep recovering and pace the next try; a
                    // failed first connect: surface it (no auto-retry).
                    if (recovering) { retry_at = steady_clock::now() + std::chrono::seconds{ reconnect_delay_sec }; mark("Reconnect attempt failed; retrying..."); }
                    else mark("parvionsftp exited unexpectedly.");
                }
                else if (!session.alive() && stage == s_connected)
                {
                    begin_recover(); // An established control link dropped (idle timeout / network / server restart).
                }
            }
            drive_reconnect(); // Backoff-paced (re)connect attempts while recovering.
            maybe_watchdog();  // Fast death detection while waiting on a command/keepalive reply.
            maybe_keepalive(); // Keep an idle control link warm so the server doesn't time it out.
            pump_queue(); // Drive transfer workers (independent of the control session).
            reap_idle_workers(); // Close pooled transfer connections that have gone idle (or were dropped).
            pump_hash(); // Drive checksum workers (own queue/pool; runs even when disconnected for local files).
            // A completed upload landed in the displayed remote dir: re-list it. Only when
            // the control session is idle, so we don't clobber an in-flight cd/ls (checked
            // here, after pump_queue, so `await` reflects this tick's drained replies).
            if (remote_refresh_pending && recop == rec_none && connected() && await == c_none)
            {
                remote_refresh_pending = faux;
                list_dir();
            }
            // Test seam: a post-cd `ls` held by PARVION_DEBUG_LS_DELAY_MS is now due.
            if (ls_deferred && await == c_cd && steady_clock::now() >= ls_due) { ls_deferred = faux; list_dir(); }
            drive_recop(); // Pace a recursive folder download/upload/delete on the idle control session.
            // Fold async local-delete progress into local_gen (the timer re-lists the local pane
            // per tick, so rows vanish as items go). `running` is read before `done`: seeing 0
            // means every worker's last `done` increment is already visible, so `done` is final.
            auto del_running = ldel->running.load();
            if (auto g = ldel->done.load(); g != ldel_seen)
            {
                ldel_seen = g;
                ++local_gen;
                if (del_running == 0)
                {
                    auto f = ldel->fails.exchange(0);
                    if (f) fail(std::to_string(f) + " local item(s) could not be deleted.");
                    else   mark("Delete finished.");
                }
            }
        }

        // The established control link dropped: remember where we were and hand off to the
        // backoff-paced reconnect driver. The stale `items` stay on screen (pane shows the
        // "Reconnecting..." status) until the restored listing replaces them.
        void begin_recover()
        {
            if (recovering) return;
            recovering = true;
            reconnect_tries = 0;
            resume_path = path;                 // cd back here once re-authed
            await = c_none;
            ls_deferred = faux;                 // Drop any held (test-seam) listing.
            keep_skip = 0;
            remote_refresh_pending = faux;
            retry_at = steady_clock::now();     // first attempt immediately
            stage = s_idle;                     // leave s_connected; drive_reconnect() relaunches
            fail("Control connection lost. Reconnecting...");
        }
        // While recovering and not mid-attempt, relaunch on the backoff clock until the link
        // is back or we exhaust the attempt cap.
        void drive_reconnect()
        {
            if (!recovering) return;
            if (stage == s_greeting || stage == s_opening || stage == s_connected) return; // attempt in flight / already up
            if (steady_clock::now() < retry_at) return;
            if (max_reconnect_tries > 0 && reconnect_tries >= max_reconnect_tries)
            {
                recovering = faux;
                fail("Reconnect attempts exhausted. Press Connect to retry.");
                stage = s_failed;
                return;
            }
            // Pace from the attempt's start so a fast failure (error -> s_failed, or backend
            // death) can't spin a hot retry loop before this delay elapses.
            retry_at = steady_clock::now() + std::chrono::seconds{ reconnect_delay_sec };
            ++reconnect_tries;
            do_reconnect();
        }
        void do_reconnect()
        {
            session.stop();
            await = c_none;
            keep_skip = 0;
            restoring = true; // restore resume_path after auth (see complete()/c_open)
            mark("Reconnecting to " + host + " (attempt " + std::to_string(reconnect_tries) + ")...");
            session.runargs = runargs;
            if (cfg.compression) session.runargs.push_back("-C"); // SFTP compression (Settings -> SFTP).
            if (!session.launch(exe)) { stage = s_failed; retry_at = steady_clock::now() + std::chrono::seconds{ reconnect_delay_sec }; return; }
            pass_asked.clear(); account_asked = faux; last_preamble.clear(); last_instruction.clear(); // Fresh backend auth (keep cached passphrases for this server).
            stage = s_greeting;
            last_activity = steady_clock::now();
        }
        // Inactivity/response watchdog (FileZilla CControlSocket OPTION_TIMEOUT parity). While the
        // link is "waiting" -- a real command outstanding (await != c_none) or a keepalive pwd in
        // flight (keep_skip > 0) -- if no control traffic has arrived within response_timeout_sec the
        // SSH link is dead even though the backend helper hasn't noticed (silent half-open: no
        // RST/FIN). Force recovery rather than wait for the helper's slower internal kill. last_activity
        // is reset on every inbound drain (poll(), so each streaming `ls` chunk re-arms it) and every
        // outbound send, so a slow-but-alive listing never trips this. RST/FIN is still caught instantly
        // via session.alive() in poll(); this covers only the silent case.
        void maybe_watchdog()
        {
            if (response_timeout_sec <= 0) return;
            if (stage != s_connected || !session.alive()) return;
            if (recovering) return;                        // A recovery is already in flight; don't double-fire.
            if (await == c_none && keep_skip == 0) return; // Link genuinely idle: nothing outstanding to time out.
            if (steady_clock::now() - last_activity <= std::chrono::seconds{ response_timeout_sec }) return;
            trace(dbg_warning, "Control response timeout (" + std::to_string(response_timeout_sec) + "s); link presumed dead.");
            begin_recover();
        }
        // Keep the idle control link warm with a cheap `pwd` (mirrors CFtpControlSocket's
        // keep-alive). It runs only when the link is genuinely idle (no nav command, no
        // keepalive already outstanding); its reply is swallowed via keep_skip so it never
        // disturbs the navigation state machine. Fires during transfers too -- that's when
        // the control link is most likely to be idled out from under the post-upload refresh.
        void maybe_keepalive()
        {
            if (keepalive_sec <= 0) return;
            if (stage != s_connected || !session.alive()) return;
            auto now = steady_clock::now();
            if (keep_skip > 0) return;    // A keepalive is still outstanding; maybe_watchdog() owns its timeout now.
            if (await != c_none) return; // Control session busy with a real command.
            if (now - last_activity < std::chrono::seconds{ keepalive_sec }) return;
            ++keep_skip;
            last_activity = now;
            trace(dbg_verbose, "Sending keep-alive command (pwd)"); // FileZilla logs keep-alive; keep at Trace to avoid log spam.
            session.write_line("pwd");
        }

        void enqueue_download(text const& name, si64 size)
        {
            if (!connected()) return;
            auto it = queue_item{};
            it.download    = true;
            it.remote_path = child_path(path, name, faux);
            it.local_path  = child_path(local_dir, name, true);
            it.dest_dir    = local_dir; // Refresh the local pane on completion if it still shows this dir.
            it.size        = size;
            it.status      = queue_item::queued;
            queue.push_back(std::move(it));
            dirty = true;
        }
        void enqueue_upload(text const& local_path_, text const& name, si64 size)
        {
            if (!connected()) return;
            auto it = queue_item{};
            it.download    = false;
            it.local_path  = local_path_;
            it.remote_path = child_path(path, name, faux);
            it.dest_dir    = path; // Refresh the remote pane on completion if it still shows this dir.
            it.size        = size;
            it.status      = queue_item::queued;
            queue.push_back(std::move(it));
            dirty = true;
        }
        // Enqueue a transfer between explicit absolute endpoints (used by the recursive folder walk,
        // where the file sits in a sub-directory rather than directly in the displayed pane). dest_dir
        // stays the displayed root so the destination pane refreshes once the subtree finishes.
        void enqueue_download_path(text const& remote_full, text const& local_full, si64 size)
        {
            auto it = queue_item{};
            it.download    = true;
            it.remote_path = remote_full;
            it.local_path  = local_full;
            it.dest_dir    = local_dir;
            it.size        = size < 0 ? 0 : size;
            it.status      = queue_item::queued;
            queue.push_back(std::move(it));
            dirty = true;
        }

        // --- recursive folder operations (entry points; the work is paced by drive_recop) -----------
        // Download a remote directory `name` (under the current path) into local_dir/name, recursing
        // into sub-directories. Additive: selecting several folders extends the same walk.
        void download_folder(text const& name)
        {
            if (!connected() || name.empty() || recop == rec_delete || recop == rec_upload) return;
            auto r = child_path(path, name, faux);
            auto l = child_path(local_dir, name, true);
            auto ec = std::error_code{}; fs::create_directories(fs::path{ l }, ec);
            recop = rec_download;
            rec_stack.push_back({ r, l });
            mark("Downloading directory " + name + "...");
        }
        // Recursively remove a remote directory `name` (plain rmdir cannot recurse): walk it to collect
        // its files and sub-dirs, then rm/rmdir them deepest-first (driven later by drive_recop).
        void delete_folder(text const& name)
        {
            if (!connected() || name.empty() || recop == rec_download || recop == rec_upload) return;
            auto r = child_path(path, name, faux);
            recop = rec_delete;
            rec_deldirs.push_back(r);        // the folder itself, rmdir'd last.
            rec_stack.push_back({ r, {} });
            mark("Deleting directory " + name + "...");
        }
        // Remove a single remote file as part of a (possibly mixed) delete selection: routed through
        // the same recop engine so it can't race a concurrent folder walk on the control session.
        void delete_remote_file(text const& name)
        {
            if (!connected() || name.empty() || recop == rec_download || recop == rec_upload) return;
            recop = rec_delete;
            rec_delfiles.push_back(child_path(path, name, faux));
        }
        // Remove local items (absolute paths, recursing into folders) on a detached worker so a
        // big subtree can't freeze the UI. The worker owns the shared atomics block only; poll()
        // folds its `done` ticks into local_gen, which re-lists the local pane as items vanish.
        void delete_local_async(std::vector<text> paths)
        {
            if (paths.empty()) return;
            mark("Deleting " + std::to_string(paths.size()) + " local item(s)...");
            ldel->running.fetch_add(1);
            std::thread{ [st = ldel, paths = std::move(paths), delay = dbg_local_del_delay_ms]
            {
                for (auto& p : paths)
                {
                    if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds{ delay });
                    auto ec = std::error_code{};
                    fs::remove_all(fs::path{ p }, ec);
                    if (ec) st->fails.fetch_add(1);
                    st->done.fetch_add(1);
                }
                st->running.fetch_sub(1);
            }}.detach();
        }
        // Upload a local directory `local_full` into the current remote path as `name`, recursing into
        // sub-directories. The remote directory tree is created first (mkdir, parent-first); the per-file
        // uploads are enqueued only once those mkdirs have run, so no upload outruns its parent dir.
        void upload_folder(text const& local_full, text const& name)
        {
            if (!connected() || name.empty() || recop == rec_download || recop == rec_delete) return;
            recop = rec_upload;
            auto rroot = child_path(path, name, faux);
            rec_cmds.push_back("mkdir " + quote_name(rroot));
            walk_local_for_upload(local_full, rroot);
            mark("Uploading directory " + name + "...");
        }
        void clear_finished()
        {
            std::erase_if(queue, [](queue_item const& q){ return q.status == queue_item::succeeded || q.status == queue_item::failed; });
            if (active >= (si32)queue.size()) active = -1;
            dirty = true;
        }
        // Detach the active transfer without changing the item's status, terminating its
        // connections outright (FileZilla parity: StopItem -> CSftpControlSocket::Cancel ->
        // DoClose -> process_->kill(); a paused/cancelled/removed in-flight transfer never
        // keeps its connection — only successfully completed transfers pool theirs). The
        // on-disk parallel state file is left in place so a later restart can resume from it.
        void stop_active()
        {
            for (auto& w : workers) if (w) w->stop();
            workers.clear();
            active = -1;
            holding_followers = faux;
            active_state_path.clear(); active_md_size = 0; active_resume = faux;
        }
        // Queue context-menu actions (driven from parvion/queue.hpp). Each acts on the items matching
        // `pred` — the panel's selection for the per-item menu, or the active tab for the "All" menu.
        // Pause: stop+hold matched items; a live transfer reverts to a paused-queued state.
        template<class P> void queue_pause(P pred)
        {
            auto changed = faux;
            for (auto i = si32{}; i < (si32)queue.size(); ++i)
            {
                auto& it = queue[i];
                if (!pred(it)) continue;
                if (it.status == queue_item::transferring)
                {
                    if (active == i) { stop_active(); refresh_panes(it); } // Pause terminates the in-flight connections (FileZilla parity); resume reconnects.
                    it.status = queue_item::queued; it.paused = true; it.rate.speed = 0.0;
                    changed = true;
                }
                else if (it.status == queue_item::queued && !it.paused) { it.paused = true; changed = true; }
            }
            if (changed) dirty = true;
        }
        // Start: release a paused-queued item, or re-queue a finished one to transfer again.
        template<class P> void queue_start(P pred)
        {
            auto changed = faux;
            for (auto& it : queue)
            {
                if (!pred(it)) continue;
                if (it.status == queue_item::queued && it.paused) { it.paused = faux; changed = true; }
                else if (it.status == queue_item::failed || it.status == queue_item::succeeded)
                {   // Re-queue to transfer again (Failed retries; Succeeded re-transfers).
                    it.status = queue_item::queued; it.paused = faux; it.done = 0; it.rate.speed = 0.0; it.error.clear();
                    changed = true;
                }
            }
            if (changed) dirty = true;
        }
        // Remove: drop matched items. If the active transfer is among them, its connections are
        // terminated (FileZilla parity, same as Pause); `active` is recomputed by identity so it
        // keeps pointing at the same (surviving) transfer across the erase.
        template<class P> void queue_remove(P pred)
        {
            if (active >= 0 && active < (si32)queue.size() && pred(queue[active])) { refresh_panes(queue[active]); stop_active(); }
            auto act = (active >= 0 && active < (si32)queue.size()) ? &queue[active] : nullptr;
            auto out = std::vector<queue_item>{};
            out.reserve(queue.size());
            auto new_active = si32{ -1 };
            for (auto& it : queue)
            {
                if (pred(it)) continue;
                if (&it == act) new_active = (si32)out.size();
                out.push_back(std::move(it));
            }
            queue = std::move(out);
            active = new_active;
            dirty = true;
        }
        // Pin to Top: move matched *pending* (queued) items to the front of the pending group,
        // preserving their relative order. A newer pin lands ahead of an earlier one, so the newest
        // pin is processed first. Ongoing/finished items are untouched. Delegates to the generic,
        // side-effect-safe reorder (parvion/reorder.hpp): pinning a selection that matches no pending
        // item is a no-op that leaves the queue (and its contents) intact.
        template<class P> void queue_pin_top(P pred)
        {
            auto changed = faux;
            active = reorder_pin_to_front(queue, active,
                [&](queue_item const& it){ return pred(it); },
                [](queue_item const& it){ return it.status == queue_item::queued; },
                changed);
            if (changed) dirty = true;
        }

        // Browsing and one-shot remote ops are blocked while a recursive folder operation owns the
        // control session (FileZilla disables navigation during a recursive operation), so a stray
        // cd/mkdir can't interleave with the walk's `ls` commands.
        void chdir(text const& name)
        {
            if (!connected() || recop != rec_none || await != c_none) return; // Ignore a second nav while one is in flight.
            pending_path = child_path(path, name, faux);
            path_pending = true;
            await = c_cd;
            send_cmd("cd " + quote_name(pending_path));
            mark("Entering " + pending_path + "...");
        }
        void cdup()
        {
            if (!connected() || recop != rec_none || await != c_none) return; // Ignore a second nav while one is in flight.
            pending_path = parent_path(path, faux);
            path_pending = true;
            await = c_cd;
            send_cmd("cd " + quote_name(pending_path));
        }
        // Address-bar navigation: cd to an explicit path (absolute, or resolved against the
        // current dir when relative). A failed cd keeps the old path (handled as any browse).
        void chdir_abs(text const& newpath)
        {
            if (!connected() || recop != rec_none || await != c_none || newpath.empty()) return;
            // Resolve against the current dir when relative, then collapse "."/".." segments so the
            // committed path (and the title) is the real target, not e.g. "/home/user/..".
            auto full = newpath.front() == '/' ? newpath : child_path(path, newpath, faux);
            pending_path = normalize_posix(full);
            path_pending = true;
            await = c_cd;
            send_cmd("cd " + quote_name(pending_path));
            mark("Entering " + pending_path + "...");
        }
        // Remote file operations (psftp/parvionsftp verbs): each fires the command and then re-lists the
        // current directory once the backend reports done (handled as c_op in command_done).
        void remote_mkdir(text const& name)
        {
            if (!connected() || recop != rec_none || await != c_none || name.empty()) return;
            await = c_op;
            send_cmd("mkdir " + quote_name(child_path(path, name, faux)));
            mark("Creating directory " + name + "...");
        }
        void remote_remove(text const& name, bool is_dir)
        {
            if (!connected() || recop != rec_none || await != c_none || name.empty()) return;
            await = c_op;
            send_cmd((is_dir ? text{ "rmdir " } : text{ "rm " }) + quote_name(child_path(path, name, faux)));
            mark("Deleting " + name + "...");
        }
        void remote_rename(text const& oldname, text const& newname)
        {
            if (!connected() || recop != rec_none || await != c_none || oldname.empty() || newname.empty()) return;
            await = c_op;
            send_cmd("mv " + quote_name(child_path(path, oldname, faux)) + " " + quote_name(child_path(path, newname, faux)));
            mark("Renaming " + oldname + " to " + newname + "...");
        }
        // Re-list the current remote directory when the control link is next idle (poll() drains it).
        void request_refresh() { remote_refresh_pending = true; }

    private:
        // Synchronous local tree-walk for an upload: append a parent-first `mkdir` for each local
        // sub-directory and stage a queue_item for each file, both rooted at the remote `rroot`.
        // Directories are recursed in sorted order so the mkdir list is always parent-before-child.
        void walk_local_for_upload(text const& lroot, text const& rroot)
        {
            auto subdirs = std::vector<std::pair<text, text>>{}; // (local, remote) sub-dirs to recurse, sorted.
            for (auto& e : read_local_dir(fs::path{ lroot }))
            {
                if (e.is_link) continue; // Don't follow symlinks (FileZilla treats them as leaves).
                auto lchild = child_path(lroot, e.name, true);
                auto rchild = child_path(rroot, e.name, faux);
                if (e.is_dir)
                {
                    rec_cmds.push_back("mkdir " + quote_name(rchild));
                    subdirs.emplace_back(lchild, rchild);
                }
                else
                {
                    auto it = queue_item{};
                    it.download    = false;
                    it.local_path  = lchild;
                    it.remote_path = rchild;
                    it.dest_dir    = path;
                    it.size        = e.size < 0 ? 0 : e.size;
                    it.status      = queue_item::queued;
                    rec_uploads.push_back(std::move(it));
                }
            }
            for (auto& [l, r] : subdirs) walk_local_for_upload(l, r); // Recurse after this level's mkdirs.
        }
        // Pace a recursive folder operation on the (idle) control session: one `ls` per pending dir
        // during the walk, then one queued mkdir/rm/rmdir per tick during the command phase. Called
        // from poll() once the session is connected and no other command is in flight.
        void drive_recop()
        {
            if (recop == rec_none || !connected() || await != c_none) return;
            if (!rec_stack.empty()) // Walk phase: list the next remote dir.
            {
                rec_cur = rec_stack.back();
                rec_stack.pop_back();
                pending.clear();
                await = c_rls;
                send_cmd("ls " + quote_name(rec_cur.remote));
                return;
            }
            if (recop == rec_delete && !rec_cmds_built) // Walk done: assemble the removal commands.
            {
                for (auto& f : rec_delfiles) rec_cmds.push_back("rm " + quote_name(f));
                for (auto i = rec_deldirs.size(); i-- > 0;) rec_cmds.push_back("rmdir " + quote_name(rec_deldirs[i]));
                rec_cmds_built = true;
            }
            if (rec_cmd_i < rec_cmds.size()) // Command phase: mkdir (upload) / rm+rmdir (delete).
            {
                await = c_recop;
                send_cmd(rec_cmds[rec_cmd_i++]);
                return;
            }
            finish_recop();
        }
        // Discard all recursive-operation state (on connect/disconnect), abandoning any walk in flight.
        void reset_recop()
        {
            recop = rec_none;
            rec_stack.clear(); rec_cmds.clear(); rec_cmd_i = 0; rec_cmds_built = faux;
            rec_delfiles.clear(); rec_deldirs.clear(); rec_uploads.clear();
        }
        void finish_recop()
        {
            auto was = recop;
            recop = rec_none;
            rec_stack.clear(); rec_cmds.clear(); rec_cmd_i = 0; rec_cmds_built = faux;
            rec_delfiles.clear(); rec_deldirs.clear();
            if (was == rec_upload)
            {
                for (auto& it : rec_uploads) queue.push_back(std::move(it)); // Dirs exist now: safe to upload.
                rec_uploads.clear();
                remote_refresh_pending = true; // Show the freshly-created remote tree.
            }
            else if (was == rec_delete)
            {
                list_dir(); // The subtree is gone: refresh the remote pane.
            }
            // download: per-file downloads were enqueued during the walk and run on their own
            // connections; each completion bumps local_gen, which re-lists the local pane.
            dirty = true;
        }

    private:
        // --- transfer-connection pool (single-stream): reuse authenticated backends across files -----
        // Take a still-connected worker from the idle pool (dropping any the server has since closed),
        // or null when none is usable. Newest-first (LIFO) keeps the most-recently-validated one.
        auto acquire_worker() -> std::unique_ptr<xfer_worker>
        {
            while (!idle_pool.empty())
            {
                auto w = std::move(idle_pool.back());
                idle_pool.pop_back();
                if (w && w->reusable()) return w; // Still at the prompt, connection alive.
                // else: dead / server-dropped -> w destructs here, closing the process.
            }
            return nullptr;
        }
        // Return a finished single-stream worker to the idle pool (kept connected) for reuse, or let it
        // drop (closing its process) when pooling is disabled, the worker is unusable, or the pool is full.
        void recycle_worker(std::unique_ptr<xfer_worker> w)
        {
            if (xfer_idle_sec > 0 && w && w->reusable() && (si32)idle_pool.size() < (si32)max_connections)
            {
                w->idle_since = steady_clock::now();
                idle_pool.push_back(std::move(w));
            }
            // else: w destructs here.
        }
        // Close pooled connections that have been idle past the timeout or that the server has dropped
        // (FileZilla's 60s idle-disconnect timer). Called from poll() so quiet queues release backends.
        // Each closure is reported in the message log so released connections are visible to the user.
        void reap_idle_workers()
        {
            if (xfer_idle_sec <= 0) { idle_pool.clear(); return; }
            auto now = steady_clock::now();
            std::erase_if(idle_pool, [&](auto const& w)
            {
                if (!w) return true;
                if (!w->session.alive())
                {
                    log_line(logtype::status, "Pooled transfer connection was closed by the server.");
                    return true;
                }
                if (now - w->idle_since > std::chrono::seconds{ xfer_idle_sec })
                {
                    log_line(logtype::status, "Disconnecting transfer connection after " + std::to_string(xfer_idle_sec) + "s idle.");
                    return true;
                }
                return faux;
            });
        }
        void cfg_worker(xfer_worker& w, queue_item const& item, bool par, si64 off, si64 len, bool init = faux)
        {
            w.exe = exe; w.host = host; w.user = user; w.pass = pass; w.port = port;
            w.runargs = runargs;
            if (cfg.compression) w.runargs.push_back("-C"); // SFTP compression for the transfer connection.
            w.keyfiles = cfg.keyfiles;                      // Public-key auth on the transfer connection.
            w.download = item.download;
            w.remote_path = item.remote_path;
            w.local_path = item.local_path;
            w.parallel = par; w.offset = off; w.length = len; w.initialize = init;
            // Funnel this transfer connection's commands/replies/errors into the
            // shared message log, exactly as FileZilla routes every control socket
            // through the one StatusView.
            w.logsink = [this](logtype t, text s, si32 level){ log_line(t, std::move(s), level); };
            // Resolve a key passphrase from the controller's session cache (workers never prompt; the
            // control connection authenticates first and fills it). Mirrors logsink's controller hand-off.
            w.passphrase_provider = [this](text const& keyfile, text& out){ return lookup_passphrase(keyfile, out); };
        }
        void start_item(si32 i)
        {
            auto& item = queue[i];
            item.status = queue_item::transferring;
            item.done = 0;
            item.started = std::time(nullptr);
            active = i;
            workers.clear();
            holding_followers = faux;
            active_state_path.clear(); active_md_size = 0; active_resume = faux;
            active_reused = faux;
            auto chunks = part_count(item.size); // size-scaled, clamped to max_connections
            item.chunk_count = chunks;
            if (chunks <= 1)
            {
                // Reuse a pooled, already-authenticated connection if one is available (skips the
                // open+auth handshake); otherwise spawn and connect a fresh one.
                if (auto w = acquire_worker())
                {
                    cfg_worker(*w, item, faux, 0, -1);
                    w->rearm();
                    active_reused = true;
                    workers.push_back(std::move(w));
                }
                else
                {
                    auto fresh = std::make_unique<xfer_worker>();
                    cfg_worker(*fresh, item, faux, 0, -1);
                    fresh->begin();
                    workers.push_back(std::move(fresh));
                }
            }
            else
            {
                auto csz = (si64)((item.size + chunks - 1) / chunks);
                auto parts = std::vector<state_part>{};
                for (auto c = ui32{}; c < chunks; ++c)
                {
                    auto start = (si64)c * csz;
                    auto len = std::min(csz, item.size - start);
                    if (len <= 0) break;
                    parts.push_back({ (ui64)start, (ui64)len, 0 });
                }
                item.chunk_count = (ui32)parts.size();
                // Both directions use a resumable PARVIONC2 state file. Fresh: every
                // chunk starts at its offset (upload chunk 0 truncates+creates the
                // remote; download removes+recreates the local target). Resume: each
                // chunk continues from its recorded `transferred` offset.
                setup_parallel_state(item, parts); // applies resumed offsets
                for (auto c = size_t{}; c < parts.size(); ++c)
                {
                    auto S = (si64)parts[c].start, Z = (si64)parts[c].size, T = (si64)parts[c].transferred;
                    if (T >= Z) // already fully transferred in a prior attempt (resume): no connection needed.
                    {
                        auto w = std::make_unique<xfer_worker>();
                        w->chunk_index = (si32)c; w->state = xfer_worker::s_ok; w->done = Z; w->persisted = Z;
                        workers.push_back(std::move(w));
                        continue;
                    }
                    // Starting many fresh SSH handshakes at the exact same tick can trip OpenSSH
                    // MaxStartups/per-source throttling on localhost before any bytes move. Start a
                    // bounded burst, then pump_queue releases parked chunks as those handshakes finish.
                    // Fresh upload followers still additionally wait for chunk 0 to truncate/open the
                    // remote target before they can write absolute offsets.
                    auto burst = (size_t)std::min(max_connections, parallel_connect_burst);
                    auto start_now = (item.download || active_resume) ? c < burst : c == 0;
                    // Reuse a pooled connection for any chunk (leader or follower). A parked follower
                    // taken from the pool is forced to s_init so it isn't counted as finished; pump_queue
                    // tells a pooled parked worker (still connected) from a fresh one and rearms vs begins.
                    auto w = acquire_worker();
                    auto reused = (bool)w;
                    if (reused) active_reused = true;
                    if (!w) w = std::make_unique<xfer_worker>();
                    // Upload leader (chunk 0) truncates only on a fresh transfer.
                    cfg_worker(*w, item, true, S + T, Z - T, !item.download && !active_resume && c == 0);
                    w->chunk_index = (si32)c;
                    if (start_now) { if (reused) w->rearm(); else w->begin(); }
                    else { holding_followers = true; if (reused) w->state = xfer_worker::s_init; }
                    // Seed the resumed byte count AFTER begin()/rearm() (which zero done),
                    // so progress + state persistence account for already-sent bytes.
                    w->done = T; w->persisted = T;
                    workers.push_back(std::move(w));
                }
            }
            // Baseline the rate meter on the bytes already on disk (resumed parallel chunks): this
            // run's progress and speed are measured from here, so the first sample doesn't spike on
            // the carried-over amount and the average reflects only what this run actually moved.
            auto base = si64{};
            for (auto& w : workers) base += w->done;
            item.done = base;
            item.rate.start(base, std::chrono::steady_clock::now());
            dirty = true;
        }
        // Compute the PARVIONC2 state path/metadata for a parallel transfer and,
        // if a matching state file exists, adopt its per-chunk `transferred`
        // values (resume); otherwise write a fresh state file. A fresh download
        // also removes the (possibly stale) local target so chunks recreate it.
        void setup_parallel_state(queue_item const& item, std::vector<state_part>& parts)
        {
            active_state_path.clear(); active_md_size = 0; active_resume = faux;
            auto dl = item.download;
            auto ls = item.local_path.find_last_of("/\\"); // local separator (POSIX or Windows)
            auto ldir  = ls == text::npos ? text{ "." } : (ls == 0 ? text{ "/" } : item.local_path.substr(0, ls));
            auto lfile = ls == text::npos ? item.local_path : item.local_path.substr(ls + 1);
            auto rs = item.remote_path.find_last_of('/');
            auto rdir  = rs == text::npos ? text{ "/" } : (rs == 0 ? text{ "/" } : item.remote_path.substr(0, rs));
            auto rfile = rs == text::npos ? item.remote_path : item.remote_path.substr(rs + 1);
            auto server   = fmt_server(host, port, user);
            auto rsafe    = safe_remote_path(rdir);
            auto metadata = build_metadata(dl, server, item.local_path, rsafe, rfile);
            auto key      = state_key(dl, server, ldir, lfile, rsafe, rfile);
            auto spath    = state_path(ldir, lfile, dl, key);
            auto e_total = ui64{}; auto e_status = ui32{}; auto e_md = text{}; auto e_parts = std::vector<state_part>{};
            if (read_state_file(spath, e_total, e_status, e_md, e_parts)
                && e_status != state_aborted && e_total == (ui64)item.size
                && e_parts.size() == parts.size() && e_md == metadata)
            {
                auto ok = true;
                for (auto j = size_t{}; j < parts.size(); ++j)
                    if (e_parts[j].start != parts[j].start || e_parts[j].size != parts[j].size
                        || e_parts[j].transferred > parts[j].size) { ok = faux; break; }
                if (ok) { parts = std::move(e_parts); active_resume = true; }
            }
            if (!active_resume)
            {
                // Download is ready immediately (no remote-open wait); upload waits
                // until its leader truncates+opens the remote target.
                write_state_file(spath, (ui64)item.size, parts, dl ? state_ready : state_waiting, metadata);
                if (dl)
                {
                    // Size the existing local target to the download length instead of unlinking it.
                    // setup_parallel_state runs on the UI thread, and unlinking a multi-GB file (freeing
                    // all its blocks) can stall the UI for ~1s+ — the cause of the lag at the start of a
                    // download that overwrites an existing large file (and, back-to-back, at the "end" of
                    // the previous one). The chunks overwrite [0, size) in place (their opens are
                    // no-truncate), so a resize to the final length is correct and instant for a same-size
                    // re-download (only a size delta is ever touched). Fall back to remove if resize fails.
                    auto ec = std::error_code{};
                    if (fs::exists(item.local_path, ec))
                    {
                        fs::resize_file(item.local_path, (uintmax_t)std::max<si64>(0, item.size), ec);
                        if (ec) std::remove(item.local_path.c_str());
                    }
                }
            }
            active_state_path = spath;
            active_md_size = (ui32)metadata.size();
        }
    public: // Checksum entry points called from the panes/queue UI.
        auto find_hash(ui64 id) -> hash_item*
        {
            for (auto& it : hash_queue) if (it.id == id) return &it;
            return nullptr;
        }
        // Enqueue a checksum task (own queue; runs concurrently with transfers). `remote` selects a
        // stream-while-downloading hash of a remote file (no local copy) vs a local-file hash; both
        // run in the independent `parvionhash` backend. Credentials are snapshotted for remote tasks.
        void enqueue_hash(text filepath, text name, bool remote, si32 algo, si64 size)
        {
            auto it = hash_item{};
            it.id     = ++hash_id_seq;
            it.remote = remote;
            it.path   = std::move(filepath);
            it.name   = std::move(name);
            it.algo   = std::clamp(algo, 0, hash_algo_count - 1);
            it.size   = size;
            it.status = hash_item::queued;
            if (remote) { it.host = host; it.user = user; it.pass = pass; it.port = port; it.keyfiles = cfg.keyfiles; }
            log_line(logtype::status, "Queued " + text{ hash_algo_label(it.algo) } + " checksum of " + it.name + ".");
            hash_queue.push_back(std::move(it));
            dirty = true;
        }
        // Checksums-tab actions. Removing an item whose worker is still running is safe: the next
        // pump_hash finds no matching item for that worker and stops+drops it.
        void hash_remove(ui64 id) { std::erase_if(hash_queue, [&](auto& it){ return it.id == id; }); dirty = true; }
        void hash_remove_selected() { std::erase_if(hash_queue, [](auto& it){ return it.selected; }); dirty = true; }
        auto hash_selected_count() const { auto n = si32{}; for (auto& it : hash_queue) if (it.selected) ++n; return n; }
        void hash_clear_finished()
        {
            std::erase_if(hash_queue, [](auto& it){ return it.status == hash_item::succeeded || it.status == hash_item::failed; });
            dirty = true;
        }
    private:
        // Drive the checksum workers: fold each active worker's progress/result into its item, reap
        // finished workers, then start queued items up to max_hash_jobs. Independent of pump_queue.
        void pump_hash()
        {
            for (auto& w : hash_workers)
            {
                w->poll();
                auto it = find_hash(w->item_id);
                if (!it) continue;
                if (w->total > 0 && it->size < 0) it->size = w->total;
                if (it->status == hash_item::hashing)
                {
                    if (w->done > it->done) { it->done = w->done; it->rate.sample(it->done, std::chrono::steady_clock::now()); dirty = true; }
                    if (w->state == hash_worker::s_ok)
                    {
                        it->status = hash_item::succeeded;
                        it->digest = w->digest;
                        if (it->size > 0) it->done = it->size;
                        it->rate.speed = 0.0;
                        log_line(logtype::status, text{ hash_algo_label(it->algo) } + " " + it->name + " = " + it->digest);
                        dirty = true;
                    }
                    else if (w->state == hash_worker::s_err)
                    {
                        it->status = hash_item::failed;
                        it->error  = w->error.empty() ? text{ "Checksum failed" } : w->error;
                        it->rate.speed = 0.0;
                        log_line(logtype::error, "Checksum of " + it->name + " failed: " + it->error);
                        dirty = true;
                    }
                }
            }
            // Drop workers whose item finished (or was removed from the queue).
            std::erase_if(hash_workers, [&](auto& w)
            {
                auto it = find_hash(w->item_id);
                if (!it || it->status == hash_item::succeeded || it->status == hash_item::failed) { w->stop(); return true; }
                return faux;
            });
            // Start queued items up to the concurrency cap (held back entirely in demo/test mode).
            if (no_autostart) return;
            auto running = (si32)hash_workers.size();
            for (auto& it : hash_queue)
            {
                if (running >= max_hash_jobs) break;
                if (it.status != hash_item::queued) continue;
                auto w = std::make_unique<hash_worker>();
                w->item_id = it.id;
                w->exe = hash_exe; w->runargs = hash_runargs;
                w->remote = it.remote; w->algo = it.algo; w->path = it.path;
                w->host = it.host; w->user = it.user; w->pass = it.pass; w->port = it.port; w->keyfiles = it.keyfiles;
                it.status  = hash_item::hashing;
                it.started = std::time(nullptr);
                it.rate.start(0, std::chrono::steady_clock::now());
                w->begin();
                hash_workers.push_back(std::move(w));
                ++running;
                dirty = true;
            }
        }
        // Auto-verify a completed transfer's destination (Settings -> Hash verification):
        //   download -> hash the saved LOCAL file; upload -> stream the REMOTE target back and hash it
        //   (verify-after-upload). Both run as ordinary hash tasks in the Checksums tab.
        void enqueue_transfer_hash(queue_item const& item)
        {
            auto basename = [](text const& p) -> text { auto pos = p.find_last_of("/\\"); return pos == text::npos ? p : p.substr(pos + 1); };
            if (item.download) enqueue_hash(item.local_path,  basename(item.local_path),  faux, cfg.hash_algo, item.size);
            else               enqueue_hash(item.remote_path, basename(item.remote_path), true, cfg.hash_algo, item.size);
        }
        void pump_queue()
        {
            if (active >= 0 && active < (si32)queue.size())
            {
                auto& item = queue[active];
                auto total = si64{};
                auto all_ok = true;
                auto any_err = faux;
                // Full byte budget of a worker's chunk, mirroring start_item()/chunk_ranges() csz math.
                // A finished-OK chunk has moved all of its bytes; the download helper's progress deltas
                // under-count (see vtm-tile.cpp), so credit it the full chunk size rather than its
                // under-counted done. This keeps the running total advancing per completed chunk (and
                // reaching item.size as the last one finishes) without touching w->done, which resume +
                // state persistence rely on.
                auto chunk_full = [&](xfer_worker const& w) -> si64
                {
                    if (item.chunk_count <= 1) return item.size; // single stream = whole file
                    auto csz = (item.size + item.chunk_count - 1) / item.chunk_count;
                    return std::min(csz, item.size - (si64)w.chunk_index * csz);
                };
                for (auto& w : workers)
                {
                    w->poll();
                    total += (w->state == xfer_worker::s_ok) ? chunk_full(*w) : w->done;
                    if (!w->finished()) all_ok = faux;
                    if (w->state == xfer_worker::s_err) { any_err = true; if (item.error.empty()) item.error = w->error; }
                }
                // Release parked parallel workers. For a fresh upload, followers must wait until the
                // leader has truncated/opened the remote file; downloads and upload-resume have no
                // truncate race, so their parked workers are only the SSH-handshake burst limiter.
                auto upload_followers_ready = item.download || active_resume
                                           || (!workers.empty() && workers.front()->leader_ready());
                if (holding_followers && !any_err && upload_followers_ready)
                {
                    auto connecting = si32{};
                    for (auto& w : workers) if (w->state == xfer_worker::s_connecting) ++connecting;
                    auto burst = (si32)std::min(max_connections, parallel_connect_burst);
                    auto still_parked = faux;
                    for (auto i = size_t{ 1 }; i < workers.size(); ++i)
                    {
                        if (workers[i]->state != xfer_worker::s_init) continue;
                        if (connecting >= burst) { still_parked = true; continue; }
                        // A pooled worker is already authenticated, so rearm it immediately; a fresh
                        // worker enters s_connecting and counts against this tick's handshake burst.
                        if (workers[i]->session.alive()) workers[i]->rearm();
                        else { workers[i]->begin(); ++connecting; }
                    }
                    holding_followers = still_parked;
                    if (!holding_followers && !active_state_path.empty() && !item.download && !active_resume)
                        write_state_status(active_state_path, state_ready);
                }
                // Persist each chunk's progress so an interrupted upload resumes.
                if (!active_state_path.empty())
                    for (auto& w : workers) if (w->done != w->persisted)
                    {
                        write_state_transferred(active_state_path, active_md_size, (ui32)w->chunk_index, (ui64)w->done);
                        w->persisted = w->done;
                    }
                // While followers are parked they sit in s_init (neither busy nor
                // finished), so all_ok stays false until they have actually run.
                if (holding_followers) all_ok = faux;
                item.done = total;
                // Feed every poll into the ~1s sliding-window rate (rate.hpp): the shown speed
                // covers only the last second, measured from this run's resume baseline.
                item.rate.sample(item.done, std::chrono::steady_clock::now());
                dirty = true;
                // On failure keep the state file on disk so a retry resumes; on
                // success the transfer is complete, so remove it.
                // Settle the Speed column on the overall average for this run (bytes moved this run /
                // elapsed) so a finished row keeps showing a meaningful rate for both outcomes, and
                // report the run's bytes (not the resumed total) in the summary log.
                auto avg_speed = [&](si64 bytes){ return item.rate.average(bytes, (si64)(std::time(nullptr) - item.started)); };
                auto run_bytes = [&](si64 bytes){ return std::max<si64>(0, bytes - item.rate.base); };
                if (any_err && active_reused && total == 0)
                {
                    // A reused pooled connection failed before transferring a byte: almost certainly a
                    // stale connection the server dropped while idle. Drop the whole pool and re-queue
                    // the item so it retries immediately on a fresh connection (no spurious failure).
                    for (auto& w : workers) w->stop();
                    workers.clear();
                    idle_pool.clear();
                    item.status = queue_item::queued; item.error.clear(); item.done = 0; item.rate.speed = 0.0;
                    active = -1; holding_followers = faux; active_state_path.clear();
                }
                else if (any_err) { log_transfer_result(item, faux, run_bytes(total)); item.status = queue_item::failed; item.rate.speed = avg_speed(item.done); for (auto& w : workers) w->stop(); workers.clear(); active = -1; holding_followers = faux; active_state_path.clear(); refresh_panes(item); }
                else if (all_ok)  { log_transfer_result(item, true, run_bytes(item.size > 0 ? item.size : total)); if (item.size > 0) item.done = item.size; item.status = queue_item::succeeded; item.rate.speed = avg_speed(item.done);
                                    // Success: return every still-connected worker (the single stream, or each
                                    // parallel chunk) to the idle pool for reuse (recycle_worker caps the pool).
                                    for (auto& w : workers) recycle_worker(std::move(w));
                                    workers.clear(); active = -1; holding_followers = faux;
                                    if (!active_state_path.empty()) { std::remove(active_state_path.c_str()); active_state_path.clear(); }
                                    // Refresh the panes still showing the dirs the transfer touched (FileZilla
                                    // refreshes displayed directories only): the destination, and for an upload
                                    // the local source dir, whose resume-state file was just removed.
                                    refresh_panes(item);
                                    // Settings -> Hash verification: auto-checksum the transfer's target.
                                    if (cfg.hash_on_transfer) enqueue_transfer_hash(item); }
            }
            if (active == -1 && !no_autostart)
            {
                for (auto i = si32{}; i < (si32)queue.size(); ++i)
                {
                    if (queue[i].status == queue_item::queued && !queue[i].paused) { start_item(i); break; }
                }
            }
        }
        void list_dir() { pending.clear(); await = c_ls; send_cmd("ls"); }

        void process(sftp_msg const& m)
        {
            // Deepest level: a raw wire-frame trace of every event the backend
            // sends (event name + first payload line). Recorded unconditionally;
            // shown only at the Debug log level.
            trace(dbg_debug, "recv " + text{ sftp_evt_name(m.type) } + (m.line.empty() ? text{} : ": " + text{ m.first() }));
            switch (m.type)
            {
                case sftp_evt::status:
                case sftp_evt::info:
                    if (!m.line.empty()) mark(text{ m.first() });
                    break;
                case sftp_evt::verbose:
                    // The backend's verbose log lines (FileZilla maps Verbose ->
                    // debug_info). Recorded at the Info sub-level; the render-time
                    // filter hides them below that log level.
                    if (!m.line.empty()) trace(dbg_info, text{ m.first() });
                    break;
                case sftp_evt::error:
                    fail(text{ m.first() });
                    if (stage != s_connected) stage = s_failed;
                    break;
                case sftp_evt::ask_hostkey:
                case sftp_evt::ask_hostkey_changed:
                case sftp_evt::ask_hostkey_betteralg:
                    session.write_line("y"); // Accept (trust) the host key.
                    break;
                // FileZilla console_get_userpass_input emits the prompt's preamble/instruction before
                // ask_password. The preamble "SSH key passphrase" (ssh2userauth.c) distinguishes a key
                // passphrase from the account password; remember it to route the answer below.
                case sftp_evt::request_preamble:    last_preamble    = m.line.empty() ? text{} : text{ m.first() }; break;
                case sftp_evt::request_instruction: last_instruction = m.line.empty() ? text{} : text{ m.first() }; break;
                case sftp_evt::ask_password:
                    if (last_preamble == "SSH key passphrase") // SSH key passphrase.
                    {
                        auto kf = sftp_keyfile_from_prompt(m.first());
                        if (pass_asked.count(kf)) // Re-ask: the previous answer (typed or cached) was wrong.
                        {
                            key_passphrases.erase(kf);
                            sec_req = { true, text{ m.first() }, kf, true };
                            sec = sec_pending;
                        }
                        else
                        {
                            pass_asked.insert(kf);
                            auto pp = text{};
                            if (lookup_passphrase(kf, pp)) session.write_line(pp);          // Cached from a prior backend.
                            else { sec_req = { true, text{ m.first() }, kf, faux }; sec = sec_pending; } // Ask the user.
                        }
                    }
                    else // Account password.
                    {
                        if (!account_asked)
                        {
                            account_asked = true;
                            if (!pass.empty()) session.write_line(pass);                    // Quick Connect bar password.
                            else { sec_req = { false, text{ m.first() }, text{}, faux }; sec = sec_pending; }
                        }
                        else { sec_req = { false, text{ m.first() }, text{}, true }; sec = sec_pending; } // Wrong password: re-ask.
                    }
                    last_preamble.clear(); last_instruction.clear();
                    break;
                case sftp_evt::listentry:
                    if (await == c_ls || await == c_rls) // Plain browse listing or a recursive-walk listing.
                    {
                        auto e = to_direntry(m);
                        if (e.name != "." && e.name != "..") pending.push_back(std::move(e));
                    }
                    break;
                case sftp_evt::reply:
                    // A keepalive `pwd` reply: swallow it (don't log or advance the state
                    // machine). psftp emits exactly one terminal event per command -- a
                    // reply OR a done -- and keepalive's is the reply, so this consumes it
                    // even if the user issued a real command in the meantime (FIFO order).
                    if (keep_skip > 0) { --keep_skip; if (!m.line.empty()) trace(dbg_verbose, text{ m.first() }); break; }
                    // The server's reply to the last command (FileZilla: Response).
                    if (!m.line.empty()) log_line(logtype::response, text{ m.first() });
                    on_reply(m);
                    break;
                // Done carries the command result code (FileZilla OnSftpEvent:
                // only "1" is success). A failed browse command (e.g. cd/ls into a
                // forbidden dir) must NOT advance the navigation state machine; the
                // Error event above has already surfaced the reason via fail().
                // A recursive-walk `ls` and a recop one-shot (mkdir/rm/rmdir) advance the operation
                // regardless of the result code: a failed/empty ls is just an empty level, and a
                // mkdir-exists / rmdir-nonempty must not stall the rest of the command sequence.
                case sftp_evt::done:  if (await == c_keyfile) send_next_keyfile_or_open(); // Key registered; next key or open.
                                      else if (await == c_rls || await == c_recop) complete();
                                      else if (m.first() == "1") complete();
                                      else { await = c_none; path_pending = faux; ls_deferred = faux; } // Failed browse
                                          // cmd (cd/ls/op): it still terminated, so release the control session (else
                                          // the in-flight guard would wedge navigation) and drop any staged path/ls.
                                      break;
                default: break;
            }
        }

        // Send the next existing private key as a `keyfile <path>` command (await c_keyfile) or,
        // once every configured key is registered, the `open` command. The backend emits a Done
        // for EVERY command (psftp.c do_sftp), so each keyfile's Done must be consumed before the
        // next command is sent — they cannot be pipelined ahead of `open` (that premature Done
        // would fire complete() for c_open before auth). Mirrors FileZilla connect.cpp's
        // connect_keys -> connect_open state sequence.
        void send_next_keyfile_or_open()
        {
            while (connect_keyfile_i < cfg.keyfiles.size())
            {
                auto& kf = cfg.keyfiles[connect_keyfile_i++];
                if (kf.empty()) continue;
                auto ec = std::error_code{};
                if (!fs::is_regular_file(fs::path{ kf }, ec)) { log_line(logtype::status, "Skipping non-existing key file " + kf); continue; }
                await = c_keyfile;
                send_cmd("keyfile " + quote_name(kf));
                return;
            }
            await = c_open;
            mark("Authenticating...");
            send_cmd("open " + quote_name(user + "@" + host) + " " + std::to_string(port));
        }

        void on_reply(sftp_msg const& m)
        {
            if (stage == s_greeting) // The startup banner: register key files (if any), then open.
            {
                stage = s_opening;
                connect_keyfile_i = 0;
                send_next_keyfile_or_open();
                return;
            }
            last_reply = text{ m.first() };
            complete(); // A Reply (e.g. pwd's path) also terminates a command.
        }

        void complete()
        {
            switch (await)
            {
                case c_open:
                    stage = s_connected;
                    recovering = faux; reconnect_tries = 0; // Link restored; stand down the reconnect driver.
                    if (restoring) // Reconnect: cd back to where we were instead of landing in the home dir.
                    {
                        restoring = faux;
                        pending_path = resume_path.empty() ? text{ "/" } : resume_path;
                        path_pending = true; // Restored path is revealed once its listing returns (c_ls).
                        await = c_cd;
                        send_cmd("cd " + quote_name(pending_path));
                        mark("Reconnected. Restoring " + pending_path + "...");
                    }
                    else
                    {
                        await = c_pwd;
                        send_cmd("pwd");
                        mark("Connected. Retrieving directory listing...");
                    }
                    break;
                case c_pwd:
                    if (!last_reply.empty()) path = normalize_pwd(last_reply);
                    list_dir();
                    break;
                case c_ls:
                    items = std::move(pending);
                    pending.clear();
                    sort_dir(items);
                    if (path_pending) { path = pending_path; path_pending = faux; } // Reveal the dir only now (with its listing).
                    ++gen;
                    await = c_none;
                    mark("Directory listing of " + path + " successful");
                    break;
                case c_cd:
                    // Don't reveal pending_path yet; c_ls commits it once the listing is in. The test seam
                    // holds the `ls` (await stays c_cd) to widen this window for navigation-race tests.
                    if (dbg_ls_delay_ms > 0) { ls_deferred = true; ls_due = steady_clock::now() + std::chrono::milliseconds{ dbg_ls_delay_ms }; }
                    else list_dir();
                    break;
                case c_op:   // mkdir/rm/rmdir/mv finished: refresh the current directory.
                    list_dir();
                    break;
                case c_rls:  // A recursive-walk `ls <rec_cur.remote>` came back in `pending`.
                {
                    sort_dir(pending);
                    for (auto& e : pending)
                    {
                        auto rchild = child_path(rec_cur.remote, e.name, faux);
                        if (e.is_dir)
                        {
                            if (recop == rec_download)
                            {
                                auto lchild = child_path(rec_cur.local, e.name, true);
                                auto ec = std::error_code{}; fs::create_directories(fs::path{ lchild }, ec);
                                rec_stack.push_back({ rchild, lchild });
                            }
                            else // rec_delete: recurse, and record the dir for a deepest-first rmdir.
                            {
                                rec_deldirs.push_back(rchild);
                                rec_stack.push_back({ rchild, {} });
                            }
                        }
                        else
                        {
                            if (recop == rec_download) enqueue_download_path(rchild, child_path(rec_cur.local, e.name, true), e.size);
                            else                       rec_delfiles.push_back(rchild); // rec_delete
                        }
                    }
                    pending.clear();
                    await = c_none; // drive_recop() issues the next walk/command on the following poll.
                    break;
                }
                case c_recop: // A queued mkdir/rm/rmdir finished (success or not): let drive_recop continue.
                    await = c_none;
                    break;
                default: break;
            }
        }

        static void sort_dir(std::vector<direntry>& v)
        {
            std::sort(v.begin(), v.end(), [](direntry const& a, direntry const& b)
            {
                if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
                auto al = a.name; utf::to_lower(al);
                auto bl = b.name; utf::to_lower(bl);
                return al < bl;
            });
        }
        static auto normalize_pwd(view s) -> text
        {
            // pwd reply looks like: Current directory is: "/path"
            auto q1 = s.find('"');
            if (q1 != view::npos)
            {
                auto q2 = s.find('"', q1 + 1);
                if (q2 != view::npos && q2 > q1 + 1) return text{ s.substr(q1 + 1, q2 - q1 - 1) };
            }
            auto pos = s.find('/');
            return pos == view::npos ? text{ "/" } : text{ s.substr(pos) };
        }
    };
}
