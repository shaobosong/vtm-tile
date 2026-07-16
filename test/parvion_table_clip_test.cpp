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
        table.cols.push_back(qtable::column{ .title = text{ title }, .key = 7 });
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
        s.disp = [&]{ return (si32)order.size(); };
        s.key_of_row = [&](si32 row){ return order[(size_t)row]; };
        s.is_sel = [&](si32 key){ return selected.count(key) != 0; };
        // Key 2 was the cursor in the previous listing, but only key 0 is visibly selected now.
        return q_selected_row(s, 2) == 1;
    }

    auto test_selected_row_keeps_selected_cursor() -> bool
    {
        auto selected = std::set<si32>{ 0, 2 };
        auto order = std::vector<si32>{ 2, 0, 1 };
        auto s = qsel_cfg{};
        s.disp = [&]{ return (si32)order.size(); };
        s.key_of_row = [&](si32 row){ return order[(size_t)row]; };
        s.is_sel = [&](si32 key){ return selected.count(key) != 0; };
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
        st.body_rows = 10;
        st.scroll = 40;
        st.row_order.resize(50);
        for (auto i = si32{}; i < 50; ++i) st.row_order[(size_t)i] = i;
        auto cfg = table_cfg{};
        cfg.follow = []{ return table_follow_target{ table_follow_target::source_row, 0 }; };
        return q_follow_scroll(st, cfg) == 0
            && !q_at_follow_target(st, cfg);
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
