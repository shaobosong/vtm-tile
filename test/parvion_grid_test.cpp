// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#include "netxs/apps.hpp"
#include "vtm-common.hpp"
#include "netxs/apps/parvion/components/grid.hpp"

#include <cstdio>

using namespace netxs;
using namespace netxs::app::parvion;

namespace
{
    auto test_track_resolution() -> bool
    {
        auto weighted = resolve_grid_tracks({
            { .weight = 3, .minimum = 3 },
            { .weight = 2, .minimum = 3 },
        }, 42);
        auto fixed = resolve_grid_tracks({
            { .weight = 0, .minimum = 1, .maximum = 1 },
            { .weight = 1, .minimum = 7 },
        }, 43);
        auto clamped = resolve_grid_tracks({
            { .weight = 1, .minimum = 8 },
            { .weight = 1, .minimum = 2 },
        }, 6);
        return weighted == std::vector<si32>{ 26, 16 }
            && fixed == std::vector<si32>{ 1, 42 }
            && clamped == std::vector<si32>{ 8, 2 };
    }

    auto test_hidden_handles_have_no_geometry() -> bool
    {
        auto layout = grid::ctor({
            .columns = { { .weight = 1 } },
            .rows = {
                { .weight = 0, .minimum = 1, .maximum = 1 },
                { .weight = 1, .minimum = 7 },
            },
            .handle_mode = grid_handle_mode::hidden,
        });
        auto bar = layout->attach(ui::mock::ctor(), { .row = 0 });
        auto body = layout->attach(ui::mock::ctor(), { .row = 1 });
        layout->base::extend({ {}, { 80, 40 } });

        return !layout->handles_visible()
            && layout->get_handle_count() == 0
            && layout->get_row_sizes() == std::vector<si32>{ 1, 39 }
            && layout->get_cell_area(bar) == rect{ { 0, 0 }, { 80, 1 } }
            && layout->get_cell_area(body) == rect{ { 0, 1 }, { 80, 39 } };
    }

    auto test_enabled_handles_span_and_reset() -> bool
    {
        auto layout = grid::ctor({
            .columns = {
                { .weight = 1 },
                { .weight = 1 },
            },
            .rows = {
                { .weight = 3, .minimum = 3 },
                { .weight = 2, .minimum = 3 },
            },
            .handle_mode = grid_handle_mode::enabled,
            .column_handle_width = 2,
            .row_handle_height = 1,
        });
        auto local = layout->attach(ui::mock::ctor(), { .column = 0, .row = 0 });
        auto remote = layout->attach(ui::mock::ctor(), { .column = 1, .row = 0 });
        auto tabs = layout->attach(ui::mock::ctor(),
            { .column = 0, .row = 1, .column_span = 2 });
        layout->base::extend({ {}, { 102, 43 } });

        if (!layout->handles_visible()
         || layout->get_handle_count() != 2
         || layout->get_column_sizes() != std::vector<si32>{ 50, 50 }
         || layout->get_row_sizes() != std::vector<si32>{ 26, 16 }
         || layout->get_cell_area(local) != rect{ { 0, 0 }, { 50, 26 } }
         || layout->get_cell_area(remote) != rect{ { 52, 0 }, { 50, 26 } }
         || layout->get_cell_area(tabs) != rect{ { 0, 27 }, { 102, 16 } })
        {
            return faux;
        }

        layout->move_handle(axis::X, 1, 10);
        if (layout->get_column_sizes() != std::vector<si32>{ 60, 40 }) return faux;
        layout->reset_handle(axis::X, 1);
        if (layout->get_column_sizes() != std::vector<si32>{ 50, 50 }) return faux;

        layout->move_handle(axis::Y, 1, -100);
        if (layout->get_row_sizes() != std::vector<si32>{ 3, 39 }) return faux;
        layout->reset_handle(axis::Y, 1);
        return layout->get_row_sizes() == std::vector<si32>{ 26, 16 };
    }

    auto test_disabled_handles_are_solid_and_inert() -> bool
    {
        constexpr auto divider = 0xFF123456;
        auto layout = grid::ctor({
            .columns = { { .weight = 1 } },
            .rows = {
                { .weight = 1 },
                { .weight = 1 },
            },
            .handle_mode = grid_handle_mode::disabled,
            .row_handle_height = 1,
            .handle_color = divider,
        });
        layout->attach(ui::mock::ctor(), { .row = 0 });
        layout->attach(ui::mock::ctor(), { .row = 1 });
        layout->base::extend({ {}, { 12, 5 } });

        auto canvas = ui::face{};
        canvas.size({ 12, 5 });
        layout->render(canvas);
        auto before = layout->get_row_sizes();
        layout->move_handle(axis::Y, 1, 1);
        layout->reset_handle(axis::Y, 1);

        return layout->get_handle_mode() == grid_handle_mode::disabled
            && layout->handles_visible()
            && layout->get_handle_count() == 1
            && before == std::vector<si32>{ 2, 2 }
            && layout->get_row_sizes() == before
            && canvas[{ 0, 2 }].bgc() == argb{ divider }
            && canvas[{ 0, 2 }].link() == 0;
    }

    auto test_border_geometry_and_paint() -> bool
    {
        constexpr auto border_color = 0xFF123456;
        auto layout = grid::ctor({
            .columns = { { .weight = 1 }, { .weight = 1 } },
            .rows = { { .weight = 1 }, { .weight = 1 } },
            .handle_mode = grid_handle_mode::disabled,
            .column_handle_width = 2,
            .row_handle_height = 1,
            .column_border_width = 3, // Border thickness is independent of the handles.
            .row_border_height = 2,
            .column_padding_width = 2,
            .row_padding_height = 1,
            .handle_color = 0xFF00FFFF, // Must not affect the border paint.
            .border_color = border_color,
            .border = true,
        });
        auto a = layout->attach(ui::mock::ctor(), { .column = 0, .row = 0 });
        auto b = layout->attach(ui::mock::ctor(), { .column = 1, .row = 0 });
        auto c = layout->attach(ui::mock::ctor(), { .column = 0, .row = 1 });
        auto d = layout->attach(ui::mock::ctor(), { .column = 1, .row = 1 });
        layout->base::extend({ {}, { 106, 45 } });

        auto canvas = ui::face{};
        canvas.size({ 106, 45 });
        layout->render(canvas);

        // slot_x = (3 border + 2 padding) * 2 + one 2-cell gap = 12, so
        // the columns split 94 into 47/47.  slot_y = (2 border + 1 padding)
        // * 2 + one 1-cell gap = 7, so the rows split 38 into 19/19.
        return layout->get_column_sizes() == std::vector<si32>{ 47, 47 }
            && layout->get_row_sizes() == std::vector<si32>{ 19, 19 }
            && layout->get_cell_area(a) == rect{ { 5, 3 }, { 47, 19 } }
            && layout->get_cell_area(b) == rect{ { 54, 3 }, { 47, 19 } }
            && layout->get_cell_area(c) == rect{ { 5, 23 }, { 47, 19 } }
            && layout->get_cell_area(d) == rect{ { 54, 23 }, { 47, 19 } }
            // Vertical sides are column_border_width cells wide, horizontal
            // sides are row_border_height cells high.
            && canvas[{ 0, 0 }].bgc() == argb{ border_color }
            && canvas[{ 2, 0 }].bgc() == argb{ border_color }
            && canvas[{ 0, 1 }].bgc() == argb{ border_color }
            && canvas[{ 0, 44 }].bgc() == argb{ border_color }
            && canvas[{ 105, 44 }].bgc() == argb{ border_color }
            && canvas[{ 105, 0 }].bgc() == argb{ border_color }
            && canvas[{ 1, 20 }].bgc() == argb{ border_color }
            // Padding is unpainted, and handles begin at the content inset.
            && canvas[{ 3, 2 }].bgc() != argb{ border_color }
            && canvas[{ 4, 3 }].bgc() != argb{ border_color }
            && canvas[{ 52, 2 }].bgc() != argb{ 0xFF00FFFF }
            && canvas[{ 52, 3 }].bgc() == argb{ 0xFF00FFFF }
            && canvas[{ 4, 22 }].bgc() != argb{ 0xFF00FFFF }
            && canvas[{ 5, 22 }].bgc() == argb{ 0xFF00FFFF };
    }

    auto test_border_optional() -> bool
    {
        constexpr auto border_color = 0xFF123456;
        auto layout = grid::ctor({
            .columns = { { .weight = 1 } },
            .rows = { { .weight = 1 } },
            .handle_mode = grid_handle_mode::disabled,
            .column_padding_width = 2,
            .row_padding_height = 1,
            .border_color = border_color,
        });
        auto child = layout->attach(ui::mock::ctor());
        layout->base::extend({ {}, { 10, 5 } });

        auto canvas = ui::face{};
        canvas.size({ 10, 5 });
        layout->render(canvas);

        return layout->get_cell_area(child) == rect{ { 2, 1 }, { 6, 3 } }
            && canvas[{ 0, 0 }].bgc() != argb{ border_color };
    }
}

int main()
{
    auto ok = test_track_resolution()
           && test_hidden_handles_have_no_geometry()
           && test_enabled_handles_span_and_reset()
           && test_disabled_handles_are_solid_and_inert()
           && test_border_geometry_and_paint()
           && test_border_optional();
    if (!ok) std::fprintf(stderr, "parvion grid tests failed\n");
    return ok ? 0 : 1;
}
