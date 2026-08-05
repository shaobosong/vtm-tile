// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/hash_view.hpp: a THIN configuration over the reusable table core
// (components/table.hpp) that presents the controller's checksum queue
// (Source | Path | Algorithm | Size | Progress | Result). This is a tab adapter over the
// independent table widget.

#include "queue_actions.hpp"
#include "components/progressbar.hpp"
#include "components/tab_page.hpp"
#include "components/table.hpp"

namespace netxs::app::parvion
{
    static constexpr auto hash_headers = std::array<view, 6>{ "Source", "Path", "Algorithm", "Size", "Progress", "Result" };
    static constexpr auto hash_menu_headers = std::array<view, 6>{ "&Source", "&Path", "&Algorithm", "S&ize", "P&rogress", "R&esult" };
    static constexpr auto hash_size_body_w = si32{ 8 };

    // Per-instance session-only column widths (0 = auto-size to content).
    struct hash_cols { std::array<si32, 6> w{}; };

    inline auto hash_progress_fraction(hash_item const& item) -> double
    {
        if (item.status == hash_item::succeeded) return 1.0;
        return item.size > 0 ? progressbar_fraction((double)item.done / (double)item.size) : 0.0;
    }
    inline auto hash_progress_text(double fraction) -> text
    {
        auto buf = std::array<char, 24>{};
        std::snprintf(buf.data(), buf.size(), "%.2f%%", progressbar_fraction(fraction) * 100.0);
        return text{ buf.data() };
    }
    inline auto hash_progress_label(hash_item const& item) -> text
    {
        if (item.status == hash_item::queued)    return "queued";
        if (item.status == hash_item::hashing)   return item.size > 0 ? hash_progress_text(hash_progress_fraction(item))
                                                                      : human_size(item.done);
        if (item.status == hash_item::succeeded) return "100.00%";
        return "failed";
    }
    inline auto hash_progress_foreground(hash_item const& item) -> ui32
    {
        if (item.status == hash_item::queued)    return theme::subtext;
        if (item.status == hash_item::succeeded) return theme::dir_fg;
        if (item.status == hash_item::failed)    return theme::err_fg;
        return theme::text_fg;
    }
    inline auto hash_progress_cell(progressbar_cache& cache, hash_item const& item) -> table_cell
    {
        return { cached_progressbar(cache, item.id, hash_progress_fraction(item),
                                    hash_progress_label(item), hash_progress_foreground(item)) };
    }

    inline auto hash_content_w(sftp_remote* ctrl, si32 col) -> si32
    {
        auto w = si32{};
        if (!ctrl) return w;
        for (auto& it : ctrl->hash_queue)
        {
            auto s = text{};
            switch (col)
            {
                case 0:  s = it.source(); break;
                case 1:  s = it.path; break;
                case 2:  s = text{ hash_algo_label(it.algo) }; break;
                case 3:  s = it.size >= 0 ? human_size(it.size) : text{}; break;
                case 4:  s = hash_progress_label(it); break;
                default: s = it.status == hash_item::succeeded ? it.digest : it.status == hash_item::failed ? it.error : text{}; break;
            }
            w = std::max(w, (si32)cell_width(s));
        }
        return w;
    }
    inline auto hash_text_order(view a, view b) -> si32
    {
        auto al = text{ a }; utf::to_lower(al);
        auto bl = text{ b }; utf::to_lower(bl);
        return al < bl ? -1 : bl < al ? 1 : 0;
    }
    inline auto hash_value_order(auto const& a, auto const& b) -> si32
    {
        return a < b ? -1 : b < a ? 1 : 0;
    }
    inline auto hash_progress_order(hash_item const& a, hash_item const& b) -> si32
    {
        // queued / hashing / succeeded / failed are distinct display domains. Within a live
        // domain, compare the displayed percentage (or byte count when the total is unknown).
        if (auto cmp = hash_value_order(a.status, b.status)) return cmp;
        if (a.status == hash_item::succeeded || a.status == hash_item::failed) return 0;
        if (a.size > 0 && b.size > 0)
        {
            auto ap = (long double)a.done / (long double)a.size;
            auto bp = (long double)b.done / (long double)b.size;
            return hash_value_order(ap, bp);
        }
        if (a.size > 0 != b.size > 0) return a.size > 0 ? 1 : -1;
        return hash_value_order(a.done, b.done);
    }
    inline auto hash_result_order(hash_item const& a, hash_item const& b) -> si32
    {
        if (auto cmp = hash_value_order(a.status, b.status)) return cmp;
        if (a.status == hash_item::succeeded) return hash_text_order(a.digest, b.digest);
        if (a.status == hash_item::failed)    return hash_text_order(a.error, b.error);
        return 0;
    }
    inline auto hash_compare(sftp_remote* ctrl, si32 row_a, si32 row_b, si32 key) -> si32
    {
        if (!ctrl || row_a < 0 || row_b < 0 || row_a >= (si32)ctrl->hash_queue.size() || row_b >= (si32)ctrl->hash_queue.size()) return 0;
        auto& a = ctrl->hash_queue[(size_t)row_a];
        auto& b = ctrl->hash_queue[(size_t)row_b];
        switch (key)
        {
            case 0:  return hash_text_order(a.source(), b.source());
            case 1:  return hash_text_order(a.path, b.path);
            case 2:  return hash_value_order(a.algo, b.algo);
            case 3:  return hash_value_order(a.size, b.size);
            case 4:  return hash_progress_order(a, b);
            default: return hash_result_order(a, b);
        }
    }
    inline auto hash_columns(sftp_remote* ctrl, std::shared_ptr<hash_cols> cols) -> qtable
    {
        static constexpr auto right = std::array<bool, 6>{ faux, faux, faux, true, true, faux };
        auto t = qtable{};
        if (!ctrl) return t;
        auto src = si32{}, path = si32{}, res = si32{};
        for (auto& it : ctrl->hash_queue)
        {
            src  = std::max(src,  (si32)cell_width(it.source()));
            path = std::max(path, (si32)cell_width(it.path));
            res  = std::max(res,  (si32)cell_width(it.status == hash_item::failed ? it.error : it.digest));
        }
        path = std::min(path, 48); // Cap the Path column; longer paths scroll horizontally.
        // Size is eight content cells plus the table's one-cell trailing divider.
        auto bodyw = std::array<si32, 6>{ src, path, 9, hash_size_body_w, 10, res };
        auto autow = std::array<si32, 6>{};
        for (auto i = si32{}; i < 6; ++i)
            autow[(size_t)i] = q_col_fit_w(hash_headers[(size_t)i], bodyw[(size_t)i], true);
        for (auto i = si32{}; i < 6; ++i)
        {
            auto w = cols->w[(size_t)i] > 0 ? cols->w[(size_t)i] : autow[(size_t)i];
            t.add_column({ text{ hash_headers[(size_t)i] }, w, right[(size_t)i], true, i },
                         ctrl->hash_col_shown[(size_t)i], text{ hash_menu_headers[(size_t)i] });
        }
        t.on_show_column   = [ctrl](si32 key, bool on){ if (key >= 0 && key < (si32)ctrl->hash_col_shown.size()) ctrl->hash_col_shown[(size_t)key] = on; };
        t.on_resize_column = [cols](si32 key, si32 w){ if (key >= 0 && key < (si32)cols->w.size()) cols->w[(size_t)key] = w; };
        t.autofit   = [ctrl](si32 key){ return hash_content_w(ctrl, key); };
        return t;
    }
    inline auto hash_sel(sftp_remote* ctrl) -> qsel_cfg
    {
        auto cfg = qsel_cfg{};
        cfg.key_count  = [ctrl]{ return (si32)ctrl->hash_queue.size(); };
        cfg.is_selected   = [ctrl](si32 k){ return k >= 0 && k < (si32)ctrl->hash_queue.size() && ctrl->hash_queue[(size_t)k].selected; };
        cfg.on_select     = [ctrl](si32 k, bool v){ if (k >= 0 && k < (si32)ctrl->hash_queue.size()) ctrl->hash_queue[(size_t)k].selected = v; };
        cfg.on_clear      = [ctrl]{ for (auto& it : ctrl->hash_queue) it.selected = faux; };
        cfg.has_selection = [ctrl]{ for (auto& it : ctrl->hash_queue) if (it.selected) return true; return faux; };
        cfg.in_scope   = [ctrl](si32 k){ return k >= 0 && k < (si32)ctrl->hash_queue.size(); };
        cfg.row_count  = [ctrl]{ return (si32)ctrl->hash_queue.size(); };
        cfg.key_of_row = [ctrl](si32 i){ return i >= 0 && i < (si32)ctrl->hash_queue.size() ? i : -1; };
        return cfg;
    }
    inline auto hash_copy_digest_payload(sftp_remote* ctrl) -> text
    {
        auto out = text{};
        if (!ctrl) return out;
        for (auto& it : ctrl->hash_queue)
        {
            if (!it.selected || it.status != hash_item::succeeded || it.digest.empty()) continue;
            if (!out.empty()) out += '\n';
            out += it.digest;
        }
        return out;
    }
    inline auto hash_copy_path_payload(sftp_remote* ctrl) -> text
    {
        auto out = text{};
        if (!ctrl) return out;
        for (auto& it : ctrl->hash_queue)
        {
            if (!it.selected || it.path.empty()) continue;
            if (!out.empty()) out += '\n';
            out += it.path;
        }
        return out;
    }
    inline auto hash_remove_confirmation(sftp_remote* ctrl) -> app::shared::confirm_dialog_text
    {
        auto count = ctrl ? ctrl->hash_selected_count() : 0;
        return {
            count == 1 ? text{ "Remove this checksum?" }
                       : "Remove " + std::to_string(count) + " selected checksums?",
            "Remove", "Cancel" };
    }
    inline void hash_confirm_remove_selected(sftp_remote* ctrl,
                                             netxs::wptr<ui::base> panel_wp,
                                             netxs::wptr<ui::base> window_wp)
    {
        if (!ctrl || !ctrl->hash_selected_count()) return;
        auto run = [ctrl, panel_wp]
        {
            if (auto panel = panel_wp.lock())
            {
                ctrl->hash_remove_selected();
                panel->base::deface();
            }
        };
        if (auto window = window_wp.lock())
            app::shared::show_close_confirmation(*window, run, {}, hash_remove_confirmation(ctrl));
        else run();
    }
    inline auto hash_menu(sftp_remote* ctrl,
                          netxs::wptr<ui::base> panel_wp,
                          netxs::wptr<ui::base> window_wp) -> qmenu_cfg
    {
        namespace m = app::shared::menu;
        auto deface = [panel_wp]{ if (auto p = panel_wp.lock()) p->base::deface(); };
        auto cfg = qmenu_cfg{};
        cfg.items = [ctrl, deface, panel_wp, window_wp]() -> std::vector<m::item>
        {
            auto items = std::vector<m::item>{};
            auto paths = hash_copy_path_payload(ctrl);
            auto digests = hash_copy_digest_payload(ctrl);
            auto copy = m::item{ .alive = true, .label = "&Copy", .type = m::kind::dropdown, .disabled = paths.empty() };
            auto path = m::item{ .alive = true, .label = "&Path", .disabled = paths.empty() };
            path.action = [paths](hids& g){ if (!paths.empty()) g.set_clipboard(dot_00, paths, mime::textonly); };
            copy.children.push_back(std::move(path));
            auto digest = m::item{ .alive = true, .label = "&Digest", .disabled = digests.empty() };
            digest.action = [digests](hids& g){ if (!digests.empty()) g.set_clipboard(dot_00, digests, mime::textonly); };
            copy.children.push_back(std::move(digest));
            items.push_back(std::move(copy));

            auto rm = m::item{ .alive = true, .label = "&Remove", .disabled = !ctrl->hash_selected_count() };
            rm.action = [ctrl, panel_wp, window_wp](hids&){ hash_confirm_remove_selected(ctrl, panel_wp, window_wp); };
            items.push_back(std::move(rm));

            items.push_back(m::item{ .alive = true, .type = m::kind::separator });
            auto select_all = m::item{ .alive = true, .label = "Select &All", .disabled = ctrl->hash_queue.empty() };
            select_all.action = [ctrl, deface](hids&)
            {
                for (auto& it : ctrl->hash_queue) it.selected = true;
                deface();
            };
            items.push_back(std::move(select_all));
            return items;
        };
        cfg.on_item_rclick = [ctrl, deface](si32 hit){ if (hit >= 0 && hit < (si32)ctrl->hash_queue.size() && !ctrl->hash_queue[(size_t)hit].selected) { for (auto& it : ctrl->hash_queue) it.selected = faux; ctrl->hash_queue[(size_t)hit].selected = true; deface(); } };
        cfg.on_blank_rclick = [ctrl, deface]{ auto any = faux; for (auto& it : ctrl->hash_queue) { any |= it.selected; it.selected = faux; } if (any) deface(); };
        return cfg;
    }
    inline auto hash_cell(sftp_remote* ctrl, si32 row, si32 key,
                          progressbar_cache& progress) -> table_cell
    {
        if (row < 0 || row >= (si32)ctrl->hash_queue.size()) return {};
        auto& it = ctrl->hash_queue[(size_t)row];
        switch (key)
        {
            case 0: return { it.source(), theme::subtext };
            case 1: return { it.path, theme::text_fg };
            case 2: return { text{ hash_algo_label(it.algo) }, theme::subtext };
            case 3: return { it.size >= 0 ? human_size(it.size) : text{}, theme::subtext };
            case 4: return hash_progress_cell(progress, it);
            default:
                return { it.status == hash_item::succeeded ? it.digest : it.status == hash_item::failed ? it.error : text{}, it.status == hash_item::failed ? ui32{ theme::err_fg } : ui32{ theme::text_fg } };
        }
    }

    inline auto make_hash_view(sftp_remote* ctrl, netxs::wptr<ui::base> window_wp) -> tab_page_cfg
    {
        auto cols = std::make_shared<hash_cols>();
        auto progress = std::make_shared<progressbar_cache>();
        auto title = [ctrl]{ return "Checksums (" + std::to_string(ctrl ? ctrl->hash_queue.size() : 0) + ")"; };
        auto cfg = table_cfg{};
        cfg.deletion.window_wp = window_wp;
        cfg.columns    = [ctrl, cols]{ return hash_columns(ctrl, cols); };
        cfg.row_count  = [ctrl]{ return (si32)ctrl->hash_queue.size(); };
        cfg.cell       = [ctrl, progress](si32 row, si32 key){ return hash_cell(ctrl, row, key, *progress); };
        cfg.sort.compare    = [ctrl](si32 row_a, si32 row_b, si32 key){ return hash_compare(ctrl, row_a, row_b, key); };
        cfg.selection  = [ctrl]{ return hash_sel(ctrl); };
        cfg.menu       = [ctrl, window_wp](netxs::wptr<ui::base> panel_wp){ return hash_menu(ctrl, panel_wp, window_wp); };
        cfg.viewport.follow     = []{ return table_follow_target{ table_follow_target::tail }; };
        cfg.empty_text = []{ return text{ "(no checksums)" }; };
        cfg.deletion.enabled = true;
        cfg.deletion.on_remove_selected = [ctrl](netxs::wptr<ui::base>)
        {
            if (!ctrl) return;
            ctrl->hash_remove_selected();
        };
        cfg.deletion.confirm = [ctrl]{ return hash_remove_confirmation(ctrl); };
        return make_tab_page(make_table(std::move(cfg)), std::move(title), tab_abbreviate_count);
    }
}
