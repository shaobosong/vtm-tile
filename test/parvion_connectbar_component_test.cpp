// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#include "netxs/apps.hpp"
#include "vtm-common.hpp"
#include "netxs/apps/parvion/connectbar.hpp"

#include <cstdio>

using namespace netxs;
using namespace netxs::app::parvion;

namespace
{
    auto test_responsive_endpoints() -> bool
    {
        auto full = cb_arrange(cb_form_width());
        auto minimum = cb_arrange(cb_min_width());
        return full.layout.label == std::array<text, 4>{ "Host:", "User:", "Pass:", "Port:" }
            && full.layout.field == std::array<si32, 4>{ 16, 16, 16, 6 }
            && full.layout.connect == " Connect "
            && minimum.layout.label == std::array<text, 4>{ "H:", "U:", "P:", "#:" }
            && minimum.layout.field == std::array<si32, 4>{ 2, 2, 2, 2 }
            && minimum.layout.connect == " » ";
    }

    auto test_every_responsive_width_has_stable_geometry() -> bool
    {
        for (auto width = cb_min_width(); width <= cb_form_width(); ++width)
        {
            auto geometry = cb_arrange(width);
            if (cb_layout_width(geometry.layout) > width
             || geometry.label[cf_host].coor.x != 1
             || geometry.connect.coor.x + geometry.connect.size.x != width)
                return faux;

            for (auto i = size_t{}; i < geometry.label.size(); ++i)
            {
                if (geometry.label[i].size.x != cell_width(geometry.layout.label[i])
                 || geometry.field[i].size.x != geometry.layout.field[i]
                 || geometry.field[i].size.x < cb_field_min
                 || geometry.field[i].coor.x != geometry.label[i].coor.x
                                                + geometry.label[i].size.x + 1)
                    return faux;
                if (i + 1 < geometry.label.size()
                 && geometry.label[i + 1].coor.x != geometry.field[i].coor.x
                                                      + geometry.field[i].size.x + 1)
                    return faux;
            }
            auto last = geometry.field[cf_port];
            if (geometry.connect.coor.x < last.coor.x + last.size.x + 1) return faux;
        }
        return true;
    }

    auto test_retained_form_applies_resolved_children() -> bool
    {
        auto state = std::make_shared<connect_state>();
        state->fld[cf_port] = "22";
        auto form = connect_form::ctor(state);
        form->limits({ cb_min_width(), 1 }, { cb_form_width(), 1 });

        auto verify = [&](si32 width, view first_label, view button, view button_glyph)
        {
            form->base::extend({ {}, { width, 1 } });
            auto expected = cb_arrange(width);
            for (auto i = size_t{}; i < expected.label.size(); ++i)
            {
                if (form->get_label_area(i) != expected.label[i]
                 || form->get_input_area(i) != expected.field[i]) return faux;
            }
            if (form->get_connect_area() != expected.connect
             || state->layout.label[cf_host] != first_label
             || state->layout.connect != button) return faux;

            auto canvas = ui::face{};
            canvas.size({ width, 1 });
            form->render(canvas);
            auto host_x = expected.label[cf_host].coor.x;
            auto port_x = expected.field[cf_port].coor.x;
            auto button_x = expected.connect.coor.x;
            return canvas[{ host_x, 0 }].txt() == first_label.substr(0, 1)
                && canvas[{ port_x, 0 }].txt() == "2"
                && canvas[{ button_x + 1, 0 }].txt() == button_glyph;
        };

        return verify(cb_form_width(), "Host:", " Connect ", "C")
            && verify(cb_min_width(), "H:", " » ", "»");
    }

    auto test_outer_flex_keeps_history_flush() -> bool
    {
        auto bar = std::dynamic_pointer_cast<flex>(make_connect_bar());
        if (!bar || bar->get_item_count() != 3) return faux;
        auto width = cb_form_width() + 3 + 12;
        bar->base::extend({ {}, { width, 1 } });
        auto canvas = ui::face{};
        canvas.size({ width, 1 });
        bar->render(canvas);

        auto form_end = cb_form_width();
        auto full = cb_arrange(cb_form_width());
        return full.connect.coor.x + full.connect.size.x == form_end
            && canvas[{ form_end + 1, 0 }].txt() == "▾"
            && canvas[{ form_end + 3, 0 }].txt() == " ";
    }
}

int main()
{
    auto ok = test_responsive_endpoints()
           && test_every_responsive_width_has_stable_geometry()
           && test_retained_form_applies_resolved_children()
           && test_outer_flex_keeps_history_flush();
    if (!ok) std::fprintf(stderr, "parvion connect-bar component tests failed\n");
    return ok ? 0 : 1;
}
