// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#include "netxs/desktopio/controls.hpp"

namespace netxs::app { using namespace netxs::ui; }

#include "netxs/apps/parvion/components/flex.hpp"
#include "netxs/apps/parvion/components/checkbox.hpp"
#include "netxs/apps/parvion/components/dropdown.hpp"
#include "netxs/apps/parvion/components/groupbox.hpp"
#include "netxs/apps/parvion/components/label.hpp"

#include <cstdio>

using namespace netxs;
using namespace netxs::app::parvion;

namespace
{
    auto test_label_roles_measurement_and_wrap() -> bool
    {
        auto value = text{ "alpha beta gamma" };
        auto hint = make_label({
            .value = [&]{ return value; },
            .role = label_role::hint,
            .overflow = label_overflow::wrap,
        });
        auto widget = std::dynamic_pointer_cast<label>(hint.widget);
        if (!widget) return faux;
        widget->base::extend({ {}, { 6, 0 } });
        if (widget->base::size() != twod{ 6, 3 }
         || widget->get_lines() != std::vector<text>{ "alpha", "beta", "gamma" }
         || widget->get_foreground() != theme::subtext) return faux;

        auto unicode = make_label({
            .value = []{ return text{ "界界" }; },
        });
        auto natural = rect{};
        unicode.widget->base::recalc(natural);
        if (natural.size != twod{ 4, 1 }) return faux;

        auto canvas = ui::face{};
        canvas.size({ 6, 3 });
        hint.widget->render(canvas);
        return canvas[{ 0, 0 }].fgc() == argb{ theme::subtext }
            && canvas[{ 0, 0 }].txt() == "a"
            && canvas[{ 0, 1 }].txt() == "b";
    }

    auto test_label_overflow_ellipsis_and_tooltip() -> bool
    {
        auto value = text{ "alpha\n界界界" };
        auto clipped = make_label({ .value = [&]{ return value; } });
        auto clipped_widget = std::dynamic_pointer_cast<label>(clipped.widget);
        if (!clipped_widget) return faux;
        clipped.widget->base::extend({ {}, { 4, 2 } });
        if (clipped_widget->get_lines() != std::vector<text>{ "alpha", "界界界" }
         || !clipped_widget->get_tooltip().empty()) return faux;
        auto clipped_canvas = ui::face{};
        clipped_canvas.size({ 4, 2 });
        clipped.widget->render(clipped_canvas);
        if (clipped_canvas[{ 0, 0 }].link()) return faux;

        auto abbreviated = make_label({
            .value = [&]{ return value; },
            .overflow = label_overflow::ellipsis,
        });
        auto abbreviated_widget = std::dynamic_pointer_cast<label>(abbreviated.widget);
        if (!abbreviated_widget) return faux;
        abbreviated.widget->base::extend({ {}, { 4, 2 } });
        if (abbreviated_widget->get_lines() != std::vector<text>{ "alp…", "界…" }
         || abbreviated_widget->get_tooltip() != value) return faux;

        auto canvas = ui::face{};
        canvas.size({ 4, 2 });
        abbreviated.widget->render(canvas);
        if (canvas[{ 3, 0 }].txt() != "…"
         || canvas[{ 2, 1 }].txt() != "…"
         || canvas[{ 0, 0 }].link() != abbreviated.widget->id) return faux;

        value = "fits";
        abbreviated.widget->base::extend({ {}, { 4, 1 } });
        if (!abbreviated_widget->get_tooltip().empty()
         || abbreviated_widget->get_lines() != std::vector<text>{ "fits" }) return faux;

        auto one_cell = make_label({
            .value = []{ return text{ "wide" }; },
            .overflow = label_overflow::ellipsis,
        });
        auto one_cell_widget = std::dynamic_pointer_cast<label>(one_cell.widget);
        one_cell.widget->base::extend({ {}, { 1, 1 } });
        return one_cell_widget
            && one_cell_widget->get_lines() == std::vector<text>{ "…" }
            && one_cell_widget->get_tooltip() == "wide";
    }

    auto test_flex_shrinks_ellipsis_label_to_assigned_width() -> bool
    {
        auto layout = flex::ctor();
        auto abbreviated = make_label({
            .value = []{ return text{ "long label" }; },
            .overflow = label_overflow::ellipsis,
        });
        auto abbreviated_widget = std::dynamic_pointer_cast<label>(abbreviated.widget);
        auto label_item = layout->attach(std::move(abbreviated));
        layout->attach(ui::mock::ctor()->limits({ 4, 1 }, { 4, 1 }), { .shrink = 0 });
        layout->base::extend({ {}, { 8, 1 } });

        return abbreviated_widget
            && layout->get_item_area(label_item).size.x == 4
            && abbreviated_widget->get_lines() == std::vector<text>{ "lon…" }
            && abbreviated_widget->get_tooltip() == "long label";
    }

    auto test_groupbox_geometry_paint_and_lifecycle() -> bool
    {
        auto activated = 0;
        auto deactivated = 0;
        auto child = ui::mock::ctor()->limits({ 6, 2 }, { 6, 2 });
        auto result = make_groupbox({
            .title = "Timeout",
            .content = {
                child,
                [&]{ ++activated; },
                [&]{ ++deactivated; },
            },
        });
        auto box = std::dynamic_pointer_cast<groupbox>(result.widget);
        if (!box) return faux;
        auto area = rect{ {}, { 12, 0 } };
        box->base::recalc(area);
        box->base::notify(area);
        result.on_activate();
        result.on_deactivate();
        if (area.size != twod{ 12, 4 }
         || box->get_content_area() != rect{ { 2, 1 }, { 6, 2 } }
         || activated != 1 || deactivated != 1) return faux;

        auto canvas = ui::face{};
        canvas.size(area.size);
        box->render(canvas);
        return canvas[{ 0, 0 }].txt() == "┌"
            && canvas[{ 11, 0 }].txt() == "┐"
            && canvas[{ 0, 3 }].txt() == "└"
            && canvas[{ 11, 3 }].txt() == "┘"
            && canvas[{ 1, 0 }].txt() == "─"
            && canvas[{ 1, 0 }].fgc() == argb{ theme::subtext }
            && canvas[{ 3, 0 }].txt() == "T"
            && canvas[{ 3, 0 }].fgc() == argb{ theme::text_fg };
    }

    auto test_groupbox_clips_child_and_honors_ancestor_viewport() -> bool
    {
        auto child = ui::mock::ctor()->limits({ 4, 1 }, { 4, 1 });
        child->invoke([](auto& boss)
        {
            boss.LISTEN(tier::release, netxs::ui::e2::render::any, canvas)
            {
                // Deliberately ignore the child's logical clip and paint through
                // every side of its assigned area.
                canvas.fill(rect{ { -2, -1 }, boss.base::size() + twod{ 4, 2 } },
                            [](cell& c){ c.txt("x").link(1234); });
            };
        });
        auto result = make_groupbox({
            .title = "Group",
            .content = { child },
        });
        auto box = std::dynamic_pointer_cast<groupbox>(result.widget);
        if (!box) return faux;
        auto area = rect{ { 3, 2 }, { 10, 0 } };
        box->base::recalc(area);
        box->base::notify(area);

        auto canvas = ui::face{};
        canvas.size({ 18, 8 });
        canvas.fill(rect{ {}, canvas.size() }, [](cell& c){ c.txt("."); });
        box->render(canvas);
        auto frame_ok = canvas[{ 3, 2 }].txt() == "┌"
                     && canvas[{ 12, 2 }].txt() == "┐"
                     && canvas[{ 3, 4 }].txt() == "└"
                     && canvas[{ 12, 4 }].txt() == "┘"
                     && canvas[{ 3, 3 }].txt() == "│"
                     && canvas[{ 12, 3 }].txt() == "│"
                     && canvas[{ 5, 3 }].txt() == "x"
                     && canvas[{ 5, 3 }].link() == 1234
                     && canvas[{ 13, 3 }].txt() == ".";

        canvas.fill(rect{ {}, canvas.size() }, [](cell& c){ c.txt("."); });
        canvas.clip(rect{ { 3, 2 }, { 8, 2 } });
        box->render(canvas);

        // The ancestor clip hides the groupbox's last two columns and rows
        // below the viewport even though the parent face has room for them.
        return frame_ok
            && canvas[{ 3, 2 }].txt() == "┌"
            && canvas[{ 10, 2 }].txt() != "."
            && canvas[{ 11, 2 }].txt() == "."
            && canvas[{ 5, 4 }].txt() == "."
            && canvas[{ 2, 3 }].txt() == ".";
    }

    auto test_groupbox_direct_and_wrapped_children_share_inner_clip() -> bool
    {
        auto render_case = [](bool wrapped)
        {
            auto child = ui::mock::ctor()->limits({ 12, 1 }, { 12, 1 });
            child->invoke([](auto& boss)
            {
                boss.LISTEN(tier::release, netxs::ui::e2::render::any, canvas)
                {
                    // Model fixed-width inputs, dropdowns, labels, and
                    // checkboxes, all of which paint their complete retained
                    // area even when an ancestor offers less width.
                    canvas.fill(rect{ {}, boss.base::size() },
                                [](cell& c){ c.txt("x").link(4321); });
                };
            });

            auto content = component{ child };
            if (wrapped)
            {
                auto wrapper = flex::ctor({ .direction = flex_direction::column });
                wrapper->attach(content, { .shrink = 0 });
                content = component{ wrapper };
            }
            auto result = make_groupbox({
                .title = wrapped ? "Wrapped" : "Direct",
                .content = std::move(content),
            });
            auto box = std::dynamic_pointer_cast<groupbox>(result.widget);
            if (!box) return faux;
            auto area = rect{ { 2, 1 }, { 10, 0 } };
            box->base::recalc(area);
            box->base::notify(area);

            auto canvas = ui::face{};
            canvas.size({ 14, 5 });
            canvas.fill(rect{ {}, canvas.size() }, [](cell& c){ c.txt(".").link(0); });
            box->render(canvas);

            // Local x=8 is the right padding and x=9 the right border. The
            // oversized child may paint only local x=[2, 8).
            return canvas[{ 4, 2 }].txt() == "x"
                && canvas[{ 9, 2 }].txt() == "x"
                && canvas[{ 9, 2 }].link() == 4321
                && canvas[{ 10, 2 }].txt() == " "
                && canvas[{ 10, 2 }].link() != 4321
                && canvas[{ 11, 2 }].txt() == "│"
                && canvas[{ 11, 2 }].link() != 4321
                && canvas[{ 12, 2 }].txt() == ".";
        };

        return render_case(faux) && render_case(true);
    }

    auto test_wrapped_groupbox_and_page_item_padding() -> bool
    {
        auto make_box = []
        {
            auto content = make_label({
                .value = []{ return text{ "one two three four" }; },
                .role = label_role::hint,
                .overflow = label_overflow::wrap,
            });
            return make_groupbox({ .title = "Group", .content = std::move(content) });
        };
        auto page = flex::ctor({ .direction = flex_direction::column, .row_gap = 0 });
        auto first_component = make_box();
        auto second_component = make_box();
        auto first = first_component.widget;
        auto second = second_component.widget;
        page->attach(std::move(first_component), { .shrink = 0, .padding = { 2, 2, 1, 1 } });
        page->attach(std::move(second_component), { .shrink = 0, .padding = { 2, 2, 1, 1 } });
        page->base::extend({ {}, { 20, 20 } });

        auto first_area = page->get_item_area(first);
        auto second_area = page->get_item_area(second);
        return first_area == rect{ { 2, 1 }, { 16, 4 } }
            && second_area == rect{ { 2, 7 }, { 16, 4 } }
            && second_area.coor.y - (first_area.coor.y + first_area.size.y) == 2;
    }

    auto test_checkbox_render_measurement_and_toggle() -> bool
    {
        auto checked = faux;
        auto enabled = true;
        auto cfg = checkbox_cfg{
            .label = []{ return text{ "Enable compression" }; },
            .checked = [&]{ return checked; },
            .on_change = [&](bool value){ checked = value; },
            .enabled = [&]{ return enabled; },
        };
        if (checkbox_natural_width(cfg) != cell_width("Enable compression") + 2) return faux;
        checkbox_toggle(cfg);
        if (!checked) return faux;
        enabled = faux;
        checkbox_toggle(cfg);
        if (!checked) return faux;

        auto canvas = ui::face{};
        canvas.size({ 24, 1 });
        auto state = checkbox_state{};
        checkbox_render(state, cfg, canvas, canvas.size());
        if (canvas[{ 0, 0 }].txt() != "▣"
         || canvas[{ 2, 0 }].txt() != "E"
         || canvas[{ 0, 0 }].fgc() != argb{ theme::subtext }) return faux;

        auto component = make_checkbox(std::move(cfg));
        return component.widget->base::min_sz.y == 1
            && component.widget->base::min_sz.x == cell_width("Enable compression") + 2
            && component.widget->base::max_sz == component.widget->base::min_sz;
    }

    auto test_dropdown_width_render_and_navigation() -> bool
    {
        auto selected = si32{};
        auto cfg = dropdown_cfg{
            .options = {
                { "B" },
                { "Longest option" },
                { "Disabled", faux },
                { "界界" },
            },
            .selected = [&]{ return selected; },
            .on_change = [&](si32 value){ selected = value; },
        };
        auto expected = cell_width("Longest option") + 4;
        if (dropdown_default_width(cfg) != expected
         || dropdown_resolved_width(cfg) != expected) return faux;
        selected = 1;
        if (dropdown_resolved_width(cfg) != expected) return faux;
        if (dropdown_step(cfg, 1, +1) != 3
         || dropdown_step(cfg, 3, +1) != 0
         || dropdown_step(cfg, 0, -1) != 3) return faux;

        auto canvas = ui::face{};
        canvas.size({ expected, 1 });
        auto state = dropdown_state{};
        state.focused = true;
        dropdown_render(state, cfg, canvas, canvas.size());
        if (canvas[{ 1, 0 }].txt() != "L"
         || canvas[{ expected - 2, 0 }].txt() != "▾"
         || canvas[{ 0, 0 }].bgc() != argb{ theme::sel_bg }) return faux;

        cfg.width = 9;
        auto component = make_dropdown(std::move(cfg));
        component.widget->base::extend({ {}, { 9, 1 } });
        auto retained_canvas = ui::face{};
        retained_canvas.size({ 9, 1 });
        component.widget->render(retained_canvas);
        return component.widget->base::min_sz == twod{ 9, 1 }
            && component.widget->base::max_sz == twod{ 9, 1 }
            && retained_canvas[{ 1, 0 }].txt() == "L"
            && retained_canvas[{ 7, 0 }].txt() == "▾";
    }

    auto test_dropdown_generic_viewport_visibility_and_nonreflow_open() -> bool
    {
        auto trigger = ui::mock::ctor()->limits({ 8, 1 }, { 8, 1 });
        auto content = ui::mock::ctor()->limits({ 8, 20 }, { 8, 20 });
        content->base::attach(trigger);
        auto clipper = ui::mock::ctor()->limits({ 12, 6 }, { 12, 6 });
        clipper->invoke([](auto& boss)
        {
            auto& viewport = boss.base::field(rect{ {}, { 11, 6 } });
            boss.LISTEN(tier::request, netxs::app::e2::form::prop::viewport, requested)
            {
                requested = viewport;
            };
        });
        clipper->base::attach(content);
        auto host = ui::cake::ctor();
        host->base::kind(ui::base::reflow_root);
        host->base::attach(clipper);
        host->base::extend({ {}, { 12, 6 } });
        content->base::extend({ { 0, -8 }, { 8, 20 } });
        trigger->base::extend({ { 0, 10 }, { 8, 1 } });
        if (!dropdown_visible_in_viewports(*trigger, host)) return faux;

        auto model = ptr::shared<dropdown_model>();
        model->config = dropdown_cfg{
            .options = { { "First" }, { "Second" } },
            .selected = []{ return 0; },
        };
        auto before = content->base::region;
        open_dropdown(model, *trigger);
        auto stayed_put = model->state.open
                     && content->base::region == before;
        dismiss_dropdown(active_dropdown_popup());

        content->base::moveto({ 0, -14 });
        return stayed_put && !dropdown_visible_in_viewports(*trigger, host);
    }
}

int main()
{
    auto ok = true;
    auto check = [&](auto test, char const* name)
    {
        if (!test())
        {
            std::fprintf(stderr, "parvion form component test failed: %s\n", name);
            ok = false;
        }
    };
    check(test_label_roles_measurement_and_wrap, "label roles, measurement, and wrap");
    check(test_label_overflow_ellipsis_and_tooltip, "label overflow, ellipsis, and tooltip");
    check(test_flex_shrinks_ellipsis_label_to_assigned_width,
          "flex shrinks ellipsis label to assigned width");
    check(test_groupbox_geometry_paint_and_lifecycle, "groupbox geometry, paint, and lifecycle");
    check(test_groupbox_clips_child_and_honors_ancestor_viewport,
          "groupbox child and ancestor clipping");
    check(test_groupbox_direct_and_wrapped_children_share_inner_clip,
          "groupbox direct and wrapped child clipping");
    check(test_wrapped_groupbox_and_page_item_padding, "wrapped groupbox and page item padding");
    check(test_checkbox_render_measurement_and_toggle, "checkbox render, measurement, and toggle");
    check(test_dropdown_width_render_and_navigation, "dropdown width, render, and navigation");
    check(test_dropdown_generic_viewport_visibility_and_nonreflow_open,
          "dropdown generic viewport visibility and non-reflow open");
    return ok ? 0 : 1;
}
