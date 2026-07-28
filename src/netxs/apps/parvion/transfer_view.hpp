// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/transfer_view.hpp: a THIN configuration over the reusable table core
// (components/table.hpp) that presents the controller's transfer queue, filtered to one status
// (Transferring / Failed / Succeeded). This is a tab adapter over the independent table widget: it
// builds a table_cfg (columns + per-cell data + selection/menu adapters + follow/expand hooks),
// calls make_table(), and wraps the component in tab-page metadata.
//
// Each status tab is an INDEPENDENT make_transfer_view() instance with its own table presentation
// state — the three tabs share nothing but the controller's data.

#include "queue_actions.hpp"
#include "components/progressbar.hpp"
#include "components/tab_page.hpp"
#include "components/table.hpp"

namespace netxs::app::parvion
{
    // Resizable transfer columns: Server, Local Name, Remote Name, Size, Progress, Speed (+ a
    // "Reason" column, logical index q_ncol, on the Failed tab). Index order is shared by the
    // per-view width/visibility arrays, column builder, and cell accessor.
    enum xfer_col_key : si32 { q_server, q_local, q_remote, q_size, q_progress, q_speed };
    static constexpr auto q_ncol      = si32{ 6 };
    static constexpr auto q_headers   = std::array<view, q_ncol + 1>{ "Server", "Local Name", "Remote Name", "Size", "Progress", "Speed", "Reason" };
    static constexpr auto q_menu_headers = std::array<view, q_ncol + 1>{ "Ser&ver", "&Local Name", "&Remote Name", "&Size", "&Progress", "Sp&eed", "Re&ason" };
    static constexpr auto q_name_x    = si32{ 7 };  // First resizable column x (after the arrow + expand gutter).
    static constexpr auto q_reason_w0 = si32{ 31 }; // Initial Failed-tab "Reason" column width.

    // A flattened display row: a parent task (child == -1) or one of its expanded parallel subtasks.
    struct disp_row { si32 qi; si32 child; };

    // This view's session-only, per-instance column presentation (0 width = auto / initial).
    struct xfer_cols
    {
        // Server starts in auto mode so it fits the same user@host:port caption used by Checksums.
        std::array<si32, q_ncol> col_w{ 22, 25, 25, 9, 11, 11 };
        std::array<bool, q_ncol + 1> col_shown{ true, true, true, true, true, true, true };
        si32 reason_w_override = 0;
    };

    inline auto xfer_server(sftp_remote const* ctrl) -> text
    {
        return ctrl ? server_source(ctrl->host, ctrl->user, ctrl->port) : text{};
    }

    inline auto xfer_progress_fraction(si64 done, si64 size) -> double
    {
        return size > 0 ? progressbar_fraction((double)done / (double)size) : 0.0;
    }
    inline auto xfer_progress_text(double fraction) -> text
    {
        auto buf = std::array<char, 24>{};
        std::snprintf(buf.data(), buf.size(), "%.2f%%", progressbar_fraction(fraction) * 100.0);
        return text{ buf.data() };
    }
    using xfer_progress_cache = progressbar_cache;
    inline auto xfer_progress_cell(xfer_progress_cache& cache, si32 row,
                                   double fraction, text label, ui32 foreground) -> table_cell
    {
        return { cached_progressbar(cache, (ui64)(ui32)row, fraction,
                                    std::move(label), foreground) };
    }

    inline auto tab_status_match(si32 status, queue_item const& it) -> bool
    {
        return (status == 0 && (it.status == queue_item::queued || it.status == queue_item::transferring))
            || (status == 1 && it.status == queue_item::failed)
            || (status == 2 && it.status == queue_item::succeeded);
    }
    inline auto xfer_status_valid(si32 status) -> bool
    {
        return status >= 0 && status < (si32)queue_item::table_count;
    }
    inline auto xfer_selected(queue_item const& it, si32 status) -> bool
    {
        return xfer_status_valid(status) && it.table_ui[(size_t)status].selected;
    }
    inline void xfer_select(queue_item& it, si32 status, bool selected)
    {
        if (xfer_status_valid(status)) it.table_ui[(size_t)status].selected = selected;
    }
    inline auto xfer_expanded(queue_item const& it, si32 status) -> bool
    {
        return xfer_status_valid(status) && it.table_ui[(size_t)status].expanded;
    }
    inline void xfer_expand(queue_item& it, si32 status, bool expanded)
    {
        if (xfer_status_valid(status)) it.table_ui[(size_t)status].expanded = expanded;
    }
    inline auto xfer_selection_pred(si32 status)
    {
        return [status](queue_item const& it)
        {
            return tab_status_match(status, it) && xfer_selected(it, status);
        };
    }
    inline auto xfer_copy_payload(sftp_remote* ctrl, si32 status, text queue_item::* field) -> text
    {
        auto out = text{};
        if (!ctrl) return out;
        for (auto& it : ctrl->queue)
        {
            auto& value = it.*field;
            if (!tab_status_match(status, it) || !xfer_selected(it, status) || value.empty()) continue;
            if (!out.empty()) out += '\n';
            out += value;
        }
        return out;
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
            if (xfer_expanded(it, status) && it.chunk_count > 1)
                for (auto c = si32{}, nr = (si32)chunk_ranges(it.size, it.chunk_count).size(); c < nr; ++c) out.push_back({ qi, c });
        }
        return out;
    }
    inline auto xfer_reason_w(xfer_cols const& cols, si32 status) -> si32 { return status != 1 ? 0 : (cols.reason_w_override > 0 ? cols.reason_w_override : q_reason_w0); }
    inline auto xfer_col_visible(xfer_cols const& cols, si32 status, si32 i) -> bool
    {
        if (i < 0 || i > q_ncol) return faux;
        return i == q_ncol ? (status == 1 && cols.col_shown[(size_t)q_ncol])
                           : cols.col_shown[(size_t)i];
    }
    // Widest content in transfer column `col` (for double-click auto-fit).
    inline auto xfer_content_w(sftp_remote* ctrl, si32 status, si32 col) -> si32
    {
        auto w = si32{};
        if (!ctrl) return w;
        for (auto& it : ctrl->queue)
        {
            if (!tab_status_match(status, it)) continue;
            auto s = text{};
            switch (col)
            {
                case q_server:   s = xfer_server(ctrl); break;
                case q_local:    s = it.local_path;  break;
                case q_remote:   s = it.remote_path; break;
                case q_size:     s = human_size(it.size); break;
                case q_progress: s = it.status == queue_item::queued ? (it.paused ? text{ "paused" } : text{ "queued" }) : xfer_progress_text(xfer_progress_fraction(it.done, it.size)); break;
                case q_speed:    if (it.rate.speed > 0.0) s = human_size((si64)(it.rate.speed + 0.5)) + "/s"; break;
                default: s = it.error; break;
            }
            w = std::max(w, (si32)cell_width(s));
            if (col == q_local && xfer_expanded(it, status) && it.chunk_count > 1)
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
    inline auto xfer_text_order(view a, view b) -> si32
    {
        auto al = text{ a }; utf::to_lower(al);
        auto bl = text{ b }; utf::to_lower(bl);
        return al < bl ? -1 : bl < al ? 1 : 0;
    }
    inline auto xfer_value_order(auto const& a, auto const& b) -> si32
    {
        return a < b ? -1 : b < a ? 1 : 0;
    }
    inline auto xfer_progress_order(queue_item const& a, queue_item const& b) -> si32
    {
        // The active tab mixes queued and transferring rows. Keep status labels together, then
        // order percentage values numerically within each status.
        if (auto cmp = xfer_value_order(a.status, b.status)) return cmp;
        auto ap = a.size > 0 ? (long double)a.done / (long double)a.size : 0.0L;
        auto bp = b.size > 0 ? (long double)b.done / (long double)b.size : 0.0L;
        return xfer_value_order(ap, bp);
    }
    inline auto xfer_compare(sftp_remote* ctrl, std::vector<disp_row> const& rows,
                             si32 row_a, si32 row_b, si32 key) -> si32
    {
        if (!ctrl) return 0;
        if (row_a < 0 || row_b < 0 || row_a >= (si32)rows.size() || row_b >= (si32)rows.size()) return 0;
        // Children deliberately compare as their parent. stable_sort then retains the parent-first,
        // part-order layout while moving the whole expanded group as one unit.
        auto& a = ctrl->queue[(size_t)rows[(size_t)row_a].qi];
        auto& b = ctrl->queue[(size_t)rows[(size_t)row_b].qi];
        switch (key)
        {
            case q_server:   return 0; // Every row belongs to the controller's current server.
            case q_local:    return xfer_text_order(a.local_path, b.local_path);
            case q_remote:   return xfer_text_order(a.remote_path, b.remote_path);
            case q_size:     return xfer_value_order(a.size, b.size);
            case q_progress: return xfer_progress_order(a, b);
            case q_speed:    return xfer_value_order(a.rate.speed, b.rate.speed);
            default: return xfer_text_order(a.error, b.error); // Failed-tab Reason.
        }
    }
    // Build the transfer column model (visible columns + roster + width/visibility/autofit hooks).
    inline auto xfer_columns(sftp_remote* ctrl, si32 status, std::shared_ptr<xfer_cols> cols) -> qtable
    {
        auto t = qtable{};
        t.left = q_name_x;
        if (!ctrl) return t;
        auto n = status == 1 ? q_ncol + 1 : q_ncol;
        for (auto i = si32{}; i < n; ++i)
        {
            auto width = i < q_ncol ? cols->col_w[(size_t)i] : xfer_reason_w(*cols, status);
            t.add_column({ text{ q_headers[(size_t)i] }, width, i >= q_size && i <= q_speed, true, i },
                         xfer_col_visible(*cols, status, i), text{ q_menu_headers[(size_t)i] });
        }
        t.on_show_column   = [cols](si32 key, bool on){ if (key >= 0 && key < (si32)cols->col_shown.size()) cols->col_shown[(size_t)key] = on; };
        t.on_resize_column = [cols](si32 key, si32 w){ if (key >= 0 && key < q_ncol) cols->col_w[(size_t)key] = w; else if (key == q_ncol) cols->reason_w_override = w; };
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
        cfg.is_selected   = [ctrl, status](si32 k){ return k >= 0 && k < (si32)ctrl->queue.size() && xfer_selected(ctrl->queue[(size_t)k], status); };
        cfg.on_select     = [ctrl, status](si32 k, bool v){ if (k >= 0 && k < (si32)ctrl->queue.size()) xfer_select(ctrl->queue[(size_t)k], status, v); };
        cfg.on_clear      = [ctrl, status]{ for (auto& it : ctrl->queue) xfer_select(it, status, faux); };
        cfg.has_selection = [ctrl, status]{ for (auto& it : ctrl->queue) if (tab_status_match(status, it) && xfer_selected(it, status)) return true; return faux; };
        cfg.in_scope   = [ctrl, status](si32 k){ return k >= 0 && k < (si32)ctrl->queue.size() && tab_status_match(status, ctrl->queue[(size_t)k]); };
        cfg.row_count  = [rows]{ return (si32)rows->size(); };
        cfg.key_of_row = [rows](si32 i){ return i >= 0 && i < (si32)rows->size() && (*rows)[(size_t)i].child == -1 ? (*rows)[(size_t)i].qi : -1; };
        return cfg;
    }
    // Unified right-click menu: selected-transfer actions when a selection exists, otherwise
    // tab-wide "All" actions for blank-area and header invocations, then tab-scoped selection.
    inline auto xfer_menu(sftp_remote* ctrl, si32 status, bool blank,
                          netxs::wptr<ui::base> panel_wp,
                          netxs::wptr<ui::base> window_wp) -> std::vector<app::shared::menu::item>
    {
        namespace m = app::shared::menu;
        auto deface = [panel_wp]{ if (auto p = panel_wp.lock()) p->base::deface(); };
        auto sel    = xfer_selection_pred(status);
        auto scope  = [status](queue_item const& it){ return tab_status_match(status, it); };
        auto items  = std::vector<m::item>{};
        auto any_selected = std::ranges::any_of(ctrl->queue, sel);
        auto any_in_scope = std::ranges::any_of(ctrl->queue, scope);
        auto bulk = blank || !any_selected;
        auto target = [bulk, sel, scope](queue_item const& it){ return bulk ? scope(it) : sel(it); };
        auto any_target = bulk ? any_in_scope : any_selected;
        auto pinnable = status == 0 && std::ranges::any_of(ctrl->queue, [sel](queue_item const& it)
        {
            return sel(it) && it.status == queue_item::queued;
        });
        auto add = [&](text label, bool disabled, auto fn)
        {
            auto row = m::item{ .alive = true, .label = std::move(label), .disabled = disabled };
            row.action = [deface, fn](hids&){ fn(); deface(); };
            items.push_back(std::move(row));
        };
        add(bulk ? "&Start All" : "&Start", !any_target, [ctrl, target]{ ctrl->queue_start(target); });
        if (status == 0)
            add(bulk ? "&Pause All" : "&Pause", !any_target, [ctrl, target]{ ctrl->queue_pause(target); });
        add(bulk ? "&Remove All" : "&Remove", !any_target, [ctrl, target, bulk, panel_wp, window_wp]
        {
            auto count = si32{}; for (auto& it : ctrl->queue) if (target(it)) ++count;
            if (!count) return;
            auto run = [ctrl, target, panel_wp]{ if (auto p = panel_wp.lock()) { ctrl->queue_remove(target); p->base::deface(); } };
            auto window = window_wp.lock(); if (!window) { run(); return; }
            app::shared::show_close_confirmation(*window, run, {}, app::shared::confirm_dialog_text{
                bulk ? text{ "Remove all transfers on this tab?" }
                     : count == 1 ? text{ "Remove this transfer from the queue?" }
                                  : "Remove " + std::to_string(count) + " transfers from the queue?",
                "Remove", "Cancel" });
        });
        if (status == 0) add("Pin to &Top", !pinnable, [ctrl, sel]{ ctrl->queue_pin_top(sel); });

        {
            auto local = xfer_copy_payload(ctrl, status, &queue_item::local_path);
            auto remote = xfer_copy_payload(ctrl, status, &queue_item::remote_path);
            auto reason = status == 1 ? xfer_copy_payload(ctrl, status, &queue_item::error) : text{};
            auto copy = m::item{ .alive = true, .label = "&Copy", .type = m::kind::dropdown, .disabled = local.empty() && remote.empty() && reason.empty() };
            auto local_name = m::item{ .alive = true, .label = "&Local Name", .disabled = local.empty() };
            local_name.action = [local](hids& g){ if (!local.empty()) g.set_clipboard(dot_00, local, mime::textonly); };
            copy.children.push_back(std::move(local_name));
            auto remote_name = m::item{ .alive = true, .label = "&Remote Name", .disabled = remote.empty() };
            remote_name.action = [remote](hids& g){ if (!remote.empty()) g.set_clipboard(dot_00, remote, mime::textonly); };
            copy.children.push_back(std::move(remote_name));
            if (status == 1)
            {
                auto failed_reason = m::item{ .alive = true, .label = "&Failed Reason", .disabled = reason.empty() };
                failed_reason.action = [reason](hids& g){ if (!reason.empty()) g.set_clipboard(dot_00, reason, mime::textonly); };
                copy.children.push_back(std::move(failed_reason));
            }
            items.push_back(std::move(copy));
        }

        items.push_back(m::item{ .alive = true, .type = m::kind::separator });
        add("Select &All", !any_in_scope, [ctrl, status, scope]
        {
            for (auto& it : ctrl->queue) xfer_select(it, status, scope(it));
        });
        return items;
    }
    inline auto xfer_remove_confirmation(sftp_remote* ctrl, si32 status) -> app::shared::confirm_dialog_text
    {
        auto selected = xfer_selection_pred(status);
        auto count = si32{}; for (auto& it : ctrl->queue) if (selected(it)) ++count;
        return { count == 1 ? text{ "Remove this transfer from the queue?" }
                            : "Remove " + std::to_string(count) + " transfers from the queue?",
                 "Remove", "Cancel" };
    }
    // One transfer cell's rendered text + colour.
    inline auto xfer_cell(sftp_remote* ctrl, si32 status, si32 row, si32 key,
                          xfer_progress_cache& progress) -> table_cell
    {
        auto rows = xfer_rows(ctrl, status);
        if (row < 0 || row >= (si32)rows.size()) return {};
        auto qi = rows[(size_t)row].qi, child = rows[(size_t)row].child;
        auto& it = ctrl->queue[(size_t)qi];
        auto dir_fg = it.download ? ui32{ theme::dir_fg } : ui32{ theme::link_fg };
        if (child == -1)
        {
            switch (key)
            {
                case q_server: return { xfer_server(ctrl), theme::subtext };
                case q_local:  return { it.local_path,  dir_fg };
                case q_remote: return { it.remote_path, dir_fg };
                case q_size:   return { human_size(it.size), theme::subtext };
                case q_progress:
                    if (it.status == queue_item::queued)
                        return xfer_progress_cell(progress, row, 0.0, it.paused ? text{ "paused" } : text{ "queued" }, theme::subtext);
                    else
                    {
                        auto fraction = xfer_progress_fraction(it.done, it.size);
                        auto foreground = it.status == queue_item::succeeded ? ui32{ theme::dir_fg }
                                        : it.status == queue_item::failed    ? ui32{ theme::err_fg }
                                                                             : ui32{ theme::text_fg };
                        return xfer_progress_cell(progress, row, fraction, xfer_progress_text(fraction), foreground);
                    }
                case q_speed: return it.rate.speed > 0.0 ? table_cell{ human_size((si64)(it.rate.speed + 0.5)) + "/s", theme::subtext } : table_cell{};
                default: return it.status == queue_item::failed && !it.error.empty() ? table_cell{ it.error, theme::err_fg } : table_cell{}; // Reason.
            }
        }
        // Expanded subtask child row: only the Local Name label + Progress cells.
        auto ranges = chunk_ranges(it.size, it.chunk_count);
        if (child >= (si32)ranges.size()) return {};
        auto [start, end] = ranges[(size_t)child];
        if (key == q_local) return { "Part " + std::to_string(child + 1) + "/" + std::to_string(it.chunk_count) + "  (" + human_size(start) + "–" + human_size(end) + ")", theme::subtext };
        if (key == q_progress)
        {
            auto job = ctrl->find_transfer_job(it.id);
            auto live = job && job->workers.size() == ranges.size();
            if (live)
            {
                auto& wkr = *job->workers[(size_t)child];
                auto full = (double)(end - start);
                if (wkr.state == xfer_worker::s_ok) return xfer_progress_cell(progress, row, 1.0, "100.00%", theme::dir_fg);
                auto fraction = full > 0.0 ? progressbar_fraction((double)wkr.done / full) : 0.0;
                return xfer_progress_cell(progress, row, fraction, xfer_progress_text(fraction),
                                          wkr.state == xfer_worker::s_err ? ui32{ theme::err_fg }
                                                                          : ui32{ theme::text_fg });
            }
            if (it.status == queue_item::succeeded) return xfer_progress_cell(progress, row, 1.0, "100.00%", theme::dir_fg);
            if (it.status == queue_item::queued)    return xfer_progress_cell(progress, row, 0.0, "queued", theme::subtext);
            if (it.status == queue_item::failed)    return xfer_progress_cell(progress, row, 0.0, "—", theme::err_fg);
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
        auto expand = it.chunk_count > 1 ? (xfer_expanded(it, status) ? xp_expanded : xp_collapsed) : xp_muted;
        return { it.download ? text{ "↓" } : text{ "↑" }, arrow_fg, qi, expand };
    }

    // Build the complete transfer-view table_cfg for one status tab.
    inline auto make_transfer_view(sftp_remote* ctrl, si32 status, netxs::wptr<ui::base> window_wp) -> tab_page_cfg
    {
        auto cols = std::make_shared<xfer_cols>();
        auto progress = std::make_shared<xfer_progress_cache>();
        auto row_snapshot = std::make_shared<std::vector<disp_row>>();
        auto title = [ctrl, status]
        {
            auto n = si32{}; if (ctrl) for (auto& it : ctrl->queue) if (tab_status_match(status, it)) ++n;
            auto base = status == 0 ? "Transferring" : status == 1 ? "Failed" : "Succeeded";
            return text{ base } + " (" + std::to_string(n) + ")";
        };
        auto cfg = table_cfg{};
        cfg.window_wp = window_wp;
        cfg.columns   = [ctrl, status, cols]{ return xfer_columns(ctrl, status, cols); };
        cfg.row_count = [ctrl, status, row_snapshot]
        {
            *row_snapshot = xfer_rows(ctrl, status);
            return (si32)row_snapshot->size();
        };
        cfg.cell      = [ctrl, status, progress](si32 row, si32 key){ return xfer_cell(ctrl, status, row, key, *progress); };
        cfg.compare   = [ctrl, row_snapshot](si32 row_a, si32 row_b, si32 key)
        {
            return xfer_compare(ctrl, *row_snapshot, row_a, row_b, key);
        };
        cfg.gutter    = [ctrl, status](si32 row){ return xfer_gutter(ctrl, status, row); };
        cfg.on_toggle = [ctrl, status](si32 qi)
        {
            if (qi >= 0 && qi < (si32)ctrl->queue.size())
            {
                auto& it = ctrl->queue[(size_t)qi];
                xfer_expand(it, status, !xfer_expanded(it, status));
            }
        };
        cfg.selection = [ctrl, status]{ return xfer_sel(ctrl, status); };
        cfg.menu      = [ctrl, status, window_wp](netxs::wptr<ui::base> panel_wp)
        {
            auto deface = [panel_wp]{ if (auto p = panel_wp.lock()) p->base::deface(); };
            auto blank = ptr::shared(faux);
            auto mc = qmenu_cfg{};
            mc.items = [ctrl, status, blank, panel_wp, window_wp]
            {
                return xfer_menu(ctrl, status, *blank, panel_wp, window_wp);
            };
            mc.on_item_rclick = [ctrl, status, blank, deface](si32 hit)
            {
                *blank = faux;
                if (hit < 0 || hit >= (si32)ctrl->queue.size() || xfer_selected(ctrl->queue[(size_t)hit], status)) return;
                for (auto& it : ctrl->queue) xfer_select(it, status, faux);
                xfer_select(ctrl->queue[(size_t)hit], status, true);
                deface();
            };
            mc.on_blank_rclick = [ctrl, status, blank, deface]
            {
                *blank = true;
                auto any = faux;
                for (auto& it : ctrl->queue) { any |= xfer_selected(it, status); xfer_select(it, status, faux); }
                if (any) deface();
            };
            return mc;
        };
        cfg.follow      = [ctrl, status]
        {
            auto rows = xfer_rows(ctrl, status);
            for (auto i = si32{}; i < (si32)rows.size(); ++i)
                if (rows[(size_t)i].child == -1
                 && ctrl->find_transfer_job(ctrl->queue[(size_t)rows[(size_t)i].qi].id))
                    return table_follow_target{ table_follow_target::source_row, i };
            return table_follow_target{ table_follow_target::tail };
        };
        cfg.on_col_grab = [status, cols](si32 key){ if (key == q_ncol && cols->reason_w_override == 0) cols->reason_w_override = xfer_reason_w(*cols, status); };
        cfg.empty_text  = []{ return text{ "(no transfers — press Enter on a file to queue one)" }; };
        cfg.deletion.enabled = true;
        cfg.deletion.on_remove_selected = [ctrl, status](netxs::wptr<ui::base>)
        {
            if (ctrl) ctrl->queue_remove(xfer_selection_pred(status));
        };
        cfg.deletion.confirm = [ctrl, status]{ return xfer_remove_confirmation(ctrl, status); };
        return make_tab_page(make_table(std::move(cfg)), std::move(title), tab_abbreviate_count);
    }
}
