// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/table.hpp: the reusable TABLE core component.
//
// This is the single place that owns ALL table behavior: the render frame (background, column
// header, cells, dividers, selection highlight, scrollbars), vertical/horizontal scrolling (wheel,
// scrollbar drag/paging), row selection (click / ctrl / shift / rubber-band), column resize + auto-
// fit + show/hide menu, an optional tree gutter (direction arrow + expand button), optional single-
// select arrow-key navigation, right-click menus, and focus. A caller does NOT write event handlers:
// it supplies a `table_cfg` (column model + per-cell data + selection/menu adapters + a few hooks)
// and calls make_table(). Concrete views (the transfer views, the checksums view) are just table_cfg
// builders and optional container adapters — they own no widget code of their own.
//
// make_table() returns the independent table widget. Containers can wrap it as needed.

#include "panes.hpp" // ui::sptr, sftp_remote, theme, shared menu utilities.

namespace netxs::app::parvion
{
    // Column model + cell/header/divider paint helpers. A caller builds one per render from live
    // state; the width/visibility hooks route a dragged column / auto-fit / header toggle back to the
    // caller's backing store, so the core drives resize + the show/hide menu with no per-tab code.
    struct qtable
    {
        struct column
        {
            text title;
            si32 width = 10;     // Total column width INCLUDING its trailing 1-cell divider.
            bool right = faux;   // Right-align the cell text within the column.
            bool resizable = true;
            si32 key = -1;       // Caller tag (a logical column id, stable across show/hide + reorder).
        };
        std::vector<column> cols; // Visible columns, left to right.
        si32 left = 1;            // Content-x of the first column (the gutter width before it).

        struct col_toggle { text title; si32 key = -1; bool shown = true; };
        std::vector<col_toggle>                roster;    // All columns (shown + hidden) for the header menu.
        std::function<void(si32 key, bool on)> set_shown; // Flip a column's visibility (backing write).
        std::function<void(si32 key, si32 w)>  resize;    // Persist a dragged column width.
        std::function<si32(si32 key)>          autofit;   // Auto-fit (widest-content) width for a column.

        auto col_x(si32 i) const -> si32
        {
            auto x = left;
            for (auto j = si32{}; j < i && j < (si32)cols.size(); ++j) x += cols[(size_t)j].width;
            return x;
        }
        auto border_cx(si32 i) const -> si32 { return i < 0 || i >= (si32)cols.size() ? -1 : col_x(i) + cols[(size_t)i].width - 1; }
        auto content_w() const -> si32 { auto x = left; for (auto& c : cols) x += c.width; return x; }

        void paint_cell(auto& canvas, si32 i, si32 y, view s, ui32 fg, ui32 bg, si32 hscroll, si32 disp_w) const
        {
            if (i < 0 || i >= (si32)cols.size()) return;
            auto& c    = cols[(size_t)i];
            auto  colw = std::max(0, c.width - 1); // Keep the divider cell clear.
            auto  cx   = col_x(i);
            if (c.right)
            {
                auto t  = fit_ellipsis(s, colw);
                auto tw = (si32)cell_width(t);
                paint_at(canvas, cx + std::max(0, colw - tw), tw, y, t, fg, bg, hscroll, disp_w);
            }
            else paint_at(canvas, cx, colw, y, s, fg, bg, hscroll, disp_w);
        }
        void paint_header(auto& canvas, si32 row, si32 hscroll, si32 disp_w) const
        {
            canvas.fill(rect{{ 0, row }, { disp_w, 1 }}, [&](cell& c){ c.bgc(theme::header); });
            for (auto i = si32{}; i < (si32)cols.size(); ++i) paint_cell(canvas, i, row, cols[(size_t)i].title, theme::subtext, theme::header, hscroll, disp_w);
        }
        void paint_dividers(auto& canvas, si32 top, si32 bottom, si32 hscroll, si32 disp_w, si32 hover_idx, si32 drag_idx) const
        {
            for (auto i = si32{}; i < (si32)cols.size(); ++i)
            {
                auto bx = border_cx(i) - hscroll;
                if (bx < 0 || bx >= disp_w) continue;
                auto fg = (hover_idx == i || drag_idx == i) ? ui32{ theme::text_fg } : ui32{ theme::subtext };
                canvas.fill(rect{{ bx, top }, { 1, bottom - top }}, [&](cell& c){ c.fgc(fg).txt("\xE2\x94\x82"); }); // │
            }
        }
        // Paint text into the cell [x, x+w) at row y, shifted left by `hscroll`, right-clipped to
        // `disp_w`; wider content is tail-ellipsized (fit_ellipsis). The shared cell primitive.
        static void paint_at(auto& canvas, si32 x, si32 w, si32 y, view s, ui32 fg, ui32 bg, si32 hscroll, si32 disp_w)
        {
            auto sx   = x - hscroll;
            auto room = std::min(w, disp_w - sx);
            if (room <= 0 || sx >= disp_w) return;
            put_str(canvas, sx, y, fit_ellipsis(s, w), fg, bg, room);
        }
    };

    // The table's mutable state: scroll/geometry cache + drag + selection-transient + expand-transient.
    // The body paints into the widget canvas from row 1 (row 0 is the column header); `tab_row` is the
    // exclusive bottom of the body (== canvas height).
    struct table_state
    {
        enum dmode { d_none, d_vsb, d_hsb, d_col, d_rubber };

        sftp_remote*          ctrl = nullptr;
        netxs::wptr<ui::base> window_wp;
        bool                  focused = faux;

        si32 scroll = 0, hscroll = 0;
        bool follow = true;
        bool sb_hover = faux,  sb_drag = faux;  si32 sb_grab = 0;
        bool hsb_hover = faux, hsb_drag = faux; si32 hsb_grab = 0;
        si32 hover_border = -1, col_drag = -1;
        si32 sel_anchor = -1;
        si32 rubber_a = -1, rubber_b = -1;
        std::set<si32> drag_base;
        bool rubber_ctrl = faux, rubber_add = true;
        dmode drag = d_none;

        std::vector<std::pair<rect, si32>> row_hit{};    // Row rect -> selection key.
        std::vector<std::pair<rect, si32>> expand_hit{}; // Expand-button box -> expand id.
        si32 hover_expand = -1, press_expand = -1;

        si32 body_top = 1, body_rows = 0, tab_row = 0;
        si32 content_w = 0, disp_w = 0, vsb_x = 0, hsb_y = 0;
        si32 div_bottom = 1;
        si32 total = 0;
        bool has_vsb = faux, has_hsb = faux;
    };

    // Optional left-gutter content for one row: a direction arrow + an expand indicator/button.
    enum expand_kind { xp_none, xp_muted, xp_collapsed, xp_expanded };
    struct gutterval { text arrow; ui32 arrow_fg = 0; si32 expand_id = -1; expand_kind expand = xp_none; };
    // One cell's rendered content.
    struct cellval { text s; ui32 fg = 0; };

    // Standard gutter geometry (arrow then expand button), used when a table has a tree gutter.
    static constexpr auto g_arrow_x  = si32{ 1 };
    static constexpr auto g_arrow_w  = si32{ 2 };
    static constexpr auto g_expand_x = si32{ 3 };
    static constexpr auto g_expand_w = si32{ 3 };
    static constexpr auto g_col_min  = si32{ 2 };
    static constexpr auto g_col_max  = si32{ 200 };

    // Row selection adapter: a flat key space (key = a selectable row identity). `key_of_row` maps a
    // display row to its key (-1 = not selectable, e.g. a subtask child). The core drives all click /
    // ctrl / shift / rubber-band selection through this; a table with no selection leaves it null.
    struct qsel_cfg
    {
        std::function<si32()>           key_count;
        std::function<bool(si32)>       is_sel;
        std::function<void(si32, bool)> set_sel;
        std::function<void()>           clear;
        std::function<bool()>           any;
        std::function<bool(si32)>       in_scope;   // key -> selectable now (Shift-range filter).
        std::function<si32()>           disp;       // Number of display rows.
        std::function<si32(si32)>       key_of_row; // Display row -> key (-1 if not selectable).
    };

    // Right-click menus for a table. The column-header show/hide menu is intrinsic (built from the
    // column roster); item/blank menus are supplied here. Any builder may be null.
    struct qmenu_cfg
    {
        std::function<std::vector<app::shared::menu::item>(si32)> item;            // Per-row menu (null = none).
        std::function<std::vector<app::shared::menu::item>()>     blank;           // Blank-area menu (null = none).
        std::function<void(si32)>                                 on_item_rclick;  // Selection sync for a row (null = none).
        std::function<void()>                                     on_blank_rclick; // Selection reset on blank (null = none).
    };

    // The complete table configuration a caller supplies. Everything here is DATA / adapters; there
    // is no event-handling code. `columns` / `rows` / `cell` / `gutter` / `follow` are re-queried from
    // live state each render or hit-test.
    struct table_cfg
    {
        sftp_remote*          ctrl = nullptr; // Live controller (for the widget's readiness guards).
        netxs::wptr<ui::base> window_wp;      // App window: anchor for confirm dialogs.
        std::function<qtable()>                          columns;     // Column model (rebuilt each render/hit).
        std::function<si32()>                            rows;        // Number of display rows.
        std::function<cellval(si32 row, si32 key)>       cell;        // Cell text+fg for logical column `key`.
        std::function<gutterval(si32 row)>               gutter;      // Left-gutter arrow/expand (null => no gutter).
        std::function<void(si32 expand_id)>              toggle;      // Toggle a row's expansion.
        std::function<qsel_cfg()>                        selection;   // null => not selectable.
        std::function<qmenu_cfg(netxs::wptr<ui::base>)>  menu;        // Item/blank menus (null => none; header menu still shows).
        std::function<si32()>                            follow;      // -1 => pin to bottom; >=0 => keep that display row visible.
        std::function<void(si32 key)>                    on_col_grab; // A column-border drag begins (null => none).
        std::function<text()>                            empty_text;  // Message shown when rows()==0 (null => none).
        std::function<bool(hids&, netxs::wptr<ui::base>)> on_key;     // App keys (Delete clear-finished); null => none.
        bool                                             wide_hit = faux;  // Row hit-box spans full body width (else content width).
        bool                                             arrow_nav = faux; // Single-select arrow-key navigation.
    };

    // ---- Scroll layer ------------------------------------------------------------------------------
    inline void tbl_layout(table_state& st, si32 w, si32 h, si32 total, si32 content_w, si32 body_top)
    {
        st.total     = total;
        st.tab_row   = h; // Exclusive body bottom: the page canvas has no reserved strip/handle rows.
        st.body_top  = body_top;
        st.content_w = content_w;
        auto avail = std::max(0, st.tab_row - st.body_top);
        for (auto pass = si32{}; pass < 2; ++pass)
        {
            st.has_vsb   = avail > 0 && total > avail;
            st.disp_w    = w - (st.has_vsb ? 1 : 0);
            st.has_hsb   = st.content_w > st.disp_w;
            st.body_rows = std::max(0, avail - (st.has_hsb ? 1 : 0));
            st.has_vsb   = st.body_rows > 0 && total > st.body_rows;
            st.disp_w    = w - (st.has_vsb ? 1 : 0);
            st.has_hsb   = st.content_w > st.disp_w;
            st.body_rows = std::max(0, avail - (st.has_hsb ? 1 : 0));
        }
        st.vsb_x = w - 1;
        st.hsb_y = st.body_top + st.body_rows;
    }
    struct q_sb { bool ok = faux; si32 x = 0, top = 0, track_h = 0, thumb_y = 0, thumb_h = 0, maxscroll = 0; };
    inline auto tbl_vsb(table_state const& st) -> q_sb
    {
        auto sb = q_sb{};
        sb.ok = st.has_vsb && st.body_rows > 0 && st.total > st.body_rows;
        if (!sb.ok) return sb;
        sb.x = st.vsb_x; sb.top = st.body_top; sb.track_h = st.body_rows;
        sb.thumb_h = std::max(1, st.body_rows * st.body_rows / st.total);
        sb.maxscroll = st.total - st.body_rows;
        sb.thumb_y = sb.top + (st.body_rows - sb.thumb_h) * st.scroll / sb.maxscroll;
        return sb;
    }
    inline auto tbl_hsb(table_state const& st) -> q_sb
    {
        auto sb = q_sb{};
        sb.ok = st.has_hsb && st.disp_w > 0 && st.content_w > st.disp_w;
        if (!sb.ok) return sb;
        sb.x = 0; sb.top = st.hsb_y; sb.track_h = st.disp_w;
        sb.thumb_h = std::max(1, st.disp_w * st.disp_w / st.content_w);
        sb.maxscroll = st.content_w - st.disp_w;
        sb.thumb_y = (st.disp_w - sb.thumb_h) * st.hscroll / sb.maxscroll;
        return sb;
    }
    inline void tbl_vsb_to(table_state& st, si32 local_y, q_sb const& sb)
    {
        auto travel = sb.track_h - sb.thumb_h;
        if (travel <= 0) return;
        st.scroll = std::clamp((local_y - st.sb_grab - sb.top) * sb.maxscroll / travel, 0, sb.maxscroll);
    }
    inline void tbl_hsb_to(table_state& st, si32 local_x, q_sb const& sb)
    {
        auto travel = sb.track_h - sb.thumb_h;
        if (travel <= 0) return;
        st.hscroll = std::clamp((local_x - st.hsb_grab - sb.x) * sb.maxscroll / travel, 0, sb.maxscroll);
    }
    inline void tbl_paint_scrollbars(table_state const& st, auto& canvas)
    {
        if (auto sb = tbl_vsb(st); sb.ok)
        {
            auto mark = (st.sb_drag || st.sb_hover) ? "\xe2\x96\x88" : "\xe2\x96\x90"; // █ : ▐
            canvas.fill(rect{{ sb.x, sb.top }, { 1, sb.track_h }}, [&](cell& c){ c.bgc(theme::bg).fgc(theme::sb_track).txt(mark); });
            auto tc = st.sb_drag ? ui32{ theme::sb_drag } : st.sb_hover ? ui32{ theme::sb_hover } : ui32{ theme::sb_thumb };
            canvas.fill(rect{{ sb.x, sb.thumb_y }, { 1, sb.thumb_h }}, [&](cell& c){ c.bgc(theme::bg).fgc(tc).txt(mark); });
        }
        if (auto sb = tbl_hsb(st); sb.ok)
        {
            auto mark = (st.hsb_drag || st.hsb_hover) ? "\xe2\x96\x84" : "\xe2\x96\x82"; // ▄ : ▂
            canvas.fill(rect{{ sb.x, sb.top }, { sb.track_h, 1 }}, [&](cell& c){ c.bgc(theme::bg).fgc(theme::sb_track).txt(mark); });
            auto tc = st.hsb_drag ? ui32{ theme::sb_drag } : st.hsb_hover ? ui32{ theme::sb_hover } : ui32{ theme::sb_thumb };
            canvas.fill(rect{{ sb.x + sb.thumb_y, sb.top }, { sb.thumb_h, 1 }}, [&](cell& c){ c.bgc(theme::bg).fgc(tc).txt(mark); });
        }
    }
    inline void tbl_clamp(table_state& st)
    {
        st.scroll  = std::clamp(st.scroll,  0, std::max(0, st.total - st.body_rows));
        st.hscroll = std::clamp(st.hscroll, 0, std::max(0, st.content_w - st.disp_w));
    }
    inline auto q_row_w(table_state const& st) -> si32 { return std::clamp(st.content_w - st.hscroll, 0, st.disp_w); }
    inline auto q_border_hit(qtable const& t, si32 mx, si32 hscroll) -> si32
    {
        for (auto v = si32{}; v < (si32)t.cols.size(); ++v)
            if (t.cols[(size_t)v].resizable && mx == t.border_cx(v) - hscroll) return v;
        return -1;
    }
    inline void q_set_col_w(qtable const& t, si32 v, si32 w) { if (v >= 0 && v < (si32)t.cols.size() && t.resize) t.resize(t.cols[(size_t)v].key, w); }
    inline auto q_col_autofit(qtable const& t, si32 v) -> si32 { return v >= 0 && v < (si32)t.cols.size() && t.autofit ? t.autofit(t.cols[(size_t)v].key) : 0; }
    inline auto q_row_at(table_state const& st, si32 mx, si32 my) -> si32
    {
        for (auto& [b, key] : st.row_hit)
            if (my == b.coor.y && mx >= b.coor.x && mx < b.coor.x + b.size.x) return key;
        return -1;
    }

    // ---- Selection layer ---------------------------------------------------------------------------
    inline auto q_sel_press(table_state& st, qsel_cfg const& s, si32 hit, bool ctl, bool shft) -> bool
    {
        if (hit < 0) return faux;
        if (shft && st.sel_anchor >= 0 && st.sel_anchor < s.key_count())
        {
            auto lo = std::min(st.sel_anchor, hit), hi = std::max(st.sel_anchor, hit);
            for (auto j = si32{}; j < s.key_count(); ++j) s.set_sel(j, j >= lo && j <= hi && s.in_scope(j));
        }
        else if (ctl) { s.set_sel(hit, !s.is_sel(hit)); st.sel_anchor = hit; }
        else          { s.clear(); s.set_sel(hit, true); st.sel_anchor = hit; }
        return true;
    }
    inline auto q_sel_clear_blank(table_state& st, qsel_cfg const& s) -> bool { if (!s.any()) return faux; s.clear(); st.sel_anchor = -1; return true; }
    inline void q_sel_snapshot(table_state& st, qsel_cfg const& s) { st.drag_base.clear(); for (auto k = si32{}; k < s.key_count(); ++k) if (s.is_sel(k)) st.drag_base.insert(k); }
    inline void q_rubber_begin(table_state& st, qsel_cfg const& s, si32 press_row, bool ctl)
    {
        st.rubber_a = st.rubber_b = std::max(0, press_row);
        st.drag = table_state::d_rubber;
        auto anchor_key = press_row >= 0 && press_row < s.disp() ? s.key_of_row(press_row) : -1;
        st.rubber_ctrl = ctl;
        if (st.rubber_ctrl) st.rubber_add = !st.drag_base.count(anchor_key);
        else                s.clear();
        if (anchor_key >= 0) st.sel_anchor = anchor_key;
    }
    inline void q_rubber_pull(table_state& st, qsel_cfg const& s, si32 cur_row)
    {
        st.rubber_b = cur_row;
        auto lo = std::min(st.rubber_a, st.rubber_b), hi = std::max(st.rubber_a, st.rubber_b);
        if (st.rubber_ctrl)
        {
            for (auto k = si32{}; k < s.key_count(); ++k) s.set_sel(k, !!st.drag_base.count(k));
            for (auto i = lo; i <= hi; ++i) { auto k = i >= 0 && i < s.disp() ? s.key_of_row(i) : -1; if (k >= 0) s.set_sel(k, st.rubber_add); }
        }
        else
        {
            s.clear();
            for (auto i = lo; i <= hi; ++i) { auto k = i >= 0 && i < s.disp() ? s.key_of_row(i) : -1; if (k >= 0) s.set_sel(k, true); }
        }
    }

    // ---- Menu layer --------------------------------------------------------------------------------
    inline auto build_columns_menu(std::vector<qtable::col_toggle> const& roster,
                                   std::function<void(si32, bool)> set_shown, std::function<void()> deface) -> std::vector<app::shared::menu::item>
    {
        namespace m = app::shared::menu;
        auto items = std::vector<m::item>{};
        auto shown = si32{};
        for (auto& c : roster) if (c.shown) ++shown;
        for (auto& c : roster)
        {
            auto row = m::item{ .alive = true, .label = c.title, .type = m::kind::check, .checked = c.shown };
            row.action = [set_shown, deface, key = c.key, on = c.shown, shown](hids&)
            {
                if (on && shown <= 1) return; // Never hide the last visible column.
                set_shown(key, !on); deface();
            };
            items.push_back(std::move(row));
        }
        return items;
    }
    inline void q_context_menu(auto& boss, table_state& st, si32 mx, si32 my, qtable const& t, qmenu_cfg const& cfg)
    {
        namespace m = app::shared::menu;
        auto at = twod{ mx, my };
        if (my == st.body_top - 1 && st.body_top > 0) // Column-header row: the intrinsic show/hide menu.
        {
            if (!t.roster.empty() && t.set_shown)
            {
                auto panel_wp = ptr::shadow(boss.This());
                auto deface   = [panel_wp]{ if (auto p = panel_wp.lock()) p->base::deface(); };
                m::open_dropdown_popup(boss, build_columns_menu(t.roster, t.set_shown, deface), faux, -1, at);
            }
            return;
        }
        if (my < st.body_top || my >= st.tab_row) return;
        auto hit = q_row_at(st, mx, my);
        if (hit >= 0)
        {
            if (cfg.on_item_rclick) cfg.on_item_rclick(hit);
            st.sel_anchor = hit; // The right-clicked row becomes the shift-range anchor.
            if (cfg.item)   m::open_dropdown_popup(boss, cfg.item(hit), faux, -1, at);
        }
        else
        {
            if (cfg.on_blank_rclick) { cfg.on_blank_rclick(); st.sel_anchor = -1; }
            if (cfg.blank)           m::open_dropdown_popup(boss, cfg.blank(), faux, -1, at);
        }
    }

    // ---- Render ------------------------------------------------------------------------------------
    inline void table_render(table_state& st, table_cfg const& cfg, auto& canvas, twod size)
    {
        auto w = size.x, h = size.y;
        if (w <= 0 || h <= 0 || !st.ctrl) return;
        canvas.fill(rect{{ 0, 0 }, { w, h }}, [&](cell& c){ c.bgc(theme::bg).fgc(theme::text_fg); });
        st.row_hit.clear();
        st.expand_hit.clear();

        auto t     = cfg.columns();
        auto nrows = cfg.rows ? cfg.rows() : 0;
        tbl_layout(st, w, h, nrows, t.content_w(), /*body_top=*/1);

        auto maxscroll = std::max(0, st.total - st.body_rows);
        if (st.follow)
        {
            auto f = cfg.follow ? cfg.follow() : -1;
            st.scroll = f >= 0 ? std::clamp(f - st.body_rows + 1, 0, maxscroll) : maxscroll;
        }
        tbl_clamp(st);

        auto hs = st.hscroll, clipw = st.disp_w;
        t.paint_header(canvas, st.body_top - 1, hs, clipw);

        if (nrows == 0)
        {
            if (cfg.empty_text && st.body_rows > 0)
                put_str(canvas, 1, st.body_top, cfg.empty_text(), theme::subtext, theme::bg, std::max(1, st.disp_w - 1));
        }
        else
        {
            auto has_sel = (bool)cfg.selection;
            auto sel = has_sel ? cfg.selection() : qsel_cfg{};
            auto rubber_on = st.drag == table_state::d_rubber && st.rubber_a >= 0;
            auto rlo = std::min(st.rubber_a, st.rubber_b), rhi = std::max(st.rubber_a, st.rubber_b);
            auto first = st.scroll, last = std::min(nrows, st.scroll + st.body_rows);
            for (auto i = first; i < last; ++i)
            {
                auto y   = st.body_top + (i - first);
                auto key = has_sel ? sel.key_of_row(i) : -1;
                auto selected = key >= 0 && (sel.is_sel(key) || (rubber_on && i >= rlo && i <= rhi));
                auto row_bg = selected ? ui32{ theme::sel_bg } : ui32{ theme::bg };
                if (selected)
                {
                    canvas.fill(rect{{ 0, y }, { q_row_w(st), 1 }}, [&](cell& c){ c.bgc(theme::sel_bg); });
                    if (st.focused) canvas.fill(rect{{ 0, y }, { 1, 1 }}, [&](cell& c){ c.bgc(theme::sel_bg_act); });
                }
                if (cfg.gutter)
                {
                    auto g = cfg.gutter(i);
                    if (!g.arrow.empty()) qtable::paint_at(canvas, g_arrow_x, g_arrow_w, y, g.arrow, g.arrow_fg, row_bg, hs, clipw);
                    if (g.expand == xp_muted) qtable::paint_at(canvas, g_expand_x, g_expand_w, y, " + ", theme::subtext, row_bg, hs, clipw);
                    else if (g.expand == xp_collapsed || g.expand == xp_expanded)
                    {
                        auto bx = g_expand_x - hs;
                        if (bx >= 0 && bx + g_expand_w <= clipw)
                        {
                            auto btn = rect{{ bx, y }, { g_expand_w, 1 }};
                            canvas.fill(btn, [&](cell& c){ c.bgc(theme::sel_bg); });
                            put_str(canvas, bx, y, g.expand == xp_expanded ? " - " : " + ", theme::text_fg, theme::sel_bg, g_expand_w);
                            if      (st.press_expand == g.expand_id) canvas.fill(btn, [](cell& c){ c.xlight(2); });
                            else if (st.hover_expand == g.expand_id) canvas.fill(btn, [](cell& c){ c.xlight(); });
                            st.expand_hit.emplace_back(btn, g.expand_id);
                        }
                    }
                }
                for (auto vc = si32{}; vc < (si32)t.cols.size(); ++vc)
                {
                    auto cv = cfg.cell(i, t.cols[(size_t)vc].key);
                    if (!cv.s.empty()) t.paint_cell(canvas, vc, y, cv.s, cv.fg, row_bg, hs, clipw);
                }
                if (key >= 0) st.row_hit.emplace_back(rect{{ 0, y }, { cfg.wide_hit ? std::max(0, st.disp_w) : q_row_w(st), 1 }}, key);
            }
        }
        auto rows_drawn = std::clamp(st.total - st.scroll, 0, st.body_rows);
        st.div_bottom = st.body_top + rows_drawn;
        t.paint_dividers(canvas, st.body_top - 1, st.div_bottom, hs, clipw, st.hover_border, st.col_drag);
        tbl_paint_scrollbars(st, canvas);
    }

    // ---- Widget ------------------------------------------------------------------------------------
    inline auto make_table(table_cfg cfg) -> ui::sptr
    {
        auto form = ui::mock::ctor()->active()
            ->plugin<pro::mouse>()->plugin<pro::focus>(pro::focus::mode::focusable)->plugin<pro::keybd>();
        form->invoke([&, cfgv = std::move(cfg)](auto& boss)
        {
            auto& st  = boss.base::field(table_state{});
            auto& cfg = boss.base::field(table_cfg{ cfgv });
            st.ctrl      = cfgv.ctrl;      // Threaded from the view; drives the widget's readiness guards.
            st.window_wp = cfgv.window_wp;
            boss.LISTEN(tier::release, e2::render::any, parent_canvas)
            {
                table_render(st, cfg, parent_canvas, boss.base::size());
            };
            boss.LISTEN(tier::release, e2::form::state::focus::count, count) { st.focused = !!count; boss.base::deface(); };

            boss.on(tier::mouserelease, input::key::LeftDown, [&](hids& gear)
            {
                pro::focus::set(boss.This(), gear.id, solo::on);
                auto mx = (si32)gear.coord.x, my = (si32)gear.coord.y;
                if (cfg.selection) q_sel_snapshot(st, cfg.selection());
                if (auto sb = tbl_vsb(st); sb.ok && mx == sb.x && my >= sb.top && my < sb.top + sb.track_h) return;
                if (auto sb = tbl_hsb(st); sb.ok && my == sb.top && mx >= sb.x && mx < sb.x + sb.track_h) return;
                for (auto& [b, id] : st.expand_hit)
                    if (my == b.coor.y && mx >= b.coor.x && mx < b.coor.x + b.size.x)
                    { if (st.press_expand != id) { st.press_expand = id; boss.base::deface(); } return; }
                if (my >= st.body_top - 1 && my < st.div_bottom)
                    if (q_border_hit(cfg.columns(), mx, st.hscroll) >= 0) return;
                if (cfg.selection)
                {
                    auto ctl = !!(gear.ctlstat & hids::anyCtrl), shft = !!(gear.ctlstat & hids::anyShift);
                    auto s = cfg.selection();
                    auto hit = q_row_at(st, mx, my);
                    if (hit >= 0) { q_sel_press(st, s, hit, ctl, shft); boss.base::deface(); gear.dismiss(); return; }
                    if (!ctl && my >= st.body_top && my < st.tab_row && q_sel_clear_blank(st, s)) { boss.base::deface(); gear.dismiss(); return; }
                }
                boss.base::deface(); gear.dismiss();
            });
            boss.on(tier::mouserelease, input::key::LeftUp, [&](hids&) { if (st.press_expand != -1) { st.press_expand = -1; boss.base::deface(); } });
            boss.on(tier::mouserelease, input::key::LeftClick, [&](hids& gear)
            {
                pro::focus::set(boss.This(), gear.id, solo::on);
                auto mx = (si32)gear.coord.x, my = (si32)gear.coord.y;
                if (auto sb = tbl_vsb(st); sb.ok && mx == sb.x && my >= sb.top && my < sb.top + sb.track_h)
                {
                    auto page = std::max(1, st.body_rows);
                    if (my < sb.thumb_y)                    st.scroll = std::clamp(st.scroll - page, 0, sb.maxscroll);
                    else if (my >= sb.thumb_y + sb.thumb_h) st.scroll = std::clamp(st.scroll + page, 0, sb.maxscroll);
                    st.follow = st.scroll == sb.maxscroll; boss.base::deface(); gear.dismiss(); return;
                }
                if (auto sb = tbl_hsb(st); sb.ok && my == sb.top && mx >= sb.x && mx < sb.x + sb.track_h)
                {
                    auto page = std::max(1, st.disp_w); auto tx = sb.x + sb.thumb_y;
                    if (mx < tx)                    st.hscroll = std::clamp(st.hscroll - page, 0, sb.maxscroll);
                    else if (mx >= tx + sb.thumb_h) st.hscroll = std::clamp(st.hscroll + page, 0, sb.maxscroll);
                    boss.base::deface(); gear.dismiss(); return;
                }
                for (auto& [b, id] : st.expand_hit)
                    if (my == b.coor.y && mx >= b.coor.x && mx < b.coor.x + b.size.x)
                    { if (cfg.toggle) cfg.toggle(id); boss.base::deface(); gear.dismiss(); return; }
                gear.dismiss();
            });
            boss.on(tier::mouserelease, input::key::RightClick, [&](hids& gear)
            {
                if (!st.ctrl) { gear.dismiss(); return; }
                pro::focus::set(boss.This(), gear.id, solo::on);
                auto mx = (si32)gear.coord.x, my = (si32)gear.coord.y;
                auto panel_wp = ptr::shadow(boss.This());
                q_context_menu(boss, st, mx, my, cfg.columns(), cfg.menu ? cfg.menu(panel_wp) : qmenu_cfg{});
                gear.dismiss();
            });
            boss.on(tier::mouserelease, input::key::MouseMove, [&](hids& gear)
            {
                auto mx = (si32)gear.coord.x, my = (si32)gear.coord.y;
                auto over = si32{ -1 };
                for (auto& [b, id] : st.expand_hit) if (my == b.coor.y && mx >= b.coor.x && mx < b.coor.x + b.size.x) { over = id; break; }
                if (st.hover_expand != over) { st.hover_expand = over; boss.base::deface(); }
                if (st.press_expand != -1 && st.press_expand != over) { st.press_expand = -1; boss.base::deface(); }
                auto vsb = tbl_vsb(st); auto nsb = vsb.ok && mx == vsb.x && my >= vsb.top && my < vsb.top + vsb.track_h;
                if (st.sb_hover != nsb) { st.sb_hover = nsb; boss.base::deface(); }
                auto hsb = tbl_hsb(st); auto nhsb = hsb.ok && my == hsb.top && mx >= hsb.x && mx < hsb.x + hsb.track_h;
                if (st.hsb_hover != nhsb) { st.hsb_hover = nhsb; boss.base::deface(); }
                auto nb = si32{ -1 };
                if (my >= st.body_top - 1 && my < st.div_bottom) nb = q_border_hit(cfg.columns(), mx, st.hscroll);
                if (st.hover_border != nb) { st.hover_border = nb; boss.base::deface(); }
            });
            boss.on(tier::mouserelease, input::key::MouseLeave, [&](hids&)
            {
                if (st.hover_expand != -1) { st.hover_expand = -1; boss.base::deface(); }
                if (st.press_expand != -1) { st.press_expand = -1; boss.base::deface(); }
                if (st.sb_hover)  { st.sb_hover = faux;  boss.base::deface(); }
                if (st.hsb_hover) { st.hsb_hover = faux; boss.base::deface(); }
                if (st.hover_border != -1) { st.hover_border = -1; boss.base::deface(); }
            });
            boss.on(tier::mouserelease, input::key::MouseWheel, [&](hids& gear)
            {
                if (gear.hzwhl || st.hsb_hover) { auto maxh = std::max(0, st.content_w - st.disp_w); st.hscroll = std::clamp(st.hscroll - gear.whlsi * 4, 0, maxh); }
                else { auto maxv = std::max(0, st.total - st.body_rows); st.scroll = std::clamp(st.scroll - gear.whlsi, 0, maxv); st.follow = st.scroll == maxv; }
                boss.base::deface();
            });
            boss.base::signal(tier::release, e2::form::draggable::_<hids::buttons::left>, true);
            boss.LISTEN(tier::release, e2::form::drag::start::_<hids::buttons::left>, gear)
            {
                if (!st.ctrl) return;
                auto px = (si32)gear.click.x, py = (si32)gear.click.y;
                if (auto sb = tbl_vsb(st); sb.ok && px == sb.x && py >= sb.top && py < sb.top + sb.track_h)
                {
                    pro::focus::set(boss.This(), gear.id, solo::on);
                    if (py >= sb.thumb_y && py < sb.thumb_y + sb.thumb_h) st.sb_grab = py - sb.thumb_y;
                    else { st.sb_grab = sb.thumb_h / 2; tbl_vsb_to(st, py, sb); }
                    st.sb_drag = st.sb_hover = true; st.drag = table_state::d_vsb;
                    st.follow = st.scroll == std::max(0, st.total - st.body_rows); boss.base::deface(); return;
                }
                if (auto sb = tbl_hsb(st); sb.ok && py == sb.top && px >= sb.x && px < sb.x + sb.track_h)
                {
                    pro::focus::set(boss.This(), gear.id, solo::on);
                    auto tx = sb.x + sb.thumb_y;
                    if (px >= tx && px < tx + sb.thumb_h) st.hsb_grab = px - tx;
                    else { st.hsb_grab = sb.thumb_h / 2; tbl_hsb_to(st, px, sb); }
                    st.hsb_drag = st.hsb_hover = true; st.drag = table_state::d_hsb; boss.base::deface(); return;
                }
                if (py >= st.body_top - 1 && py < st.div_bottom)
                {
                    auto tbl = cfg.columns();
                    auto v = q_border_hit(tbl, px, st.hscroll);
                    if (v >= 0)
                    {
                        pro::focus::set(boss.This(), gear.id, solo::on);
                        st.col_drag = v;
                        if (cfg.on_col_grab) cfg.on_col_grab(tbl.cols[(size_t)v].key);
                        st.drag = table_state::d_col; boss.base::deface(); return;
                    }
                }
                if (cfg.selection && py >= st.body_top && py < st.body_top + st.body_rows)
                {
                    pro::focus::set(boss.This(), gear.id, solo::on);
                    q_rubber_begin(st, cfg.selection(), st.scroll + (py - st.body_top), !!(gear.ctlstat & hids::anyCtrl));
                    boss.base::deface(); return;
                }
            };
            boss.LISTEN(tier::release, e2::form::drag::pull::_<hids::buttons::left>, gear)
            {
                auto mx = (si32)gear.coord.x, my = (si32)gear.coord.y;
                switch (st.drag)
                {
                    case table_state::d_vsb: tbl_vsb_to(st, my, tbl_vsb(st)); st.follow = st.scroll == std::max(0, st.total - st.body_rows); boss.base::deface(); break;
                    case table_state::d_hsb: tbl_hsb_to(st, mx, tbl_hsb(st)); boss.base::deface(); break;
                    case table_state::d_col:
                    {
                        auto tbl = cfg.columns();
                        if (st.col_drag >= 0 && st.col_drag < (si32)tbl.cols.size())
                        {
                            auto left = tbl.col_x(st.col_drag);
                            auto ww = std::clamp(mx + st.hscroll - left + 1, g_col_min, g_col_max);
                            q_set_col_w(tbl, st.col_drag, ww); boss.base::deface();
                        }
                        break;
                    }
                    case table_state::d_rubber:
                        q_rubber_pull(st, cfg.selection(), st.scroll + std::clamp(my - st.body_top, 0, std::max(0, st.body_rows - 1))); boss.base::deface(); break;
                    default: break;
                }
            };
            // A drag ends: drop the gesture mode and transient drag flags (inlined into both events —
            // the LISTEN macro captures by reference, so a shared local lambda would dangle).
            boss.LISTEN(tier::release, e2::form::drag::stop::_<hids::buttons::left>,   gear)
            { auto was = st.drag; st.drag = table_state::d_none; st.sb_drag = st.hsb_drag = faux; st.col_drag = -1; st.rubber_a = st.rubber_b = -1; if (was != table_state::d_none) boss.base::deface(); };
            boss.LISTEN(tier::release, e2::form::drag::cancel::_<hids::buttons::left>, gear)
            { auto was = st.drag; st.drag = table_state::d_none; st.sb_drag = st.hsb_drag = faux; st.col_drag = -1; st.rubber_a = st.rubber_b = -1; if (was != table_state::d_none) boss.base::deface(); };
            boss.on(tier::mouserelease, input::key::LeftDoubleClick, [&](hids& gear)
            {
                if (!st.ctrl) return;
                auto mx = (si32)gear.coord.x, my = (si32)gear.coord.y;
                if (my < st.body_top - 1 || my >= st.div_bottom) return;
                auto tbl = cfg.columns();
                auto v = q_border_hit(tbl, mx, st.hscroll);
                if (v >= 0) { auto cw = q_col_autofit(tbl, v); q_set_col_w(tbl, v, std::clamp(cw + 1, g_col_min, g_col_max)); boss.base::deface(); gear.dismiss(); }
            });
            boss.LISTEN(tier::preview, input::events::keybd::any, gear)
            {
                if (!st.focused || !st.ctrl) return;
                if (gear.payload != input::keybd::type::keypress) return;
                if (gear.keystat == input::key::released || gear.keystat == input::key::interrupted) return;
                if (gear.keybd::handled) return;
                if (cfg.on_key && cfg.on_key(gear, ptr::shadow(boss.This()))) return;
                if (!cfg.arrow_nav || !cfg.selection) return;
                auto k = gear.keybd::generic();
                auto s = cfg.selection();
                auto n = cfg.rows ? cfg.rows() : 0;
                auto maxv = std::max(0, st.total - st.body_rows);
                auto page = std::max(1, st.body_rows - 1);
                auto sels = std::vector<si32>{}; // Ordered selectable display rows.
                for (auto i = si32{}; i < n; ++i) if (s.key_of_row(i) >= 0) sels.push_back(i);
                auto select_at = [&](si32 p)
                {
                    if (sels.empty()) return;
                    p = std::clamp(p, 0, (si32)sels.size() - 1);
                    auto dr = sels[(size_t)p]; auto key = s.key_of_row(dr);
                    s.clear(); s.set_sel(key, true); st.sel_anchor = key;
                    if      (dr < st.scroll)                 st.scroll = dr;
                    else if (dr >= st.scroll + st.body_rows) st.scroll = dr - st.body_rows + 1;
                    st.scroll = std::clamp(st.scroll, 0, maxv); st.follow = st.scroll == maxv;
                };
                auto cur = si32{ -1 };
                for (auto p = si32{}; p < (si32)sels.size(); ++p) if (s.key_of_row(sels[(size_t)p]) == st.sel_anchor) { cur = p; break; }
                if (cur < 0) for (auto p = si32{}; p < (si32)sels.size(); ++p) if (s.is_sel(s.key_of_row(sels[(size_t)p]))) { cur = p; break; }
                auto act = true;
                     if (k == input::key::KeyUpArrow)    select_at(cur < 0 ? (si32)sels.size() - 1 : cur - 1);
                else if (k == input::key::KeyDownArrow)  select_at(cur < 0 ? 0 : cur + 1);
                else if (k == input::key::KeyHome)     { st.scroll = 0;    st.follow = maxv == 0; }
                else if (k == input::key::KeyEnd)      { st.scroll = maxv; st.follow = true; }
                else if (k == input::key::KeyPageUp)   { st.scroll = std::clamp(st.scroll - page, 0, maxv); st.follow = st.scroll == maxv; }
                else if (k == input::key::KeyPageDown) { st.scroll = std::clamp(st.scroll + page, 0, maxv); st.follow = st.scroll == maxv; }
                else if (k == input::key::Esc)         { s.clear(); st.sel_anchor = -1; }
                else act = false;
                if (act) { gear.set_handled(); boss.base::deface(); }
            };
        });
        return form;
    }
}
