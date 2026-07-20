// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#include "netxs/apps.hpp"
#include "vtm-common.hpp"
#include "netxs/apps/parvion/button.hpp"
#include "netxs/apps/parvion/connectbar.hpp"

#include <cstdio>
#include <vector>

using namespace netxs;
using namespace netxs::app::parvion;

namespace
{
    struct mock_canvas
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

    auto render(button_state state, button_palette palette = {}) -> mock_canvas
    {
        auto canvas = mock_canvas{};
        auto cfg = button_cfg{ .label = []{ return text{ "Go" }; }, .palette = palette };
        button_render(state, cfg, canvas, { 6, 1 });
        return canvas;
    }

    auto test_palette_and_centering() -> bool
    {
        auto bg = ui32{ 0xFF010203u };
        auto fg = ui32{ 0xFF040506u };
        auto canvas = render({}, { bg, fg });
        if (canvas.writes.size() < 3) return faux;
        auto const& fill = canvas.writes[0];
        auto const& g = canvas.writes[1];
        auto const& o = canvas.writes[2];
        return fill.area == rect{ {}, { 6, 1 } }
            && fill.value.bgc() == argb{ bg }
            && g.area.coor == twod{ 2, 0 }
            && o.area.coor == twod{ 3, 0 }
            && g.value.fgc() == argb{ fg }
            && g.value.bgc() == argb{ bg };
    }

    auto test_hover_and_press_overlays() -> bool
    {
        auto rest  = render({});
        auto hover = render({ .hover = true });
        auto press = render({ .hover = true, .press = true });
        return hover.writes.size() == rest.writes.size() + 1
            && press.writes.size() == rest.writes.size() + 1
            && hover.writes.back().area == rect{ {}, { 6, 1 } }
            && press.writes.back().area == rect{ {}, { 6, 1 } };
    }

    auto test_hit_bounds() -> bool
    {
        return button_hit({ 6, 1 }, 0, 0)
            && button_hit({ 6, 1 }, 5, 0)
            && !button_hit({ 6, 1 }, 6, 0)
            && !button_hit({ 6, 1 }, 0, 1)
            && !button_hit({ 6, 1 }, -1, 0);
    }

    auto test_connect_caption_remains_responsive() -> bool
    {
        return cb_resolve(cb_form_width()).connect == " Connect "
            && cb_resolve(cb_min_width()).connect == " » ";
    }
}

int main()
{
    auto ok = test_palette_and_centering()
           && test_hover_and_press_overlays()
           && test_hit_bounds()
           && test_connect_caption_remains_responsive();
    if (!ok) std::fprintf(stderr, "parvion button tests failed\n");
    return ok ? 0 : 1;
}
