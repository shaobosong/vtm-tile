// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/log_view.hpp: a THIN configuration over the reusable text-view core
// (components/textbox.hpp) that presents the controller's message log (typed, colour-coded,
// FileZilla-style). This is a tab adapter over the independent textbox widget.

#include "queue_actions.hpp"
#include "components/tab_page.hpp"
#include "components/textbox.hpp"

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
    // Memoized visible-lines cache: rebuilt only when the committed log changes, so
    // the text-view's per-line queries stay cheap even with a full (1000-line) log.
    struct log_vis_cache
    {
        std::vector<sftp_remote::logline const*> vis;
        ui64        sig_epoch = (ui64)-1;
        auto get(sftp_remote* ctrl) -> std::vector<sftp_remote::logline const*> const&
        {
            if (sig_epoch != ctrl->logger.epoch)
            {
                vis.clear();
                for (auto& ln : ctrl->logger.lines) vis.push_back(&ln);
                sig_epoch = ctrl->logger.epoch;
            }
            return vis;
        }
    };

    // The Message-log right-click menu: Copy / Clear all / separator / Select all.
    inline auto build_log_menu(sftp_remote* ctrl,
                               netxs::wptr<ui::base> panel_wp,
                               app::shared::menu::item copy,
                               app::shared::menu::item select_all) -> std::vector<app::shared::menu::item>
    {
        namespace m = app::shared::menu;
        auto deface = [panel_wp]{ if (auto p = panel_wp.lock()) p->base::deface(); };
        auto items = std::vector<m::item>{};

        items.push_back(std::move(copy));

        auto clear = m::item{ .alive = true, .label = "C&lear All", .disabled = ctrl->logger.lines.empty() };
        clear.action = [ctrl, deface](hids&){ ctrl->logger.clear(); ctrl->dirty = true; deface(); };
        items.push_back(std::move(clear));

        items.push_back(m::item{ .alive = true, .type = m::kind::separator });

        items.push_back(std::move(select_all));
        return items;
    }

    inline auto make_log_view(sftp_remote* ctrl, netxs::wptr<ui::base> window_wp) -> tab_page_cfg
    {
        auto cache = std::make_shared<log_vis_cache>();
        auto title = []{ return text{ "Message Log" }; };
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
        cfg.menu       = [ctrl](netxs::wptr<ui::base> panel_wp, auto copy, auto select_all)
        {
            return build_log_menu(ctrl, panel_wp, std::move(copy), std::move(select_all));
        };
        cfg.on_key     = [ctrl, window_wp](hids& gear, netxs::wptr<ui::base> self){ return clear_finished_on_key(gear, ctrl, window_wp, self); };
        auto box = make_textbox(std::move(cfg));
        return make_tab_page(std::move(box), std::move(title));
    }
}
