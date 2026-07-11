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
}

int main()
{
    struct test_case { char const* name; bool (*run)(); };
    auto tests = std::vector<test_case>{
        { "left_clip_ascii",        test_left_clip_ascii },
        { "left_clip_wide_cluster", test_left_clip_wide_cluster },
        { "fully_hidden_left",      test_fully_hidden_left },
        { "short_text_hidden",      test_short_text_hidden_in_wide_cell },
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
