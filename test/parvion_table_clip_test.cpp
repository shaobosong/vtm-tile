// Copyright (c) Shaobo Song
// Licensed under the MIT license.

// Unit tests for Parvion table horizontal clipping. The regression: when the
// table is horizontally scrolled, text that starts before the visible viewport
// must not be passed to the canvas with a negative x coordinate.

#include "netxs/apps.hpp"
#include "vtm-common.hpp"
#include "netxs/apps/parvion/table.hpp"

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

    auto test_posix_name_validation() -> bool
    {
        auto reason = text{};
        return pane_validate_name("notes\\draft:*?", faux, reason)
            && !pane_validate_name("", faux, reason)
            && !pane_validate_name(".", faux, reason)
            && !pane_validate_name("..", faux, reason)
            && !pane_validate_name("a/b", faux, reason);
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
        { "posix_name_validation", test_posix_name_validation },
        { "windows_name_validation", test_windows_name_validation },
        { "default_directory_numbering", test_default_directory_numbering },
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
