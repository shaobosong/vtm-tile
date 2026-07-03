// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/transfer_view.hpp: a THIN configuration over the reusable table core (table.hpp) that
// presents the controller's transfer queue, filtered to one status (Transferring / Failed /
// Succeeded). This is a tab adapter over the independent table widget: it builds a table_cfg
// (columns + per-cell data + selection/menu adapters + follow/expand hooks), calls make_table(), and
// wraps the widget as a tab_page.
//
// Each status tab is an INDEPENDENT make_transfer_view() instance with its own column widths and
// scroll — the three tabs share nothing but the controller's data.

#include "queue_actions.hpp"
#include "tab_page.hpp"
#include "table.hpp"

namespace netxs::app::parvion
{
    // Resizable transfer columns: Local Name, Remote Name, Size, Progress, Speed (+ a "Reason"
    // column, logical index q_ncol, on the Failed tab). Index order is shared by the width array,
    // the column builder, the cell accessor, and ctrl->col_shown.
    static constexpr auto q_ncol      = si32{ 5 };
    static constexpr auto q_headers   = std::array<view, q_ncol + 1>{ "Local Name", "Remote Name", "Size", "Progress", "Speed", "Reason" };
    static constexpr auto q_name_x    = si32{ 7 };  // First resizable column x (after the arrow + expand gutter).
    static constexpr auto q_reason_w0 = si32{ 20 }; // Initial Failed-tab "Reason" column width.

    // A flattened display row: a parent task (child == -1) or one of its expanded parallel subtasks.
    struct disp_row { si32 qi; si32 child; };

    // This view's session-only, per-instance column widths (0 override = auto / initial).
    struct xfer_cols { std::array<si32, q_ncol> col_w{ 24, 24, 11, 10, 11 }; si32 reason_w_override = 0; };

    inline auto tab_status_match(si32 status, queue_item const& it) -> bool
    {
        return (status == 0 && (it.status == queue_item::queued || it.status == queue_item::transferring))
            || (status == 1 && it.status == queue_item::failed)
            || (status == 2 && it.status == queue_item::succeeded);
    }
    // Reconstruct a file's parallel-chunk byte ranges (mirrors start_item's csz math in session.hpp).
    inline auto chunk_ranges(si64 size, ui32 count) -> std::vector<std::pair<si64, si64>>
    {
        auto out = std::vector<std::pair<si64, si64>>{};
        if (count == 0 || size < 0) return out;
        auto csz = (si64)((size + count - 1) / count);
        for (auto c = ui32{}; c < count; ++c) { auto start = (si64)c * csz; if (start >= size) break; out.emplace_back(start, std::min(start + csz, size)); }
        return out;
    }
    // Flatten the queue (this status) into display rows (parents + expanded subtasks).
    inline auto xfer_rows(sftp_remote* ctrl, si32 status) -> std::vector<disp_row>
    {
        auto out = std::vector<disp_row>{};
        if (!ctrl) return out;
        for (auto qi = si32{}; qi < (si32)ctrl->queue.size(); ++qi)
        {
            auto& it = ctrl->queue[(size_t)qi];
            if (!tab_status_match(status, it)) continue;
            out.push_back({ qi, -1 });
            if (it.expanded && it.chunk_count > 1)
                for (auto c = si32{}, nr = (si32)chunk_ranges(it.size, it.chunk_count).size(); c < nr; ++c) out.push_back({ qi, c });
        }
        return out;
    }
    inline auto xfer_reason_w(xfer_cols const& cols, si32 status) -> si32 { return status != 1 ? 0 : (cols.reason_w_override > 0 ? cols.reason_w_override : q_reason_w0); }
    inline auto xfer_col_visible(sftp_remote* ctrl, si32 status, si32 i) -> bool
    {
        if (!ctrl || i < 0 || i > q_ncol) return faux;
        return i == q_ncol ? (status == 1 && ctrl->col_shown[(size_t)q_ncol]) : ctrl->col_shown[(size_t)i];
    }
    // Widest content in transfer column `col` (for double-click auto-fit).
    inline auto xfer_content_w(sftp_remote* ctrl, si32 status, si32 col) -> si32
    {
        auto w = (si32)cell_width(q_headers[(size_t)col]);
        if (!ctrl) return w;
        auto pct = [](double p){ auto b = std::array<char, 24>{}; std::snprintf(b.data(), b.size(), "%.2f%%", p); return text{ b.data() }; };
        for (auto& it : ctrl->queue)
        {
            if (!tab_status_match(status, it)) continue;
            auto s = text{};
            switch (col)
            {
                case 0:  s = it.local_path;  break;
                case 1:  s = it.remote_path; break;
                case 2:  s = human_size(it.size); break;
                case 3:  s = it.status == queue_item::queued ? (it.paused ? text{ "paused" } : text{ "queued" }) : pct(it.size > 0 ? 100.0 * (double)it.done / (double)it.size : 0.0); break;
                case 4:  if (it.rate.speed > 0.0) s = human_size((si64)(it.rate.speed + 0.5)) + "/s"; break;
                default: s = it.error; break;
            }
            w = std::max(w, (si32)cell_width(s));
            if (col == 0 && it.expanded && it.chunk_count > 1)
            {
                auto ranges = chunk_ranges(it.size, it.chunk_count);
                for (auto c = si32{}; c < (si32)ranges.size(); ++c)
                {
                    auto [start, end] = ranges[(size_t)c];
                    w = std::max(w, (si32)cell_width("Part " + std::to_string(c + 1) + "/" + std::to_string(it.chunk_count) + "  (" + human_size(start) + "–" + human_size(end) + ")"));
                }
            }
        }
        return w;
    }
    // Build the transfer column model (visible columns + roster + width/visibility/autofit hooks).
    inline auto xfer_columns(sftp_remote* ctrl, si32 status, std::shared_ptr<xfer_cols> cols) -> qtable
    {
        auto t = qtable{};
        t.left = q_name_x;
        if (!ctrl) return t;
        for (auto i = si32{}; i < q_ncol; ++i)
            if (ctrl->col_shown[(size_t)i])
                t.cols.push_back(qtable::column{ text{ q_headers[(size_t)i] }, cols->col_w[(size_t)i], /*right*/ i >= 2 && i <= 4, true, i });
        if (status == 1 && ctrl->col_shown[(size_t)q_ncol])
            t.cols.push_back(qtable::column{ text{ q_headers[(size_t)q_ncol] }, xfer_reason_w(*cols, status), faux, true, q_ncol });
        auto n = status == 1 ? q_ncol + 1 : q_ncol;
        for (auto i = si32{}; i < n; ++i) t.roster.push_back({ text{ q_headers[(size_t)i] }, i, ctrl->col_shown[(size_t)i] });
        t.set_shown = [ctrl](si32 key, bool on){ if (key >= 0 && key < (si32)ctrl->col_shown.size()) ctrl->col_shown[(size_t)key] = on; };
        t.resize    = [cols](si32 key, si32 w){ if (key < q_ncol) cols->col_w[(size_t)key] = w; else cols->reason_w_override = w; };
        t.autofit   = [ctrl, status](si32 key){ return xfer_content_w(ctrl, status, key); };
        return t;
    }
    // The transfer selection adapter: keys are queue indices; parent rows are selectable, subtask
    // children are not.
    inline auto xfer_sel(sftp_remote* ctrl, si32 status) -> qsel_cfg
    {
        auto rows = std::make_shared<std::vector<disp_row>>(xfer_rows(ctrl, status));
        auto cfg = qsel_cfg{};
        cfg.key_count  = [ctrl]{ return (si32)ctrl->queue.size(); };
        cfg.is_sel     = [ctrl](si32 k){ return k >= 0 && k < (si32)ctrl->queue.size() && ctrl->queue[(size_t)k].selected; };
        cfg.set_sel    = [ctrl](si32 k, bool v){ if (k >= 0 && k < (si32)ctrl->queue.size()) ctrl->queue[(size_t)k].selected = v; };
        cfg.clear      = [ctrl]{ for (auto& it : ctrl->queue) it.selected = faux; };
        cfg.any        = [ctrl]{ for (auto& it : ctrl->queue) if (it.selected) return true; return faux; };
        cfg.in_scope   = [ctrl, status](si32 k){ return k >= 0 && k < (si32)ctrl->queue.size() && tab_status_match(status, ctrl->queue[(size_t)k]); };
        cfg.disp       = [rows]{ return (si32)rows->size(); };
        cfg.key_of_row = [rows](si32 i){ return i >= 0 && i < (si32)rows->size() && (*rows)[(size_t)i].child == -1 ? (*rows)[(size_t)i].qi : -1; };
        return cfg;
    }
    // Per-item right-click menu (Start / Pause / Remove, + Pin to Top on Transferring).
    inline auto xfer_item_menu(sftp_remote* ctrl, si32 status, netxs::wptr<ui::base> panel_wp, netxs::wptr<ui::base> window_wp) -> std::vector<app::shared::menu::item>
    {
        namespace m = app::shared::menu;
        auto deface = [panel_wp]{ if (auto p = panel_wp.lock()) p->base::deface(); };
        auto sel    = [](queue_item const& it){ return it.selected; };
        auto items  = std::vector<m::item>{};
        auto add = [&](text label, auto fn){ auto row = m::item{ .alive = true, .label = std::move(label) }; row.action = [deface, fn](hids&){ fn(); deface(); }; items.push_back(std::move(row)); };
        add("Start", [ctrl, sel]{ ctrl->queue_start(sel); });
        if (status == 0) add("Pause", [ctrl, sel]{ ctrl->queue_pause(sel); });
        add("Remove", [ctrl, sel, panel_wp, window_wp]
        {
            auto count = si32{}; for (auto& it : ctrl->queue) if (it.selected) ++count;
            if (!count) return;
            auto run = [ctrl, sel, panel_wp]{ if (auto p = panel_wp.lock()) { ctrl->queue_remove(sel); p->base::deface(); } };
            auto window = window_wp.lock(); if (!window) { run(); return; }
            app::shared::show_close_confirmation(*window, run, {}, app::shared::confirm_dialog_text{
                count == 1 ? text{ "Remove this transfer from the queue?" } : "Remove " + std::to_string(count) + " transfers from the queue?", "Remove", "Cancel" });
        });
        auto pinnable = faux;
        if (status == 0) for (auto& it : ctrl->queue) if (it.selected && it.status == queue_item::queued) { pinnable = true; break; }
        if (pinnable) { items.push_back(m::item{ .alive = true, .type = m::kind::separator }); add("Pin to Top", [ctrl, sel]{ ctrl->queue_pin_top(sel); }); }
        return items;
    }
    // Blank-area right-click menu (Start All / Pause All / Remove All), scoped to this tab.
    inline auto xfer_blank_menu(sftp_remote* ctrl, si32 status, netxs::wptr<ui::base> panel_wp, netxs::wptr<ui::base> window_wp) -> std::vector<app::shared::menu::item>
    {
        namespace m = app::shared::menu;
        auto deface = [panel_wp]{ if (auto p = panel_wp.lock()) p->base::deface(); };
        auto scope  = [status](queue_item const& it){ return tab_status_match(status, it); };
        auto items  = std::vector<m::item>{};
        auto add = [&](text label, auto fn){ auto row = m::item{ .alive = true, .label = std::move(label) }; row.action = [deface, fn](hids&){ fn(); deface(); }; items.push_back(std::move(row)); };
        add("Start All", [ctrl, scope]{ ctrl->queue_start(scope); });
        if (status == 0) add("Pause All", [ctrl, scope]{ ctrl->queue_pause(scope); });
        add("Remove All", [ctrl, scope, panel_wp, window_wp]
        {
            auto count = si32{}; for (auto& it : ctrl->queue) if (scope(it)) ++count;
            if (!count) return;
            auto run = [ctrl, scope, panel_wp]{ if (auto p = panel_wp.lock()) { ctrl->queue_remove(scope); p->base::deface(); } };
            auto window = window_wp.lock(); if (!window) { run(); return; }
            app::shared::show_close_confirmation(*window, run, {}, app::shared::confirm_dialog_text{ "Remove all transfers on this tab?", "Remove", "Cancel" });
        });
        return items;
    }
    // One transfer cell's rendered text + colour.
    inline auto xfer_cell(sftp_remote* ctrl, si32 status, si32 row, si32 key) -> cellval
    {
        auto rows = xfer_rows(ctrl, status);
        if (row < 0 || row >= (si32)rows.size()) return {};
        auto qi = rows[(size_t)row].qi, child = rows[(size_t)row].child;
        auto& it = ctrl->queue[(size_t)qi];
        auto dir_fg = it.download ? ui32{ theme::dir_fg } : ui32{ theme::link_fg };
        auto pct = [](double p){ auto b = std::array<char, 24>{}; std::snprintf(b.data(), b.size(), "%.2f%%", p); return text{ b.data() }; };
        if (child == -1)
        {
            switch (key)
            {
                case 0: return { it.local_path,  dir_fg };
                case 1: return { it.remote_path, dir_fg };
                case 2: return { human_size(it.size), theme::subtext };
                case 3:
                    if (it.status == queue_item::queued) return { it.paused ? text{ "paused" } : text{ "queued" }, theme::subtext };
                    else return { pct(it.size > 0 ? 100.0 * (double)it.done / (double)it.size : 0.0), it.status == queue_item::succeeded ? ui32{ theme::dir_fg } : it.status == queue_item::failed ? ui32{ theme::err_fg } : ui32{ theme::text_fg } };
                case 4: return it.rate.speed > 0.0 ? cellval{ human_size((si64)(it.rate.speed + 0.5)) + "/s", theme::subtext } : cellval{};
                default: return it.status == queue_item::failed && !it.error.empty() ? cellval{ it.error, theme::err_fg } : cellval{}; // Reason.
            }
        }
        // Expanded subtask child row: only the Local Name label + Progress cells.
        auto ranges = chunk_ranges(it.size, it.chunk_count);
        if (child >= (si32)ranges.size()) return {};
        auto [start, end] = ranges[(size_t)child];
        if (key == 0) return { "Part " + std::to_string(child + 1) + "/" + std::to_string(it.chunk_count) + "  (" + human_size(start) + "–" + human_size(end) + ")", theme::subtext };
        if (key == 3)
        {
            auto live = qi == ctrl->active && ctrl->workers.size() == ranges.size();
            if (live)
            {
                auto& wkr = *ctrl->workers[(size_t)child];
                auto full = (double)(end - start);
                if (wkr.state == xfer_worker::s_ok) return { "100.00%", theme::dir_fg };
                return { pct(full > 0.0 ? std::min(100.0, 100.0 * (double)wkr.done / full) : 0.0), wkr.state == xfer_worker::s_err ? ui32{ theme::err_fg } : ui32{ theme::text_fg } };
            }
            if (it.status == queue_item::succeeded) return { "100.00%", theme::dir_fg };
            if (it.status == queue_item::queued)    return { "queued",  theme::subtext };
            if (it.status == queue_item::failed)    return { "—",       theme::err_fg };
        }
        return {};
    }
    // The direction arrow + expand button for a parent row (empty for subtask children).
    inline auto xfer_gutter(sftp_remote* ctrl, si32 status, si32 row) -> gutterval
    {
        auto rows = xfer_rows(ctrl, status);
        if (row < 0 || row >= (si32)rows.size() || rows[(size_t)row].child != -1) return {};
        auto qi = rows[(size_t)row].qi;
        auto& it = ctrl->queue[(size_t)qi];
        auto arrow_fg = it.download ? ui32{ theme::dir_fg } : ui32{ theme::link_fg };
        auto expand = it.chunk_count > 1 ? (it.expanded ? xp_expanded : xp_collapsed) : xp_muted;
        return { it.download ? text{ "↓" } : text{ "↑" }, arrow_fg, qi, expand };
    }

    // Build the complete transfer-view table_cfg for one status tab.
    inline auto make_transfer_view(sftp_remote* ctrl, si32 status, netxs::wptr<ui::base> window_wp) -> tab_page_ptr
    {
        auto cols = std::make_shared<xfer_cols>();
        auto title = [ctrl, status]
        {
            auto n = si32{}; if (ctrl) for (auto& it : ctrl->queue) if (tab_status_match(status, it)) ++n;
            auto base = status == 0 ? "Transferring" : status == 1 ? "Failed" : "Succeeded";
            return text{ base } + " (" + std::to_string(n) + ")";
        };
        auto cfg = table_cfg{};
        cfg.ctrl = ctrl;
        cfg.window_wp = window_wp;
        cfg.columns   = [ctrl, status, cols]{ return xfer_columns(ctrl, status, cols); };
        cfg.rows      = [ctrl, status]{ return (si32)xfer_rows(ctrl, status).size(); };
        cfg.cell      = [ctrl, status](si32 row, si32 key){ return xfer_cell(ctrl, status, row, key); };
        cfg.gutter    = [ctrl, status](si32 row){ return xfer_gutter(ctrl, status, row); };
        cfg.toggle    = [ctrl](si32 qi){ if (qi >= 0 && qi < (si32)ctrl->queue.size()) ctrl->queue[(size_t)qi].expanded = !ctrl->queue[(size_t)qi].expanded; };
        cfg.selection = [ctrl, status]{ return xfer_sel(ctrl, status); };
        cfg.menu      = [ctrl, status, window_wp](netxs::wptr<ui::base> panel_wp)
        {
            auto deface = [panel_wp]{ if (auto p = panel_wp.lock()) p->base::deface(); };
            auto mc = qmenu_cfg{};
            mc.item  = [ctrl, status, panel_wp, window_wp](si32){ return xfer_item_menu(ctrl, status, panel_wp, window_wp); };
            mc.blank = [ctrl, status, panel_wp, window_wp]{ return xfer_blank_menu(ctrl, status, panel_wp, window_wp); };
            mc.on_item_rclick = [ctrl, deface](si32 hit){ if (hit >= 0 && hit < (si32)ctrl->queue.size() && !ctrl->queue[(size_t)hit].selected) { for (auto& it : ctrl->queue) it.selected = faux; ctrl->queue[(size_t)hit].selected = true; deface(); } };
            mc.on_blank_rclick = [ctrl, deface]{ auto any = faux; for (auto& it : ctrl->queue) { any |= it.selected; it.selected = faux; } if (any) deface(); };
            return mc;
        };
        cfg.follow      = [ctrl, status]{ auto rows = xfer_rows(ctrl, status); for (auto i = si32{}; i < (si32)rows.size(); ++i) if (rows[(size_t)i].child == -1 && rows[(size_t)i].qi == ctrl->active) return i; return -1; };
        cfg.on_col_grab = [status, cols](si32 key){ if (key == q_ncol && cols->reason_w_override == 0) cols->reason_w_override = xfer_reason_w(*cols, status); };
        cfg.empty_text  = []{ return text{ "(no transfers — press Enter on a file to queue one)" }; };
        cfg.on_key      = [ctrl, window_wp](hids& gear, netxs::wptr<ui::base> self){ return clear_finished_on_key(gear, ctrl, window_wp, self); };
        cfg.arrow_nav   = true;
        return make_tab_page(make_table(std::move(cfg)), std::move(title));
    }
}
