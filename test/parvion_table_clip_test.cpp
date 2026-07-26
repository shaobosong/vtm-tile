// Copyright (c) Shaobo Song
// Licensed under the MIT license.

// Unit tests for Parvion table horizontal clipping. The regression: when the
// table is horizontally scrolled, text that starts before the visible viewport
// must not be passed to the canvas with a negative x coordinate.

#include "netxs/apps.hpp"
#include "vtm-common.hpp"
#include "netxs/apps/parvion/table.hpp"
#include "netxs/apps/parvion/textbox.hpp"

#include <cstdio>
#include <vector>

using namespace netxs;
using namespace netxs::app::parvion;

namespace
{
    struct mock_canvas
    {
        std::vector<rect> writes;

        template<class Fx>
        void fill(rect area, Fx fx)
        {
            writes.push_back(area);
            auto c = cell{};
            fx(c);
        }
    };

    struct state_canvas
    {
        struct write { rect area; cell value; };
        std::vector<write> writes;

        template<class Fx>
        void fill(rect area, Fx fx)
        {
            auto value = cell{};
            fx(value);
            writes.push_back({ area, value });
        }
    };

    auto inside(rect area, si32 disp_w) -> bool
    {
        return area.coor.x >= 0
            && area.size.x >= 0
            && area.coor.x + area.size.x <= disp_w;
    }

    auto all_inside(mock_canvas const& canvas, si32 disp_w) -> bool
    {
        for (auto const& area : canvas.writes)
        {
            if (!inside(area, disp_w)) return faux;
        }
        return true;
    }

    auto test_left_clip_ascii() -> bool
    {
        auto canvas = mock_canvas{};
        qtable::paint_at(canvas, 0, 12, 0, "abcdefghijkl", theme::text_fg, theme::bg, 4, 5);
        return !canvas.writes.empty()
            && canvas.writes.front().coor.x == 0
            && canvas.writes.size() == 5
            && all_inside(canvas, 5);
    }

    auto test_left_clip_wide_cluster() -> bool
    {
        auto canvas = mock_canvas{};
        qtable::paint_at(canvas, 0, 8, 0, "\xE8\xA1\xA8" "abcdef", theme::text_fg, theme::bg, 1, 5); // 表abcdef
        return !canvas.writes.empty()
            && canvas.writes.front().coor.x >= 0
            && all_inside(canvas, 5);
    }

    auto test_fully_hidden_left() -> bool
    {
        auto canvas = mock_canvas{};
        qtable::paint_at(canvas, 0, 4, 0, "abcd", theme::text_fg, theme::bg, 4, 5);
        return canvas.writes.empty();
    }

    auto test_short_text_hidden_in_wide_cell() -> bool
    {
        auto canvas = mock_canvas{};
        qtable::paint_at(canvas, 0, 20, 0, "abc", theme::text_fg, theme::bg, 8, 5);
        return canvas.writes.empty();
    }

    auto autofit_table(view title, si32 body_w) -> qtable
    {
        auto table = qtable{};
        table.add_column(qtable::column{ .title = text{ title }, .key = 7 }, true);
        table.autofit = [body_w](si32){ return body_w; };
        return table;
    }

    auto test_sortable_header_controls_autofit() -> bool
    {
        auto table = autofit_table("Header", 3);
        return q_col_autofit(table, 0, true) == 9; // 6 title + separator + glyph + divider.
    }

    auto test_body_content_controls_autofit() -> bool
    {
        auto table = autofit_table("Name", 12);
        return q_col_autofit(table, 0, true) == 13; // 12 body cells + divider.
    }

    auto test_plain_header_has_no_sort_suffix() -> bool
    {
        auto table = autofit_table("Header", 0);
        return q_col_autofit(table, 0, faux) == 7; // 6 title + divider.
    }

    auto test_empty_sortable_table_fits_header() -> bool
    {
        auto table = autofit_table("Name", 0);
        return q_col_autofit(table, 0, true) == 7; // 4 title + separator + glyph + divider.
    }

    auto test_add_column_keeps_roster_in_sync() -> bool
    {
        auto table = qtable{};
        table.add_column(qtable::column{ .title = text{ "Shown" }, .key = 4 }, true, "&Shown");
        table.add_column(qtable::column{ .title = text{ "Hidden" }, .key = 7 }, faux);
        return table.cols.size() == 1
            && table.cols.front().key == 4
            && table.roster.size() == 2
            && table.roster[0].title == "&Shown"
            && table.roster[0].shown
            && table.roster[1].title == "Hidden"
            && !table.roster[1].shown
            && table.roster[1].key == 7;
    }

    auto test_column_width_write_is_clamped() -> bool
    {
        auto table = qtable{};
        auto width = si32{};
        table.add_column(qtable::column{ .title = text{ "Name" }, .key = 3 }, true);
        table.on_resize_column = [&](si32 key, si32 w){ if (key == 3) width = w; };
        q_set_col_w(table, 0, g_col_max + 10);
        if (width != g_col_max) return faux;
        q_set_col_w(table, 0, g_col_min - 10);
        return width == g_col_min;
    }

    auto test_numeric_menu_shortcuts() -> bool
    {
        namespace m = app::shared::menu;
        return m::label_shortcut_char("SHA-&256") == '2'
            && m::label_display_length("SHA-&256") == 7;
    }

    auto test_column_menu_uses_caller_shortcuts() -> bool
    {
        namespace m = app::shared::menu;
        auto roster = std::vector<qtable::col_toggle>{
            { "&Source",    0, true },
            { "&Path",      1, true },
            { "&Algorithm", 2, true },
            { "S&ize",      3, true },
            { "P&rogress",  4, true },
            { "R&esult",    5, true },
        };
        auto items = build_columns_menu(roster, {}, {});
        auto keys = text{};
        for (auto& row : items) keys += m::label_shortcut_char(row.label);
        auto plain = build_columns_menu({ { "Plain", 0, true } }, {}, {});
        return keys == "spaire"
            && m::label_shortcut_char(plain.front().label) == 0;
    }

    auto test_page_navigation_visits_edges_first() -> bool
    {
        auto down = q_page_nav(50, 10, 0, 3, +1);
        auto up   = q_page_nav(50, 10, 0, 6, -1);
        return down.offset == 0 && down.cursor == 9
            && up.offset == 0 && up.cursor == 0;
    }

    auto test_page_navigation_scrolls_from_edges() -> bool
    {
        auto down = q_page_nav(50, 10, 0, 9, +1);
        auto up   = q_page_nav(50, 10, 20, 20, -1);
        return down.offset == 10 && down.cursor == 19
            && up.offset == 10 && up.cursor == 10;
    }

    auto test_page_navigation_partial_last_page() -> bool
    {
        auto edge = q_page_nav(48, 10, 30, 33, +1);
        auto page = q_page_nav(48, 10, edge.offset, edge.cursor, +1);
        return edge.offset == 30 && edge.cursor == 39
            && page.offset == 38 && page.cursor == 47;
    }

    auto test_page_navigation_uses_visible_page() -> bool
    {
        auto below = q_page_nav(50, 10, 30, 3, +1);
        auto above = q_page_nav(50, 10, 0, 45, -1);
        return below.offset == 30 && below.cursor == 39
            && above.offset == 0 && above.cursor == 0;
    }

    auto test_reveal_scroll_keeps_visible_row() -> bool
    {
        return q_reveal_scroll(50, 10, 20, 20) == 20
            && q_reveal_scroll(50, 10, 20, 25) == 20
            && q_reveal_scroll(50, 10, 20, 29) == 20;
    }

    auto test_reveal_scroll_moves_minimally() -> bool
    {
        return q_reveal_scroll(50, 10, 20, 12) == 12
            && q_reveal_scroll(50, 10, 20, 35) == 26;
    }

    auto test_reveal_scroll_clamps_bounds() -> bool
    {
        return q_reveal_scroll(8, 10, 4, 7) == 0
            && q_reveal_scroll(50, 10, 90, 45) == 40
            && q_reveal_scroll(50, 0, 10, 20) == 10;
    }

    auto test_selected_row_rejects_stale_cursor() -> bool
    {
        auto selected = std::set<si32>{ 0 };
        auto order = std::vector<si32>{ 2, 0, 1 };
        auto s = qsel_cfg{};
        s.row_count = [&]{ return (si32)order.size(); };
        s.key_of_row = [&](si32 row){ return order[(size_t)row]; };
        s.is_selected = [&](si32 key){ return selected.count(key) != 0; };
        // Key 2 was the cursor in the previous listing, but only key 0 is visibly selected now.
        return q_selected_row(s, 2) == 1;
    }

    auto test_selected_row_keeps_selected_cursor() -> bool
    {
        auto selected = std::set<si32>{ 0, 2 };
        auto order = std::vector<si32>{ 2, 0, 1 };
        auto s = qsel_cfg{};
        s.row_count = [&]{ return (si32)order.size(); };
        s.key_of_row = [&](si32 row){ return order[(size_t)row]; };
        s.is_selected = [&](si32 key){ return selected.count(key) != 0; };
        return q_selected_row(s, 0) == 1;
    }

    auto test_revision_resets_selection_state() -> bool
    {
        auto st = table_state{};
        st.sel_anchor = 4;
        st.nav_cursor = 5;
        st.rubber_a = 2;
        st.rubber_b = 6;
        st.drag_base = { 2, 3 };
        st.rubber_ctrl = true;
        st.drag = table_state::d_rubber;
        q_reset_selection_state(st);
        return st.sel_anchor == -1 && st.nav_cursor == -1
            && st.rubber_a == -1 && st.rubber_b == -1
            && st.drag_base.empty() && !st.rubber_ctrl
            && st.drag == table_state::d_none;
    }

    auto test_tail_follow_rearms_at_bottom() -> bool
    {
        auto st = table_state{};
        st.total = 50;
        st.total_lines = 50;
        st.body_rows = 10;
        st.scroll = 40;
        auto cfg = table_cfg{};
        cfg.follow = []{ return table_follow_target{ table_follow_target::tail }; };
        return q_follow_scroll(st, cfg) == 40
            && q_at_follow_target(st, cfg);
    }

    auto test_source_follow_does_not_rearm_at_unrelated_bottom() -> bool
    {
        auto st = table_state{};
        st.total = 50;
        st.total_lines = 50;
        st.body_rows = 10;
        st.scroll = 40;
        st.row_order.resize(50);
        st.row_offsets.resize(51);
        for (auto i = si32{}; i < 50; ++i) st.row_order[(size_t)i] = i;
        for (auto i = si32{}; i <= 50; ++i) st.row_offsets[(size_t)i] = i;
        auto cfg = table_cfg{};
        cfg.follow = []{ return table_follow_target{ table_follow_target::source_row, 0 }; };
        return q_follow_scroll(st, cfg) == 0
            && !q_at_follow_target(st, cfg);
    }

    auto test_variable_row_line_geometry() -> bool
    {
        auto st = table_state{};
        auto cfg = table_cfg{};
        cfg.row_height = [](si32 row){ return std::array<si32, 3>{ 2, 4, 1 }[(size_t)row]; };
        q_build_order(st, cfg, 3);
        q_build_row_offsets(st, cfg);
        return st.total_lines == 7
            && st.row_offsets == std::vector<si32>{ 0, 2, 6, 7 }
            && q_visual_at_line(st, 0) == 0
            && q_visual_at_line(st, 1) == 0
            && q_visual_at_line(st, 2) == 1
            && q_visual_at_line(st, 5) == 1
            && q_visual_at_line(st, 6) == 2;
    }

    auto test_variable_row_reveal_uses_lines() -> bool
    {
        auto st = table_state{};
        st.total = 3;
        st.total_lines = 7;
        st.body_rows = 3;
        st.row_offsets = { 0, 2, 6, 7 };
        return q_reveal_visual(st, 0, 0) == 0
            && q_reveal_visual(st, 0, 1) == 2 // Taller than the viewport: reveal its top.
            && q_reveal_visual(st, 2, 2) == 4
            && q_reveal_visual(st, 4, 1) == 2;
    }

    auto test_rubber_band_preserves_blank_endpoint() -> bool
    {
        auto st = table_state{};
        st.total = 3;
        st.total_lines = 3;
        st.body_top = 1;
        st.body_rows = 8;
        st.row_offsets = { 0, 1, 2, 3 };
        st.drag_y = 6;
        return q_rubber_visual_at_line(st, 1) == 1
            && q_rubber_row_at_drag(st) == 5;
    }

    auto test_rubber_overshoot_keeps_last_row_as_keyboard_cursor() -> bool
    {
        auto st = table_state{};
        auto selected = std::set<si32>{};
        auto s = qsel_cfg{};
        s.key_count = []{ return 6; };
        s.is_selected = [&](si32 key){ return selected.contains(key); };
        s.on_select = [&](si32 key, bool on)
        {
            if (on) selected.insert(key);
            else    selected.erase(key);
        };
        s.on_clear = [&]{ selected.clear(); };
        s.has_selection = [&]{ return !selected.empty(); };
        s.in_scope = [](si32 key){ return key >= 0 && key < 6; };
        s.row_count = []{ return 6; };
        s.key_of_row = [](si32 row){ return row >= 0 && row < 6 ? row : -1; };

        q_sel_press(st, s, 1, faux, faux);
        q_sel_snapshot(st, s);
        q_rubber_begin(st, s, 1, faux);
        q_rubber_pull(st, s, 10); // No sampled move on rows 2-5 before entering blank space.

        return selected == std::set<si32>{ 1, 2, 3, 4, 5 }
            && st.nav_cursor == 5; // Up will therefore move exactly once, to row 4.
    }

    auto test_component_cells_retain_and_reconcile_widgets() -> bool
    {
        auto st = table_state{};
        auto host = ui::cake::ctor();
        host->base::extend(rect{{ 0, 0 }, { 40, 10 }});
        st.cell_host_wp = ptr::shadow(host);
        auto activated = 0;
        auto deactivated = 0;
        auto child = component{
            ui::mock::ctor(),
            [&]{ ++activated; },
            [&]{ ++deactivated; },
        };
        auto value = table_cell{ child };
        auto embedded = std::get_if<table_component_cell>(&value.value);
        if (!embedded || embedded->content.widget != child.widget) return faux;

        auto visible = std::unordered_map<id_t, rect>{};
        q_place_component(st, embedded->content, rect{{ 2, 1 }, { 8, 3 }}, visible);
        // The same retained object cannot be placed into a second visible cell.
        q_place_component(st, embedded->content, rect{{ 20, 1 }, { 8, 3 }}, visible);
        q_reconcile_components(st, visible);
        if (activated != 1 || child.widget->base::parent() != host
         || child.widget->base::area() != rect{{ 2, 1 }, { 8, 3 }}) return faux;

        q_reconcile_components(st, {});
        // base::detach() deliberately leaves its weak father link intact, so verify the
        // visual-tree membership instead of parent().
        if (deactivated != 1 || !host->base::subset.empty() || !st.attached.empty()) return faux;

        // The same stable widget can be attached again when its cell scrolls back into view.
        visible.clear();
        q_place_component(st, embedded->content, rect{{ 4, 2 }, { 10, 2 }}, visible);
        q_reconcile_components(st, visible);
        return activated == 2 && host->base::subset.size() == 1
            && child.widget->base::area() == rect{{ 4, 2 }, { 10, 2 }};
    }

    auto test_table_can_be_nested_as_component_content() -> bool
    {
        auto cfg = table_cfg{};
        cfg.columns = []{ return qtable{}; };
        cfg.row_count = []{ return 0; };
        cfg.cell = [](si32, si32){ return table_cell{}; };
        auto nested = make_table(std::move(cfg));
        auto value = table_cell{ nested };
        auto embedded = std::get_if<table_component_cell>(&value.value);
        return embedded && embedded->content.widget == nested.widget;
    }

    auto make_unclipped_test_component() -> ui::sptr
    {
        auto raw = ui::mock::ctor();
        raw->invoke([id = raw->id](auto& boss)
        {
            boss.LISTEN(tier::release, app::e2::render::any, canvas, -, (id))
            {
                canvas.fill(rect{{}, boss.base::size()}, [id](cell& c)
                {
                    c.bgc(theme::sel_bg).fgc(theme::text_fg).txt("X").link(id);
                });
            };
        });
        return raw;
    }

    auto test_component_host_clips_every_edge() -> bool
    {
        auto raw = make_unclipped_test_component();
        auto host = make_table_cell_host();
        host->base::attach(raw);
        host->base::extend(rect{{ 5, 2 }, { 4, 2 }});

        auto const sentinel = id_t{ 0x7FFFFFFFu };
        auto render_case = [&](rect child_area, twod visible, twod outside)
        {
            auto canvas = ui::face{};
            canvas.size({ 14, 7 });
            canvas.fill(rect{{}, canvas.size()}, [&](cell& c){ c.txt("Z").link(sentinel); });
            raw->base::extend(child_area);
            host->render(canvas);
            return canvas[visible].txt() == "X"
                && canvas[visible].link() == raw->id
                && canvas[outside].txt() == "Z"
                && canvas[outside].link() == sentinel;
        };
        auto honors_ancestor_clip = [&]
        {
            auto canvas = ui::face{};
            canvas.size({ 14, 7 });
            canvas.fill(rect{{}, canvas.size()}, [&](cell& c){ c.txt("Z").link(sentinel); });
            canvas.clip(rect{{ 6, 2 }, { 2, 2 }});
            raw->base::extend(rect{{}, { 4, 2 }});
            host->render(canvas);
            return canvas[{ 6, 2 }].txt() == "X"
                && canvas[{ 6, 2 }].link() == raw->id
                && canvas[{ 5, 2 }].txt() == "Z"
                && canvas[{ 5, 2 }].link() == sentinel
                && canvas[{ 8, 2 }].txt() == "Z"
                && canvas[{ 8, 2 }].link() == sentinel;
        };

        return render_case(rect{{  3,  0 }, { 4, 1 }}, { 8, 2 }, { 9, 2 }) // Right.
            && render_case(rect{{ -3,  0 }, { 4, 1 }}, { 5, 2 }, { 4, 2 }) // Left.
            && render_case(rect{{  0, -2 }, { 1, 3 }}, { 5, 2 }, { 5, 1 }) // Top.
            && render_case(rect{{  0,  1 }, { 1, 3 }}, { 5, 3 }, { 5, 4 }) // Bottom.
            && honors_ancestor_clip();
    }

    auto test_component_cells_clip_during_horizontal_scroll() -> bool
    {
        // Deliberately ignore canvas.clip() to model an arbitrary retained component. Render the
        // table away from the parent origin so both its left overflow and its scrollbar-side
        // overflow are observable in the backing canvas.
        auto raw = make_unclipped_test_component();
        auto embedded = component{ raw };
        auto cfg = table_cfg{};
        cfg.columns = []
        {
            auto table = qtable{};
            table.add_column({ .title = "Progress", .width = 14, .key = 0 }, true);
            return table;
        };
        cfg.row_count = []{ return 5; };
        cfg.cell = [embedded](si32 row, si32)
        {
            return row == 0 ? table_cell{ embedded } : table_cell{};
        };

        auto st = table_state{};
        auto host = make_table_cell_host();
        st.cell_host_wp = ptr::shadow(host);
        auto const sentinel = id_t{ 0x7FFFFFFFu };
        auto render_at = [&](si32 hscroll, twod visible)
        {
            auto canvas = ui::face{};
            canvas.size({ 20, 8 });
            canvas.fill(rect{{}, canvas.size()}, [&](cell& c){ c.txt("Z").link(sentinel); });
            {
                auto context = canvas.change_basis(rect{{ 5, 2 }, { 10, 4 }});
                if (!context) return faux;
                st.hscroll = hscroll;
                table_render(st, cfg, canvas, { 10, 4 });
                host->render(canvas);
            }
            return canvas[visible].txt() == "X"
                && canvas[visible].link() == raw->id
                && canvas[{ 4, 3 }].txt() == "Z"       // Immediately left of the table.
                && canvas[{ 4, 3 }].link() == sentinel
                && canvas[{ 14, 3 }].txt() == "\xe2\x96\x90"; // Vertical scrollbar.
        };

        return render_at(0, { 6, 3 })       // Component extends through the right viewport edge.
            && render_at(100, { 5, 3 });    // Clamped to max scroll; component extends past the left edge.
    }

    auto test_table_scrollbar_press_promotes_to_drag_paint() -> bool
    {
        auto st = table_state{};
        st.body_top = 1;
        st.body_rows = 4;
        st.total_lines = 8;
        st.has_vsb = true;
        st.vsb_x = 9;
        st.sb_hover = st.sb_press = true;

        auto pressed = state_canvas{};
        auto pal = table_palette{};
        tbl_paint_scrollbars(st, pressed, pal);
        if (pressed.writes.size() != 3
         || pressed.writes.back().area != rect{{ 9, 1 }, { 1, 2 }}) return faux;

        st.sb_press = faux;
        st.sb_drag = true;
        auto dragging = state_canvas{};
        tbl_paint_scrollbars(st, dragging, pal);
        return dragging.writes.size() == 3
            && dragging.writes[1].value.fgc() == argb{ pal.sb_drag };
    }

    auto test_textbox_scrollbar_press_promotes_to_drag_paint() -> bool
    {
        auto st = textbox_state{};
        st.disp_w = 8;
        st.content_w = 16;
        st.has_hsb = true;
        st.hsb_y = 5;
        st.hsb_hover = st.hsb_press = true;

        auto pressed = state_canvas{};
        tb_paint_scrollbars(st, pressed);
        if (pressed.writes.size() != 3
         || pressed.writes.back().area != rect{{ 0, 5 }, { 4, 1 }}) return faux;

        st.hsb_press = faux;
        st.hsb_drag = true;
        auto dragging = state_canvas{};
        tb_paint_scrollbars(st, dragging);
        return dragging.writes.size() == 3
            && dragging.writes[1].value.fgc() == argb{ theme::sb_drag };
    }

    auto test_posix_name_validation() -> bool
    {
        auto reason = text{};
        return pane_validate_name("notes\\draft:*?", faux, reason)
            && !pane_validate_name("", faux, reason)
            && !pane_validate_name(".", faux, reason)
            && !pane_validate_name("..", faux, reason)
            && !pane_validate_name("a/b", faux, reason);
    }

    // ---- Scrollbar thumb rounding (vsb) ----
    auto test_vsb_thumb_rounding_up() -> bool
    {
        auto st = table_state{};
        st.body_top = 1; st.body_rows = 5; st.total_lines = 9;
        st.scroll = 1; st.has_vsb = true;
        auto sb = tbl_vsb(st);
        if (!sb.ok) return faux;
        return sb.thumb_y == 2
            && sb.thumb_h == 2 && sb.maxscroll == 4;
    }
    auto test_vsb_thumb_rounding_down() -> bool
    {
        auto st = table_state{};
        st.body_top = 1; st.body_rows = 5; st.total_lines = 9;
        st.scroll = 0; st.has_vsb = true;
        auto sb = tbl_vsb(st);
        if (!sb.ok) return faux;
        return sb.thumb_y == st.body_top
            && sb.thumb_h == 2 && sb.maxscroll == 4;
    }
    auto test_vsb_to_rounding() -> bool
    {
        auto st = table_state{};
        st.body_top = 1; st.body_rows = 5; st.total_lines = 9;
        st.has_vsb = true; st.sb_grab = 0;
        auto sb = tbl_vsb(st);
        if (!sb.ok) return faux;
        tbl_vsb_to(st, 3, sb); // local_y - sb_grab - top = 3 - 0 - 1 = 2; travel=3,max=4; old=2,new=3
        return st.scroll == 3;
    }
    auto test_vsb_roundtrip_stable() -> bool
    {
        auto st = table_state{};
        st.body_top = 1; st.body_rows = 5; st.total_lines = 9;
        st.has_vsb = true; st.scroll = 1;
        auto sb = tbl_vsb(st);
        if (!sb.ok) return faux;
        auto thumb_y = sb.thumb_y;
        st.sb_grab = 0;
        tbl_vsb_to(st, thumb_y, sb);
        return st.scroll == 1; // roundtrip: scroll→thumb→scroll must be stable
    }
    auto test_vsb_drag_monotonic() -> bool
    {
        auto st = table_state{};
        st.body_top = 0; st.body_rows = 10; st.total_lines = 100;
        st.has_vsb = true; st.sb_grab = 0;
        auto sb = tbl_vsb(st);
        if (!sb.ok) return faux;
        auto prev_thumb = sb.thumb_y;
        auto prev_scroll = st.scroll;
        for (auto y = sb.top; y <= sb.top + sb.track_h; ++y)
        {
            tbl_vsb_to(st, y, sb);
            sb = tbl_vsb(st);
            if (sb.thumb_y < prev_thumb) return faux;
            if (st.scroll < prev_scroll) return faux;
            prev_thumb = sb.thumb_y;
            prev_scroll = st.scroll;
        }
        return st.scroll == sb.maxscroll;
    }
    auto test_vsb_endpoints_exact() -> bool
    {
        auto st = table_state{};
        st.body_top = 2; st.body_rows = 10; st.total_lines = 100;
        st.has_vsb = true;
        st.scroll = 0;
        auto sb0 = tbl_vsb(st);
        st.scroll = sb0.maxscroll;
        auto sb1 = tbl_vsb(st);
        return sb0.ok && sb1.ok
            && sb0.thumb_y == st.body_top
            && sb1.thumb_y == st.body_top + sb0.track_h - sb0.thumb_h;
    }
    // ---- Scrollbar thumb rounding (hsb) ----
    auto test_hsb_to_rounding() -> bool
    {
        auto st = table_state{};
        st.disp_w = 6; st.content_w = 11; st.has_hsb = true; st.hsb_grab = 0;
        auto sb = tbl_hsb(st);
        if (!sb.ok) return faux;
        tbl_hsb_to(st, 3, sb); // diff = 3 - 0 - 0 = 3; thumb_h=3, travel=3, maxscroll=5; old=5, new→
        auto ok = st.hscroll >= 0 && st.hscroll <= sb.maxscroll;
        return ok;
    }
    auto test_hsb_thumb_rounding() -> bool
    {
        auto st = table_state{};
        st.disp_w = 6; st.content_w = 10; st.has_hsb = true; st.hscroll = 3;
        auto sb = tbl_hsb(st);
        // disp_w=6, content=10: thumb_h=max(1,6*6/10)=3, maxscroll=4
        // travel=6-3=3; thumb_y=(3*3+2)/4=11/4=2
        return sb.ok && sb.thumb_h == 3 && sb.maxscroll == 4 && sb.thumb_y == 2;
    }
    // ---- Textbox scrollbar thumb rounding ----
    auto test_tb_vsb_thumb_rounding() -> bool
    {
        auto st = textbox_state{};
        st.body_top = 1; st.body_rows = 5; st.total = 9;
        st.has_vsb = true; st.scroll = 1;
        auto sb = tb_vsb(st);
        if (!sb.ok) return faux;
        return sb.thumb_y == 2 && sb.thumb_h == 2;
    }
    auto test_tb_vsb_to_rounding() -> bool
    {
        auto st = textbox_state{};
        st.body_top = 1; st.body_rows = 5; st.total = 9;
        st.has_vsb = true; st.sb_grab = 0;
        auto sb = tb_vsb(st);
        if (!sb.ok) return faux;
        tb_vsb_to(st, 3, sb);
        return st.scroll == 3;
    }
    auto test_tb_vsb_roundtrip_stable() -> bool
    {
        auto st = textbox_state{};
        st.body_top = 1; st.body_rows = 5; st.total = 9;
        st.has_vsb = true; st.scroll = 1;
        auto sb = tb_vsb(st);
        if (!sb.ok) return faux;
        auto thumb_y = sb.thumb_y;
        st.sb_grab = 0;
        tb_vsb_to(st, thumb_y, sb);
        return st.scroll == 1;
    }
    auto test_tb_hsb_thumb_rounding() -> bool
    {
        auto st = textbox_state{};
        st.disp_w = 6; st.content_w = 10; st.has_hsb = true; st.hscroll = 3;
        auto sb = tb_hsb(st);
        return sb.ok && sb.thumb_h == 3 && sb.maxscroll == 4 && sb.thumb_y == 2;
    }

    auto test_windows_name_validation() -> bool
    {
        auto reason = text{};
        return pane_validate_name("notes.txt", true, reason)
            && !pane_validate_name("a:b", true, reason)
            && !pane_validate_name("a\\b", true, reason)
            && !pane_validate_name("trail. ", true, reason)
            && !pane_validate_name("CON", true, reason)
            && !pane_validate_name("nul.txt", true, reason)
            && !pane_validate_name("Com9.log", true, reason)
            && !pane_validate_name("lpt1", true, reason);
    }

    auto test_default_directory_numbering() -> bool
    {
        auto st = pane_state{};
        st.items = {
            direntry{ .name = "New folder",     .is_dir = true },
            direntry{ .name = "New folder (2)", .is_dir = false },
            direntry{ .name = "New folder (4)", .is_dir = true },
        };
        return pane_default_dir_name(st) == "New folder (3)"
            && pane_names_equal("Readme", "README", true)
            && !pane_names_equal("Readme", "README", faux);
    }
}

int main()
{
    struct test_case { char const* name; bool (*run)(); };
    auto tests = std::vector<test_case>{
        { "left_clip_ascii",        test_left_clip_ascii },
        { "left_clip_wide_cluster", test_left_clip_wide_cluster },
        { "fully_hidden_left",      test_fully_hidden_left },
        { "short_text_hidden",      test_short_text_hidden_in_wide_cell },
        { "sortable_header_controls_autofit", test_sortable_header_controls_autofit },
        { "body_content_controls_autofit", test_body_content_controls_autofit },
        { "plain_header_has_no_sort_suffix", test_plain_header_has_no_sort_suffix },
        { "empty_sortable_table_fits_header", test_empty_sortable_table_fits_header },
        { "add_column_keeps_roster_in_sync", test_add_column_keeps_roster_in_sync },
        { "column_width_write_is_clamped", test_column_width_write_is_clamped },
        { "numeric_menu_shortcuts", test_numeric_menu_shortcuts },
        { "column_menu_uses_caller_shortcuts", test_column_menu_uses_caller_shortcuts },
        { "page_navigation_edges_first", test_page_navigation_visits_edges_first },
        { "page_navigation_scroll_from_edges", test_page_navigation_scrolls_from_edges },
        { "page_navigation_partial_last_page", test_page_navigation_partial_last_page },
        { "page_navigation_visible_page", test_page_navigation_uses_visible_page },
        { "reveal_scroll_keeps_visible_row", test_reveal_scroll_keeps_visible_row },
        { "reveal_scroll_moves_minimally", test_reveal_scroll_moves_minimally },
        { "reveal_scroll_clamps_bounds", test_reveal_scroll_clamps_bounds },
        { "selected_row_rejects_stale_cursor", test_selected_row_rejects_stale_cursor },
        { "selected_row_keeps_selected_cursor", test_selected_row_keeps_selected_cursor },
        { "revision_resets_selection_state", test_revision_resets_selection_state },
        { "tail_follow_rearms_at_bottom", test_tail_follow_rearms_at_bottom },
        { "source_follow_does_not_rearm_at_unrelated_bottom", test_source_follow_does_not_rearm_at_unrelated_bottom },
        { "variable_row_line_geometry", test_variable_row_line_geometry },
        { "variable_row_reveal_uses_lines", test_variable_row_reveal_uses_lines },
        { "rubber_band_preserves_blank_endpoint", test_rubber_band_preserves_blank_endpoint },
        { "rubber_overshoot_keeps_last_cursor", test_rubber_overshoot_keeps_last_row_as_keyboard_cursor },
        { "component_cells_retain_and_reconcile_widgets", test_component_cells_retain_and_reconcile_widgets },
        { "table_can_be_nested_as_component_content", test_table_can_be_nested_as_component_content },
        { "component_host_clips_every_edge", test_component_host_clips_every_edge },
        { "component_cells_clip_during_horizontal_scroll", test_component_cells_clip_during_horizontal_scroll },
        { "table_scrollbar_press_promotes_to_drag_paint", test_table_scrollbar_press_promotes_to_drag_paint },
        { "textbox_scrollbar_press_promotes_to_drag_paint", test_textbox_scrollbar_press_promotes_to_drag_paint },
        { "posix_name_validation", test_posix_name_validation },
        { "windows_name_validation", test_windows_name_validation },
        { "default_directory_numbering", test_default_directory_numbering },
        { "vsb_thumb_rounding_up", test_vsb_thumb_rounding_up },
        { "vsb_thumb_rounding_down", test_vsb_thumb_rounding_down },
        { "vsb_to_rounding", test_vsb_to_rounding },
        { "vsb_roundtrip_stable", test_vsb_roundtrip_stable },
        { "vsb_drag_monotonic", test_vsb_drag_monotonic },
        { "vsb_endpoints_exact", test_vsb_endpoints_exact },
        { "hsb_to_rounding", test_hsb_to_rounding },
        { "hsb_thumb_rounding", test_hsb_thumb_rounding },
        { "tb_vsb_thumb_rounding", test_tb_vsb_thumb_rounding },
        { "tb_vsb_to_rounding", test_tb_vsb_to_rounding },
        { "tb_vsb_roundtrip_stable", test_tb_vsb_roundtrip_stable },
        { "tb_hsb_thumb_rounding", test_tb_hsb_thumb_rounding },
    };

    auto failed = 0;
    for (auto const& t : tests)
    {
        auto ok = t.run();
        std::printf("TEST: %s %s\n", t.name, ok ? "PASS" : "FAIL");
        if (!ok) ++failed;
    }
    return failed ? 1 : 0;
}
