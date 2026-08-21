// Copyright (c) Shaobo Song
// Licensed under the MIT license.

// Unit tests for the Parvion textbox mouse and keyboard selection modes, including
// directional anchor/caret behavior and quadruple-click "select all": the whole-document
// span helper, the select-all arming (anchor/head/base + mode), the selection
// text it produces, drag stability in sel_all mode, and id-anchored survival of
// live log appends.

#include "netxs/apps.hpp"
#include "vtm-common.hpp"
#include "netxs/apps/parvion/components/textbox.hpp"

#include <cstdio>
#include <vector>

using namespace netxs;
using namespace netxs::app::parvion;

namespace
{
    auto make_cfg(std::vector<text> lines) -> textbox_cfg
    {
        auto cfg = textbox_cfg{};
        cfg.line_count = [lines]{ return (si32)lines.size(); };
        cfg.line = [lines](si32 i)
        {
            auto out = std::vector<textseg>{};
            if (i >= 0 && i < (si32)lines.size()) out.push_back(textseg{ lines[(size_t)i] });
            return out;
        };
        return cfg;
    }

    void arm_caret(textbox_state& st, textbox_cfg const& cfg, tb_pos p)
    {
        st.anchor_ln = st.head_ln = p.ln;
        st.anchor_cl = st.head_cl = p.cl;
        st.anchor_id = st.head_id = cfg.line_id ? cfg.line_id(p.ln) : nullptr;
        st.selm = textbox_state::sel_char;
        st.sel = true;
        st.seen_epoch = cfg.epoch ? cfg.epoch() : 0;
    }

    // Minimal canvas stub: textbox_render only needs fill(rect, cell-mutator).
    struct mock_canvas
    {
        template<class Fx>
        void fill(rect, Fx fx)
        {
            auto c = cell{};
            fx(c);
        }
    };

    auto test_whole_span_nonempty() -> bool
    {
        auto cfg = make_cfg({ "abc", "de", "fgh" });
        auto [lo, hi] = tb_whole_span(cfg);
        return lo.ln == 0 && lo.cl == 0
            && hi.ln == 2 && hi.cl == 3; // Last line "fgh": 3 clusters.
    }

    auto test_whole_span_empty() -> bool
    {
        auto cfg = make_cfg({});
        auto [lo, hi] = tb_whole_span(cfg);
        return lo.ln == 0 && lo.cl == 0 && hi.ln == 0 && hi.cl == 0;
    }

    auto test_select_all_arms_sel_all_mode() -> bool
    {
        auto cfg = make_cfg({ "abc", "de", "fgh" });
        auto st = textbox_state{};
        st.follow = true;
        if (!tb_select_all(st, cfg)) return faux;
        return st.sel
            && st.selm == textbox_state::sel_all
            && st.anchor_ln == 0 && st.anchor_cl == 0
            && st.head_ln == 2 && st.head_cl == 3
            && st.base_lo_ln == 0 && st.base_lo_cl == 0
            && st.base_hi_ln == 2 && st.base_hi_cl == 3
            && !st.follow && !st.dragging
            && st.drag == textbox_state::d_none;
    }

    auto test_select_all_empty_log_rejected() -> bool
    {
        auto cfg = make_cfg({});
        auto st = textbox_state{};
        return !tb_select_all(st, cfg);
    }

    auto test_select_all_bounds() -> bool
    {
        auto cfg = make_cfg({ "abc", "de", "fgh" });
        auto st = textbox_state{};
        if (!tb_select_all(st, cfg)) return faux;
        auto [lo, hi] = tb_sel_bounds(st, 3);
        return lo.ln == 0 && lo.cl == 0 && hi.ln == 2 && hi.cl == 3;
    }

    auto test_select_all_text() -> bool
    {
        auto cfg = make_cfg({ "abc", "de", "fgh" });
        auto st = textbox_state{};
        if (!tb_select_all(st, cfg)) return faux;
        return tb_selection_text(st, cfg) == "abc\nde\nfgh";
    }

    auto test_select_all_text_wide_clusters() -> bool
    {
        auto cfg = make_cfg({ "\xE8\xA1\xA8" "a", "b" }); // 表a / b
        auto st = textbox_state{};
        if (!tb_select_all(st, cfg)) return faux;
        return tb_selection_text(st, cfg) == "\xE8\xA1\xA8" "a\nb";
    }

    auto test_select_all_drag_keeps_whole_doc() -> bool
    {
        auto cfg = make_cfg({ "abc", "de", "fgh" });
        auto st = textbox_state{};
        if (!tb_select_all(st, cfg)) return faux;
        st.dragging = true;
        if (!tb_drag_head(st, cfg, 2, 1)) return faux; // Drag somewhere in the middle.
        auto [lo, hi] = tb_sel_bounds(st, 3);
        return lo.ln == 0 && lo.cl == 0 && hi.ln == 2 && hi.cl == 3;
    }

    auto test_select_all_clear_resets_mode() -> bool
    {
        auto cfg = make_cfg({ "abc", "de", "fgh" });
        auto st = textbox_state{};
        if (!tb_select_all(st, cfg)) return faux;
        tb_sel_clear(st);
        return !st.sel
            && st.selm == textbox_state::sel_none
            && !st.dragging
            && st.anchor_id == nullptr && st.head_id == nullptr;
    }

    auto test_select_all_sets_line_ids() -> bool
    {
        auto cfg = make_cfg({ "abc", "de" });
        cfg.line_id = [](si32 i){ return reinterpret_cast<const void*>((intptr_t)i + 1); };
        auto st = textbox_state{};
        if (!tb_select_all(st, cfg)) return faux;
        return st.anchor_id == reinterpret_cast<const void*>((intptr_t)1)
            && st.head_id   == reinterpret_cast<const void*>((intptr_t)2);
    }

    auto test_select_all_survives_appends() -> bool
    {
        // Lines are appended live after the select-all: the selection must stay
        // anchored to the same lines (not slide, not lose its endpoints).
        auto count = si32{ 2 };
        auto cfg = textbox_cfg{};
        cfg.line_count = [&count]{ return count; };
        cfg.line = [](si32 i){ return std::vector<textseg>{ textseg{ i == 0 ? text{ "aa" } : text{ "bb" } } }; };
        cfg.line_id = [](si32 i){ return reinterpret_cast<const void*>((intptr_t)i + 1); };
        auto st = textbox_state{};
        if (!tb_select_all(st, cfg)) return faux;
        count = 4; // Two more lines appended below the selection.
        if (!tb_reanchor(st, cfg, count)) return faux;
        auto [lo, hi] = tb_sel_bounds(st, count);
        return lo.ln == 0 && lo.cl == 0 && hi.ln == 1 && hi.cl == 2;
    }

    auto test_select_all_rejects_epoch_shift() -> bool
    {
        auto count = si32{ 2 };
        auto epoch = si32{ 0 };
        auto cfg = textbox_cfg{};
        cfg.line_count = [&count]{ return count; };
        cfg.line = [](si32){ return std::vector<textseg>{ textseg{ text{ "aa" } } }; };
        cfg.epoch = [&epoch]{ return (ui64)epoch; };
        auto st = textbox_state{};
        if (!tb_select_all(st, cfg)) return faux;
        epoch = 1; // A layout shift invalidates the columns.
        return !tb_reanchor(st, cfg, count);
    }

    auto test_line_span_select_unchanged_by_sel_all() -> bool
    {
        // The pre-existing modes keep their exact spans (regression guard).
        auto cfg = make_cfg({ "abc", "de", "fgh" });
        auto [wlo, whi] = tb_word_span(cfg, tb_pos{ 1, 1 });
        auto [llo, lhi] = tb_line_span(cfg, tb_pos{ 1, 1 });
        return wlo.ln == 1 && wlo.cl == 0 && whi.ln == 1 && whi.cl == 2
            && llo.ln == 1 && llo.cl == 0 && lhi.ln == 1 && lhi.cl == 2;
    }

    auto test_word_drag_uses_directional_anchor_and_caret() -> bool
    {
        auto cfg = make_cfg({ "zero one two" });
        auto st = textbox_state{};
        st.body_rows = 1;
        if (!tb_select_span(st, cfg, tb_pos{ 0, 6 }, textbox_state::sel_word, true)) return faux;
        if (st.anchor_cl != 5 || st.head_cl != 8) return faux;

        // Reverse: the original word's right edge is fixed; the caret is at the new left edge.
        if (!tb_drag_head(st, cfg, 2, 0)
         || st.anchor_ln != 0 || st.anchor_cl != 8
         || st.head_ln != 0 || st.head_cl != 0
         || tb_selection_text(st, cfg) != "zero one") return faux;

        // Forward and crossing back over the origin restore the corresponding directional endpoints.
        if (!tb_drag_head(st, cfg, 11, 0)
         || st.anchor_cl != 5 || st.head_cl != 12
         || tb_selection_text(st, cfg) != "one two") return faux;
        return tb_drag_head(st, cfg, 7, 0)
            && st.anchor_cl == 5 && st.head_cl == 8;
    }

    auto test_line_drag_uses_directional_anchor_and_caret() -> bool
    {
        auto cfg = make_cfg({ "abc", "de", "f" });
        auto st = textbox_state{};
        st.body_rows = 3;
        if (!tb_select_span(st, cfg, tb_pos{ 1, 1 }, textbox_state::sel_line, true)
         || st.anchor_ln != 1 || st.anchor_cl != 0
         || st.head_ln != 1 || st.head_cl != 2) return faux;
        if (!tb_drag_head(st, cfg, 2, 0)
         || st.anchor_ln != 1 || st.anchor_cl != 2
         || st.head_ln != 0 || st.head_cl != 0
         || tb_selection_text(st, cfg) != "abc\nde") return faux;
        return tb_drag_head(st, cfg, 2, 2)
            && st.anchor_ln == 1 && st.anchor_cl == 0
            && st.head_ln == 2 && st.head_cl == 1
            && tb_selection_text(st, cfg) == "de\nf";
    }

    auto test_key_select_char_crosses_anchor_and_lines() -> bool
    {
        auto cfg = make_cfg({ "ab", "cd" });
        auto st = textbox_state{};
        arm_caret(st, cfg, tb_pos{ 0, 1 });
        auto shift = input::hids::LShift;
        if (!tb_key_select(st, cfg, input::key::KeyRightArrow, shift)
         || st.anchor_cl != 1 || st.head_ln != 0 || st.head_cl != 2
         || tb_selection_text(st, cfg) != "b") return faux;
        if (!tb_key_select(st, cfg, input::key::KeyLeftArrow, shift)
         || tb_has_selection(st)) return faux;
        if (!tb_key_select(st, cfg, input::key::KeyLeftArrow, shift)
         || st.anchor_cl != 1 || st.head_cl != 0
         || tb_selection_text(st, cfg) != "a") return faux;
        if (!tb_key_select(st, cfg, input::key::KeyRightArrow, shift)
         || !tb_key_select(st, cfg, input::key::KeyRightArrow, shift)
         || !tb_key_select(st, cfg, input::key::KeyRightArrow, shift)) return faux;
        return st.head_ln == 1 && st.head_cl == 0
            && tb_selection_text(st, cfg) == "b\n";
    }

    auto test_key_select_vertical_retains_visual_goal() -> bool
    {
        auto cfg = make_cfg({ "abcdef", "x", "\xE8\xA1\xA8" "abc" }); // 表abc is five cells.
        auto st = textbox_state{};
        arm_caret(st, cfg, tb_pos{ 0, 5 });
        auto shift = input::hids::RShift;
        if (!tb_key_select(st, cfg, input::key::KeyDownArrow, shift)
         || st.head_ln != 1 || st.head_cl != 1 || st.vert_cell != 5) return faux;
        if (!tb_key_select(st, cfg, input::key::NumpadDownArrow, shift)
         || st.head_ln != 2 || st.head_cl != 4 || st.vert_cell != 5) return faux;
        if (!tb_key_select(st, cfg, input::key::KeyUpArrow, shift)
         || !tb_key_select(st, cfg, input::key::KeyUpArrow, shift)) return faux;
        return st.head_ln == 0 && st.head_cl == 5 && st.vert_cell == 5;
    }

    auto test_key_select_platform_word_boundaries() -> bool
    {
        auto cfg = make_cfg({ "one  two.three", "", " four" });
        auto st = textbox_state{};
        arm_caret(st, cfg, tb_pos{ 0, 0 });
        auto chord = input::hids::LShift | input::hids::LCtrl;
        if (!tb_key_select(st, cfg, input::key::KeyRightArrow, chord)
         || st.head_ln != 0 || st.head_cl != 5) return faux;  // Skip "one" and spaces.
        if (!tb_key_select(st, cfg, input::key::KeyRightArrow, chord)
         || st.head_cl != 8) return faux;                     // Stop before punctuation.
        if (!tb_key_select(st, cfg, input::key::KeyRightArrow, chord)
         || st.head_cl != 9) return faux;                     // Punctuation is its own run.
        if (!tb_key_select(st, cfg, input::key::KeyRightArrow, chord)
         || st.head_ln != 2 || st.head_cl != 1) return faux;  // Skip newlines, blank line, and space.
        if (!tb_key_select(st, cfg, input::key::NumpadLeftArrow, chord)) return faux;
        return st.head_ln == 0 && st.head_cl == 9;
    }

    auto test_key_select_home_end_scopes() -> bool
    {
        auto cfg = make_cfg({ "abc", "de", "fgh" });
        auto st = textbox_state{};
        arm_caret(st, cfg, tb_pos{ 1, 1 });
        auto shift = input::hids::LShift;
        auto chord = shift | input::hids::RCtrl;
        if (!tb_key_select(st, cfg, input::key::KeyEnd, shift)
         || st.head_ln != 1 || st.head_cl != 2) return faux;
        if (!tb_key_select(st, cfg, input::key::NumpadHome, shift)
         || st.head_ln != 1 || st.head_cl != 0) return faux;
        if (!tb_key_select(st, cfg, input::key::KeyEnd, chord)
         || st.head_ln != 2 || st.head_cl != 3) return faux;
        if (!tb_key_select(st, cfg, input::key::NumpadHome, chord)
         || st.head_ln != 0 || st.head_cl != 0) return faux;
        return st.anchor_ln == 1 && st.anchor_cl == 1
            && tb_selection_text(st, cfg) == "abc\nd";
    }

    auto test_key_select_ids_visibility_and_mode() -> bool
    {
        auto cfg = make_cfg({ "a", "b", "c", "0123456789" });
        cfg.line_id = [](si32 i){ return reinterpret_cast<const void*>((intptr_t)i + 1); };
        auto st = textbox_state{};
        st.body_rows = 2; st.disp_w = 5; st.content_w = 11; st.total = 4;
        arm_caret(st, cfg, tb_pos{ 2, 0 });
        st.selm = textbox_state::sel_word;
        st.dragging = true;
        auto shift = input::hids::LShift;
        if (!tb_key_select(st, cfg, input::key::KeyDownArrow, shift)
         || st.head_ln != 3 || st.head_id != reinterpret_cast<const void*>((intptr_t)4)
         || st.anchor_id != reinterpret_cast<const void*>((intptr_t)3)
         || st.scroll != 2 || st.selm != textbox_state::sel_char || st.dragging) return faux;
        return tb_key_select(st, cfg, input::key::KeyEnd, shift)
            && st.head_cl == 10 && st.hscroll == 6 && !st.follow;
    }

    auto test_key_select_defaults_and_modifier_filter() -> bool
    {
        auto cfg = make_cfg({ "ab" });
        auto st = textbox_state{};
        auto shift = input::hids::LShift;
        if (tb_key_select(st, cfg, input::key::KeyRightArrow, 0)
         || tb_key_select(st, cfg, input::key::KeyPageDown, shift)
         || tb_key_select(st, cfg, input::key::KeyUpArrow, shift | input::hids::LCtrl)
         || tb_key_select(st, cfg, input::key::KeyLeftArrow, shift | input::hids::LAlt)) return faux;
        return tb_key_select(st, cfg, input::key::NumpadRightArrow, shift)
            && st.sel && st.anchor_ln == 0 && st.anchor_cl == 0
            && st.head_ln == 0 && st.head_cl == 1
            && tb_selection_text(st, cfg) == "a";
    }

    auto test_key_scroll_pages_and_clamps() -> bool
    {
        auto st = textbox_state{};
        st.total = 100; st.body_rows = 10; st.scroll = 50;
        if (!tb_key_scroll(st, input::key::KeyPageUp, 0) || st.scroll != 40 || st.follow) return faux;
        if (!tb_key_scroll(st, input::key::KeyPageDown, 0) || st.scroll != 50 || st.follow) return faux;
        st.scroll = 6;
        if (!tb_key_scroll(st, input::key::KeyPageUp, 0) || st.scroll != 0 || st.follow) return faux;
        st.scroll = 86;
        return tb_key_scroll(st, input::key::KeyPageDown, 0)
            && st.scroll == 90
            && st.follow;
    }

    auto test_key_scroll_arrows_and_clamps() -> bool
    {
        auto st = textbox_state{};
        st.total = 12; st.body_rows = 5; st.scroll = 3;
        if (!tb_key_scroll(st, input::key::KeyUpArrow, 0) || st.scroll != 2 || st.follow) return faux;
        if (!tb_key_scroll(st, input::key::KeyDownArrow, 0) || st.scroll != 3 || st.follow) return faux;
        st.scroll = 0;
        if (!tb_key_scroll(st, input::key::NumpadUpArrow, 0) || st.scroll != 0 || st.follow) return faux;
        st.scroll = 6;
        return tb_key_scroll(st, input::key::NumpadDownArrow, 0)
            && st.scroll == 7
            && st.follow;
    }

    auto test_key_scroll_ctrl_home_end() -> bool
    {
        auto st = textbox_state{};
        st.total = 25; st.body_rows = 8; st.scroll = 9;
        if (!tb_key_scroll(st, input::key::KeyHome, input::hids::LCtrl)
         || st.scroll != 0 || st.follow) return faux;
        return tb_key_scroll(st, input::key::NumpadEnd, input::hids::RCtrl)
            && st.scroll == 17
            && st.follow;
    }

    auto test_key_scroll_modifier_filter_and_numpad() -> bool
    {
        auto st = textbox_state{};
        st.total = 30; st.body_rows = 5; st.scroll = 10; st.follow = faux;
        if (tb_key_scroll(st, input::key::KeyHome, 0)
         || tb_key_scroll(st, input::key::KeyEnd, input::hids::LCtrl | input::hids::LShift)
         || tb_key_scroll(st, input::key::KeyUpArrow, input::hids::LShift)
         || tb_key_scroll(st, input::key::KeyDownArrow, input::hids::LCtrl)
         || tb_key_scroll(st, input::key::KeyPageUp, input::hids::LCtrl)
         || tb_key_scroll(st, input::key::KeyPageDown, input::hids::LAlt)
         || st.scroll != 10 || st.follow) return faux;
        return tb_key_scroll(st, input::key::NumpadPageUp, 0)
            && st.scroll == 5
            && !st.follow;
    }

    auto test_empty_render_rearms_follow() -> bool
    {
        // Clear All empties the log while follow is disengaged (scrolled up / selecting):
        // rendering the empty view must re-arm tail-follow so new lines auto-scroll again.
        auto cfg = textbox_cfg{};
        cfg.line_count = []{ return si32{ 0 }; };
        cfg.line = [](si32){ return std::vector<textseg>{}; };
        auto st = textbox_state{};
        st.follow = faux;
        auto canvas = mock_canvas{};
        textbox_render(st, cfg, canvas, twod{ 20, 10 });
        return st.follow;
    }

    auto test_nonempty_render_keeps_follow_disengaged() -> bool
    {
        // A populated view must not silently re-arm follow: the user scrolled up on purpose.
        auto cfg = make_cfg({ "abc", "de", "fgh" });
        auto st = textbox_state{};
        st.follow = faux;
        auto canvas = mock_canvas{};
        textbox_render(st, cfg, canvas, twod{ 20, 10 });
        return !st.follow;
    }

    auto test_context_menu_uses_shared_popup_items() -> bool
    {
        auto empty_cfg = make_cfg({});
        auto empty_st = textbox_state{};
        auto empty = tb_context_menu(empty_st, empty_cfg, {});
        if (empty.items.size() != 3
         || empty.items[0].label != "&Copy"
         || empty.items[0].enabled
         || empty.items[1].kind != popup_menu_item_kind::separator
         || empty.items[2].label != "Select &All"
         || empty.items[2].enabled) return faux;

        auto cfg = make_cfg({ "abc" });
        auto st = textbox_state{};
        if (!tb_select_all(st, cfg)) return faux;
        auto selected = tb_context_menu(st, cfg, {});
        return selected.items.size() == 3
            && selected.items[0].enabled
            && selected.items[2].enabled;
    }

    auto test_context_menu_adapter_arranges_core_items() -> bool
    {
        auto cfg = make_cfg({ "abc" });
        auto called = faux;
        cfg.menu = [&called](netxs::wptr<ui::base>, popup_menu_item copy,
                            popup_menu_item select_all)
        {
            called = copy.label == "&Copy"
                  && !copy.enabled
                  && select_all.label == "Select &All"
                  && select_all.enabled;
            auto items = std::vector<popup_menu_item>{};
            items.push_back(std::move(copy));
            items.push_back(popup_menu_item{ .label = "C&lear All" });
            items.push_back(popup_menu_item{ .kind = popup_menu_item_kind::separator });
            items.push_back(std::move(select_all));
            return popup_menu_content{ .items = std::move(items) };
        };
        auto st = textbox_state{};
        auto content = tb_context_menu(st, cfg, {});
        return called
            && content.items.size() == 4
            && content.items[1].label == "C&lear All"
            && content.items[2].kind == popup_menu_item_kind::separator;
    }
}

int main()
{
    struct test_case { char const* name; bool (*run)(); };
    auto tests = std::vector<test_case>{
        { "whole_span_nonempty",          test_whole_span_nonempty },
        { "whole_span_empty",             test_whole_span_empty },
        { "select_all_arms_sel_all_mode", test_select_all_arms_sel_all_mode },
        { "select_all_empty_log_rejected", test_select_all_empty_log_rejected },
        { "select_all_bounds",            test_select_all_bounds },
        { "select_all_text",              test_select_all_text },
        { "select_all_text_wide_clusters", test_select_all_text_wide_clusters },
        { "select_all_drag_keeps_whole_doc", test_select_all_drag_keeps_whole_doc },
        { "select_all_clear_resets_mode", test_select_all_clear_resets_mode },
        { "select_all_sets_line_ids",     test_select_all_sets_line_ids },
        { "select_all_survives_appends",  test_select_all_survives_appends },
        { "select_all_rejects_epoch_shift", test_select_all_rejects_epoch_shift },
        { "line_span_select_unchanged_by_sel_all", test_line_span_select_unchanged_by_sel_all },
        { "word_drag_uses_directional_anchor_and_caret", test_word_drag_uses_directional_anchor_and_caret },
        { "line_drag_uses_directional_anchor_and_caret", test_line_drag_uses_directional_anchor_and_caret },
        { "key_select_char_crosses_anchor_and_lines", test_key_select_char_crosses_anchor_and_lines },
        { "key_select_vertical_retains_visual_goal", test_key_select_vertical_retains_visual_goal },
        { "key_select_platform_word_boundaries", test_key_select_platform_word_boundaries },
        { "key_select_home_end_scopes", test_key_select_home_end_scopes },
        { "key_select_ids_visibility_and_mode", test_key_select_ids_visibility_and_mode },
        { "key_select_defaults_and_modifier_filter", test_key_select_defaults_and_modifier_filter },
        { "key_scroll_pages_and_clamps", test_key_scroll_pages_and_clamps },
        { "key_scroll_arrows_and_clamps", test_key_scroll_arrows_and_clamps },
        { "key_scroll_ctrl_home_end", test_key_scroll_ctrl_home_end },
        { "key_scroll_modifier_filter_and_numpad", test_key_scroll_modifier_filter_and_numpad },
        { "empty_render_rearms_follow", test_empty_render_rearms_follow },
        { "nonempty_render_keeps_follow_disengaged", test_nonempty_render_keeps_follow_disengaged },
        { "context_menu_uses_shared_popup_items", test_context_menu_uses_shared_popup_items },
        { "context_menu_adapter_arranges_core_items", test_context_menu_adapter_arranges_core_items },
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
