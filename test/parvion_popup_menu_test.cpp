// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#include "netxs/desktopio/controls.hpp"

namespace netxs::app { using namespace netxs::ui; }

#include "netxs/apps/parvion/components/popup_menu.hpp"

#include <cstdio>
#include <vector>

using namespace netxs;
using namespace netxs::app::parvion;
namespace dm = netxs::app::parvion::popup_menu_detail;

template<class T>
concept has_mouse_halt_hook = requires (T& value) { value.halt_hook; };

// Mouse-device halt represents the pointer leaving a window or dtvt viewport.
// Popup-menu sessions deliberately do not subscribe to it; they remain open
// until an explicit dismissal event is delivered to their host.
static_assert(!has_mouse_halt_hook<dm::menu_session>);

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

    auto test_label_parsing() -> bool
    {
        auto leading = dm::parse_label("&File");
        auto middle = dm::parse_label("F&ile");
        auto escaped = dm::parse_label("&&Save");
        auto multiple = dm::parse_label("A&B&C");
        auto plain = dm::parse_label("界面");
        return leading.visible == "File"
            && leading.shortcut_offset == 0
            && leading.shortcut == 'f'
            && leading.width == 4
            && middle.visible == "File"
            && middle.shortcut_offset == 1
            && middle.shortcut == 'i'
            // The first '&' is literal; the second introduces the shortcut.
            && escaped.visible == "&Save"
            && escaped.shortcut_offset == 1
            && escaped.shortcut == 's'
            // Only the first valid marker is removed.
            && multiple.visible == "AB&C"
            && multiple.shortcut == 'b'
            // Measurement is in terminal cells, not UTF-8 bytes or clusters.
            && plain.width == 4
            && plain.shortcut == 0;
    }

    auto test_item_state_and_navigation() -> bool
    {
        auto separator = popup_menu_item{
            .label = "-",
            .kind = popup_menu_item_kind::separator,
        };
        auto disabled = popup_menu_item{ .label = "Disabled", .enabled = false };
        auto items = std::vector<popup_menu_item>{
            separator,
            disabled,
            popup_menu_item{ .label = "&Open" },
            popup_menu_item{ .label = "E&xit" },
        };
        return !dm::is_selectable(separator)
            && !dm::is_interactive(separator)
            && !dm::is_interactive(disabled)
            && dm::is_interactive(items[2])
            && dm::next_interactive(items, -1, +1) == 2
            && dm::next_interactive(items, 2, +1) == 3
            && dm::next_interactive(items, 2, -1) == 3
            && dm::shortcut_row(items, 'O') == 2
            && dm::shortcut_row(items, 'x') == 3
            && dm::shortcut_row(items, 'z') == -1;
    }

    auto test_live_state_resolution() -> bool
    {
        auto radio = popup_menu_item{
            .kind = popup_menu_item_kind::radio_submenu,
            .children = {
                popup_menu_item{ .label = "A" },
                popup_menu_item{ .label = "B", .checked = true },
                popup_menu_item{ .label = "C" },
            },
        };
        if (dm::resolve_selected(radio) != 1) return faux;
        radio.selected_index = []{ return 2; };
        if (dm::resolve_selected(radio) != 2) return faux;
        radio.selected_index = []{ return 42; };
        if (dm::resolve_selected(radio) != -1) return faux;

        auto queried = si32{};
        auto rows = std::vector<popup_menu_item>{
            popup_menu_item{
                .label = "Check",
                .kind = popup_menu_item_kind::checkbox,
                .is_checked = [&]{ ++queried; return true; },
            },
            popup_menu_item{ .label = "Static", .checked = true },
        };
        auto snapshot = dm::snapshot_items(rows);
        return queried == 1 && snapshot[0].checked && snapshot[1].checked;
    }

    auto test_popup_dimensions() -> bool
    {
        auto separator = popup_menu_item{
            .label = "Ignored",
            .kind = popup_menu_item_kind::separator,
        };
        if (dm::popup_dimensions({ separator }, 1) != twod{ 2, 1 }) return faux;
        auto items = std::vector<popup_menu_item>{
            popup_menu_item{ .label = "Short" },
            popup_menu_item{ .label = "A much longer label" },
            separator,
        };
        if (dm::popup_dimensions(items, 1) != twod{ 21, 3 }) return faux;
        items[1].children.push_back(popup_menu_item{ .label = "child" });
        if (dm::popup_dimensions(items, 1) != twod{ 23, 3 }) return faux;
        if (dm::popup_dimensions(items, 1, true) != twod{ 25, 3 }) return faux;
        items.push_back(popup_menu_item{
            .label = "C",
            .kind = popup_menu_item_kind::checkbox,
        });
        if (dm::popup_dimensions(items, 1) != twod{ 25, 4 }) return faux;
        if (dm::popup_dimensions(items, 3) != twod{ 29, 4 }) return faux;

        auto wide = std::vector<popup_menu_item>{ popup_menu_item{ .label = "界面" } };
        return dm::popup_dimensions(wide, 1) == twod{ 6, 1 };
    }

    auto test_live_popup_geometry() -> bool
    {
        if (dm::constrain_popup({ 8, 3 }, { 10, 4 }, { 30, 12 }) != twod{ 8, 3 }) return faux;
        if (dm::constrain_popup({ 24, 10 }, { 10, 4 }, { 30, 12 }) != twod{ 20, 8 }) return faux;
        if (dm::constrain_popup({ -3, -2 }, { 10, 4 }, { 30, 12 }) != twod{ 0, 0 }) return faux;

        auto parent = rect{ { 10, 2 }, { 12, 5 } };
        auto child = twod{ 8, 3 };
        return dm::submenu_anchor(parent, 2, child, 35) == twod{ 22, 4 }
            && dm::submenu_anchor(parent, 2, child, 28) == twod{ 2, 4 };
    }

    auto test_paint_label() -> bool
    {
        auto canvas = mock_canvas{};
        dm::paint_label(canvas, 2, 0, "&File", 0xFF111111u, 0xFF222222u, 10, 77);
        if (canvas.writes.size() != 4) return faux;
        auto const& first = canvas.writes[0];
        auto const& second = canvas.writes[1];
        auto const& last = canvas.writes[3];
        if (first.area.coor != twod{ 2, 0 }
         || second.area.coor != twod{ 3, 0 }
         || last.area.coor != twod{ 5, 0 }) return faux;
        if (first.value.und() != unln::line || second.value.und() == unln::line) return faux;
        if (first.value.link() != 77 || last.value.link() != 77) return faux;
        if (first.value.fgc() != argb{ 0xFF111111u }
         || first.value.bgc() != argb{ 0xFF222222u }) return faux;

        auto wide = mock_canvas{};
        dm::paint_label(wide, 0, 0, "界X", 0, 0, 3);
        if (wide.writes.size() != 3
         || wide.writes[0].area.coor != twod{ 0, 0 }
         || wide.writes[1].area.coor != twod{ 1, 0 }
         || wide.writes[2].area.coor != twod{ 2, 0 }) return faux;
        auto clipped = mock_canvas{};
        dm::paint_label(clipped, 0, 0, "界X", 0, 0, 1);
        return clipped.writes.empty();
    }

    auto test_config_defaults_and_trigger_width() -> bool
    {
        auto cfg = popup_menu_cfg{};
        if (!dm::enabled(cfg)
         || cfg.padding != 1
         || cfg.source != popup_menu_source::dropdown_menu
         || cfg.radio_group) return faux;
        auto open = popup_menu_open_cfg{};
        if (open.source != popup_menu_source::dropdown_menu) return faux;
        if (dm::trigger_width(cfg, "&Menu") != 8) return faux;
        if (dm::trigger_width(cfg, "") != 3) return faux;
        cfg.padding = 0;
        if (dm::trigger_width(cfg, "") != 3) return faux;
        cfg.enabled = []{ return faux; };
        return !dm::enabled(cfg);
    }

    auto test_make_trigger_factories() -> bool
    {
        // A structurally identical trigger is expected from every public
        // factory; each pins a different popup source (verified by
        // construction), while geometry and padding pass through unchanged.
        auto check = [](auto make) -> bool
        {
            auto cfg = popup_menu_cfg{};
            cfg.label = []{ return text{ "&Menu" }; };
            cfg.items = []{ return std::vector<popup_menu_item>{ { .label = "Item" } }; };
            auto component = make(std::move(cfg));
            if (!component || !component.widget) return faux;
            if (component.widget->base::min_sz != twod{ 8, 1 }
             || component.widget->base::max_sz != twod{ 8, 1 }) return faux;
            if (component.widget->base::property("menu.padding", si32{ 1 }) != 1) return faux;

            auto arrow_cfg = popup_menu_cfg{};
            arrow_cfg.label = []{ return text{}; };
            arrow_cfg.items = []{ return std::vector<popup_menu_item>{ { .label = "X" } }; };
            auto arrow = make(std::move(arrow_cfg));
            if (!arrow || arrow.widget->base::min_sz.x != 3) return faux;

            auto padded_cfg = popup_menu_cfg{};
            padded_cfg.label = []{ return text{ "M" }; };
            padded_cfg.padding = 3;
            auto padded = make(std::move(padded_cfg));
            return padded && padded.widget->base::property("menu.padding", si32{ 1 }) == 3;
        };
        return check(make_dropdown_menu)
            && check(make_context_menu)
            && check(make_menu_bar);
    }
}

int main()
{
    auto ok = test_label_parsing()
           && test_item_state_and_navigation()
           && test_live_state_resolution()
           && test_popup_dimensions()
           && test_live_popup_geometry()
           && test_paint_label()
           && test_config_defaults_and_trigger_width()
           && test_make_trigger_factories();
    if (!ok) std::fprintf(stderr, "parvion dropdown menu tests failed\n");
    return ok ? 0 : 1;
}
