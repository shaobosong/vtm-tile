// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#include "netxs/apps.hpp"
#include "vtm-common.hpp"
#include "netxs/apps/parvion/components/flex.hpp"

#include <cstdio>
#include <memory>

using namespace netxs;
using namespace netxs::app::parvion;

namespace
{
    auto test_row_grow_shrink_and_constraints() -> bool
    {
        auto layout = flex::ctor({ .column_gap = 1 });
        auto a = layout->attach(ui::mock::ctor(),
            { .grow = 1, .shrink = 1, .basis = 10, .minimum = 4 });
        auto b = layout->attach(ui::mock::ctor(),
            { .grow = 2, .shrink = 1, .basis = 10, .minimum = 5 });
        auto c = layout->attach(ui::mock::ctor(),
            { .basis = 10, .minimum = 0 });
        layout->base::extend({ {}, { 35, 6 } });

        if (layout->get_item_area(a) != rect{ { 0, 0 }, { 11, 6 } }
         || layout->get_item_area(b) != rect{ { 12, 0 }, { 12, 6 } }
         || layout->get_item_area(c) != rect{ { 25, 0 }, { 10, 6 } }) return faux;

        layout->base::extend({ {}, { 20, 6 } });
        if (layout->get_item_area(a) != rect{ { 0, 0 }, { 6, 6 } }
         || layout->get_item_area(b) != rect{ { 7, 0 }, { 6, 6 } }
         || layout->get_item_area(c) != rect{ { 14, 0 }, { 6, 6 } }) return faux;

        auto capped = flex::ctor();
        auto left = capped->attach(ui::mock::ctor(),
            { .grow = 1, .basis = 5, .maximum = 8 });
        auto right = capped->attach(ui::mock::ctor(),
            { .grow = 1, .basis = 5 });
        capped->base::extend({ {}, { 30, 2 } });
        if (capped->get_item_area(left).size.x != 8
         || capped->get_item_area(right).size.x != 22) return faux;

        auto clamped = flex::ctor();
        auto first = clamped->attach(ui::mock::ctor(),
            { .basis = 8, .minimum = 7 });
        auto second = clamped->attach(ui::mock::ctor(), { .basis = 8 });
        clamped->base::extend({ {}, { 10, 2 } });
        return clamped->get_item_area(first).size.x == 7
            && clamped->get_item_area(second).size.x == 3;
    }

    auto test_column_flow() -> bool
    {
        auto layout = flex::ctor({
            .direction = flex_direction::column,
            .row_gap = 1,
        });
        auto top = layout->attach(ui::mock::ctor(), { .grow = 1, .basis = 4 });
        auto bottom = layout->attach(ui::mock::ctor(), { .grow = 1, .basis = 4 });
        layout->base::extend({ {}, { 8, 15 } });
        return layout->get_item_area(top) == rect{ { 0, 0 }, { 8, 7 } }
            && layout->get_item_area(bottom) == rect{ { 0, 8 }, { 8, 7 } };
    }

    auto test_wrapping_and_gaps() -> bool
    {
        auto layout = flex::ctor({
            .wrapping = flex_wrap::wrap,
            .column_gap = 2,
            .row_gap = 1,
        });
        auto a = ui::mock::ctor();
        auto b = ui::mock::ctor();
        auto c = ui::mock::ctor();
        a->limits({ 0, 2 });
        b->limits({ 0, 2 });
        c->limits({ 0, 2 });
        layout->attach(a, { .basis = 5 });
        layout->attach(b, { .basis = 5 });
        layout->attach(c, { .basis = 5 });
        layout->base::extend({ {}, { 12, 10 } });

        return layout->get_item_area(a) == rect{ { 0, 0 }, { 5, 5 } }
            && layout->get_item_area(b) == rect{ { 7, 0 }, { 5, 5 } }
            && layout->get_item_area(c) == rect{ { 0, 6 }, { 5, 4 } };
    }

    auto justify_geometry(flex_justify mode, si32 first_x, si32 second_x) -> bool
    {
        auto layout = flex::ctor({
            .justify_content = mode,
            .align_items = flex_align::start,
        });
        auto a = layout->attach(ui::mock::ctor(), { .basis = 4 });
        auto b = layout->attach(ui::mock::ctor(), { .basis = 4 });
        layout->base::extend({ {}, { 20, 2 } });
        return layout->get_item_area(a).coor.x == first_x
            && layout->get_item_area(b).coor.x == second_x;
    }

    auto test_justification_and_item_alignment() -> bool
    {
        if (!justify_geometry(flex_justify::start,          0,  4)
         || !justify_geometry(flex_justify::end,           12, 16)
         || !justify_geometry(flex_justify::center,        6, 10)
         || !justify_geometry(flex_justify::space_between, 0, 16)
         || !justify_geometry(flex_justify::space_around,  3, 13)
         || !justify_geometry(flex_justify::space_evenly,  4, 12)) return faux;

        auto layout = flex::ctor({ .align_items = flex_align::center });
        auto centered = ui::mock::ctor();
        auto ended = ui::mock::ctor();
        auto stretched = ui::mock::ctor();
        centered->limits({ 0, 2 }, { -1, 2 });
        ended->limits({ 0, 2 }, { -1, 2 });
        stretched->limits({ 0, 2 });
        layout->attach(centered, { .basis = 4 });
        layout->attach(ended, { .basis = 4, .align_self = flex_align_self::end });
        layout->attach(stretched, { .basis = 4, .align_self = flex_align_self::stretch });
        layout->base::extend({ {}, { 12, 8 } });
        return layout->get_item_area(centered) == rect{ { 0, 3 }, { 4, 2 } }
            && layout->get_item_area(ended) == rect{ { 4, 6 }, { 4, 2 } }
            && layout->get_item_area(stretched) == rect{ { 8, 0 }, { 4, 8 } };
    }

    auto content_geometry(flex_content_align mode, si32 first_y, si32 second_y) -> bool
    {
        auto layout = flex::ctor({
            .wrapping = flex_wrap::wrap,
            .align_items = flex_align::start,
            .align_content = mode,
        });
        auto a = ui::mock::ctor();
        auto b = ui::mock::ctor();
        a->limits({ 0, 2 });
        b->limits({ 0, 2 });
        layout->attach(a, { .basis = 5 });
        layout->attach(b, { .basis = 5 });
        layout->base::extend({ {}, { 5, 10 } });
        return layout->get_item_area(a).coor.y == first_y
            && layout->get_item_area(b).coor.y == second_y;
    }

    auto test_content_alignment() -> bool
    {
        return content_geometry(flex_content_align::start,          0, 2)
            && content_geometry(flex_content_align::end,            6, 8)
            && content_geometry(flex_content_align::center,         3, 5)
            && content_geometry(flex_content_align::stretch,        0, 5)
            && content_geometry(flex_content_align::space_between,  0, 8)
            && content_geometry(flex_content_align::space_around,   2, 7)
            && content_geometry(flex_content_align::space_evenly,   2, 6);
    }

    auto test_intrinsic_basis_order_hidden_remove_and_retention() -> bool
    {
        auto intrinsic_layout = flex::ctor({
            .justify_content = flex_justify::center,
            .align_items = flex_align::center,
        });
        auto intrinsic = ui::mock::ctor();
        intrinsic->limits({ 7, 3 }, { 7, 3 });
        intrinsic_layout->attach(intrinsic);
        intrinsic_layout->base::extend({ {}, { 15, 7 } });
        if (intrinsic_layout->get_item_area(intrinsic) != rect{ { 4, 2 }, { 7, 3 } }) return faux;

        auto layout = flex::ctor();
        auto a = layout->attach(ui::mock::ctor(), { .basis = 5, .order = 2 });
        auto b = layout->attach(ui::mock::ctor(), { .basis = 5, .order = -1 });
        auto c = layout->attach(ui::mock::ctor(), { .basis = 5, .order = 2 });
        layout->base::extend({ {}, { 15, 2 } });
        if (layout->get_item_area(b).coor.x != 0
         || layout->get_item_area(a).coor.x != 5
         || layout->get_item_area(c).coor.x != 10) return faux;

        a->base::hidden = true;
        layout->base::resize({ 15, 2 });
        if (layout->get_item_area(a) != rect{}
         || layout->get_item_area(b).coor.x != 0
         || layout->get_item_area(c).coor.x != 5) return faux;

        layout->remove(b);
        layout->base::resize({ 15, 2 });
        if (layout->get_item_count() != 2 || layout->get_item_area(c).coor.x != 0) return faux;

        auto retained = flex::ctor();
        auto widget = ui::mock::ctor();
        auto token = std::make_shared<int>(42);
        auto weak = std::weak_ptr<int>{ token };
        retained->attach(component{ widget, [token]{}, {} }, { .basis = 1 });
        token.reset();
        if (weak.expired()) return faux;
        retained->remove(widget);
        return weak.expired();
    }

    auto test_border_geometry_and_paint() -> bool
    {
        constexpr auto border_color = 0xFF123456;
        auto layout = flex::ctor({
            .column_gap = 2,
            .column_border_width = 2,
            .row_border_height = 1,
            .column_padding_width = 1,
            .row_padding_height = 2,
            .border_color = border_color,
            .border = true,
        });
        auto a = layout->attach(ui::mock::ctor(), { .grow = 1, .basis = 0 });
        auto b = layout->attach(ui::mock::ctor(), { .grow = 1, .basis = 0 });
        layout->base::extend({ {}, { 24, 12 } });

        auto canvas = ui::face{};
        canvas.size({ 24, 12 });
        layout->render(canvas);

        return layout->get_item_area(a) == rect{ { 3, 3 }, { 8, 6 } }
            && layout->get_item_area(b) == rect{ { 13, 3 }, { 8, 6 } }
            && canvas[{ 0, 0 }].bgc() == argb{ border_color }
            && canvas[{ 1, 5 }].bgc() == argb{ border_color }
            && canvas[{ 23, 11 }].bgc() == argb{ border_color }
            && canvas[{ 2, 1 }].bgc() != argb{ border_color }
            && canvas[{ 3, 2 }].bgc() != argb{ border_color };
    }

    auto test_padding_without_border() -> bool
    {
        constexpr auto border_color = 0xFF123456;
        auto layout = flex::ctor({
            .direction = flex_direction::column,
            .column_padding_width = 2,
            .row_padding_height = 1,
            .border_color = border_color,
        });
        auto child = layout->attach(ui::mock::ctor(), { .grow = 1, .basis = 0 });
        layout->base::extend({ {}, { 10, 5 } });

        auto canvas = ui::face{};
        canvas.size({ 10, 5 });
        layout->render(canvas);

        return layout->get_item_area(child) == rect{ { 2, 1 }, { 6, 3 } }
            && canvas[{ 0, 0 }].bgc() != argb{ border_color };
    }

    auto test_item_padding_uses_outer_slot() -> bool
    {
        auto layout = flex::ctor({ .column_gap = 2 });
        auto a = layout->attach(ui::mock::ctor(), {
            .basis = 9,
            .padding = { 1, 2, 1, 2 },
        });
        auto b = layout->attach(ui::mock::ctor(), {
            .basis = 9,
            .padding = { -3, 20, -4, 20 },
        });
        layout->base::extend({ {}, { 20, 8 } });

        // Basis and gap still allocate outer slots [0, 9) and [11, 20).
        // Padding only changes the final child rectangles inside those slots.
        return layout->get_item_area(a) == rect{ { 1, 1 }, { 6, 5 } }
            && layout->get_item_area(b) == rect{ { 11, 0 }, { 0, 0 } };
    }

    auto test_column_separator_is_fixed_spacing() -> bool
    {
        auto layout = flex::ctor({
            .direction = flex_direction::column,
            .column_padding_width = 2,
            .row_padding_height = 1,
        });
        auto before = layout->attach(ui::mock::ctor(), {
            .shrink = 0,
            .basis = 1,
            .minimum = 1,
            .maximum = 1,
        });
        auto separator = layout->attach_separator(1);
        auto after = layout->attach(ui::mock::ctor(), {
            .shrink = 0,
            .basis = 1,
            .minimum = 1,
            .maximum = 1,
        });
        layout->base::extend({ {}, { 12, 5 } });

        return layout->get_item_area(before) == rect{ { 2, 1 }, { 8, 1 } }
            && layout->get_item_area(separator) == rect{ { 2, 2 }, { 8, 1 } }
            && layout->get_item_area(after) == rect{ { 2, 3 }, { 8, 1 } };
    }
}

int main()
{
    // Keep each scenario independent so a failure reports the exact contract.
    auto row = test_row_grow_shrink_and_constraints();
    auto column = test_column_flow();
    auto wrapping = test_wrapping_and_gaps();
    auto justification = test_justification_and_item_alignment();
    auto content = test_content_alignment();
    auto lifecycle = test_intrinsic_basis_order_hidden_remove_and_retention();
    auto border = test_border_geometry_and_paint();
    auto padding = test_padding_without_border();
    auto item_padding = test_item_padding_uses_outer_slot();
    auto separator = test_column_separator_is_fixed_spacing();
    auto ok = row && column && wrapping && justification && content && lifecycle
           && border && padding && item_padding && separator;
    if (!ok) std::fprintf(stderr,
        "row=%d column=%d wrapping=%d justification=%d content=%d lifecycle=%d border=%d padding=%d item_padding=%d separator=%d\n",
        row, column, wrapping, justification, content, lifecycle, border, padding, item_padding, separator);
    if (!ok) std::fprintf(stderr, "parvion flex tests failed\n");
    return ok ? 0 : 1;
}
