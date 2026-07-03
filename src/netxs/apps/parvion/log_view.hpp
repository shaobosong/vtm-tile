// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/log_view.hpp: a THIN configuration over the reusable text-view core (textbox.hpp) that
// presents the controller's message log (typed, colour-coded, FileZilla-style). This is a tab adapter
// over the independent textbox widget.

#include "queue_actions.hpp"
#include "tab_page.hpp"
#include "textbox.hpp"

namespace netxs::app::parvion
{
    // FileZilla StatusView prefix + colour per message type.
    inline auto log_prefix(logtype t) -> view
    {
        using lt = logtype;
        switch (t)
        {
            case lt::error:    return "Error: ";
            case lt::command:  return "Command: ";
            case lt::response: return "Response: ";
            case lt::trace:    return "Trace: ";
            case lt::listing:  return "Listing: ";
            default:           return "Status: ";
        }
    }
    inline auto log_color(logtype t) -> ui32
    {
        using lt = logtype;
        switch (t)
        {
            case lt::error:    return theme::err_fg;
            case lt::command:  return theme::dir_fg;
            case lt::response: return theme::link_fg;
            case lt::trace:    return theme::trace_fg;
            case lt::listing:  return theme::link_fg;
            default:           return theme::subtext;
        }
    }
    // "Show detailed log" gates Command / Response / Trace; the debug level additionally gates Trace.
    inline auto log_visible(sftp_remote::logline const& ln, bool show_detailed, si32 debug_level) -> bool
    {
        using lt = logtype;
        if (ln.type == lt::trace)                              return show_detailed && debug_level >= ln.level;
        if (ln.type == lt::command || ln.type == lt::response) return show_detailed;
        return true;
    }

    // Memoized filtered-visible-lines cache: rebuilt only when the log contents or filters change, so
    // the text-view's per-line queries stay cheap even with a full (1000-line) log.
    struct log_vis_cache
    {
        std::vector<sftp_remote::logline const*> vis;
        size_t      sig_size = (size_t)-1;
        bool        sig_detail = faux;
        si32        sig_level = -1;
        const void* sig_front = nullptr;
        const void* sig_back  = nullptr;
        auto get(sftp_remote* ctrl) -> std::vector<sftp_remote::logline const*> const&
        {
            auto front = ctrl->logbuf.empty() ? nullptr : (const void*)&ctrl->logbuf.front();
            auto back  = ctrl->logbuf.empty() ? nullptr : (const void*)&ctrl->logbuf.back();
            if (sig_size != ctrl->logbuf.size() || sig_detail != ctrl->show_detailed || sig_level != ctrl->debug_level || sig_front != front || sig_back != back)
            {
                vis.clear();
                for (auto& ln : ctrl->logbuf) if (log_visible(ln, ctrl->show_detailed, ctrl->debug_level)) vis.push_back(&ln);
                sig_size = ctrl->logbuf.size(); sig_detail = ctrl->show_detailed; sig_level = ctrl->debug_level; sig_front = front; sig_back = back;
            }
            return vis;
        }
    };

    // The Message-log right-click menu (Show detailed log / Copy to clipboard / Clear all + a Log
    // level radio submenu). The text-view core prepends the selection "Copy" item.
    inline auto build_log_menu(sftp_remote* ctrl, netxs::wptr<ui::base> panel_wp) -> std::vector<app::shared::menu::item>
    {
        namespace m = app::shared::menu;
        auto deface = [panel_wp]{ if (auto p = panel_wp.lock()) p->base::deface(); };
        auto items = std::vector<m::item>{};

        auto detail = m::item{ .alive = true, .label = "Show detailed log", .type = m::kind::check, .checked = ctrl->show_detailed };
        detail.action = [ctrl, deface](hids&){ ctrl->show_detailed = !ctrl->show_detailed; deface(); };
        items.push_back(std::move(detail));
        items.push_back(m::item{ .alive = true, .type = m::kind::separator });

        auto copy = m::item{ .alive = true, .label = "Copy to clipboard" };
        copy.action = [ctrl](hids& gear)
        {
            auto out = text{};
            for (auto& ln : ctrl->logbuf) if (log_visible(ln, ctrl->show_detailed, ctrl->debug_level))
            {
                if (ctrl->show_stamps) out += ln.stamp + " ";
                out += text{ log_prefix(ln.type) } + ln.body + "\n";
            }
            gear.set_clipboard(dot_00, out, mime::textonly);
        };
        items.push_back(std::move(copy));

        auto clear = m::item{ .alive = true, .label = "Clear all" };
        clear.action = [ctrl, deface](hids&){ ctrl->logbuf.clear(); ctrl->dirty = true; deface(); };
        items.push_back(std::move(clear));
        items.push_back(m::item{ .alive = true, .type = m::kind::separator });

        auto level = m::item{ .alive = true, .label = "Log level", .type = m::kind::radiomenu };
        static constexpr auto names = std::array<view, 5>{ "None", "Warning", "Info", "Verbose", "Debug" };
        for (auto i = si32{}; i < 5; ++i)
        {
            auto row = m::item{ .alive = true, .label = text{ names[(size_t)i] }, .checked = (ctrl->debug_level == i) };
            row.action = [ctrl, deface, i](hids&){ ctrl->debug_level = i; deface(); };
            level.children.push_back(std::move(row));
        }
        items.push_back(std::move(level));
        return items;
    }

    inline auto make_log_view(sftp_remote* ctrl, netxs::wptr<ui::base> window_wp) -> tab_page_ptr
    {
        auto cache = std::make_shared<log_vis_cache>();
        auto title = []{ return text{ "Message log" }; };
        auto cfg = textbox_cfg{};
        cfg.line_count = [ctrl, cache]{ return (si32)cache->get(ctrl).size(); };
        cfg.line       = [ctrl, cache](si32 i) -> std::vector<textseg>
        {
            auto& vis = cache->get(ctrl);
            if (i < 0 || i >= (si32)vis.size()) return {};
            auto& ln = *vis[(size_t)i];
            auto segs = std::vector<textseg>{};
            if (ctrl->show_stamps) segs.push_back({ ln.stamp + " ", theme::subtext });
            segs.push_back({ text{ log_prefix(ln.type) }, log_color(ln.type) });
            segs.push_back({ ln.body, log_color(ln.type) });
            return segs;
        };
        cfg.line_id    = [ctrl, cache](si32 i) -> const void* { auto& vis = cache->get(ctrl); return i >= 0 && i < (si32)vis.size() ? (const void*)vis[(size_t)i] : nullptr; };
        cfg.epoch      = [ctrl]{ return ctrl->show_stamps ? ui64{ 1 } : ui64{ 0 }; };
        cfg.empty_text = []{ return text{ "(no messages)" }; };
        cfg.menu       = [ctrl](netxs::wptr<ui::base> panel_wp){ return build_log_menu(ctrl, panel_wp); };
        cfg.on_key     = [ctrl, window_wp](hids& gear, netxs::wptr<ui::base> self){ return clear_finished_on_key(gear, ctrl, window_wp, self); };
        auto box = make_textbox(std::move(cfg));
        return make_tab_page(std::move(box.widget), std::move(title), {}, std::move(box.clear_selection));
    }
}
