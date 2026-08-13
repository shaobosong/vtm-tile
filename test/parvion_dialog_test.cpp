// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#include "netxs/desktopio/controls.hpp"

namespace netxs::app { using namespace netxs::ui; }

#include "netxs/apps/parvion/components/dialog.hpp"
#include "netxs/apps/parvion/components/flex.hpp"
#include "netxs/apps/parvion/components/label.hpp"

#include <cstdio>

using namespace netxs;
using namespace netxs::app::parvion;

namespace
{
    auto test_fixed_and_proportional_geometry() -> bool
    {
        auto fixed = dialog_cfg{
            .position = dialog_anchor::center,
            .size = {
                .width = dialog_length::cells(40),
                .height = dialog_length::cells(20),
            },
        };
        auto proportional = dialog_cfg{
            .position = dialog_anchor::bottom_right,
            .size = {
                .width = dialog_length::ratio(0.5),
                .height = dialog_length::ratio(0.5),
            },
            .minimum = { 10, 8 },
            .maximum = { 60, 30 },
        };
        return resolve_dialog_rect(fixed, { 100, 50 }) == rect{ { 30, 15 }, { 40, 20 } }
            && resolve_dialog_rect(proportional, { 100, 40 }) == rect{ { 50, 20 }, { 50, 20 } };
    }

    auto test_anchor_offset_and_host_clamp() -> bool
    {
        auto cfg = dialog_cfg{
            .position = dialog_anchor::top_right,
            .offset = { 10, -5 },
            .size = {
                .width = dialog_length::cells(30),
                .height = dialog_length::cells(10),
            },
            .minimum = { 52, 18 },
            .maximum = { 104, 42 },
        };
        // The minimum itself is clamped to the 40x12 host; the offset cannot
        // move any edge outside that host.
        return resolve_dialog_rect(cfg, { 40, 12 }) == rect{ {}, { 40, 12 } };
    }

    auto test_single_row_button_bar_and_lifecycle() -> bool
    {
        auto activated = 0;
        auto deactivated = 0;
        auto button_widget = ui::sptr{};
        auto make_child = [&]
        {
            return component{
                ui::mock::ctor(),
                [&]{ ++activated; },
                [&]{ ++deactivated; },
            };
        };
        auto popup = make_dialog({
            .title = make_child(),
            .content = make_child(),
            .buttons = [&]
            {
                auto child = make_child();
                button_widget = child.widget;
                return child;
            }(),
            .size = {
                .width = dialog_length::cells(30),
                .height = dialog_length::cells(12),
            },
        });
        popup.widget->base::extend({ {}, { 80, 30 } });
        auto dialog_widget = std::dynamic_pointer_cast<dialog>(popup.widget);
        if (!dialog_widget) return faux;
        auto rows = dialog_widget->get_card()->get_row_sizes();
        popup.on_activate();
        popup.on_deactivate();
        return rows == std::vector<si32>{ 1, 10, 1 }
            && dialog_widget->get_card()->get_cell_area(button_widget)
                == rect{ { 1, 11 }, { 28, 1 } }
            && dialog_widget->get_card_area() == rect{ { 25, 9 }, { 30, 12 } }
            && activated == 3
            && deactivated == 3;
    }

    auto test_content_fitted_height_tracks_wrapping() -> bool
    {
        auto content = flex::ctor({
            .direction = flex_direction::column,
            .column_padding_width = 2,
            .row_padding_height = 1,
        });
        auto prompt = make_label({
            .value = []{ return text{ "alpha beta gamma delta" }; },
            .wrap = true,
        });
        auto prompt_widget = prompt.widget;
        content->attach(std::move(prompt), { .shrink = 0 });
        auto separator = content->attach_separator(1);
        auto input = ui::mock::ctor()->limits({ 0, 1 }, { -1, 1 });
        content->attach(input, { .shrink = 0, .basis = 1, .minimum = 1, .maximum = 1 });

        auto popup = make_dialog({
            .title = { ui::mock::ctor() },
            .content = { content },
            .buttons = { ui::mock::ctor() },
            .position = dialog_anchor::center,
            .size = {
                .width = dialog_length::cells(30),
                .height = dialog_length::cells(30), // Ignored in fitted mode.
            },
            .fit_content_height = true,
        });
        auto dialog_widget = std::dynamic_pointer_cast<dialog>(popup.widget);
        if (!dialog_widget) return faux;

        popup.widget->base::extend({ {}, { 80, 40 } });
        auto wide_area = dialog_widget->get_card_area();
        auto wide_rows = dialog_widget->get_card()->get_row_sizes();
        auto wide_prompt = content->get_item_area(prompt_widget);
        auto wide_separator = content->get_item_area(separator);
        auto wide_input = content->get_item_area(input);

        // Inner width 26 fits the prompt on one row.  Content is top padding,
        // prompt, separator, input, bottom padding: five rows total.
        if (wide_area != rect{ { 25, 16 }, { 30, 7 } }
         || wide_rows != std::vector<si32>{ 1, 5, 1 }
         || wide_prompt != rect{ { 2, 1 }, { 26, 1 } }
         || wide_separator != rect{ { 2, 2 }, { 26, 1 } }
         || wide_input != rect{ { 2, 3 }, { 26, 1 } }) return faux;

        popup.widget->base::extend({ {}, { 14, 40 } });
        auto narrow_area = dialog_widget->get_card_area();
        auto narrow_rows = dialog_widget->get_card()->get_row_sizes();
        auto narrow_prompt = content->get_item_area(prompt_widget);

        // Inner width 10 wraps to three rows.  The shell grows by two rows and
        // re-centers while preserving the exact padding/separator sequence.
        return narrow_area == rect{ { 0, 15 }, { 14, 9 } }
            && narrow_rows == std::vector<si32>{ 1, 7, 1 }
            && narrow_prompt == rect{ { 2, 1 }, { 10, 3 } };
    }

    auto test_content_fitted_height_honors_bounds_and_host() -> bool
    {
        auto content = ui::mock::ctor()->limits({ 1, 10 }, { -1, 10 });
        auto popup = make_dialog({
            .title = { ui::mock::ctor() },
            .content = { content },
            .buttons = { ui::mock::ctor() },
            .size = {
                .width = dialog_length::cells(20),
                .height = dialog_length::cells(4),
            },
            .maximum = { -1, 8 },
            .fit_content_height = true,
        });
        auto dialog_widget = std::dynamic_pointer_cast<dialog>(popup.widget);
        if (!dialog_widget) return faux;

        popup.widget->base::extend({ {}, { 40, 20 } });
        if (dialog_widget->get_card_area() != rect{ { 10, 6 }, { 20, 8 } }) return faux;

        popup.widget->base::extend({ {}, { 16, 6 } });
        return dialog_widget->get_card_area() == rect{ {}, { 16, 6 } };
    }

    auto test_dismiss_is_idempotent() -> bool
    {
        auto cancelled = 0;
        auto popup = make_dialog({
            .title = { ui::mock::ctor() },
            .content = { ui::mock::ctor() },
            .buttons = { ui::mock::ctor() },
            .on_cancel = [&]{ ++cancelled; },
        });
        auto root = ui::cake::ctor();
        root->attach(popup.widget);
        auto dialog_widget = std::dynamic_pointer_cast<dialog>(popup.widget);
        if (!dialog_widget) return faux;
        dialog_widget->dismiss();
        dialog_widget->dismiss();
        return cancelled == 1;
    }

    auto test_dismiss_can_be_temporarily_vetoed() -> bool
    {
        auto allowed = faux;
        auto cancelled = 0;
        auto popup = make_dialog({
            .title = { ui::mock::ctor() },
            .content = { ui::mock::ctor() },
            .buttons = { ui::mock::ctor() },
            .can_dismiss = [&]{ return allowed; },
            .on_cancel = [&]{ ++cancelled; },
        });
        auto root = ui::cake::ctor();
        root->attach(popup.widget);
        auto dialog_widget = std::dynamic_pointer_cast<dialog>(popup.widget);
        if (!dialog_widget) return faux;

        dialog_widget->dismiss();
        if (cancelled) return faux;
        allowed = true;
        dialog_widget->dismiss();
        dialog_widget->dismiss();
        return cancelled == 1;
    }

    auto test_extended_and_modified_mouse_events_do_not_escape_dialog() -> bool
    {
        auto cancelled = 0;
        auto popup = make_dialog({
            .title = { ui::mock::ctor() },
            .content = { ui::mock::ctor() },
            .buttons = { ui::mock::ctor() },
            .on_cancel = [&]{ ++cancelled; },
        });
        auto root = ui::cake::ctor();
        auto double_clicks = 0;
        auto multi_clicks = 0;
        auto left_clicks = 0;
        auto wheel_events = 0;
        root->on(tier::mouserelease, input::key::MouseDoubleClick,
            [&](input::hids&){ ++double_clicks; });
        root->on(tier::mouserelease, input::key::MouseMultiClick,
            [&](input::hids&){ ++multi_clicks; });
        root->on(tier::mouserelease, input::key::LeftClick,
            [&](input::hids&){ ++left_clicks; });
        root->on(tier::mouserelease, input::key::MouseWheel,
            [&](input::hids&){ ++wheel_events; });
        root->attach(popup.widget);

        auto dialog_widget = std::dynamic_pointer_cast<dialog>(popup.widget);
        if (!dialog_widget) return faux;
        auto& gear = *ui::tui_domain()._null_gear_sptr;
        auto send = [&](hint cause, si32 modifiers = 0)
        {
            gear.nodbl = faux;
            gear.replay(*dialog_widget->get_card(),
                        cause,
                        fp2d{},
                        fp2d{},
                        fp2d{},
                        0,
                        input::hids::buttons::left,
                        faux,
                        modifiers,
                        0.0f,
                        0,
                        faux);
            return !gear.alive && gear.nodbl;
        };
        auto pass = [&](hint cause)
        {
            gear.nodbl = faux;
            gear.replay(*dialog_widget->get_card(),
                        cause,
                        fp2d{},
                        fp2d{},
                        fp2d{},
                        0,
                        input::hids::buttons::left,
                        faux,
                        0,
                        0.0f,
                        0,
                        faux);
            return gear.alive && !gear.nodbl;
        };

        return send(input::key::LeftDoubleClick)
            && send(input::key::LeftMultiClick)
            && send(input::key::LeftClick, input::hids::LCtrl)
            && send(input::key::LeftClick, input::hids::LAlt)
            && send(input::key::LeftClick, input::hids::LShift)
            && send(input::key::MouseWheel, input::hids::RCtrl)
            && send(input::key::MouseWheel, input::hids::RAlt)
            && send(input::key::MouseWheel, input::hids::RShift)
            && pass(input::key::LeftClick)
            && pass(input::key::MouseWheel)
            && double_clicks == 0
            && multi_clicks == 0
            && left_clicks == 1
            && wheel_events == 1
            && cancelled == 0
            && dialog_widget->base::parent();
    }

    auto test_card_clears_attributes_and_backdrop_preserves_content() -> bool
    {
        auto popup = make_dialog({
            .title = { ui::mock::ctor() },
            .content = { ui::mock::ctor() },
            .buttons = { ui::mock::ctor() },
            .size = {
                .width = dialog_length::cells(20),
                .height = dialog_length::cells(8),
            },
        });
        popup.widget->base::extend({ {}, { 40, 20 } });
        auto dialog_widget = std::dynamic_pointer_cast<dialog>(popup.widget);
        if (!dialog_widget) return faux;

        auto canvas = ui::face{};
        canvas.size({ 40, 20 });
        auto original_fg = argb{ 0xff80a0c0 };
        auto original_bg = argb{ 0xff203040 };
        auto original_underline = argb{ 0xffd7875f };
        auto expected_underline = argb{ argb::vt256[original_underline.to_256cube()] };
        expected_underline.faint();
        auto expected_underline_index = expected_underline.to_256cube();
        canvas.fill(rect{ {}, canvas.size() }, [&](cell& c)
        {
            c.bgc(original_bg)
             .fgc(original_fg)
             .txt("Z")
             .bld(true)
             .itc(true)
             .und(unln::wavy)
             .unc(original_underline)
             .inv(true)
             .fnt(faux)
             .ovr(true)
             .stk(true)
             .blk(true)
             .hid(true)
             .cur(text_cursor::block)
             .img(5678)
             .link(1234);
        });
        auto inherited_underline = twod{ 1, 0 };
        canvas[inherited_underline].unc(0);
        auto ordinary = twod{ 2, 0 };
        canvas[ordinary].inv(faux);
        popup.widget->render(canvas);

        auto outside = twod{ 0, 0 };
        auto card_area = dialog_widget->get_card_area();
        auto title_left = card_area.coor;
        auto title_right = title_left + twod{ card_area.size.x - 1, 0 };
        auto content = title_left + twod{ 0, 1 };
        auto buttons = twod{
            title_left.x,
            title_left.y + card_area.size.y - 1,
        };
        auto card_attribute_free = [&](twod point)
        {
            auto const& c = canvas[point];
            return !c.bld()
                && !c.fnt()
                && !c.itc()
                && c.und() == unln::none
                && c.unc() == 0
                && !c.inv()
                && !c.ovr()
                && !c.stk()
                && !c.blk()
                && !c.hid()
                && c.cur() == text_cursor::none
                && c.img() == 0;
        };
        return canvas[outside].txt() == "Z"
            && canvas[outside].fgc() != original_fg
            && canvas[outside].bgc() != original_bg
            && canvas[outside].bld()
            && !canvas[outside].fnt()
            && canvas[outside].itc()
            && canvas[outside].und() == unln::wavy
            && canvas[outside].unc() == expected_underline_index
            && canvas[inherited_underline].unc() == 0
            && canvas[outside].inv()
            && !canvas[ordinary].inv()
            && !canvas[ordinary].fnt()
            && canvas[outside].ovr()
            && canvas[outside].stk()
            && canvas[outside].blk()
            && canvas[outside].hid()
            && canvas[outside].cur() == text_cursor::none
            && canvas[outside].img() == 5678
            && canvas[outside].link() == popup.widget->bell::id
            && canvas[title_left].txt() == " "
            && canvas[title_left].bgc() == argb{ theme::header }
            && canvas[title_right].bgc() == argb{ theme::header }
            && canvas[content].bgc() == argb{ theme::bg }
            && canvas[buttons].bgc() == argb{ theme::surface }
            && card_attribute_free(title_left)
            && card_attribute_free(title_right)
            && card_attribute_free(content)
            && card_attribute_free(buttons);
    }
}

int main()
{
    auto ok = true;
    auto check = [&](auto test, char const* name)
    {
        if (!test())
        {
            std::fprintf(stderr, "parvion dialog test failed: %s\n", name);
            ok = false;
        }
    };
    check(test_fixed_and_proportional_geometry, "fixed and proportional geometry");
    check(test_anchor_offset_and_host_clamp, "anchor offset and host clamp");
    check(test_single_row_button_bar_and_lifecycle, "single-row button bar and lifecycle");
    check(test_content_fitted_height_tracks_wrapping, "content-fitted height tracks wrapping");
    check(test_content_fitted_height_honors_bounds_and_host, "content-fitted height honors bounds and host");
    check(test_dismiss_is_idempotent, "idempotent dismissal");
    check(test_dismiss_can_be_temporarily_vetoed, "dismissal predicate");
    check(test_extended_and_modified_mouse_events_do_not_escape_dialog,
          "extended and modified mouse-event containment");
    check(test_card_clears_attributes_and_backdrop_preserves_content,
          "card clears attributes and backdrop preserves content");
    return ok ? 0 : 1;
}
