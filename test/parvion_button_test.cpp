// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#include "netxs/apps.hpp"
#include "vtm-common.hpp"
#include "netxs/apps/parvion/components/button.hpp"
#include "netxs/apps/parvion/connectbar.hpp"
#include "netxs/apps/parvion/components/tabs.hpp"

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

    auto test_counted_tab_abbreviation_preserves_suffix() -> bool
    {
        return tab_abbreviate_count("Transferring (0)", 15) == "Transferri… (0)"
            && tab_abbreviate_count("Transferring (0)", 6) == "T… (0)"
            && tab_abbreviate_count("Transferring (1000)", 6) == "T… (1000)";
    }

    auto test_tab_abbreviation_prioritizes_longer_titles() -> bool
    {
        auto pages = std::vector<tab_page_cfg>{
            { {}, []{ return text{ "Transferring (0)" }; }, tab_abbreviate_count },
            { {}, []{ return text{ "Failed (0)" }; },       tab_abbreviate_count },
        };
        // Full buttons occupy 30 cells. The longer title absorbs both reductions.
        auto labels = resolve_tab_titles(pages, 28);
        return labels.size() == 2
            && labels[0] == "Transferr… (0)"
            && labels[1] == "Failed (0)"
            && cell_width(labels[0]) + cell_width(labels[1]) + 4 == 28;
    }

    auto test_tab_abbreviation_balances_tied_titles() -> bool
    {
        auto pages = std::vector<tab_page_cfg>{
            { {}, []{ return text{ "Succeeded (0)" }; }, tab_abbreviate_count },
            { {}, []{ return text{ "Checksums (0)" }; },  tab_abbreviate_count },
            { {}, []{ return text{ "Completed (0)" }; },  tab_abbreviate_count },
        };
        // Full buttons occupy 45 cells. Two reductions rotate through the first two tied titles and
        // stop at the exact requested width.
        auto labels = resolve_tab_titles(pages, 43);
        return labels.size() == 3
            && labels[0] == "Succeed… (0)"
            && labels[1] == "Checksu… (0)"
            && labels[2] == "Completed (0)"
            && cell_width(labels[0]) + cell_width(labels[1]) + cell_width(labels[2]) + 6 == 43;
    }

    auto test_tab_abbreviation_balances_visible_width() -> bool
    {
        auto pages = std::vector<tab_page_cfg>{
            { {}, []{ return text{ "Short (1000)" }; }, tab_abbreviate_count },
            { {}, []{ return text{ "Longer (0)" }; },   tab_abbreviate_count },
        };
        // The longer protected count makes the first rendered label wider, so its title contracts
        // first while both count suffixes remain unchanged.
        auto labels = resolve_tab_titles(pages, 25);
        return labels.size() == 2
            && labels[0] == "Sho… (1000)"
            && labels[1] == "Longer (0)"
            && cell_width(labels[0]) + cell_width(labels[1]) + 4 == 25;
    }
}

int main()
{
    auto ok = test_palette_and_centering()
           && test_hover_and_press_overlays()
           && test_hit_bounds()
           && test_connect_caption_remains_responsive()
           && test_counted_tab_abbreviation_preserves_suffix()
           && test_tab_abbreviation_prioritizes_longer_titles()
           && test_tab_abbreviation_balances_tied_titles()
           && test_tab_abbreviation_balances_visible_width();
    if (!ok) std::fprintf(stderr, "parvion button tests failed\n");
    return ok ? 0 : 1;
}
