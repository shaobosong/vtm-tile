// Copyright (c) Shaobo Song
// Licensed under the MIT license.

// Unit tests for the Parvion textbox selection modes, with the focus on the
// quadruple-click "select all" mode (textbox_state::sel_all): the whole-document
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
