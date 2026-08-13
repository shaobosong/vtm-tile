// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#include "netxs/desktopio/controls.hpp"

namespace netxs::app { using namespace netxs::ui; }

#include "netxs/apps/parvion/components/flex.hpp"
#include "netxs/apps/parvion/components/scrollview.hpp"

#include <cstdio>

using namespace netxs;
using namespace netxs::app::parvion;

namespace
{
    auto test_gear() -> input::hids&
    {
        return *ui::tui_domain()._null_gear_sptr;
    }

    auto send_wheel(scrollview& view, si32 delta, si32 modifiers = 0,
                    bool horizontal = faux) -> bool
    {
        auto& gear = test_gear();
        gear.whlsi = delta;
        gear.hzwhl = horizontal;
        gear.ctlstat = modifiers;
        auto handled = view.wheel(gear);
        gear.whlsi = 0;
        gear.hzwhl = faux;
        gear.ctlstat = 0;
        return handled;
    }

    auto send_key(scrollview& view, si32 key, si32 modifiers = 0) -> bool
    {
        auto& gear = test_gear();
        gear.keybd::handled = faux;
        gear.payload = input::keybd::type::keypress;
        gear.keystat = input::key::pressed;
        gear.keycode = key;
        gear.ctlstat = modifiers;
        auto handled = view.navigate(gear);
        gear.keystat = input::key::released;
        gear.keycode = input::key::undef;
        gear.ctlstat = 0;
        return handled;
    }

    auto fixed_content(twod size) -> component
    {
        auto child = ui::mock::ctor()->limits(size, size);
        child->invoke([](auto& boss)
        {
            boss.LISTEN(tier::release, netxs::ui::e2::render::any, canvas)
            {
                canvas.fill(rect{ {}, boss.base::size() }, [](cell& c)
                {
                    c.bgc(theme::bg).fgc(theme::text_fg).txt("x");
                });
            };
        });
        return { child };
    }

    auto test_vertical_default() -> bool
    {
        auto view = scrollview::ctor({ .content = fixed_content({ 8, 20 }) });
        view->base::extend({ {}, { 10, 5 } });
        auto bar = view->vertical_scrollbar();
        auto reported = view->base::signal(tier::request,
                                           netxs::app::e2::form::prop::viewport);
        auto ok = view->has_vertical_scrollbar()
            && !view->has_horizontal_scrollbar()
            && view->get_viewport().size == twod{ 9, 5 }
            && reported == view->get_viewport()
            && view->get_content_size() == twod{ 8, 20 }
            && bar.ok && bar.track == rect{ { 9, 0 }, { 1, 5 } };
        if (!ok) std::fprintf(stderr, "vertical: bars=%d/%d viewport=%d,%d content=%d,%d track=%d,%d %d,%d\n",
            view->has_vertical_scrollbar(), view->has_horizontal_scrollbar(),
            view->get_viewport().size.x, view->get_viewport().size.y,
            view->get_content_size().x, view->get_content_size().y,
            bar.track.coor.x, bar.track.coor.y, bar.track.size.x, bar.track.size.y);
        return ok;
    }

    auto test_both_axes_clamp_and_paint() -> bool
    {
        auto view = scrollview::ctor({
            .content = fixed_content({ 20, 20 }),
            .scroll_axes = axes::all,
        });
        view->base::extend({ {}, { 10, 5 } });
        view->set_offset({ 999, 999 });
        auto canvas = ui::face{};
        canvas.size({ 10, 5 });
        view->render(canvas);
        return view->has_vertical_scrollbar()
            && view->has_horizontal_scrollbar()
            && view->get_viewport().size == twod{ 9, 4 }
            && view->get_offset() == twod{ 11, 16 }
            && canvas[{ 9, 0 }].txt() == "\xe2\x96\x90"
            && canvas[{ 0, 4 }].txt() == "\xe2\x96\x82";
    }

    auto test_primary_bar_can_create_secondary_overflow() -> bool
    {
        auto vertical = scrollview::ctor({
            .content = fixed_content({ 10, 20 }),
            .scroll_axes = axes::all,
        });
        vertical->base::extend({ {}, { 10, 5 } });
        if (!vertical->has_vertical_scrollbar()
         || !vertical->has_horizontal_scrollbar()
         || vertical->get_viewport().size != twod{ 9, 4 }) return faux;

        auto horizontal = scrollview::ctor({
            .content = fixed_content({ 20, 5 }),
            .primary_axis = axis::X,
            .scroll_axes = axes::all,
        });
        horizontal->base::extend({ {}, { 10, 5 } });
        return horizontal->has_horizontal_scrollbar()
            && horizontal->has_vertical_scrollbar()
            && horizontal->get_viewport().size == twod{ 9, 4 };
    }

    auto test_vertical_content_is_clipped_to_viewport() -> bool
    {
        auto view = scrollview::ctor({ .content = fixed_content({ 8, 20 }) });
        view->base::extend({ { 2, 2 }, { 10, 5 } });
        auto canvas = ui::face{};
        canvas.size({ 14, 10 });
        canvas.fill(rect{ {}, canvas.size() }, [](cell& c){ c.txt("."); });
        view->render(canvas);

        // The child covers the viewport, but its rows below the five-cell
        // scrollview boundary must not reach the larger parent backing face.
        return canvas[{ 2, 2 }].txt() == "x"
            && canvas[{ 2, 6 }].txt() == "x"
            && canvas[{ 2, 7 }].txt() == "."
            && canvas[{ 1, 2 }].txt() == "."
            && canvas[{ 12, 2 }].txt() == ".";
    }

    auto test_horizontal_content_is_clipped_to_viewport() -> bool
    {
        auto view = scrollview::ctor({
            .content = fixed_content({ 20, 3 }),
            .primary_axis = axis::X,
            .scroll_axes = axes::X_only,
        });
        view->base::extend({ { 3, 1 }, { 10, 5 } });
        auto canvas = ui::face{};
        canvas.size({ 16, 8 });
        canvas.fill(rect{ {}, canvas.size() }, [](cell& c){ c.txt("."); });
        view->render(canvas);

        // The horizontal scrollbar owns the bottom row; content must be
        // physically bounded at x=10 instead of leaking into the parent.
        return view->has_horizontal_scrollbar()
            && !view->has_vertical_scrollbar()
            && canvas[{ 12, 1 }].txt() == "x"
            && canvas[{ 13, 1 }].txt() == "."
            && canvas[{ 15, 3 }].txt() == "."
            && canvas[{ 3, 5 }].txt() == "\xe2\x96\x82";
    }

    auto test_track_paging_and_drag() -> bool
    {
        auto view = scrollview::ctor({ .content = fixed_content({ 8, 40 }) });
        view->base::extend({ {}, { 10, 10 } });
        auto bar = view->vertical_scrollbar();
        auto below = twod{ bar.track.coor.x, bar.track.coor.y + bar.track.size.y - 1 };
        if (!view->page_at(below) || view->get_offset().y != 10) return faux;
        view->start_drag(below);
        view->pull_drag(below);
        view->stop_drag();
        return view->get_offset().y == 30;
    }

    auto test_horizontal_track_paging_and_drag() -> bool
    {
        auto view = scrollview::ctor({
            .content = fixed_content({ 40, 3 }),
            .primary_axis = axis::X,
            .scroll_axes = axes::X_only,
        });
        view->base::extend({ {}, { 10, 5 } });
        auto bar = view->horizontal_scrollbar();
        auto after = twod{ bar.track.coor.x + bar.track.size.x - 1, bar.track.coor.y };
        if (!view->page_at(after) || view->get_offset().x != 10) return faux;
        view->start_drag(after);
        view->pull_drag(after);
        view->stop_drag();
        return view->get_offset().x == 30;
    }

    auto test_primary_axis_wheel_routing() -> bool
    {
        auto horizontal = scrollview::ctor({
            .content = fixed_content({ 30, 30 }),
            .primary_axis = axis::X,
            .scroll_axes = axes::all,
        });
        horizontal->base::extend({ {}, { 10, 6 } });
        if (!send_wheel(*horizontal, -1) || horizontal->get_offset() != twod{ 4, 0 }) return faux;
        if (!send_wheel(*horizontal, -1, input::hids::LShift)
         || horizontal->get_offset() != twod{ 4, 1 }) return faux;
        if (!send_wheel(*horizontal, -1, input::hids::LAlt)
         || horizontal->get_offset() != twod{ 4, 2 }) return faux;
        if (!send_wheel(*horizontal, -1, 0, true)
         || horizontal->get_offset() != twod{ 8, 2 }) return faux;
        auto vertical_bar = horizontal->vertical_scrollbar();
        horizontal->update_hover(vertical_bar.track.coor);
        if (!send_wheel(*horizontal, -1)
         || horizontal->get_offset() != twod{ 8, 3 }) return faux;
        horizontal->leave();
        if (send_wheel(*horizontal, -1, input::hids::LCtrl)
         || horizontal->get_offset() != twod{ 8, 3 }) return faux;

        auto vertical = scrollview::ctor({
            .content = fixed_content({ 30, 30 }),
            .scroll_axes = axes::all,
        });
        vertical->base::extend({ {}, { 10, 6 } });
        if (!send_wheel(*vertical, -1)
         || vertical->get_offset() != twod{ 0, 1 }) return faux;
        if (!send_wheel(*vertical, -1, input::hids::LShift)
         || vertical->get_offset() != twod{ 4, 1 }) return faux;
        if (!send_wheel(*vertical, -1, 0, true)
         || vertical->get_offset() != twod{ 8, 1 }) return faux;

        auto horizontal_only = scrollview::ctor({
            .content = fixed_content({ 30, 3 }),
            .primary_axis = axis::X,
            .scroll_axes = axes::X_only,
        });
        horizontal_only->base::extend({ {}, { 10, 5 } });
        return send_wheel(*horizontal_only, -1, input::hids::LShift)
            && horizontal_only->get_offset() == twod{ 4, 0 };
    }

    auto test_primary_axis_keyboard_routing() -> bool
    {
        auto view = scrollview::ctor({
            .content = fixed_content({ 30, 30 }),
            .primary_axis = axis::X,
            .scroll_axes = axes::all,
        });
        view->base::extend({ {}, { 10, 6 } });
        if (!send_key(*view, input::key::KeyRightArrow)
         || view->get_offset() != twod{ 1, 0 }) return faux;
        if (send_key(*view, input::key::KeyDownArrow)
         || view->get_offset() != twod{ 1, 0 }) return faux;
        if (!send_key(*view, input::key::KeyPageDown)
         || view->get_offset() != twod{ 10, 0 }) return faux;
        if (!send_key(*view, input::key::KeyEnd)
         || view->get_offset() != twod{ 21, 0 }) return faux;
        if (!send_key(*view, input::key::KeyHome)
         || view->get_offset() != twod{ 0, 0 }) return faux;
        if (!send_key(*view, input::key::KeyDownArrow, input::hids::LShift)
         || view->get_offset() != twod{ 0, 1 }) return faux;
        if (send_key(*view, input::key::KeyRightArrow, input::hids::LShift)
         || view->get_offset() != twod{ 0, 1 }) return faux;
        if (!send_key(*view, input::key::KeyPageDown, input::hids::LShift)
         || view->get_offset() != twod{ 0, 6 }) return faux;
        if (!send_key(*view, input::key::KeyEnd, input::hids::LShift)
         || view->get_offset() != twod{ 0, 25 }) return faux;
        if (!send_key(*view, input::key::KeyHome, input::hids::LShift)
         || view->get_offset() != twod{ 0, 0 }) return faux;

        auto horizontal_only = scrollview::ctor({
            .content = fixed_content({ 30, 3 }),
            .primary_axis = axis::X,
            .scroll_axes = axes::X_only,
        });
        horizontal_only->base::extend({ {}, { 10, 5 } });
        return send_key(*horizontal_only, input::key::KeyRightArrow, input::hids::LShift)
            && horizontal_only->get_offset() == twod{ 1, 0 };
    }

    auto test_nested_flex_and_lifecycle() -> bool
    {
        auto activated = 0;
        auto deactivated = 0;
        auto layout = flex::ctor({ .direction = flex_direction::column });
        layout->attach(ui::mock::ctor(), { .basis = 4, .minimum = 4 });
        layout->attach(ui::mock::ctor(), { .basis = 5, .minimum = 5 });
        auto nested = component{
            layout,
            [&]{ ++activated; },
            [&]{ ++deactivated; },
        };
        auto view = make_scrollview({ .content = nested });
        view.widget->base::extend({ {}, { 12, 6 } });
        auto widget = std::dynamic_pointer_cast<scrollview>(view.widget);
        view.on_activate();
        view.on_deactivate();
        return widget
            && widget->has_vertical_scrollbar()
            && widget->get_content_size().y == 9
            && activated == 1
            && deactivated == 1;
    }
}

int main()
{
    auto check = [](view name, bool result)
    {
        if (!result) std::fprintf(stderr, "failed: %.*s\n", (int)name.size(), name.data());
        return result;
    };
    auto ok = true;
    ok &= check("vertical_default", test_vertical_default());
    ok &= check("both_axes_clamp_and_paint", test_both_axes_clamp_and_paint());
    ok &= check("primary_bar_creates_secondary_overflow", test_primary_bar_can_create_secondary_overflow());
    ok &= check("vertical_content_clipping", test_vertical_content_is_clipped_to_viewport());
    ok &= check("horizontal_content_clipping", test_horizontal_content_is_clipped_to_viewport());
    ok &= check("track_paging_and_drag", test_track_paging_and_drag());
    ok &= check("horizontal_track_paging_and_drag", test_horizontal_track_paging_and_drag());
    ok &= check("primary_axis_wheel_routing", test_primary_axis_wheel_routing());
    ok &= check("primary_axis_keyboard_routing", test_primary_axis_keyboard_routing());
    ok &= check("nested_flex_and_lifecycle", test_nested_flex_and_lifecycle());
    if (!ok) std::fprintf(stderr, "parvion scrollview tests failed\n");
    return ok ? 0 : 1;
}
