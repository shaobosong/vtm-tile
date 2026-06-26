// Copyright (c) Dmitry Sapozhnikov
// Licensed under the MIT license.

#include "netxs/apps.hpp"
#include "netxs/apps/desk.hpp"
#include "vtm-common.hpp"
#include "netxs/apps/tile.hpp"

using namespace netxs;

enum class type { client, server, daemon, logmon, runapp, config, sessions };
enum class code { noaccess, noserver, nodaemon, nosrvlog, interfer, errormsg };

// In-process FileZilla/PuTTY parvionsftp backend entry point. This is the backend's
// own platform main(), renamed to parvionsftp_main via a per-file compile define in
// CMake (parvionsftp_be lib) so it links alongside vtm-tile's main() without clashing.
extern "C" int parvionsftp_main(int argc, char** argv);
// In-process pvputtygen entry point (FileZilla cmdgen.c), renamed to pvputtygen_main via
// the same per-file compile define (pvputtygen_be lib). Parses private-key Comment/Data
// (fingerprint) for the Settings dialog's key picker; reached via `vtm-tile -r pvputtygen`.
extern "C" int pvputtygen_main(int argc, char** argv);

namespace tile_session_reg
{
    namespace fs = std::filesystem;

    auto registry_dir()
    {
        return os::path::home / ".config" / "vtm" / "sessions";
    }
    auto pid_alive(ui32 pid) -> bool
    {
        if (!pid) return faux;
        #if defined(_WIN32)
            auto h = ::OpenProcess(SYNCHRONIZE, FALSE, (DWORD)pid);
            if (h)
            {
                ::CloseHandle(h);
                return true;
            }
            return faux;
        #else
            if (::kill((pid_t)pid, 0) == 0) return true;
            return errno == EPERM; // Process exists but signaling is denied.
        #endif
    }
    auto json_escape(view s)
    {
        auto out = text{};
        out.reserve(s.size() + 2);
        out.push_back('"');
        for (auto c : s)
        {
                 if (c == '"')  out += "\\\"";
            else if (c == '\\') out += "\\\\";
            else if (c == '\n') out += "\\n";
            else if (c == '\r') out += "\\r";
            else if (c == '\t') out += "\\t";
            else if ((unsigned char)c >= 0x20) out.push_back(c);
        }
        out.push_back('"');
        return out;
    }
    auto parse_field(view body, view key) -> text
    {
        auto qkey = text{ "\"" } + text{ key } + text{ "\"" };
        auto pos = body.find(qkey);
        if (pos == view::npos) return {};
        pos += qkey.size();
        while (pos < body.size() && (body[pos] == ' ' || body[pos] == ':' || body[pos] == '\t')) pos++;
        if (pos >= body.size()) return {};
        auto out = text{};
        if (body[pos] == '"')
        {
            ++pos;
            while (pos < body.size() && body[pos] != '"')
            {
                if (body[pos] == '\\' && pos + 1 < body.size())
                {
                    auto c = body[pos + 1];
                         if (c == 'n')  out.push_back('\n');
                    else if (c == 'r')  out.push_back('\r');
                    else if (c == 't')  out.push_back('\t');
                    else                out.push_back(c);
                    pos += 2;
                }
                else
                {
                    out.push_back(body[pos]);
                    ++pos;
                }
            }
        }
        else
        {
            while (pos < body.size() && body[pos] != ',' && body[pos] != '\n' && body[pos] != '\r' && body[pos] != '}')
            {
                if (body[pos] != ' ' && body[pos] != '\t') out.push_back(body[pos]);
                ++pos;
            }
        }
        return out;
    }
    auto epoch_ms_now()
    {
        return (ui64)std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    }
    auto write(view pipe, view user, view cwd, view params)
    {
        auto dir = registry_dir();
        auto ec = std::error_code{};
        fs::create_directories(dir, ec);
        auto path = dir / (text{ pipe } + ".json");
        auto body = text{};
        body += "{\n";
        body += "  \"pipe\": "    + json_escape(pipe)                            + ",\n";
        body += "  \"pid\": "     + std::to_string(os::process::id.first)        + ",\n";
        body += "  \"user\": "    + json_escape(user)                            + ",\n";
        body += "  \"started\": " + std::to_string(epoch_ms_now())               + ",\n";
        body += "  \"cwd\": "     + json_escape(cwd)                             + ",\n";
        body += "  \"params\": "  + json_escape(params)                          +  "\n";
        body += "}\n";
        auto out = std::ofstream{ path, std::ios::binary | std::ios::trunc };
        if (out) out.write(body.data(), (std::streamsize)body.size());
        return path;
    }
    auto erase(fs::path const& path)
    {
        auto ec = std::error_code{};
        fs::remove(path, ec);
    }
    struct scoped
    {
        fs::path path;
        scoped(view pipe, view user, view cwd, view params)
            : path{ write(pipe, user, cwd, params) }
        { }
        ~scoped() { erase(path); }
        scoped(scoped const&) = delete;
        scoped& operator=(scoped const&) = delete;
    };
    struct entry
    {
        text pipe;
        ui32 pid;
        text user;
        ui64 started;
        text cwd;
        text params;
    };
    auto load_for_user(view current_user) -> std::vector<entry>
    {
        auto out = std::vector<entry>{};
        auto dir = registry_dir();
        auto ec = std::error_code{};
        if (!fs::exists(dir, ec)) return out;
        for (auto& it : fs::directory_iterator(dir, ec))
        {
            if (ec) break;
            if (!it.is_regular_file()) continue;
            if (it.path().extension() != ".json") continue;
            auto in = std::ifstream{ it.path(), std::ios::binary };
            if (!in) continue;
            auto body = text{ std::istreambuf_iterator<char>{ in }, std::istreambuf_iterator<char>{} };
            auto pid_str = parse_field(body, "pid");
            if (pid_str.empty())
            {
                remove(it.path());
                continue;
            }
            auto pid = (ui32)std::strtoul(pid_str.c_str(), nullptr, 10);
            if (!pid_alive(pid))
            {
                remove(it.path());
                continue;
            }
            auto user = parse_field(body, "user");
            if (current_user.size() && user != current_user) continue;
            auto e = entry{};
            e.pipe    = parse_field(body, "pipe");
            e.pid     = pid;
            e.user    = user;
            e.started = (ui64)std::strtoull(parse_field(body, "started").c_str(), nullptr, 10);
            e.cwd     = parse_field(body, "cwd");
            e.params  = parse_field(body, "params");
            if (e.pipe.empty()) e.pipe = it.path().stem().string();
            out.push_back(std::move(e));
        }
        std::sort(out.begin(), out.end(), [](auto& a, auto& b){ return a.started < b.started; });
        return out;
    }
    auto format_uptime(ui64 started)
    {
        auto now = epoch_ms_now();
        auto secs = now > started ? (now - started) / 1000 : (ui64)0;
        auto d = secs / 86400; secs %= 86400;
        auto h = secs /  3600; secs %=  3600;
        auto m = secs /    60;
        auto s = secs %    60;
        auto buf = std::array<char, 32>{};
        if (d) std::snprintf(buf.data(), buf.size(), "%llud%02llu:%02llu:%02llu",
                             (unsigned long long)d, (unsigned long long)h, (unsigned long long)m, (unsigned long long)s);
        else   std::snprintf(buf.data(), buf.size(),       "%02llu:%02llu:%02llu",
                             (unsigned long long)h, (unsigned long long)m, (unsigned long long)s);
        return text{ buf.data() };
    }
    auto pin_id(view pipe)
    {
        constexpr auto suffix = view{ "-tile" };
        return pipe.ends_with(suffix) ? text{ pipe.substr(0, pipe.size() - suffix.size()) }
                                      : text{ pipe };
    }
    auto render_table(std::vector<entry> const& entries)
    {
        if (entries.empty()) return text{ "No tile sessions found.\n" };
        auto headers = std::array<text, 4>{ "ID", "PID", "UPTIME", "CWD" };
        auto widths  = std::array<size_t, 4>{ headers[0].size(), headers[1].size(), headers[2].size(), headers[3].size() };
        auto rows    = std::vector<std::array<text, 4>>{};
        rows.reserve(entries.size());
        for (auto& e : entries)
        {
            auto row = std::array<text, 4>{ pin_id(e.pipe), std::to_string(e.pid), format_uptime(e.started), e.cwd };
            for (auto i = 0u; i < row.size(); ++i) widths[i] = std::max(widths[i], row[i].size());
            rows.push_back(std::move(row));
        }
        auto out = text{};
        auto emit = [&](std::array<text, 4> const& r)
        {
            for (auto i = 0u; i < r.size(); ++i)
            {
                out += r[i];
                if (i + 1 < r.size()) out.append(widths[i] - r[i].size() + 2, ' ');
            }
            out.push_back('\n');
        };
        emit(headers);
        for (auto& r : rows) emit(r);
        return out;
    }
}

// Multi-call perf harness: `vtm-tile -r parvionbench <get|put> <host> <port> <user> <pass>
// <remote> <local> [<parallel> <offset> <length>]`. Drives ONE parvionsftp transfer through
// the production xfer_worker (real shared-memory io_* ring + crypto/SFTP path), printing
// elapsed seconds and MiB/s. Headless (no UI/gate init); for single-channel benchmarking only.
// Set PARVIONBENCH_QUIET=1 to suppress per-line status logging.
static int parvionbench_main(int argc, char** argv)
{
    using xw = app::parvion::xfer_worker;
    if (argc < 7)
    {
        std::fprintf(stderr, "usage: -r parvionbench <get|put> <host> <port> <user> <pass>"
                             " <remote> <local> [<parallel> <offset> <length>]\n");
        return 2;
    }
    auto dir = view{ argv[0] };
    auto w = xw{};
    if (auto e = std::getenv("PARVION_SFTP_BIN"); e && *e) { w.exe = text{ e }; w.runargs.clear(); } // external helper (e.g. an -O2 build)
    else { w.exe = os::process::binary(); w.runargs = { "-r", "parvionsftp" }; }
    w.host        = argv[1];
    w.port        = std::atoi(argv[2]);
    w.user        = argv[3];
    w.pass        = argv[4];
    w.remote_path = argv[5];
    w.local_path  = argv[6];
    w.download    = dir == "get";
    if (auto e = std::getenv("PARVIONBENCH_KEYFILE"); e && *e) // public-key auth (register before open)
    {
        w.keyfiles = { text{ e } };
        w.passphrase_provider = [](text const&, text& out) { out.clear(); return true; }; // unencrypted key
    }
    if (argc >= 10) // ranged single chunk (for parallel-path measurement)
    {
        w.parallel   = std::atoi(argv[7]) != 0;
        w.offset     = (si64)std::atoll(argv[8]);
        w.length     = (si64)std::atoll(argv[9]);
        w.initialize = w.offset == 0; // lone chunk is its own leader (truncate+create)
    }
    auto quiet = std::getenv("PARVIONBENCH_QUIET") != nullptr;
    w.logsink = [quiet](app::parvion::logtype t, text s, si32)
    {
        if (!quiet || t == app::parvion::logtype::error) std::fprintf(stderr, "[%d] %s\n", (si32)t, s.c_str());
    };
    auto t0 = std::chrono::steady_clock::now();
    auto t_open = t0;
    auto seen_open = faux;
    w.begin();
    while (!w.finished())
    {
        w.poll();
        if (!seen_open && w.opened) { t_open = std::chrono::steady_clock::now(); seen_open = true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); // bulk data runs on the session reader thread, not here
    }
    auto t1    = std::chrono::steady_clock::now();
    auto total = std::chrono::duration<double>(t1 - t0).count();
    auto xfer  = std::chrono::duration<double>(t1 - (seen_open ? t_open : t0)).count();
    // The helper's transfer-progress deltas under-count on the download path, so derive the
    // transferred byte count from ground truth: the ranged chunk length, else the local file
    // size (= bytes received for a whole-file download / sent for a whole-file upload).
    auto bytes = w.done;
    if (argc >= 10 && w.length > 0) bytes = w.length;
    else { auto ec = std::error_code{}; auto fsz = (si64)std::filesystem::file_size(std::filesystem::path{ w.local_path }, ec); if (!ec && fsz > 0) bytes = fsz; }
    auto mib = (double)bytes / 1048576.0;
    std::fprintf(stdout, "RESULT dir=%s state=%s bytes=%lld done_counter=%lld total_s=%.3f xfer_s=%.3f MiBps_total=%.2f MiBps_xfer=%.2f\n",
                 w.download ? "get" : "put", w.state == xw::s_ok ? "ok" : "err",
                 (long long)bytes, (long long)w.done, total, xfer, mib / (total > 0 ? total : 1), mib / (xfer > 0 ? xfer : total));
    std::fflush(stdout);
    if (w.state != xw::s_ok && !w.error.empty()) std::fprintf(stderr, "ERROR: %s\n", w.error.c_str());
    w.stop();
    return w.state == xw::s_ok ? 0 : 1;
}

// Headless harness for the REAL applet transfer orchestration (sftp_remote + queue +
// pump_queue), unlike parvionbench which drives a bare xfer_worker. Drives a single-channel
// download/upload exactly as the UI does, at a configurable poll cadence (last arg, ms), to
// isolate whether throughput depends on the applet's driving rather than the worker itself.
// `-r parvionxfer <get|put> <host> <port> <user> <pass> <remote> <local> <size> [poll_ms]`
static int parvionxfer_main(int argc, char** argv)
{
    using namespace std::chrono;
    using sr = app::parvion::sftp_remote;
    using qi = app::parvion::queue_item;
    if (argc < 8) { std::fprintf(stderr, "usage: -r parvionxfer <get|put> <host> <port> <user> <pass> <remote> <local> <size> [poll_ms]\n"); return 2; }
    auto dir     = view{ argv[0] };
    auto port    = std::atoi(argv[2]);
    auto user    = text{ argv[3] };
    auto pass    = text{ argv[4] };
    auto remote  = text{ argv[5] };
    auto local   = text{ argv[6] };
    auto size    = (si64)std::atoll(argv[7]);
    auto poll_ms = argc >= 9 ? std::atoi(argv[8]) : 5;
    auto ctrl = std::make_unique<sr>();
    if (auto e = std::getenv("PARVION_SFTP_BIN"); e && *e) { ctrl->exe = text{ e }; ctrl->runargs.clear(); }
    else { ctrl->exe = os::process::binary(); ctrl->runargs = { "-r", "parvionsftp" }; }
    ctrl->use_parallel = faux; // force single-channel (as the user runs it)
    ctrl->connect(text{ argv[1] }, port, user, pass);
    auto t0 = steady_clock::now();
    while (!ctrl->connected() && ctrl->stage != sr::s_failed
           && duration_cast<seconds>(steady_clock::now() - t0).count() < 25)
    { ctrl->poll(); std::this_thread::sleep_for(milliseconds(5)); }
    if (!ctrl->connected()) { std::fprintf(stderr, "connect failed (stage=%d)\n", (int)ctrl->stage); return 1; }
    if (dir == "get") ctrl->enqueue_download_path(remote, local, size);
    else              ctrl->enqueue_upload(local, remote, size);
    auto tstart = steady_clock::now();
    auto last_done = si64{ 0 };
    for (;;)
    {
        ctrl->poll();
        auto busy = faux;
        for (auto& it : ctrl->queue) { last_done = it.done; if (it.status == qi::queued || it.status == qi::transferring) busy = true; }
        if (!busy) break;
        std::this_thread::sleep_for(milliseconds(poll_ms));
        if (duration_cast<seconds>(steady_clock::now() - tstart).count() > 600) break;
    }
    auto secs = duration<double>(steady_clock::now() - tstart).count();
    auto ec = std::error_code{};
    auto fsz = (si64)std::filesystem::file_size(std::filesystem::path{ local }, ec);
    auto bytes = (!ec && fsz > 0) ? fsz : (last_done > 0 ? last_done : size);
    auto mib = (double)bytes / 1048576.0;
    std::fprintf(stdout, "XFER dir=%s poll_ms=%d secs=%.3f MiBps=%.2f bytes=%lld done_counter=%lld\n",
                 dir == "get" ? "get" : "put", poll_ms, secs, mib / (secs > 0 ? secs : 1),
                 (long long)bytes, (long long)last_done);
    std::fflush(stdout);
    ctrl->disconnect();
    return 0;
}

// Headless DUAL-/MULTI-channel transfer benchmark. Drives the REAL parallel orchestration
// (sftp_remote + pump_queue): the file is split into <channels> chunks, each transferred by
// its own parvionsftp connection through its own shared-memory io_* ring, exactly as the UI
// does for large files. Reports aggregate MiB/s. "Dual-channel" = <channels> 2.
// `-r parvionmc <get|put> <host> <port> <user> <pass> <remote> <local> <size> <channels> [poll_ms]`
// PARVIONBENCH_KEYFILE=<path> selects public-key auth (e.g. sshd:22); PARVION_SFTP_BIN picks
// an external -O2 helper. The local target dir must exist.
static int parvionmc_main(int argc, char** argv)
{
    using namespace std::chrono;
    using sr = app::parvion::sftp_remote;
    using qi = app::parvion::queue_item;
    if (argc < 9) { std::fprintf(stderr, "usage: -r parvionmc <get|put> <host> <port> <user> <pass> <remote> <local> <size> <channels> [poll_ms]\n"); return 2; }
    auto dir      = view{ argv[0] };
    auto port     = std::atoi(argv[2]);
    auto user     = text{ argv[3] };
    auto pass     = text{ argv[4] };
    auto remote   = text{ argv[5] };
    auto local    = text{ argv[6] };
    auto size     = (si64)std::atoll(argv[7]);
    auto channels = std::clamp(std::atoi(argv[8]), 1, 16);
    auto poll_ms  = argc >= 10 ? std::atoi(argv[9]) : 2;
    auto ctrl = std::make_unique<sr>();
    if (auto e = std::getenv("PARVION_SFTP_BIN"); e && *e) { ctrl->exe = text{ e }; ctrl->runargs.clear(); }
    else { ctrl->exe = os::process::binary(); ctrl->runargs = { "-r", "parvionsftp" }; }
    if (auto e = std::getenv("PARVIONBENCH_KEYFILE"); e && *e) ctrl->cfg.keyfiles = { text{ e } }; // public-key auth (unencrypted)
    // Force exactly <channels> chunks: a threshold of size/channels makes part_count's
    // ceil(size/threshold) land on <channels>, then it's clamped to max_connections.
    ctrl->use_parallel       = channels > 1;
    ctrl->max_connections    = (ui32)channels;
    ctrl->parallel_threshold = std::max<si64>(1, size / channels);
    ctrl->connect(text{ argv[1] }, port, user, pass);
    auto t0 = steady_clock::now();
    while (!ctrl->connected() && ctrl->stage != sr::s_failed
           && duration_cast<seconds>(steady_clock::now() - t0).count() < 25)
    { ctrl->poll(); std::this_thread::sleep_for(milliseconds(5)); }
    if (!ctrl->connected()) { std::fprintf(stderr, "connect failed (stage=%d)\n", (int)ctrl->stage); return 1; }
    if (dir == "get") ctrl->enqueue_download_path(remote, local, size);
    else              ctrl->enqueue_upload(local, remote, size);
    auto tstart = steady_clock::now();
    auto last_done = si64{ 0 };
    auto chunks = 0;
    auto failed = faux;
    for (;;)
    {
        ctrl->poll();
        auto busy = faux;
        for (auto& it : ctrl->queue)
        {
            last_done = it.done; chunks = (int)it.chunk_count;
            if (it.status == qi::queued || it.status == qi::transferring) busy = true;
            if (it.status == qi::failed) failed = true;
        }
        if (!busy) break;
        std::this_thread::sleep_for(milliseconds(poll_ms));
        if (duration_cast<seconds>(steady_clock::now() - tstart).count() > 900) break;
    }
    auto secs = duration<double>(steady_clock::now() - tstart).count();
    // Ground truth: the local file size (received bytes for a download, full source for an upload).
    auto ec = std::error_code{};
    auto fsz = (si64)std::filesystem::file_size(std::filesystem::path{ local }, ec);
    auto bytes = (!ec && fsz > 0) ? fsz : (last_done > 0 ? last_done : size);
    auto mib = (double)bytes / 1048576.0;
    std::fprintf(stdout, "MC dir=%s channels=%d chunks=%d state=%s secs=%.3f MiBps=%.2f bytes=%lld\n",
                 dir == "get" ? "get" : "put", channels, chunks, failed ? "err" : "ok",
                 secs, mib / (secs > 0 ? secs : 1), (long long)bytes);
    std::fflush(stdout);
    ctrl->disconnect();
    return failed ? 1 : 0;
}

int main(int argc, char* argv[])
{
    // Multi-call entry: `vtm-tile -r parvionsftp [opts]` runs the in-process parvionsftp
    // backend (its renamed main) and exits — handled before any vtm console/log
    // init runs, because the backend owns stdout for its fzprintf protocol.
    for (auto i = 1; i + 1 < argc; i++)
    {
        auto flag = view{ argv[i] };
        if (flag == "-r" || flag == "--run" || flag == "--")
        {
            if (view{ argv[i + 1] }.starts_with("parvionsftp"))
            {
                static char arg0[] = "parvionsftp";
                auto args = std::vector<char*>{ arg0 };
                for (auto j = i + 2; j < argc; j++) args.push_back(argv[j]);
                args.push_back(nullptr);
                return parvionsftp_main((int)args.size() - 1, args.data());
            }
            if (view{ argv[i + 1] }.starts_with("pvputtygen"))
            {
                static char arg0[] = "pvputtygen";
                auto args = std::vector<char*>{ arg0 };
                for (auto j = i + 2; j < argc; j++) args.push_back(argv[j]);
                args.push_back(nullptr);
                return pvputtygen_main((int)args.size() - 1, args.data());
            }
            if (view{ argv[i + 1] }.starts_with("parvionbench"))
            {
                return parvionbench_main(argc - (i + 2), argv + (i + 2));
            }
            if (view{ argv[i + 1] }.starts_with("parvionxfer"))
            {
                return parvionxfer_main(argc - (i + 2), argv + (i + 2));
            }
            if (view{ argv[i + 1] }.starts_with("parvionmc"))
            {
                return parvionmc_main(argc - (i + 2), argv + (i + 2));
            }
            break; // A run flag was given but not a known backend: fall through to normal handling.
        }
    }
    auto whoami = type::client;
    auto params = text{};
    auto cliopt = text{};
    auto errmsg = text{};
    auto vtpipe = text{};
    auto script = text{};
    auto title  = text{};
    auto system = faux;
    auto getopt = os::process::args{ argc, argv };
    while (getopt)
    {
        if (getopt.match("--cwd"))
        {
            auto path = getopt.next();
            if (path.size())
            {
                if (os::env::cwd(path)) log("%%Set current working directory to '%path%'", prompt::os, path);
                else                    log("%%Failed to set current working directory to '%path%'", prompt::os, ansi::err(path));
            }
        }
        else if (getopt.match("--env"))
        {
            auto var_val = getopt.next();
            if (var_val.size())
            {
                log("%%Set environment variable '%var_val%'", prompt::os, var_val);
                os::env::set(var_val);
            }
        }
        else if (getopt.match("--svc"))
        {
            auto ok = os::process::dispatch();
            return ok ? 0 : 1;
        }
        else if (getopt.match("-0", "--session0"))
        {
            system = true;
        }
        else if (getopt.match("-r", "--", "--run"))
        {
            whoami = type::runapp;
            params = getopt.rest();
        }
        else if (getopt.match("-s", "--server"))
        {
            whoami = type::server;
        }
        else if (getopt.match("-d", "--daemon"))
        {
            whoami = type::daemon;
        }
        else if (getopt.match("-m", "--monitor"))
        {
            whoami = type::logmon;
        }
        else if (getopt.match("-p", "--pin"))
        {
            vtpipe = getopt.next();
            if (vtpipe.empty())
            {
                errmsg = "Custom pipe not specified";
                break;
            }
        }
        else if (getopt.match("-q", "--quiet"))
        {
            netxs::logger::enabled(faux);
        }
        else if (getopt.match("--list-config"))
        {
            whoami = type::config;
        }
        else if (getopt.match("--list-sessions"))
        {
            whoami = type::sessions;
        }
        else if (getopt.match("-c", "--config"))
        {
            cliopt = getopt.next();
            if (cliopt.empty())
            {
                errmsg = "Config file path not specified";
                break;
            }
        }
        else if (getopt.match("-t", "--title"))
        {
            title = getopt.next();
        }
        else if (getopt.match("-?", "-h", "--help"))
        {
            os::dtvt::initialize();
            netxs::logger::wipe();
            auto syslog = os::tty::logger();
            log("\nTiling Window Manager " + text{ app::shared::version } +
                "\n(virtual terminal multiplexer - tile mode)"
                "\n"
                "\n  Command-line options syntax:"
                "\n"
                "\n    vtm-tile [ -c <file> ][ -q ][ -p <id> ][ -s | -d | -m ][ -x <cmds> ]"
                "\n    vtm-tile [ -c <file> ][ -q ][ -t <title> ][ -r [ <type> ]][ <args...> ]"
                "\n    vtm-tile [ -c <file> ]  --list-config"
                "\n    vtm-tile --list-sessions"
                "\n    vtm-tile -v | -?"
                "\n"
                "\n  Options:"
                "\n"
                "\n    Without options, vtm-tile runs Tile Server and Tile Client."
                "\n"
                "\n    -h, -?, --help       Print command-line options."
                "\n    -v, --version        Print version."
                "\n    --list-config        Print configuration."
                "\n    --list-sessions      Print active tile sessions for the current user."
                "\n    -q, --quiet          Disable logging."
                "\n    -x, --script <cmds>  Specifies script commands."
                "\n    -c, --config <file>  Specifies a settings file to load or plain xml-data to overlay."
                "\n    -p, --pin <id>       Specifies the tile session id it will be pinned to."
                "\n    -s, --server         Run Tile Server."
                "\n    -d, --daemon         Run Tile Server in background."
                "\n    -m, --monitor        Run Log Monitor."
                "\n    -t, --title <title>  Set initial title for the applet."
                "\n    --env <var=val>      Set environment variable."
                "\n    --cwd <path>         Set current working directory."
                "\n    -r, --, --run        Run applet standalone."
                "\n    <type>               Applet to run (vtty, term, dtvt, dtty)."
                "\n    <args...>            Applet arguments."
                "\n"
                "\n    Applet                     │ Type │ Arguments"
                "\n    ───────────────────────────┼──────┼─────────────────────────────────────────────────"
                "\n    Teletype Console (default) │ vtty │ CUI application with arguments to run."
                "\n    Terminal Console           │ term │ CUI application with arguments to run."
                "\n    DirectVT Gateway           │ dtvt │ DirectVT-aware application to run."
                "\n    DirectVT Gateway with TTY  │ dtty │ CUI application to run, forwarding DirectVT I/O."
                "\n"
                "\n    Plain xml-data can be specified in place of <file> in the '--config <file>' option,"
                "\n    as well as in the $VTM_CONFIG environment variable:"
                "\n"
                "\n      vtm-tile -c \"<config><terminal><scrollback size=1000000/></terminal></config>\" -r term"
                "\n      or (using compact syntax)"
                "\n      vtm-tile -c \"<config/terminal/scrollback size=1000000/>\" -r term"
                "\n");
            return 0;
        }
        else if (getopt.match("-v", "--version"))
        {
            os::dtvt::initialize();
            netxs::logger::wipe();
            auto syslog = os::tty::logger();
            log(app::shared::version);
            return 0;
        }
        else if (getopt.match("-x", "--script"))
        {
            script = getopt.next();
        }
        else
        {
            params = getopt.rest(); // params can't be empty at this point (see utf::quote()).
            if (params.front() == '-') errmsg = utf::concat("Unknown option '", params, "'");
            else                       whoami = type::runapp;
        }
    }

    auto interactive = whoami == type::runapp || whoami == type::client;
    os::dtvt::initialize(true, interactive);

    if (os::dtvt::vtmode & ui::console::redirio && (whoami == type::runapp || whoami == type::client))
    {
        whoami = type::logmon;
    }
    auto denied = faux;
    auto syslog = os::tty::logger();
    auto userid = os::env::user();
    auto prefix_base = vtpipe.length() ? vtpipe
                                      : utf::concat(os::path::ipc_prefix, os::process::elevated ? "!-" : "-", userid.second);
    auto prefix = utf::concat(prefix_base, "-tile");
    auto prefix_log = prefix + os::path::log_suffix;
    auto failed = [&](auto cause)
    {
        os::fail(cause == code::noaccess ? "Access denied"
               : cause == code::interfer ? "Server already running"
               : cause == code::noserver ? "Failed to start server"
               : cause == code::nosrvlog ? "Failed to start session monitor"
               : cause == code::nodaemon ? "Failed to daemonize"
               : cause == code::errormsg ? errmsg.c_str()
                                         : "");
        return 1;
    };

    log(prompt::vtm, app::shared::version);
    log(getopt.show());
    if (errmsg.size())
    {
        return failed(code::errormsg);
    }
    else if (whoami == type::config)
    {
        auto config = xml::settings{};
        app::shared::load::settings(config, cliopt, true);
        log(prompt::resultant_settings, "\n", config);
    }
    else if (whoami == type::sessions)
    {
        netxs::logger::wipe();
        auto entries = tile_session_reg::load_for_user(userid.first);
        log<faux>(tile_session_reg::render_table(entries));
    }
    else if (whoami == type::logmon)
    {
        auto result = std::atomic<int>{};
        auto events = os::tty::binary::logger{ [&](auto&, auto& reply)
        {
            if (reply.size() && os::dtvt::vtmode & ui::console::redirio)
            {
                os::io::send(reply);
            }
            --result;
        }};
        auto online = flag{ true };
        auto active = flag{ faux };
        auto locker = std::mutex{};
        auto syncio = std::unique_lock{ locker };
        auto buffer = std::list{ script };
        auto stream = sptr<os::ipc::socket>{};
        auto readln = os::tty::readline([&](auto line)
        {
            auto sync = std::lock_guard{ locker };
            if (active)
            {
                ++result;
                events.command.send(stream, line);
            }
            else
            {
                log("%%No server connected: %cmd%", prompt::main, utf::debase<faux, faux>(line));
                buffer.push_back(line);
            }
        }, [&]
        {
            auto sync = std::lock_guard{ locker };
            online.exchange(faux);
            if (active) while (result && active) std::this_thread::yield();
            if (active && stream) stream->shut();
        });
        auto logmsg = true;
        while (online)
        {
            auto iolink = os::ipc::socket::open<os::role::client, faux>(prefix_log, denied);
            if (denied)
            {
                syncio.unlock();
                return failed(code::noaccess);
            }
            if (iolink)
            {
                std::swap(stream, iolink);
                result += 3;
                events.command.send(stream, utf::concat(os::process::id.first)); // First command is the monitor id.
                events.command.send(stream, os::env::add());
                events.command.send(stream, os::env::cwd());
                for (auto& line : buffer)
                {
                    ++result;
                    events.command.send(stream, line);
                }
                buffer.clear();
                active.exchange(true);
                syncio.unlock();
                directvt::binary::stream::reading_loop(stream, [&](view data){ events.s11n::sync(data); });
                syncio.lock();
                active.exchange(faux);
                break;
            }
            else
            {
                syncio.unlock();
                if (logmsg)
                {
                    log("%%Waiting for server...", prompt::main);
                    logmsg = faux;
                }
                os::sleep(500ms);
                syncio.lock();
            }
        }
        syncio.unlock();
    }
    else if (whoami == type::runapp)
    {
        auto& indexer = ui::tui_domain();
        app::shared::load::settings(indexer.config, cliopt);
        auto shadow = params;
        auto apname = view{};
        auto aptype = text{};
        utf::to_lower(shadow);
             if (shadow.starts_with(app::vtty::id))      { aptype = app::teletype::id;  apname = app::teletype::name;  }
        else if (shadow.starts_with(app::term::id))      { aptype = app::terminal::id;  apname = app::terminal::name;  }
        else if (shadow.starts_with(app::dtvt::id))      { aptype = app::dtvt::id;      apname = app::dtvt::name;      }
        else if (shadow.starts_with(app::dtty::id))      { aptype = app::dtty::id;      apname = app::dtty::name;      }
        //#if defined(DEBUG)
        else if (shadow.starts_with(app::calc::id))      { aptype = app::calc::id;      apname = app::calc::name;      }
        else if (shadow.starts_with(app::shop::id))      { aptype = app::shop::id;      apname = app::shop::name;      }
        else if (shadow.starts_with(app::test::id))      { aptype = app::test::id;      apname = app::test::name;      }
        else if (shadow.starts_with(app::empty::id))     { aptype = app::empty::id;     apname = app::empty::name;     }
        else if (shadow.starts_with(app::strobe::id))    { aptype = app::strobe::id;    apname = app::strobe::name;    }
        else if (shadow.starts_with(app::textancy::id))  { aptype = app::textancy::id;  apname = app::textancy::name;  }
        else if (shadow.starts_with(app::parvion::id))   { aptype = app::parvion::id;   apname = app::parvion::name;   }
        else if (shadow.starts_with(app::truecolor::id)) { aptype = app::truecolor::id; apname = app::truecolor::name; }

        else if (shadow.starts_with(app::app1::id)) { aptype = app::app1::id; apname = app::app1::name; }
        //#endif
        else if (shadow.starts_with("ssh"))//app::ssh::id))
        {
            params = " "s + params;
            aptype = app::dtty::id;
            apname = app::dtty::name;
        }
        else
        {
            params = " "s + params;
            aptype = app::teletype::id;
            apname = app::teletype::name;
        }
        log("%appname% %version%", apname, app::shared::version);
        auto coor = params.find(' ') + 1; // npos+1=0
        params = params.substr(coor ? coor : params.size());
        app::shared::start(params, aptype, title);
    }
    else
    {
        auto config = xml::settings{};
        app::shared::load::settings(config, cliopt);
        auto client = os::ipc::socket::open<os::role::client, faux>(prefix, denied);
        auto signal = ptr::shared<os::fire>(os::process::started(prefix)); // Signaling that the server is ready for incoming connections.

             if (denied)                           return failed(code::noaccess);
        else if (whoami != type::client && client) return failed(code::interfer);
        else if (whoami == type::client && !client)
        {
            log("%%New tile session for [%userid%]", prompt::main, userid.first);
            auto [success, successor] = os::process::fork(system, prefix_base, config.settings::utf8());
            if (successor)
            {
                whoami = type::server;
                script = {};
            }
            else
            {
                if (success) signal->wait(10s); // Waiting for confirmation of receiving the configuration.
                else         return failed(code::noserver);
            }
        }

        if (whoami == type::client)
        {
            signal.reset();
            if (client || (client = os::ipc::socket::open<os::role::client>(prefix, denied)))
            {
                auto userinit = directvt::binary::init{};
                auto env = os::env::add();
                auto cwd = os::env::cwd();
                auto cmd = script;
                auto win = os::dtvt::gridsz;
                userinit.send(client, userid.first, os::dtvt::vtmode, env, cwd, cmd, win);
                ui::tui_domain().config.swap(config);
                app::shared::get_tui_config(ui::tui_domain().config, ui::skin::globals());
                app::shared::splice(client);
                return 0;
            }
            else return failed(denied ? code::noaccess : code::noserver);
        }

        if (whoami == type::daemon)
        {
            auto [success, successor] = os::process::fork(system, prefix_base, config.settings::utf8(), script);
            if (successor)
            {
                whoami = type::server;
            }
            else
            {
                if (success)
                {
                    signal->wait(10s); // Waiting for confirmation of receiving the configuration.
                    return 0;
                }
                else return failed(code::nodaemon);
            }
        }

        os::ipc::prefix = prefix;
        auto server = os::ipc::socket::open<os::role::server>(prefix, denied);
        if (!server)
        {
            if (denied) failed(code::noaccess);
            return      failed(code::noserver);
        }
        auto srvlog = os::ipc::socket::open<os::role::server>(prefix_log, denied);
        if (!srvlog)
        {
            if (denied) failed(code::noaccess);
            return      failed(code::nosrvlog);
        }

        signal->bell(); // Signal we are started and ready for connections.
        signal.reset();

        namespace e2 = ui::e2;
        auto& indexer = ui::tui_domain();
        indexer.config.swap(config);

        auto tile_session = app::tile::hall(server, { .cmd = params });

        auto registry = tile_session_reg::scoped{ prefix, userid.first, os::env::cwd(), params };

        log("%%Tile session started"
            "\n      user: %userid%"
            "\n      pipe: %prefix%", prompt::main, userid.first, prefix);

        auto stdlog = std::thread{ [&]
        {
            while (auto monitor = srvlog->meet())
            {
                tile_session.submit([&, monitor](auto /*task_id*/)
                {
                    auto id = text{};
                    auto active = faux;
                    auto tokens = subs{};
                    auto onecmd = eccc{};
                    auto events = os::tty::binary::logger{ [&, init = 0](auto& events, auto& cmd) mutable
                    {
                        if (active)
                        {
                            onecmd.cmd = cmd;
                            // No command dispatch for tile mode (no e2::command::run handler).
                        }
                        else
                        {
                                 if (init == 0) id = cmd;
                            else if (init == 1) onecmd.env = cmd;
                            else if (init == 2)
                            {
                                active = true;
                                onecmd.cwd = cmd;
                                log("%%Monitor [%id%] connected", prompt::logs, id);
                            }
                            init++;
                        }
                        events.command.send(monitor, onecmd.cmd); // Output reply.
                    }};
                    auto writer = netxs::logger::attach([&](auto utf8)
                    {
                        events.logs.send(monitor, ui32{}, datetime::now(), text{ utf8 });
                    });
                    tile_session.applet->LISTEN(tier::general, e2::conio::quit, deal, tokens) { monitor->shut(); };
                    os::ipc::monitors++;
                    directvt::binary::stream::reading_loop(monitor, [&](view data){ events.s11n::sync(data); });
                    os::ipc::monitors--;
                    if (id.size()) log("%%Monitor [%id%] disconnected", prompt::logs, id);
                });
            }
        }};
        auto result = tile_session.run(server, userid, prefix);
        srvlog->stop();
        stdlog.join();
        tile_session.stop();  // Wait for all async tasks to complete
        indexer.stop(); // Stop quartz timer and jobs agent (matches desktop mode's base::dequeue()).
        return result;
    }

    os::release();
}
