// Copyright (c) Dmitry Sapozhnikov
// Licensed under the MIT license.

#include "netxs/apps.hpp"
#include "netxs/apps/desk.hpp"
#include "vtm-common.hpp"
#include "netxs/apps/tile.hpp"

using namespace netxs;

enum class type { client, server, daemon, logmon, runapp, config, sessions };
enum class code { noaccess, noserver, nodaemon, nosrvlog, interfer, errormsg };

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

int main(int argc, char* argv[])
{
    auto whoami = type::client;
    auto params = text{};
    auto cliopt = text{};
    auto errmsg = text{};
    auto vtpipe = text{};
    auto script = text{};
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
        else if (getopt.match("-l", "--listconfig"))
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
                "\n    vtm-tile [ -c <file> ][ -q ][ -r [ <type> ]][ <args...> ]"
                "\n    vtm-tile [ -c <file> ]  -l"
                "\n    vtm-tile --list-sessions"
                "\n    vtm-tile -v | -?"
                "\n"
                "\n  Options:"
                "\n"
                "\n    Without options, vtm-tile runs Tile Server and Tile Client."
                "\n"
                "\n    -h, -?, --help       Print command-line options."
                "\n    -v, --version        Print version."
                "\n    -l, --listconfig     Print configuration."
                "\n    --list-sessions      Print active tile sessions for the current user."
                "\n    -q, --quiet          Disable logging."
                "\n    -x, --script <cmds>  Specifies script commands."
                "\n    -c, --config <file>  Specifies a settings file to load or plain xml-data to overlay."
                "\n    -p, --pin <id>       Specifies the tile session id it will be pinned to."
                "\n    -s, --server         Run Tile Server."
                "\n    -d, --daemon         Run Tile Server in background."
                "\n    -m, --monitor        Run Log Monitor."
                "\n    -r, --, --run        Run applet standalone."
                "\n    <type>               Applet to run (vtty, term, dtvt, dtty)."
                "\n    <args...>            Applet arguments."
                "\n    --env <var=val>      Set environment variable."
                "\n    --cwd <path>         Set current working directory."
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
        app::shared::start(params, aptype);
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
