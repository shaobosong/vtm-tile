// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// Parvion message-log model inspired by FileZilla's logger + StatusView path.
// It handles generation gates, the "Show detailed log" pending queue, and a
// bounded committed log buffer. Rendering and storage policy stay in Parvion UI.

#include "../../desktopio/intmath.hpp"
#include "../../desktopio/utf.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <utility>

namespace netxs::app::parvion
{
    enum class logtype : si32 { status, error, command, response, trace, listing };

    enum log_debug : si32
    {
        log_debug_none    = 0,
        log_debug_warning = 1,
        log_debug_info    = 2,
        log_debug_verbose = 3,
        log_debug_debug   = 4,
    };

    inline auto log_debug_label(si32 level) -> view
    {
        static constexpr auto names = std::array<view, 5>{ "None", "Warning", "Info", "Verbose", "Debug" };
        return names[(size_t)std::clamp(level, 0, 4)];
    }

    struct logline
    {
        logtype type = logtype::status;
        text    body;
        text    stamp;
        si32    level = 0;
    };

    struct message_logger
    {
        static constexpr auto default_cap = size_t{ 1000 };

        std::deque<logline> lines;
        std::deque<logline> queued;
        ui64 epoch = 0;
        size_t cap = default_cap;
        si32 debug_level = log_debug_none;
        bool raw_listing = faux;
        bool show_detailed = faux;

        auto should_queue() const -> bool
        {
            return !show_detailed && debug_level == log_debug_none && !raw_listing;
        }

        auto should_generate(logtype type, si32 level = 0) const -> bool
        {
            if (type == logtype::trace)   return debug_level >= level;
            if (type == logtype::listing) return raw_listing;
            return true;
        }

        void set_debug_level(si32 level)
        {
            debug_level = std::clamp(level, 0, 4);
            if (!should_queue()) flush();
        }

        void set_raw_listing(bool enabled)
        {
            raw_listing = enabled;
            if (!should_queue()) flush();
        }

        void set_show_detailed(bool enabled)
        {
            show_detailed = enabled;
            if (!should_queue()) flush();
        }

        void clear()
        {
            lines.clear();
            queued.clear();
            ++epoch;
        }

        void log(logtype type, text body, text stamp = {}, si32 level = 0)
        {
            if (!should_generate(type, level)) return;

            auto line = logline{ type, std::move(body), std::move(stamp), level };
            switch (type)
            {
                case logtype::error:
                    flush();
                    append(std::move(line));
                    break;
                case logtype::status:
                    queued.clear();
                    append(std::move(line));
                    break;
                case logtype::command:
                case logtype::response:
                    if (should_queue()) queued.push_back(std::move(line));
                    else                append(std::move(line));
                    break;
                default:
                    append(std::move(line));
                    break;
            }
        }

        void flush()
        {
            for (auto& line : queued) append(std::move(line));
            queued.clear();
        }

    private:
        void append(logline line)
        {
            lines.push_back(std::move(line));
            while (lines.size() > cap) lines.pop_front();
            ++epoch;
        }
    };
}
