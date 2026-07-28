// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/components/table.hpp: the reusable TABLE core component.
//
// This is the single place that owns ALL table behavior: the render frame (background, column
// header, text/arbitrary component cells, dividers, selection highlight, scrollbars), vertical/
// horizontal scrolling (wheel, scrollbar drag/paging), row selection (click / ctrl / shift / rubber-
// band), column resize + auto-fit + show/hide menu, an optional tree gutter (direction arrow + expand
// button), optional single-select arrow-key navigation, right-click menus, and focus. A caller does
// NOT write event handlers: it supplies a `table_cfg` (column model + per-cell data + selection/menu
// adapters + a few hooks) and calls make_table(). Concrete views (the transfer views, the checksums
// view) are just table_cfg builders and optional container adapters — they own no widget code of
// their own.
//
// make_table() returns the common retained component handle. Containers can wrap it as needed.

#include "input.hpp"

#include <algorithm>
#include <functional>
#include <set>
#include <unordered_map>
#include <variant>
#include <vector>

namespace netxs::app::parvion
{
    // All colors painted by the reusable table. Callers may override individual
    // fields to make the component fit a surrounding surface; defaults preserve
    // the transfer/checksum tables' established appearance.
    struct table_palette
    {
        ui32 bg         = theme::bg;
        ui32 header     = theme::header;
        ui32 text_fg    = theme::text_fg;
        ui32 subtext    = theme::subtext;
        ui32 sel_bg     = theme::sel_bg;
        ui32 sel_bg_act = theme::sel_bg_act;
        ui32 sort_fg    = theme::sort_fg;
        ui32 sb_track   = theme::sb_track;
        ui32 sb_thumb   = theme::sb_thumb;
        ui32 sb_hover   = theme::sb_hover;
        ui32 sb_drag    = theme::sb_drag;
    };

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
        struct col_toggle { text title; si32 key = -1; bool shown = true; };
        std::vector<column>                    cols;      // Visible columns, left to right.
        std::vector<col_toggle>                roster;    // All columns for the header show/hide menu.
        si32                                   left = 1;  // First-column x (cell 0 is reserved for the table menu).
        std::function<void(si32 key, bool on)> on_show_column;   // Flip a column's visibility.
        std::function<void(si32 key, si32 w)>  on_resize_column; // Persist a dragged column width.
        // Return the widest body-cell width. The core adds any header decoration and the divider.
        std::function<si32(si32 key)>          autofit;

        // `menu_label` is caller-owned and may contain an '&' shortcut marker. The table never adds
        // or changes one; an empty value uses the plain header title.
        void add_column(column col, bool shown, text menu_label = {})
        {
            roster.push_back({ menu_label.empty() ? col.title : std::move(menu_label), col.key, shown });
            if (shown) cols.push_back(std::move(col));
        }

        auto col_x(si32 i) const -> si32
        {
            auto x = std::max(1, left);
            for (auto j = si32{}; j < i && j < (si32)cols.size(); ++j) x += cols[(size_t)j].width;
            return x;
        }
        auto border_cx(si32 i) const -> si32 { return i < 0 || i >= (si32)cols.size() ? -1 : col_x(i) + cols[(size_t)i].width - 1; }
        auto content_w() const -> si32 { auto x = std::max(1, left); for (auto& c : cols) x += c.width; return x; }

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
        void paint_header(auto& canvas, si32 row, si32 hscroll, si32 disp_w, si32 strip_w,
                          si32 sort_key, si32 sort_dir, si32 hover_key, si32 press_key,
                          bool sortable, table_palette const& pal) const
        {
            canvas.fill(rect{{ 0, row }, { strip_w, 1 }}, [&](cell& c){ c.bgc(pal.header); });
            for (auto i = si32{}; i < (si32)cols.size(); ++i)
            {
                auto& col = cols[(size_t)i];
                auto colw = std::max(0, col.width - 1); // Divider is not part of the button.
                auto sx   = col_x(i) - hscroll;
                auto x0   = std::max(0, sx);
                auto x1   = std::min(disp_w, sx + colw);
                if (x1 <= x0) continue;

                auto active = sortable && col.key == sort_key && sort_dir != 0;
                auto mark   = active ? (sort_dir == 1 ? "\xE2\x86\x91" : "\xE2\x86\x93") : "\xE2\x86\x95"; // ↑ : ↓ : ↕
                auto markfg = active ? pal.sort_fg : pal.subtext;
                if (sortable && colw == 1) paint_at(canvas, col_x(i), 1, row, mark, markfg, pal.header, hscroll, disp_w);
                else if (colw > 0)
                {
                    auto labelw = sortable ? std::max(0, colw - 2) : colw;
                    if (col.right)
                    {
                        auto title = fit_ellipsis(col.title, labelw);
                        auto tw = (si32)cell_width(title);
                        paint_at(canvas, col_x(i) + std::max(0, labelw - tw), tw, row, title,
                                 pal.subtext, pal.header, hscroll, disp_w);
                    }
                    else paint_at(canvas, col_x(i), labelw, row, col.title, pal.subtext, pal.header, hscroll, disp_w);
                    if (sortable) paint_at(canvas, col_x(i) + colw - 1, 1, row, mark, markfg, pal.header, hscroll, disp_w);
                }
                auto box = rect{{ x0, row }, { x1 - x0, 1 }};
                if      (col.key == press_key) canvas.fill(box, [](cell& c){ c.xlight(2); });
                else if (col.key == hover_key) canvas.fill(box, [](cell& c){ c.xlight(); });
            }
        }
        void paint_header_menu(auto& canvas, si32 row, si32 disp_w, bool hover, bool press,
                               bool enabled, table_palette const& pal) const
        {
            if (disp_w <= 0 || !enabled) return;
            auto box = rect{{ 0, row }, { 1, 1 }};
            put_str(canvas, 0, row, "\xE2\x89\xA1", pal.subtext, pal.header, 1); // ≡
            if      (press) canvas.fill(box, [](cell& c){ c.xlight(2); });
            else if (hover) canvas.fill(box, [](cell& c){ c.xlight(); });
        }
        void paint_dividers(auto& canvas, si32 top, si32 bottom, si32 hscroll, si32 disp_w,
                            si32 hover_idx, si32 drag_idx, table_palette const& pal) const
        {
            for (auto i = si32{}; i < (si32)cols.size(); ++i)
            {
                auto bx = border_cx(i) - hscroll;
                if (bx < 0 || bx >= disp_w) continue;
                auto fg = (hover_idx == i || drag_idx == i) ? pal.text_fg : pal.subtext;
                canvas.fill(rect{{ bx, top }, { 1, bottom - top }}, [&](cell& c){ c.fgc(fg).txt("\xE2\x94\x82"); }); // │
            }
        }
        // Paint text into the cell [x, x+w) at row y, shifted left by `hscroll`, clipped to
        // [0, disp_w); wider content is tail-ellipsized (fit_ellipsis). The shared cell primitive.
        static void paint_at(auto& canvas, si32 x, si32 w, si32 y, view s, ui32 fg, ui32 bg, si32 hscroll, si32 disp_w)
        {
            auto sx = x - hscroll;
            if (w <= 0 || disp_w <= 0 || sx >= disp_w || sx + w <= 0) return;

            auto text = fit_ellipsis(s, w);
            auto cut  = std::max(0, -sx);
            auto used = si32{};
            auto from = size_t{};
            if (cut)
            {
                utf::decode_clusters(text, [&](view cl)
                {
                    auto cw = gc_cells(utf::cluster(cl));
                    if (used >= cut) return faux;
                    used += cw;
                    from += cl.size();
                    return true;
                });
                if (used < cut) return;
            }

            auto dx   = sx + used;
            auto room = std::min(w - used, disp_w - dx);
            if (room <= 0 || dx >= disp_w) return;
            put_str(canvas, dx, y, view{ text }.substr(from), fg, bg, room);
        }
    };

    // The table's mutable state: scroll/geometry cache + drag + selection-transient + expand-transient.
    // The body paints into the widget canvas from row 1 (row 0 is the column header); `tab_row` is the
    // exclusive bottom of the body (== canvas height).
    struct table_state
    {
        enum dmode { d_none, d_vsb, d_hsb, d_col, d_rubber };
        enum sort_mode { sort_default, sort_ascending, sort_descending };

        bool focused = faux;

        si32 scroll = 0, hscroll = 0;
        bool live_follow = faux;
        bool sb_hover = faux,  sb_press = faux,  sb_drag = faux;  si32 sb_grab = 0;
        bool hsb_hover = faux, hsb_press = faux, hsb_drag = faux; si32 hsb_grab = 0;
        si32 hover_border = -1, col_drag = -1;
        si32 sort_key = -1, sort_dir = sort_default;
        si32 hover_header = -1, press_header = -1;
        bool hover_header_menu = faux, press_header_menu = faux;
        si32 sel_anchor = -1; // Fixed endpoint for Shift ranges.
        si32 nav_cursor = -1; // Movable keyboard/range endpoint.
        si32 rubber_a = -1, rubber_b = -1;
        si32 drag_y = 0;
        std::set<si32> drag_base;
        bool rubber_ctrl = faux, rubber_add = true;
        dmode drag = d_none;

        std::vector<std::pair<rect, si32>> row_hit{};    // Row rect -> selection key.
        std::vector<std::pair<rect, si32>> expand_hit{}; // Expand-button box -> expand id.
        std::vector<si32> row_order{};                   // Visual row -> caller/source row.
        std::vector<si32> row_offsets{};                 // Visual row -> content-line offset; includes end sentinel.
        std::vector<rect> selected_component_areas{};    // Selected component cells in body-host coordinates.
        std::vector<rect> selected_row_areas{};          // Selected visible row spans in body-host coordinates.
        std::unordered_map<id_t, component> attached{};  // Components currently attached to the clipped body host.
        netxs::wptr<ui::base> cell_host_wp{};
        si32 hover_expand = -1, press_expand = -1;

        si32 body_top = 1, body_rows = 0, tab_row = 0;
        si32 content_w = 0, disp_w = 0, vsb_x = 0, hsb_y = 0;
        si32 div_bottom = 1;
        si32 total = 0, total_lines = 0;
        bool has_vsb = faux, has_hsb = faux;
        ui64 revision = ~ui64{}; // Last caller data/view revision (optional scroll-reset hook).
    };

    // Optional left-gutter content for one row: a direction arrow + an expand indicator/button.
    enum expand_kind { xp_none, xp_muted, xp_collapsed, xp_expanded };
    struct gutterval { text arrow; ui32 arrow_fg = 0; si32 expand_id = -1; expand_kind expand = xp_none; };
    struct table_text_cell
    {
        text value;
        ui32 foreground = 0;
    };
    struct table_component_cell
    {
        component content;
    };
    struct table_cell
    {
        using value_type = std::variant<std::monostate, table_text_cell, table_component_cell>;
        value_type value;

        table_cell() = default;
        table_cell(text text_value, ui32 foreground = 0)
            : value{ table_text_cell{ std::move(text_value), foreground } } { }
        table_cell(char const* text_value, ui32 foreground = 0)
            : table_cell{ text{ text_value }, foreground } { }
        table_cell(component content)
            : value{ table_component_cell{ std::move(content) } } { }
    };

    // Persistent live-update target. Unlike a one-shot reveal, this target is reapplied on every
    // render while live following is enabled.
    struct table_follow_target
    {
        enum kind { tail, source_row } mode = tail;
        si32 row = -1;
    };

    // Result of a caller-owned key handler. A row reveal is a one-shot viewport operation and can
    // independently request that the horizontal viewport return to its origin.
    struct table_viewport_action
    {
        enum kind { unhandled, handled, reveal_row } mode = unhandled;
        si32 row = -1;
        bool reset_horizontal = faux;
    };

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
        std::function<bool(si32)>       is_selected;
        std::function<void(si32, bool)> on_select;
        std::function<void()>           on_clear;
        std::function<bool()>           has_selection;
        std::function<bool(si32)>       in_scope;   // key -> selectable now (Shift-range filter).
        std::function<si32()>           row_count;  // Number of display rows.
        std::function<si32(si32)>       key_of_row; // Display row -> key (-1 if not selectable).
    };

    // Right-click menus for a table. The column-header show/hide menu is intrinsic (built from the
    // column roster); the body uses one menu after synchronizing selection with the click target.
    struct qmenu_cfg
    {
        std::function<std::vector<app::shared::menu::item>()>     items;           // Unified body menu (null = none).
        std::function<void(si32)>                                 on_item_rclick;  // Selection sync for a row (null = none).
        std::function<void()>                                     on_blank_rclick; // Selection reset on blank (null = none).
    };

    // Opt-in Delete-key behavior. `on_delete` is the upstream override: returning true consumes
    // the key and suppresses the table's default selected-row removal. A null `confirm` removes
    // immediately; otherwise the table owns the confirmation flow and dialog lifetime.
    struct table_delete_cfg
    {
        bool                                             enabled = faux;
        std::function<void(netxs::wptr<ui::base>)>       on_remove_selected;
        std::function<app::shared::confirm_dialog_text()> confirm;
        std::function<bool(hids&, netxs::wptr<ui::base>)> on_delete;
    };

    // The complete table configuration a caller supplies. Everything here is DATA / adapters; there
    // is no event-handling code. `columns` / `rows` / `cell` / `gutter` / `follow` are re-queried from
    // live state each render or hit-test.
    struct table_cfg
    {
        netxs::wptr<ui::base> window_wp;      // App window: anchor for confirm dialogs.
        std::function<qtable()>                          columns;     // Column model (rebuilt each render/hit).
        std::function<si32()>                            row_count;   // Number of display rows.
        std::function<si32(si32 source_row)>             row_height;  // Explicit content height; defaults to one line.
        std::function<table_cell(si32 row, si32 key)>    cell;        // Text or retained component for logical column `key`.
        std::function<gutterval(si32 row)>               gutter;      // Left-gutter arrow/expand (null => no gutter).
        std::function<void(si32 expand_id)>              on_toggle;   // Toggle a row's expansion.
        std::function<qsel_cfg()>                        selection;   // null => not selectable.
        std::function<qmenu_cfg(netxs::wptr<ui::base>)>  menu;        // Unified body menu (null => none; header menu still shows).
        std::function<table_follow_target()>             follow;      // Persistent live-update target (null => no live following).
        std::function<ui64()>                            revision;    // Change token: reset vertical/horizontal viewport when it changes.
        std::function<si32()>                            revision_row;// One-shot source row to reveal after a revision change (-1 => top).
        std::function<si32(si32 source_row)>             sort_group;  // Fixed ascending group rank; direction only reverses within a group.
        std::function<si32(si32, si32, si32)>            compare;     // Source rows a/b + column key -> negative/equal/positive.
        std::function<void(si32 source_row)>             on_activate; // Double-click/Enter activation (null => none).
        table_delete_cfg                                 deletion;    // Opt-in Delete-key selected-row removal.
        std::function<void(si32 key)>                    on_col_grab; // A column-border drag begins (null => none).
        std::function<text()>                            empty_text;  // Message shown when rows()==0 (null => none).
        std::function<table_viewport_action(hids&, netxs::wptr<ui::base>)> on_key; // App key + optional one-shot viewport action.
        bool                                             wide_hit = faux;  // Row hit-box spans full body width (else content width).
        bool                                             arrow_nav = true; // Single-select arrow-key navigation for selectable tables.
        bool                                             focus_on_start = faux; // Construct with initial focus (modal picker lists).
        table_palette                                    palette{};         // Complete table paint palette.
    };

    // ---- Scroll layer ------------------------------------------------------------------------------
    inline void tbl_layout(table_state& st, si32 w, si32 h, si32 total_lines, si32 content_w, si32 body_top)
    {
        st.total_lines = std::max(0, total_lines);
        st.tab_row   = h; // Exclusive body bottom: the page canvas has no reserved strip/handle rows.
        st.body_top  = body_top;
        st.content_w = content_w;
        auto avail = std::max(0, st.tab_row - st.body_top);
        for (auto pass = si32{}; pass < 2; ++pass)
        {
            st.has_vsb   = avail > 0 && st.total_lines > avail;
            st.disp_w    = w - (st.has_vsb ? 1 : 0);
            st.has_hsb   = st.content_w > st.disp_w;
            st.body_rows = std::max(0, avail - (st.has_hsb ? 1 : 0));
            st.has_vsb   = st.body_rows > 0 && st.total_lines > st.body_rows;
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
        sb.ok = st.has_vsb && st.body_rows > 0 && st.total_lines > st.body_rows;
        if (!sb.ok) return sb;
        sb.x = st.vsb_x; sb.top = st.body_top; sb.track_h = st.body_rows;
        sb.thumb_h = std::max(1, st.body_rows * st.body_rows / st.total_lines);
        sb.maxscroll = st.total_lines - st.body_rows;
        sb.thumb_y = sb.top + ((st.body_rows - sb.thumb_h) * st.scroll + sb.maxscroll / 2) / sb.maxscroll;
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
        sb.thumb_y = ((st.disp_w - sb.thumb_h) * st.hscroll + sb.maxscroll / 2) / sb.maxscroll;
        return sb;
    }
    inline void tbl_vsb_to(table_state& st, si32 local_y, q_sb const& sb)
    {
        auto travel = sb.track_h - sb.thumb_h;
        if (travel <= 0) return;
        st.scroll = std::clamp(((local_y - st.sb_grab - sb.top) * sb.maxscroll + travel / 2) / travel, 0, sb.maxscroll);
    }
    inline void tbl_hsb_to(table_state& st, si32 local_x, q_sb const& sb)
    {
        auto travel = sb.track_h - sb.thumb_h;
        if (travel <= 0) return;
        st.hscroll = std::clamp(((local_x - st.hsb_grab - sb.x) * sb.maxscroll + travel / 2) / travel, 0, sb.maxscroll);
    }
    inline void tbl_paint_scrollbars(table_state const& st, auto& canvas, table_palette const& pal)
    {
        if (auto sb = tbl_vsb(st); sb.ok)
        {
            auto mark = (st.sb_drag || st.sb_hover) ? "\xe2\x96\x88" : "\xe2\x96\x90"; // █ : ▐
            canvas.fill(rect{{ sb.x, sb.top }, { 1, sb.track_h }}, [&](cell& c){ c.bgc(pal.bg).fgc(pal.sb_track).txt(mark); });
            auto tc = st.sb_drag ? pal.sb_drag : st.sb_hover ? pal.sb_hover : pal.sb_thumb;
            canvas.fill(rect{{ sb.x, sb.thumb_y }, { 1, sb.thumb_h }}, [&](cell& c){ c.bgc(pal.bg).fgc(tc).txt(mark); });
            if (st.sb_press || st.sb_drag)
                canvas.fill(rect{{ sb.x, sb.thumb_y }, { 1, sb.thumb_h }}, [](cell& c){ c.xlight(2); });
        }
        if (auto sb = tbl_hsb(st); sb.ok)
        {
            auto mark = (st.hsb_drag || st.hsb_hover) ? "\xe2\x96\x84" : "\xe2\x96\x82"; // ▄ : ▂
            canvas.fill(rect{{ sb.x, sb.top }, { sb.track_h, 1 }}, [&](cell& c){ c.bgc(pal.bg).fgc(pal.sb_track).txt(mark); });
            auto tc = st.hsb_drag ? pal.sb_drag : st.hsb_hover ? pal.sb_hover : pal.sb_thumb;
            canvas.fill(rect{{ sb.x + sb.thumb_y, sb.top }, { sb.thumb_h, 1 }}, [&](cell& c){ c.bgc(pal.bg).fgc(tc).txt(mark); });
            if (st.hsb_press || st.hsb_drag)
                canvas.fill(rect{{ sb.x + sb.thumb_y, sb.top }, { sb.thumb_h, 1 }}, [](cell& c){ c.fgc().xlight(2); });
        }
    }
    inline void tbl_clamp(table_state& st)
    {
        st.scroll  = std::clamp(st.scroll,  0, std::max(0, st.total_lines - st.body_rows));
        st.hscroll = std::clamp(st.hscroll, 0, std::max(0, st.content_w - st.disp_w));
    }
    struct page_nav_state { si32 offset = 0, cursor = 0; };
    // Explorer-style paging: visit the current edge before scrolling a page.
    inline auto q_page_nav(si32 total, si32 page, si32 offset, si32 cursor, si32 dir) -> page_nav_state
    {
        if (total <= 0) return {};
        page = std::max(1, page);
        auto maxoff = std::max(0, total - page);
        cursor = std::clamp(cursor, 0, total - 1);
        offset = std::clamp(offset, 0, maxoff);
        auto top = offset;
        auto bottom = std::min(total - 1, offset + page - 1);
        if (dir < 0)
        {
            if (cursor != top) cursor = top;
            else if (offset > 0)
            {
                offset = std::max(0, offset - page);
                cursor = offset;
            }
            else
                cursor = 0;
        }
        else
        {
            if (cursor != bottom) cursor = bottom;
            else if (offset < maxoff)
            {
                offset = std::min(maxoff, offset + page);
                cursor = std::min(total - 1, offset + page - 1);
            }
            else
                cursor = total - 1;
        }
        return { offset, cursor };
    }
    inline auto q_reveal_scroll(si32 total, si32 page, si32 offset, si32 row) -> si32
    {
        page = std::max(0, page);
        auto maxoff = std::max(0, total - page);
        offset = std::clamp(offset, 0, maxoff);
        if (total <= 0 || page <= 0 || row < 0) return offset;
        row = std::clamp(row, 0, total - 1);
        if (row < offset)        return row;
        if (row >= offset + page) return std::clamp(row - page + 1, 0, maxoff);
        return offset;
    }
    inline auto q_row_w(table_state const& st) -> si32 { return std::clamp(st.content_w - st.hscroll, 0, st.disp_w); }
    inline auto q_border_hit(qtable const& t, si32 mx, si32 hscroll) -> si32
    {
        for (auto v = si32{}; v < (si32)t.cols.size(); ++v)
            if (t.cols[(size_t)v].resizable && mx == t.border_cx(v) - hscroll) return v;
        return -1;
    }
    inline auto q_header_hit(qtable const& t, si32 mx, si32 hscroll) -> si32
    {
        auto cx = mx + hscroll;
        for (auto v = si32{}; v < (si32)t.cols.size(); ++v)
        {
            auto x = t.col_x(v);
            if (cx >= x && cx < x + std::max(0, t.cols[(size_t)v].width - 1)) return v;
        }
        return -1;
    }
    inline auto q_col_fit_w(view title, si32 body_w, bool sortable) -> si32
    {
        auto header_w = (si32)cell_width(title) + (sortable ? 2 : 0); // Separator + sort glyph.
        return std::clamp(std::max(body_w, header_w) + 1, g_col_min, g_col_max); // Trailing divider.
    }
    inline void q_set_col_w(qtable const& t, si32 v, si32 w)
    {
        if (v >= 0 && v < (si32)t.cols.size() && t.on_resize_column)
            t.on_resize_column(t.cols[(size_t)v].key, std::clamp(w, g_col_min, g_col_max));
    }
    inline auto q_col_autofit(qtable const& t, si32 v, bool sortable) -> si32
    {
        if (v < 0 || v >= (si32)t.cols.size()) return g_col_min;
        auto& col = t.cols[(size_t)v];
        auto body_w = t.autofit ? t.autofit(col.key) : si32{};
        return q_col_fit_w(col.title, body_w, sortable);
    }
    inline auto q_row_at(table_state const& st, si32 mx, si32 my) -> si32
    {
        for (auto& [b, key] : st.row_hit)
            if (my >= b.coor.y && my < b.coor.y + b.size.y
             && mx >= b.coor.x && mx < b.coor.x + b.size.x) return key;
        return -1;
    }
    inline auto q_source_row(table_state const& st, si32 visual_row) -> si32
    {
        return visual_row >= 0 && visual_row < (si32)st.row_order.size()
             ? st.row_order[(size_t)visual_row]
             : -1;
    }
    inline auto q_visual_row(table_state const& st, si32 source_row) -> si32
    {
        for (auto i = si32{}; i < (si32)st.row_order.size(); ++i)
            if (st.row_order[(size_t)i] == source_row) return i;
        return -1;
    }
    inline auto q_visual_at_line(table_state const& st, si32 line) -> si32
    {
        if (st.total <= 0 || st.row_offsets.size() < 2) return -1;
        line = std::clamp(line, 0, std::max(0, st.total_lines - 1));
        auto iter = std::upper_bound(st.row_offsets.begin(), st.row_offsets.end(), line);
        return std::clamp((si32)(iter - st.row_offsets.begin()) - 1, 0, st.total - 1);
    }
    inline auto q_row_top(table_state const& st, si32 visual_row) -> si32
    {
        return visual_row >= 0 && visual_row < st.total ? st.row_offsets[(size_t)visual_row] : -1;
    }
    inline auto q_row_bottom(table_state const& st, si32 visual_row) -> si32
    {
        return visual_row >= 0 && visual_row < st.total ? st.row_offsets[(size_t)visual_row + 1] : -1;
    }
    inline auto q_reveal_visual(table_state const& st, si32 offset, si32 visual_row) -> si32
    {
        auto maxscroll = std::max(0, st.total_lines - st.body_rows);
        offset = std::clamp(offset, 0, maxscroll);
        if (visual_row < 0 || visual_row >= st.total || st.body_rows <= 0) return offset;
        auto top = q_row_top(st, visual_row);
        auto bottom = q_row_bottom(st, visual_row);
        if (top < offset) return std::clamp(top, 0, maxscroll);
        if (bottom > offset + st.body_rows)
            return std::clamp(bottom - top > st.body_rows ? top : bottom - st.body_rows, 0, maxscroll);
        return offset;
    }
    inline auto q_follow_scroll(table_state const& st, table_cfg const& cfg) -> si32
    {
        if (!cfg.follow) return -1;
        auto maxscroll = std::max(0, st.total_lines - st.body_rows);
        auto target = cfg.follow();
        if (target.mode == table_follow_target::tail) return maxscroll;
        auto row = q_visual_row(st, target.row);
        return row >= 0 ? std::clamp(q_row_bottom(st, row) - st.body_rows, 0, maxscroll) : -1;
    }
    inline auto q_at_follow_target(table_state const& st, table_cfg const& cfg) -> bool
    {
        auto scroll = q_follow_scroll(st, cfg);
        return scroll >= 0 && st.scroll == scroll;
    }
    inline void q_build_order(table_state& st, table_cfg const& cfg, si32 nrows)
    {
        st.total = std::max(0, nrows);
        st.row_order.resize((size_t)std::max(0, nrows));
        for (auto i = si32{}; i < nrows; ++i) st.row_order[(size_t)i] = i;
        if (!cfg.compare || st.sort_key < 0 || st.sort_dir == table_state::sort_default) return;
        auto descending = st.sort_dir == table_state::sort_descending;
        std::stable_sort(st.row_order.begin(), st.row_order.end(), [&](si32 a, si32 b)
        {
            if (cfg.sort_group)
            {
                auto ga = cfg.sort_group(a);
                auto gb = cfg.sort_group(b);
                if (ga != gb) return ga < gb;
            }
            auto cmp = cfg.compare(a, b, st.sort_key);
            return descending ? cmp > 0 : cmp < 0;
        });
    }
    inline void q_build_row_offsets(table_state& st, table_cfg const& cfg)
    {
        st.row_offsets.assign((size_t)st.total + 1, 0);
        for (auto visual = si32{}; visual < st.total; ++visual)
        {
            auto source = q_source_row(st, visual);
            auto height = cfg.row_height && source >= 0 ? cfg.row_height(source) : 1;
            st.row_offsets[(size_t)visual + 1] = st.row_offsets[(size_t)visual] + std::max(1, height);
        }
        st.total_lines = st.row_offsets.empty() ? 0 : st.row_offsets.back();
    }
    inline auto q_ordered_sel(table_state const& st, qsel_cfg s) -> qsel_cfg
    {
        auto key_of_source = s.key_of_row;
        s.row_count = [&st]{ return (si32)st.row_order.size(); };
        s.key_of_row = [&st, key_of_source](si32 visual_row)
        {
            auto source_row = q_source_row(st, visual_row);
            return source_row >= 0 && key_of_source ? key_of_source(source_row) : -1;
        };
        return s;
    }

    // Resolve the one selected row that keyboard actions should operate on.  The table keeps a
    // navigation cursor so a multi-selection has a movable endpoint, but caller-owned selection can
    // be replaced independently (directory navigation, refresh, delete settlement, etc.).  A stale
    // cursor must therefore never outrank what is actually painted as selected.
    inline auto q_selected_row(qsel_cfg const& s, si32 preferred_key) -> si32
    {
        auto fallback = si32{ -1 };
        for (auto row = si32{}; row < s.row_count(); ++row)
        {
            auto key = s.key_of_row(row);
            if (key < 0 || !s.is_selected(key)) continue;
            if (key == preferred_key) return row;
            if (fallback < 0) fallback = row;
        }
        return fallback;
    }

    inline void q_reset_selection_state(table_state& st)
    {
        st.sel_anchor = st.nav_cursor = -1;
        st.rubber_a = st.rubber_b = -1;
        st.drag_y = 0;
        st.drag_base.clear();
        st.rubber_ctrl = faux;
        st.rubber_add = true;
        st.drag = table_state::d_none;
    }

    // ---- Selection layer ---------------------------------------------------------------------------
    inline auto q_sel_press(table_state& st, qsel_cfg const& s, si32 hit, bool ctl, bool shft) -> bool
    {
        if (hit < 0) return faux;
        if (shft && st.sel_anchor >= 0 && st.sel_anchor < s.key_count())
        {
            auto anchor_row = si32{ -1 }, hit_row = si32{ -1 };
            for (auto i = si32{}; i < s.row_count(); ++i)
            {
                auto key = s.key_of_row(i);
                if (key == st.sel_anchor) anchor_row = i;
                if (key == hit)           hit_row = i;
            }
            if (anchor_row >= 0 && hit_row >= 0)
            {
                auto lo = std::min(anchor_row, hit_row), hi = std::max(anchor_row, hit_row);
                s.on_clear();
                for (auto i = lo; i <= hi; ++i)
                {
                    auto key = s.key_of_row(i);
                    if (key >= 0 && s.in_scope(key)) s.on_select(key, true);
                }
                s.on_select(hit, true); // Keep the caller's navigation cursor on the movable endpoint.
            }
            else
            {
                s.on_clear();
                s.on_select(hit, true);
                st.sel_anchor = hit;
            }
        }
        else if (ctl) { s.on_select(hit, !s.is_selected(hit)); st.sel_anchor = hit; }
        else          { s.on_clear(); s.on_select(hit, true); st.sel_anchor = hit; }
        st.nav_cursor = hit;
        return true;
    }
    inline auto q_sel_clear_blank(table_state& st, qsel_cfg const& s) -> bool
    {
        if (!s.has_selection()) { st.sel_anchor = st.nav_cursor = -1; return faux; }
        s.on_clear(); st.sel_anchor = st.nav_cursor = -1; return true;
    }
    inline void q_sel_snapshot(table_state& st, qsel_cfg const& s) { st.drag_base.clear(); for (auto k = si32{}; k < s.key_count(); ++k) if (s.is_selected(k)) st.drag_base.insert(k); }
    inline void q_rubber_begin(table_state& st, qsel_cfg const& s, si32 press_row, bool ctl)
    {
        st.rubber_a = st.rubber_b = std::max(0, press_row);
        st.drag = table_state::d_rubber;
        auto anchor_key = press_row >= 0 && press_row < s.row_count() ? s.key_of_row(press_row) : -1;
        st.rubber_ctrl = ctl;
        if (st.rubber_ctrl) st.rubber_add = !st.drag_base.count(anchor_key);
        else                s.on_clear();
        if (anchor_key >= 0) st.sel_anchor = st.nav_cursor = anchor_key;
    }
    inline auto q_rubber_endpoint_key(qsel_cfg const& s, si32 anchor_row, si32 cur_row) -> si32
    {
        auto count = s.row_count();
        if (count <= 0) return -1;
        auto lo = std::max(0,         std::min(anchor_row, cur_row));
        auto hi = std::min(count - 1, std::max(anchor_row, cur_row));
        if (lo > hi) return -1; // The entire band is outside the row range.
        auto downward = cur_row >= anchor_row;
        auto row  = downward ? hi : lo;
        auto end  = downward ? lo : hi;
        auto step = downward ? -1 : 1;
        while (true)
        {
            if (auto key = s.key_of_row(row); key >= 0) return key;
            if (row == end) return -1;
            row += step;
        }
    }
    inline void q_rubber_pull(table_state& st, qsel_cfg const& s, si32 cur_row)
    {
        st.rubber_b = cur_row;
        // Mouse-move events are sampled, so a pull can jump directly from an earlier row into the
        // blank area beyond the table.  Keep the keyboard cursor at the selectable edge of the
        // rubber band instead of leaving it on whichever row happened to receive the last event.
        if (auto key = q_rubber_endpoint_key(s, st.rubber_a, cur_row); key >= 0) st.nav_cursor = key;
        auto lo = std::min(st.rubber_a, st.rubber_b), hi = std::max(st.rubber_a, st.rubber_b);
        if (st.rubber_ctrl)
        {
            for (auto k = si32{}; k < s.key_count(); ++k) s.on_select(k, !!st.drag_base.count(k));
            for (auto i = lo; i <= hi; ++i) { auto k = i >= 0 && i < s.row_count() ? s.key_of_row(i) : -1; if (k >= 0) s.on_select(k, st.rubber_add); }
        }
        else
        {
            s.on_clear();
            for (auto i = lo; i <= hi; ++i) { auto k = i >= 0 && i < s.row_count() ? s.key_of_row(i) : -1; if (k >= 0) s.on_select(k, true); }
        }
    }
    inline auto q_drag_step(si32 distance) -> si32
    {
        return std::clamp(distance, si32{ 1 }, si32{ 8 });
    }
    inline auto q_rubber_visual_at_line(table_state const& st, si32 line) -> si32
    {
        return line >= st.total_lines ? st.total + (line - st.total_lines)
                                      : q_visual_at_line(st, line);
    }
    inline auto q_rubber_row_at_drag(table_state const& st) -> si32
    {
        auto line = st.scroll + std::clamp(st.drag_y - st.body_top, 0, std::max(0, st.body_rows - 1));
        return q_rubber_visual_at_line(st, line);
    }
    inline auto q_rubber_autoscroll(table_state& st, qsel_cfg const& s) -> bool
    {
        if (st.drag != table_state::d_rubber || st.body_rows <= 0) return faux;
        auto maxv = std::max(0, st.total_lines - st.body_rows);
        auto next = st.scroll;
        if      (st.drag_y <  st.body_top)                next -= q_drag_step(st.body_top - st.drag_y);
        else if (st.drag_y >= st.body_top + st.body_rows) next += q_drag_step(st.drag_y - (st.body_top + st.body_rows - 1));
        next = std::clamp(next, 0, maxv);
        if (next == st.scroll) return faux;
        st.scroll = next;
        st.live_follow = faux;
        q_rubber_pull(st, s, q_rubber_row_at_drag(st));
        return true;
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
    inline auto q_has_columns_menu(qtable const& t) -> bool
    {
        return !t.roster.empty() && (bool)t.on_show_column;
    }
    inline auto q_header_menu_hit(table_state const& st, bool enabled, si32 mx, si32 my) -> bool
    {
        return enabled && st.disp_w > 0 && mx == 0 && my == st.body_top - 1;
    }
    inline void q_open_columns_menu(auto& boss, qtable const& t, twod at)
    {
        namespace m = app::shared::menu;
        if (!q_has_columns_menu(t)) return;
        auto panel_wp = ptr::shadow(boss.This());
        auto deface   = [panel_wp]{ if (auto p = panel_wp.lock()) p->base::deface(); };
        m::open_dropdown_popup(boss, build_columns_menu(t.roster, t.on_show_column, deface),
            { .source = m::popup_source::context_menu, .cursor = at });
    }
    inline void q_open_table_menu(auto& boss, qmenu_cfg const& cfg, twod at)
    {
        namespace m = app::shared::menu;
        if (cfg.items) m::open_dropdown_popup(boss, cfg.items(),
            { .source = m::popup_source::context_menu, .cursor = at });
    }
    inline void q_context_menu(auto& boss, table_state& st, si32 mx, si32 my, qtable const& t, qmenu_cfg const& cfg)
    {
        auto at = twod{ mx, my };
        if (my == st.body_top - 1 && st.body_top > 0) // Column-header row: the intrinsic show/hide menu.
        {
            q_open_columns_menu(boss, t, at);
            return;
        }
        if (my < st.body_top || my >= st.tab_row) return;
        auto hit = q_row_at(st, mx, my);
        if (hit >= 0)
        {
            if (cfg.on_item_rclick) cfg.on_item_rclick(hit);
            st.sel_anchor = st.nav_cursor = hit; // The right-clicked row becomes the keyboard/range cursor.
        }
        else
        {
            if (cfg.on_blank_rclick) { cfg.on_blank_rclick(); st.sel_anchor = st.nav_cursor = -1; }
        }
        q_open_table_menu(boss, cfg, at);
    }

    inline void q_place_component(table_state& st, component content, rect area,
                                  std::unordered_map<id_t, rect>& visible)
    {
        auto widget = content.widget;
        auto host = st.cell_host_wp.lock();
        if (!widget || !host || area.size.x <= 0 || area.size.y <= 0) return;
        auto id = widget->id;
        if (!visible.emplace(id, area).second) return; // A retained widget may occupy only one visible cell.
        auto found = st.attached.find(id);
        if (found == st.attached.end())
        {
            auto parent = widget->base::parent();
            auto in_tree = parent && widget->base::holder != parent->base::subset.end();
            if (in_tree && parent != host) return;
            if (!in_tree) host->base::attach(widget);
            content.on_activate();
            found = st.attached.emplace(id, std::move(content)).first;
        }
        else
        {
            // Refresh lifecycle callbacks while retaining the same widget identity and state.
            found->second.activate = std::move(content.activate);
            found->second.deactivate = std::move(content.deactivate);
        }
    }
    inline void q_reconcile_components(table_state& st, std::unordered_map<id_t, rect> const& visible)
    {
        for (auto iter = st.attached.begin(); iter != st.attached.end();)
        {
            if (visible.contains(iter->first)) { ++iter; continue; }
            iter->second.on_deactivate();
            if (auto& widget = iter->second.widget; widget && widget->base::parent()) widget->base::detach();
            iter = st.attached.erase(iter);
        }
        // host.attach() performs a normal host layout and can temporarily resize previously
        // attached siblings. Apply every final cell rectangle only after all new children exist.
        for (auto const& [id, area] : visible)
        {
            auto found = st.attached.find(id);
            if (found == st.attached.end() || !found->second.widget) continue;
            auto& widget = found->second.widget;
            widget->base::hidden = faux;
            widget->base::extend(area);
            widget->base::deface();
        }
    }
    inline auto q_component_has_focus(table_state const& st) -> bool
    {
        for (auto const& [id, content] : st.attached)
            if (content.widget
             && content.widget->base::signal(tier::request, e2::form::state::focus::count) > 0)
                return true;
        return faux;
    }

    inline void q_paint_component_selection(table_state const& st, table_palette const& pal, ui::face& viewport)
    {
        // Components paint after the table's row background. Tint only their backgrounds so their
        // own fill/track contrast, text, glyphs, and mouse-link metadata remain intact.
        for (auto const& area : st.selected_component_areas)
            viewport.fill(area, [](cell& c){ c.bgc().xlight(); });

        // A horizontally scrolled component can cross x=0 and overwrite the focused-row marker.
        // Restore that marker after all component tinting.
        if (st.focused)
            for (auto const& area : st.selected_row_areas)
                viewport.fill(rect{{ 0, area.coor.y }, { 1, area.size.y }},
                              [&](cell& c){ c.bgc(pal.sel_bg_act); });
    }

    // Render retained cell components into a canvas whose physical extent is exactly the table body
    // viewport.  A normal cake narrows only face::clip(); primitives such as face::fill(rect, ...)
    // can still address the larger parent backing canvas and leak through either horizontal edge.
    // The bounded intermediate face makes such writes impossible while preserving every child's
    // logical (possibly negative/oversized) cell rectangle and its painted mouse-link metadata.
    inline auto make_table_cell_host(std::function<void(ui::face&)> postrender = {}) -> ui::sptr
    {
        auto host = ui::mock::ctor();
        host->invoke([postrender = std::move(postrender)](auto& boss)
        {
            boss.LISTEN(tier::release, e2::render::any, parent_canvas, -, (postrender))
            {
                auto size = boss.base::size();
                if (size.x <= 0 || size.y <= 0) return;

                auto& viewport = boss.base::field(ui::face{});
                viewport.area(rect{{}, size});
                viewport.fill(parent_canvas, cell::shaders::full);
                for (auto& object : boss.base::subset)
                    object->render(viewport);
                if (postrender) postrender(viewport);
                // Honor any additional clipping imposed by an ancestor of the table when copying
                // the physically bounded body back into the real canvas.
                netxs::onclip(parent_canvas, viewport, cell::shaders::full);
            };
        });
        return host;
    }

    // ---- Render ------------------------------------------------------------------------------------
    inline void table_render(table_state& st, table_cfg const& cfg, auto& canvas, twod size)
    {
        auto w = size.x, h = size.y;
        auto visible_components = std::unordered_map<id_t, rect>{};
        st.selected_component_areas.clear();
        st.selected_row_areas.clear();
        if (w <= 0 || h <= 0) { q_reconcile_components(st, visible_components); return; }
        auto& pal = cfg.palette;
        canvas.fill(rect{{ 0, 0 }, { w, h }}, [&](cell& c){ c.bgc(pal.bg).fgc(pal.text_fg); });
        st.row_hit.clear();
        st.expand_hit.clear();

        auto revision_row = si32{ -1 };
        if (cfg.revision)
        {
            auto revision = cfg.revision();
            if (revision != st.revision)
            {
                auto old_scroll = st.scroll;
                st.revision = revision;
                if (cfg.revision_row) revision_row = cfg.revision_row();
                st.scroll = revision_row >= 0 ? old_scroll : 0;
                st.hscroll = 0;
                st.live_follow = faux;
                q_reset_selection_state(st);
            }
        }

        auto t     = cfg.columns();
        auto nrows = cfg.row_count ? cfg.row_count() : 0;
        q_build_order(st, cfg, nrows);
        q_build_row_offsets(st, cfg);
        tbl_layout(st, w, h, st.total_lines, t.content_w(), /*body_top=*/1);

        if (auto host = st.cell_host_wp.lock())
        {
            host->base::hidden = st.body_rows <= 0 || st.disp_w <= 0;
            host->base::extend(rect{{ 0, st.body_top }, { std::max(0, st.disp_w), st.body_rows }});
        }

        if (revision_row >= 0)
        {
            auto row = q_visual_row(st, revision_row);
            st.scroll = q_reveal_visual(st, st.scroll, row);
        }
        if (st.live_follow && cfg.follow)
        {
            auto follow_scroll = q_follow_scroll(st, cfg);
            if (follow_scroll >= 0) st.scroll = follow_scroll;
        }
        tbl_clamp(st);

        auto hs = st.hscroll, clipw = st.disp_w;
        t.paint_header(canvas, st.body_top - 1, hs, clipw, w,
                       st.sort_key, st.sort_dir, st.hover_header, st.press_header,
                       (bool)cfg.compare, pal);

        if (nrows == 0)
        {
            if (cfg.empty_text && st.body_rows > 0)
                put_str(canvas, 1, st.body_top, cfg.empty_text(), pal.subtext, pal.bg, std::max(1, st.disp_w - 1));
        }
        else
        {
            auto has_sel = (bool)cfg.selection;
            auto sel = has_sel ? cfg.selection() : qsel_cfg{};
            auto rubber_on = st.drag == table_state::d_rubber && st.rubber_a >= 0;
            auto rlo = std::min(st.rubber_a, st.rubber_b), rhi = std::max(st.rubber_a, st.rubber_b);
            auto first = q_visual_at_line(st, st.scroll);
            auto viewport_bottom = st.scroll + st.body_rows;
            for (auto i = std::max(0, first); i < nrows && q_row_top(st, i) < viewport_bottom; ++i)
            {
                auto source_row = q_source_row(st, i);
                auto row_top = q_row_top(st, i);
                auto row_bottom = q_row_bottom(st, i);
                auto y = st.body_top + row_top - st.scroll;
                auto y0 = std::max(st.body_top, y);
                auto y1 = std::min(st.body_top + st.body_rows,
                                   st.body_top + row_bottom - st.scroll);
                auto visible_h = std::max(0, y1 - y0);
                if (visible_h <= 0) continue;
                auto key = has_sel && source_row >= 0 ? sel.key_of_row(source_row) : -1;
                auto selected = key >= 0 && (sel.is_selected(key) || (rubber_on && i >= rlo && i <= rhi));
                auto row_bg = selected ? pal.sel_bg : pal.bg;
                if (selected)
                {
                    canvas.fill(rect{{ 0, y0 }, { q_row_w(st), visible_h }}, [&](cell& c){ c.bgc(pal.sel_bg); });
                    if (st.focused) canvas.fill(rect{{ 0, y0 }, { 1, visible_h }}, [&](cell& c){ c.bgc(pal.sel_bg_act); });
                    st.selected_row_areas.push_back(rect{{ 0, y0 - st.body_top },
                                                         { q_row_w(st), visible_h }});
                }
                if (cfg.gutter && y >= st.body_top && y < st.body_top + st.body_rows)
                {
                    auto g = cfg.gutter(source_row);
                    if (!g.arrow.empty()) qtable::paint_at(canvas, g_arrow_x, g_arrow_w, y, g.arrow, g.arrow_fg, row_bg, hs, clipw);
                    if (g.expand == xp_muted) qtable::paint_at(canvas, g_expand_x, g_expand_w, y, " + ", pal.subtext, row_bg, hs, clipw);
                    else if (g.expand == xp_collapsed || g.expand == xp_expanded)
                    {
                        auto bx = g_expand_x - hs;
                        if (bx >= 0 && bx + g_expand_w <= clipw)
                        {
                            auto btn = rect{{ bx, y }, { g_expand_w, 1 }};
                            canvas.fill(btn, [&](cell& c){ c.bgc(pal.sel_bg); });
                            put_str(canvas, bx, y, g.expand == xp_expanded ? " - " : " + ", pal.text_fg, pal.sel_bg, g_expand_w);
                            if      (st.press_expand == g.expand_id) canvas.fill(btn, [](cell& c){ c.xlight(2); });
                            else if (st.hover_expand == g.expand_id) canvas.fill(btn, [](cell& c){ c.xlight(); });
                            st.expand_hit.emplace_back(btn, g.expand_id);
                        }
                    }
                }
                for (auto vc = si32{}; vc < (si32)t.cols.size(); ++vc)
                {
                    auto col_key = t.cols[(size_t)vc].key;
                    auto cv = cfg.cell(source_row, col_key);
                    if (auto value = std::get_if<table_text_cell>(&cv.value))
                    {
                        if (y >= st.body_top && y < st.body_top + st.body_rows && !value->value.empty())
                            t.paint_cell(canvas, vc, y, value->value, value->foreground, row_bg, hs, clipw);
                    }
                    else if (auto component_value = std::get_if<table_component_cell>(&cv.value))
                    {
                        auto width = std::max(0, t.cols[(size_t)vc].width - 1);
                        auto area = rect{{ t.col_x(vc) - hs, row_top - st.scroll },
                                         { width, row_bottom - row_top }};
                        if (area.coor.x < clipw && area.coor.x + area.size.x > 0)
                        {
                            q_place_component(st, component_value->content, area, visible_components);
                            if (selected) st.selected_component_areas.push_back(area);
                        }
                    }
                }
                if (key >= 0) st.row_hit.emplace_back(rect{{ 0, y0 },
                    { cfg.wide_hit ? std::max(0, st.disp_w) : q_row_w(st), visible_h }}, key);
            }
        }
        auto lines_drawn = std::clamp(st.total_lines - st.scroll, 0, st.body_rows);
        st.div_bottom = st.body_top + lines_drawn;
        t.paint_dividers(canvas, st.body_top - 1, st.div_bottom, hs, clipw, st.hover_border, st.col_drag, pal);
        tbl_paint_scrollbars(st, canvas, pal);
        // Keep the fixed menu affordance above horizontally-scrolled headers and dividers.
        t.paint_header_menu(canvas, st.body_top - 1, clipw,
                            st.hover_header_menu, st.press_header_menu,
                            (bool)cfg.menu, pal);
        q_reconcile_components(st, visible_components);
    }

    // ---- Widget ------------------------------------------------------------------------------------
    inline auto make_table(table_cfg cfg) -> component
    {
        auto state = std::make_shared<table_state>();
        auto config = std::make_shared<table_cfg>(std::move(cfg));
        state->live_follow = !!config->follow;

        // Paint the frame first, then composite arbitrary retained cell widgets through a
        // physically bounded body host.
        auto form = ui::cake::ctor()->active()
            ->plugin<pro::mouse>()
            ->plugin<pro::focus>(config->focus_on_start ? pro::focus::mode::focused : pro::focus::mode::focusable)
            ->plugin<pro::keybd>()->plugin<pro::timer>();
        auto painter = form->attach(ui::mock::ctor());
        auto cell_host = form->attach(make_table_cell_host([state, config](ui::face& viewport)
        {
            q_paint_component_selection(*state, config->palette, viewport);
        }));
        state->cell_host_wp = ptr::shadow(cell_host);
        painter->invoke([state, config](auto& boss)
        {
            boss.LISTEN(tier::release, e2::render::any, parent_canvas, -, (state, config))
            {
                table_render(*state, *config, parent_canvas, boss.base::size());
            };
        });
        form->invoke([state, config](auto& boss)
        {
            auto& st  = *state;
            auto& cfg = *config;
            boss.LISTEN(tier::release, e2::form::state::focus::count, count)
            {
                st.focused = !!count;
                boss.base::deface();
            };
            auto arm_autoscroll = [&boss, &st, &cfg]
            {
                auto& timer = boss.base::template plugin<pro::timer>();
                timer.pacify();
                if (!cfg.selection || !q_rubber_autoscroll(st, q_ordered_sel(st, cfg.selection()))) return;
                boss.base::deface();
                timer.actify(ui::skin::globals().repeat_rate, [&boss, &st, &cfg](auto) -> bool
                {
                    if (!cfg.selection || !q_rubber_autoscroll(st, q_ordered_sel(st, cfg.selection()))) return faux;
                    boss.base::deface();
                    return true;
                });
            };

            boss.on(tier::mouserelease, input::key::LeftDown, [&](hids& gear)
            {
                pro::focus::set(boss.This(), gear.id, solo::on);
                auto mx = (si32)gear.coord.x, my = (si32)gear.coord.y;
                if (cfg.selection) q_sel_snapshot(st, cfg.selection());
                auto tbl = cfg.columns();
                if (q_header_menu_hit(st, (bool)cfg.menu, mx, my))
                {
                    if (!st.press_header_menu) { st.press_header_menu = true; boss.base::deface(); }
                    gear.dismiss();
                    return;
                }
                if (auto sb = tbl_vsb(st); sb.ok && mx == sb.x && my >= sb.top && my < sb.top + sb.track_h)
                {
                    if (!st.sb_press || st.hsb_press)
                    {
                        st.sb_press = true;
                        st.hsb_press = faux;
                        boss.base::deface();
                    }
                    return;
                }
                if (auto sb = tbl_hsb(st); sb.ok && my == sb.top && mx >= sb.x && mx < sb.x + sb.track_h)
                {
                    if (!st.hsb_press || st.sb_press)
                    {
                        st.hsb_press = true;
                        st.sb_press = faux;
                        boss.base::deface();
                    }
                    return;
                }
                for (auto& [b, id] : st.expand_hit)
                    if (my == b.coor.y && mx >= b.coor.x && mx < b.coor.x + b.size.x)
                    { if (st.press_expand != id) { st.press_expand = id; boss.base::deface(); } return; }
                if (my >= st.body_top - 1 && my < st.div_bottom)
                    if (q_border_hit(cfg.columns(), mx, st.hscroll) >= 0) return;
                if (my == st.body_top - 1 && cfg.compare)
                {
                    auto v = q_header_hit(tbl, mx, st.hscroll);
                    auto key = v >= 0 ? tbl.cols[(size_t)v].key : -1;
                    if (st.press_header != key) { st.press_header = key; boss.base::deface(); }
                    gear.dismiss();
                    return;
                }
                if (cfg.selection)
                {
                    auto ctl = !!(gear.ctlstat & hids::anyCtrl), shft = !!(gear.ctlstat & hids::anyShift);
                    auto s = q_ordered_sel(st, cfg.selection());
                    auto hit = q_row_at(st, mx, my);
                    if (hit >= 0) { q_sel_press(st, s, hit, ctl, shft); boss.base::deface(); gear.dismiss(); return; }
                    if (!ctl && my >= st.body_top && my < st.tab_row && q_sel_clear_blank(st, s)) { boss.base::deface(); gear.dismiss(); return; }
                }
                boss.base::deface(); gear.dismiss();
            });
            boss.on(tier::mouserelease, input::key::LeftUp, [&](hids&)
            {
                if (st.press_expand != -1 || st.press_header != -1 || st.press_header_menu
                 || st.sb_press || st.hsb_press)
                {
                    st.press_expand = st.press_header = -1;
                    st.press_header_menu = faux;
                    st.sb_press = st.hsb_press = faux;
                    boss.base::deface();
                }
            });
            boss.on(tier::mouserelease, input::key::LeftClick, [&](hids& gear)
            {
                pro::focus::set(boss.This(), gear.id, solo::on);
                auto mx = (si32)gear.coord.x, my = (si32)gear.coord.y;
                auto tbl = cfg.columns();
                if (q_header_menu_hit(st, (bool)cfg.menu, mx, my))
                {
                    auto panel_wp = ptr::shadow(boss.This());
                    q_open_table_menu(boss, cfg.menu(panel_wp), twod{ mx, my + 2 });
                    st.press_header_menu = faux;
                    boss.base::deface();
                    gear.dismiss();
                    return;
                }
                if (my == st.body_top - 1 && cfg.compare)
                {
                    if (q_border_hit(tbl, mx, st.hscroll) < 0)
                    {
                        auto v = q_header_hit(tbl, mx, st.hscroll);
                        if (v >= 0)
                        {
                            auto key = tbl.cols[(size_t)v].key;
                            if (st.sort_key != key)
                            {
                                st.sort_key = key;
                                st.sort_dir = table_state::sort_ascending;
                            }
                            else if (st.sort_dir == table_state::sort_ascending)  st.sort_dir = table_state::sort_descending;
                            else if (st.sort_dir == table_state::sort_descending) { st.sort_dir = table_state::sort_default; st.sort_key = -1; }
                            else                                                 st.sort_dir = table_state::sort_ascending;
                            st.scroll = 0;
                            st.live_follow = faux;
                            st.press_header = -1;
                            boss.base::deface();
                            gear.dismiss();
                            return;
                        }
                    }
                }
                if (auto sb = tbl_vsb(st); sb.ok && mx == sb.x && my >= sb.top && my < sb.top + sb.track_h)
                {
                    auto page = std::max(1, st.body_rows);
                    if (my < sb.thumb_y)                    st.scroll = std::clamp(st.scroll - page, 0, sb.maxscroll);
                    else if (my >= sb.thumb_y + sb.thumb_h) st.scroll = std::clamp(st.scroll + page, 0, sb.maxscroll);
                    st.live_follow = q_at_follow_target(st, cfg); boss.base::deface(); gear.dismiss(); return;
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
                    {
                        // Expanding can attach a retained child under the stationary pointer. The
                        // ensuing target transition reports MouseLeave for the old paint layer, so
                        // restore the logical button hover at the completed click position.
                        st.press_expand = -1;
                        st.hover_expand = id;
                        if (cfg.on_toggle) cfg.on_toggle(id);
                        boss.base::deface();
                        gear.dismiss();
                        return;
                    }
                gear.dismiss();
            });
            boss.on(tier::mouserelease, input::key::RightClick, [&](hids& gear)
            {
                pro::focus::set(boss.This(), gear.id, solo::on);
                auto mx = (si32)gear.coord.x, my = (si32)gear.coord.y;
                auto panel_wp = ptr::shadow(boss.This());
                q_context_menu(boss, st, mx, my, cfg.columns(), cfg.menu ? cfg.menu(panel_wp) : qmenu_cfg{});
                gear.dismiss();
            });
            boss.on(tier::mouserelease, input::key::MouseMove, [&](hids& gear)
            {
                auto mx = (si32)gear.coord.x, my = (si32)gear.coord.y;
                auto tbl = cfg.columns();
                auto nmenu = q_header_menu_hit(st, (bool)cfg.menu, mx, my);
                if (st.hover_header_menu != nmenu) { st.hover_header_menu = nmenu; boss.base::deface(); }
                if (st.press_header_menu && !nmenu) { st.press_header_menu = faux; boss.base::deface(); }
                auto over = si32{ -1 };
                for (auto& [b, id] : st.expand_hit) if (my == b.coor.y && mx >= b.coor.x && mx < b.coor.x + b.size.x) { over = id; break; }
                if (st.hover_expand != over) { st.hover_expand = over; boss.base::deface(); }
                if (st.press_expand != -1 && st.press_expand != over) { st.press_expand = -1; boss.base::deface(); }
                auto vsb = tbl_vsb(st); auto nsb = vsb.ok && mx == vsb.x && my >= vsb.top && my < vsb.top + vsb.track_h;
                if (st.sb_hover != nsb) { st.sb_hover = nsb; boss.base::deface(); }
                if (st.sb_press && !nsb) { st.sb_press = faux; boss.base::deface(); }
                auto hsb = tbl_hsb(st); auto nhsb = hsb.ok && my == hsb.top && mx >= hsb.x && mx < hsb.x + hsb.track_h;
                if (st.hsb_hover != nhsb) { st.hsb_hover = nhsb; boss.base::deface(); }
                if (st.hsb_press && !nhsb) { st.hsb_press = faux; boss.base::deface(); }
                auto nhdr = si32{ -1 };
                if (!nmenu && cfg.compare && my == st.body_top - 1)
                {
                    if (q_border_hit(tbl, mx, st.hscroll) < 0)
                    {
                        auto v = q_header_hit(tbl, mx, st.hscroll);
                        if (v >= 0) nhdr = tbl.cols[(size_t)v].key;
                    }
                }
                if (st.hover_header != nhdr) { st.hover_header = nhdr; boss.base::deface(); }
                if (st.press_header != -1 && st.press_header != nhdr) { st.press_header = -1; boss.base::deface(); }
                auto nb = si32{ -1 };
                if (!nmenu && my >= st.body_top - 1 && my < st.div_bottom) nb = q_border_hit(tbl, mx, st.hscroll);
                if (st.hover_border != nb) { st.hover_border = nb; boss.base::deface(); }
            });
            boss.LISTEN(tier::release, e2::form::state::mouse, hovered)
            {
                // Raw MouseLeave is emitted when the pointer crosses between retained descendants.
                // The aggregate state changes to false only after the pointer leaves the table tree.
                if (hovered) return;
                if (st.hover_expand != -1) { st.hover_expand = -1; boss.base::deface(); }
                if (st.press_expand != -1) { st.press_expand = -1; boss.base::deface(); }
                if (st.sb_hover)  { st.sb_hover = faux;  boss.base::deface(); }
                if (st.hsb_hover) { st.hsb_hover = faux; boss.base::deface(); }
                if (st.sb_press)  { st.sb_press = faux;  boss.base::deface(); }
                if (st.hsb_press) { st.hsb_press = faux; boss.base::deface(); }
                if (st.hover_border != -1) { st.hover_border = -1; boss.base::deface(); }
                if (st.hover_header_menu || st.press_header_menu)
                {
                    st.hover_header_menu = st.press_header_menu = faux;
                    boss.base::deface();
                }
                if (st.hover_header != -1 || st.press_header != -1)
                {
                    st.hover_header = st.press_header = -1;
                    boss.base::deface();
                }
            };
            boss.on(tier::mouserelease, input::key::MouseWheel, [&](hids& gear)
            {
                if (gear.hzwhl || st.hsb_hover) { auto maxh = std::max(0, st.content_w - st.disp_w); st.hscroll = std::clamp(st.hscroll - gear.whlsi * 4, 0, maxh); }
                else { auto maxv = std::max(0, st.total_lines - st.body_rows); st.scroll = std::clamp(st.scroll - gear.whlsi, 0, maxv); st.live_follow = q_at_follow_target(st, cfg); }
                boss.base::deface();
                gear.dismiss();
            });
            boss.base::signal(tier::release, e2::form::draggable::_<hids::buttons::left>, true);
            boss.LISTEN(tier::release, e2::form::drag::start::_<hids::buttons::left>, gear)
            {
                auto px = (si32)gear.click.x, py = (si32)gear.click.y;
                auto tbl = cfg.columns();
                if (q_header_menu_hit(st, (bool)cfg.menu, px, py)) return;
                if (auto sb = tbl_vsb(st); sb.ok && px == sb.x && py >= sb.top && py < sb.top + sb.track_h)
                {
                    pro::focus::set(boss.This(), gear.id, solo::on);
                    if (py >= sb.thumb_y && py < sb.thumb_y + sb.thumb_h) st.sb_grab = py - sb.thumb_y;
                    else { st.sb_grab = sb.thumb_h / 2; tbl_vsb_to(st, py, sb); }
                    st.sb_press = faux;
                    st.sb_drag = st.sb_hover = true; st.drag = table_state::d_vsb;
                    st.live_follow = q_at_follow_target(st, cfg); boss.base::deface(); return;
                }
                if (auto sb = tbl_hsb(st); sb.ok && py == sb.top && px >= sb.x && px < sb.x + sb.track_h)
                {
                    pro::focus::set(boss.This(), gear.id, solo::on);
                    auto tx = sb.x + sb.thumb_y;
                    if (px >= tx && px < tx + sb.thumb_h) st.hsb_grab = px - tx;
                    else { st.hsb_grab = sb.thumb_h / 2; tbl_hsb_to(st, px, sb); }
                    st.hsb_press = faux;
                    st.hsb_drag = st.hsb_hover = true; st.drag = table_state::d_hsb; boss.base::deface(); return;
                }
                if (py >= st.body_top - 1 && py < st.div_bottom)
                {
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
                    st.drag_y = py;
                    st.live_follow = faux;
                    q_rubber_begin(st, q_ordered_sel(st, cfg.selection()),
                                   q_rubber_visual_at_line(st, st.scroll + (py - st.body_top)),
                                   !!(gear.ctlstat & hids::anyCtrl));
                    boss.base::deface(); return;
                }
            };
            boss.LISTEN(tier::release, e2::form::drag::pull::_<hids::buttons::left>, gear, -, (arm_autoscroll))
            {
                auto mx = (si32)gear.coord.x, my = (si32)gear.coord.y;
                switch (st.drag)
                {
                    case table_state::d_vsb: tbl_vsb_to(st, my, tbl_vsb(st)); st.live_follow = q_at_follow_target(st, cfg); boss.base::deface(); break;
                    case table_state::d_hsb: tbl_hsb_to(st, mx, tbl_hsb(st)); boss.base::deface(); break;
                    case table_state::d_col:
                    {
                        auto tbl = cfg.columns();
                        if (st.col_drag >= 0 && st.col_drag < (si32)tbl.cols.size())
                        {
                            auto left = tbl.col_x(st.col_drag);
                            auto ww = mx + st.hscroll - left + 1;
                            q_set_col_w(tbl, st.col_drag, ww); boss.base::deface();
                        }
                        break;
                    }
                    case table_state::d_rubber:
                        st.drag_y = my;
                        q_rubber_pull(st, q_ordered_sel(st, cfg.selection()), q_rubber_row_at_drag(st));
                        arm_autoscroll();
                        boss.base::deface();
                        break;
                    default: break;
                }
            };
            // A drag ends: drop the gesture mode and transient drag flags (inlined into both events —
            // the LISTEN macro captures by reference, so a shared local lambda would dangle).
            boss.LISTEN(tier::release, e2::form::drag::stop::_<hids::buttons::left>,   gear)
            { boss.base::template plugin<pro::timer>().pacify(); auto was = st.drag; st.drag = table_state::d_none; st.sb_press = st.hsb_press = st.sb_drag = st.hsb_drag = faux; st.col_drag = -1; st.rubber_a = st.rubber_b = -1; if (was != table_state::d_none) boss.base::deface(); };
            boss.LISTEN(tier::release, e2::form::drag::cancel::_<hids::buttons::left>, gear)
            { boss.base::template plugin<pro::timer>().pacify(); auto was = st.drag; st.drag = table_state::d_none; st.sb_press = st.hsb_press = st.sb_drag = st.hsb_drag = faux; st.col_drag = -1; st.rubber_a = st.rubber_b = -1; if (was != table_state::d_none) boss.base::deface(); };
            boss.on(tier::mouserelease, input::key::LeftDoubleClick, [&](hids& gear)
            {
                auto mx = (si32)gear.coord.x, my = (si32)gear.coord.y;
                if (my < st.body_top - 1 || my >= st.div_bottom) return;
                auto tbl = cfg.columns();
                if (q_header_menu_hit(st, (bool)cfg.menu, mx, my)) { gear.dismiss(); return; }
                auto v = q_border_hit(tbl, mx, st.hscroll);
                if (v >= 0)
                {
                    q_set_col_w(tbl, v, q_col_autofit(tbl, v, (bool)cfg.compare));
                    boss.base::deface();
                    gear.dismiss();
                    return;
                }
                if (cfg.on_activate && my >= st.body_top)
                {
                    auto visual = q_visual_at_line(st, st.scroll + (my - st.body_top));
                    auto source = q_source_row(st, visual);
                    auto hit_w = cfg.wide_hit ? st.disp_w : q_row_w(st);
                    if (source >= 0 && source < st.total && mx >= 0 && mx < hit_w)
                    {
                        boss.base::deface();
                        gear.dismiss();
                        cfg.on_activate(source);
                    }
                }
            });
            boss.LISTEN(tier::preview, input::events::keybd::any, gear)
            {
                if (!st.focused) return;
                // Preview events reach ancestors before the focused descendant. Let a focused cell
                // component (input, nested table, or future widget) own the key before this table.
                if (q_component_has_focus(st)) return;
                if (gear.payload == input::keybd::type::keypaste)
                {
                    if (gear.keybd::handled || !cfg.on_key) return;
                    auto action = cfg.on_key(gear, ptr::shadow(boss.This()));
                    if (action.mode != table_viewport_action::unhandled)
                    {
                        gear.set_handled();
                        boss.base::deface();
                    }
                    return;
                }
                if (gear.payload != input::keybd::type::keypress) return;
                if (gear.keystat == input::key::released || gear.keystat == input::key::interrupted) return;
                if (gear.keybd::handled) return;
                auto k = gear.keybd::generic();
                if (k == input::key::Esc && cfg.selection)
                {
                    auto s = cfg.selection();
                    if (s.has_selection())
                    {
                        s.on_clear();
                        q_reset_selection_state(st);
                        gear.set_handled();
                        boss.base::deface();
                        return;
                    }
                }
                if (k == input::key::KeyDelete
                 && cfg.deletion.enabled
                 && cfg.selection)
                {
                    auto s = cfg.selection();
                    if (s.has_selection())
                    {
                        auto self = ptr::shadow(boss.This());
                        if (cfg.deletion.on_delete && cfg.deletion.on_delete(gear, self))
                        {
                            gear.set_handled();
                            return;
                        }
                        if (!cfg.deletion.on_remove_selected) return;
                        gear.set_handled();
                        auto remove = cfg.deletion.on_remove_selected;
                        auto run = [self, remove]
                        {
                            if (auto table = self.lock())
                            {
                                remove(self);
                                table->base::deface();
                            }
                        };
                        if (cfg.deletion.confirm)
                        {
                            if (auto window = cfg.window_wp.lock())
                                app::shared::show_close_confirmation(*window, run, {}, cfg.deletion.confirm());
                            else run();
                        }
                        else run();
                        return;
                    }
                }
                if (cfg.on_key)
                {
                    auto action = cfg.on_key(gear, ptr::shadow(boss.This()));
                    if (action.mode != table_viewport_action::unhandled)
                    {
                        gear.set_handled();
                        if (action.mode == table_viewport_action::reveal_row)
                        {
                            auto row = q_visual_row(st, action.row);
                            st.scroll = q_reveal_visual(st, st.scroll, row);
                            if (action.reset_horizontal) st.hscroll = 0;
                            if (cfg.selection)
                            {
                                auto key = cfg.selection().key_of_row(action.row);
                                if (key >= 0) st.sel_anchor = st.nav_cursor = key;
                            }
                            st.live_follow = faux;
                            boss.base::deface();
                        }
                        return;
                    }
                }
                if (k == input::key::KeyEnter && cfg.on_activate && cfg.selection)
                {
                    auto s = q_ordered_sel(st, cfg.selection());
                    auto visual = q_selected_row(s, st.nav_cursor);
                    auto source = q_source_row(st, visual);
                    if (source >= 0)
                    {
                        st.nav_cursor = s.key_of_row(visual);
                        gear.set_handled();
                        boss.base::deface();
                        cfg.on_activate(source);
                        return;
                    }
                }
                if (!cfg.arrow_nav || !cfg.selection) return;
                auto s = q_ordered_sel(st, cfg.selection());
                auto n = cfg.row_count ? cfg.row_count() : 0;
                auto maxv = std::max(0, st.total_lines - st.body_rows);
                auto page = std::max(1, st.body_rows);
                auto sels = std::vector<si32>{}; // Ordered selectable display rows.
                for (auto i = si32{}; i < n; ++i) if (s.key_of_row(i) >= 0) sels.push_back(i);
                auto select_at = [&](si32 p, bool extend = faux)
                {
                    if (sels.empty()) return;
                    p = std::clamp(p, 0, (si32)sels.size() - 1);
                    auto dr = sels[(size_t)p]; auto key = s.key_of_row(dr);
                    if (extend)
                    {
                        auto anchor = si32{ -1 };
                        for (auto i = si32{}; i < (si32)sels.size(); ++i)
                        {
                            auto candidate = s.key_of_row(sels[(size_t)i]);
                            if (candidate == st.sel_anchor && s.is_selected(candidate)) { anchor = i; break; }
                        }
                        if (anchor < 0)
                            for (auto i = si32{}; i < (si32)sels.size(); ++i)
                            {
                                auto candidate = s.key_of_row(sels[(size_t)i]);
                                if (s.is_selected(candidate)) { anchor = i; st.sel_anchor = candidate; break; }
                            }
                        if (anchor < 0) { anchor = p; st.sel_anchor = key; }
                        auto lo = std::min(anchor, p), hi = std::max(anchor, p);
                        s.on_clear();
                        for (auto i = lo; i <= hi; ++i) s.on_select(s.key_of_row(sels[(size_t)i]), true);
                        s.on_select(key, true); // Finish on the movable endpoint for caller-owned cursors.
                    }
                    else
                    {
                        s.on_clear(); s.on_select(key, true); st.sel_anchor = key;
                    }
                    st.nav_cursor = key;
                    st.scroll = q_reveal_visual(st, st.scroll, dr);
                    st.live_follow = q_at_follow_target(st, cfg);
                };
                auto cur = si32{ -1 };
                for (auto p = si32{}; p < (si32)sels.size(); ++p)
                {
                    auto key = s.key_of_row(sels[(size_t)p]);
                    if (key == st.nav_cursor && s.is_selected(key)) { cur = p; break; }
                }
                if (cur < 0) for (auto p = si32{}; p < (si32)sels.size(); ++p) if (s.is_selected(s.key_of_row(sels[(size_t)p]))) { cur = p; break; }
                auto select_visual = [&](si32 visual, si32 dir, bool extend)
                {
                    if (sels.empty()) return;
                    auto upper = std::lower_bound(sels.begin(), sels.end(), visual);
                    if (upper == sels.begin()) { select_at(0, extend); return; }
                    if (upper == sels.end())   { select_at((si32)sels.size() - 1, extend); return; }
                    auto hi = (si32)(upper - sels.begin()), lo = hi - 1;
                    auto dlo = visual - sels[(size_t)lo], dhi = sels[(size_t)hi] - visual;
                    select_at(dlo < dhi || (dlo == dhi && dir < 0) ? lo : hi, extend);
                };
                auto page_selection = [&](si32 dir, bool extend)
                {
                    if (sels.empty()) return;
                    auto currow = cur >= 0 ? sels[(size_t)cur] : dir < 0 ? sels.back() : sels.front();
                    auto edge_line = dir < 0 ? st.scroll
                                             : std::min(st.total_lines - 1, st.scroll + page - 1);
                    auto edge_row = q_visual_at_line(st, edge_line);
                    if (currow != edge_row) select_visual(edge_row, dir, extend);
                    else
                    {
                        st.scroll = std::clamp(st.scroll + dir * page, 0, maxv);
                        auto target_line = dir < 0 ? st.scroll
                                                  : std::min(st.total_lines - 1, st.scroll + page - 1);
                        select_visual(q_visual_at_line(st, target_line), dir, extend);
                    }
                };
                auto extend = !!(gear.ctlstat & hids::anyShift);
                auto act = true;
                     if (k == input::key::KeyUpArrow   || k == input::key::NumpadUpArrow)   select_at(cur < 0 ? (si32)sels.size() - 1 : cur - 1, extend);
                else if (k == input::key::KeyDownArrow || k == input::key::NumpadDownArrow) select_at(cur < 0 ? 0 : cur + 1, extend);
                else if (k == input::key::KeyHome      || k == input::key::NumpadHome)     { st.scroll = 0;    select_at(0, extend); }
                else if (k == input::key::KeyEnd       || k == input::key::NumpadEnd)      { st.scroll = maxv; select_at((si32)sels.size() - 1, extend); }
                else if (k == input::key::KeyPageUp    || k == input::key::NumpadPageUp)     page_selection(-1, extend);
                else if (k == input::key::KeyPageDown  || k == input::key::NumpadPageDown)   page_selection(+1, extend);
                else act = false;
                if (act) { gear.set_handled(); boss.base::deface(); }
            };
        });
        return { std::move(form) };
    }
}
