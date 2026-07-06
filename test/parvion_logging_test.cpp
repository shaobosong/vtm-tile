// Copyright (c) Shaobo Song
// Licensed under the MIT license.

// Unit tests for the Parvion logger: FileZilla-style detailed queueing,
// debug/listing generation gates, and clear behavior.

#include "netxs/apps/parvion/logging.hpp"

#include <cstdio>

using netxs::app::parvion::message_logger;
using netxs::app::parvion::logtype;

namespace
{
    using lt = logtype;

    auto body(message_logger const& log, size_t i) -> netxs::text
    {
        return i < log.lines.size() ? log.lines[i].body : netxs::text{};
    }

    auto test_command_response_queue_until_error() -> bool
    {
        auto log = message_logger{};
        log.log(lt::command, "cmd");
        log.log(lt::response, "resp");
        if (!log.lines.empty() || log.queued.size() != 2) return faux;
        log.log(lt::error, "bad");
        return log.lines.size() == 3
            && body(log, 0) == "cmd"
            && body(log, 1) == "resp"
            && body(log, 2) == "bad"
            && log.queued.empty();
    }

    auto test_status_clears_queued_detail() -> bool
    {
        auto log = message_logger{};
        log.log(lt::command, "cmd");
        log.log(lt::response, "resp");
        log.log(lt::status, "ok");
        return log.lines.size() == 1 && body(log, 0) == "ok" && log.queued.empty();
    }

    auto test_listing_status_visible_command_queued() -> bool
    {
        auto log = message_logger{};
        log.log(lt::status, "Retrieving directory listing of \"/\"...");
        log.log(lt::command, "ls");
        return log.lines.size() == 1
            && body(log, 0) == "Retrieving directory listing of \"/\"..."
            && log.queued.size() == 1
            && log.queued.front().body == "ls";
    }

    auto test_enabling_detailed_flushes_queue() -> bool
    {
        auto log = message_logger{};
        log.log(lt::command, "cmd");
        log.log(lt::response, "resp");
        log.set_show_detailed(true);
        log.log(lt::command, "cmd2");
        return log.lines.size() == 3
            && body(log, 0) == "cmd"
            && body(log, 1) == "resp"
            && body(log, 2) == "cmd2"
            && log.queued.empty();
    }

    auto test_debug_level_gates_trace() -> bool
    {
        auto log = message_logger{};
        log.log(lt::trace, "warn", {}, netxs::app::parvion::log_debug_warning);
        if (!log.lines.empty()) return faux;
        log.set_debug_level(netxs::app::parvion::log_debug_info);
        log.log(lt::trace, "info", {}, netxs::app::parvion::log_debug_info);
        log.log(lt::trace, "verbose", {}, netxs::app::parvion::log_debug_verbose);
        return log.lines.size() == 1 && body(log, 0) == "info";
    }

    auto test_raw_listing_gate() -> bool
    {
        auto log = message_logger{};
        log.log(lt::listing, "raw");
        if (!log.lines.empty()) return faux;
        log.set_raw_listing(true);
        log.log(lt::listing, "raw");
        return log.lines.size() == 1 && body(log, 0) == "raw";
    }

    auto test_clear_removes_committed_and_queued() -> bool
    {
        auto log = message_logger{};
        log.set_show_detailed(true);
        log.log(lt::status, "visible");
        log.set_show_detailed(false);
        log.log(lt::command, "hidden");
        log.clear();
        log.set_show_detailed(true);
        return log.lines.empty() && log.queued.empty();
    }
}

int main()
{
    struct { char const* name; bool (*fn)(); } tests[] =
    {
        { "command_response_queue_until_error", test_command_response_queue_until_error },
        { "status_clears_queued_detail",        test_status_clears_queued_detail },
        { "listing_status_visible_command_queued", test_listing_status_visible_command_queued },
        { "enabling_detailed_flushes_queue",    test_enabling_detailed_flushes_queue },
        { "debug_level_gates_trace",            test_debug_level_gates_trace },
        { "raw_listing_gate",                   test_raw_listing_gate },
        { "clear_removes_committed_and_queued", test_clear_removes_committed_and_queued },
    };

    auto failed = 0;
    for (auto& t : tests)
    {
        auto ok = t.fn();
        std::printf("%-38s %s\n", t.name, ok ? "PASS" : "FAIL");
        if (!ok) ++failed;
    }
    return failed ? 1 : 0;
}
