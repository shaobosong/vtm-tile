// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#include "netxs/apps.hpp"
#include "vtm-common.hpp"
#include "netxs/apps/parvion/file_picker_dialog.hpp"

#include <cstdio>

using namespace netxs;
using namespace netxs::app::parvion;

namespace
{
    auto sample_pane() -> pane_state
    {
        auto pane = pane_state{};
        pane.path = "/tmp/picker";
        pane.is_local = true;
        pane.items = {
            direntry{ .name = "folder", .is_dir = true },
            direntry{ .name = "key.pem", .size = 7 },
        };
        pane.sel = 0;
        pane.marked = { 0 };
        return pane;
    }

    auto test_open_action_by_item_type() -> bool
    {
        auto pane = sample_pane();

        auto parent = file_picker_resolve_open(pane,
            file_picker_selection::files_and_directories);
        if (parent.action != file_picker_open_action::navigate
         || parent.target.kind != file_picker_item_kind::parent) return faux;

        pane.sel = 1;
        pane.marked = { 1 };
        auto files_only_dir = file_picker_resolve_open(pane,
            file_picker_selection::files);
        auto selectable_dir = file_picker_resolve_open(pane,
            file_picker_selection::files_and_directories);
        if (files_only_dir.action != file_picker_open_action::navigate
         || selectable_dir.action != file_picker_open_action::accept
         || selectable_dir.target.kind != file_picker_item_kind::directory
         || selectable_dir.target.path != "/tmp/picker/folder") return faux;

        pane.sel = 2;
        pane.marked = { 2 };
        auto file = file_picker_resolve_open(pane, file_picker_selection::files);
        return file.action == file_picker_open_action::accept
            && file.target.kind == file_picker_item_kind::file
            && file.target.path == "/tmp/picker/key.pem";
    }

    auto test_open_requires_one_actual_mark() -> bool
    {
        auto pane = sample_pane();
        pane.sel = 2; // The navigation cursor survives selection clearing.
        pane.marked.clear();
        if (file_picker_open_enabled(pane, file_picker_selection::files)) return faux;
        if (file_picker_selected_target(pane).kind != file_picker_item_kind::none) return faux;

        pane.marked = { 1, 2 };
        if (file_picker_open_enabled(pane,
                                     file_picker_selection::files_and_directories)) return faux;

        pane.marked = { 2 };
        return file_picker_open_enabled(pane, file_picker_selection::files);
    }

    auto test_dialog_sizing_is_forwarded() -> bool
    {
        auto proportional = make_file_picker({
            .title = "Open",
            .initial_dir = ".",
            .size = {
                .width = dialog_length::ratio(0.50),
                .height = dialog_length::ratio(0.50),
            },
            .minimum = { 3, 3 },
            .maximum = { -1, -1 },
        });
        auto proportional_dialog = std::dynamic_pointer_cast<dialog>(proportional.widget);
        if (!proportional_dialog) return faux;
        proportional.widget->base::extend({ { 10, 5 }, { 100, 40 } });
        if (auto area = proportional_dialog->get_card_area(); area != rect{ { 25, 10 }, { 50, 20 } })
        {
            std::fprintf(stderr, "proportional picker area=%d,%d %dx%d\n",
                         area.coor.x, area.coor.y, area.size.x, area.size.y);
            return faux;
        }

        auto fixed = make_file_picker({
            .title = "Open",
            .initial_dir = ".",
            .size = {
                .width = dialog_length::cells(40),
                .height = dialog_length::cells(18),
            },
            .minimum = { 3, 3 },
            .maximum = { -1, -1 },
        });
        auto fixed_dialog = std::dynamic_pointer_cast<dialog>(fixed.widget);
        if (!fixed_dialog) return faux;
        fixed.widget->base::extend({ {}, { 100, 40 } });
        auto area = fixed_dialog->get_card_area();
        if (area != rect{ { 30, 11 }, { 40, 18 } })
            std::fprintf(stderr, "fixed picker area=%d,%d %dx%d\n",
                         area.coor.x, area.coor.y, area.size.x, area.size.y);
        return area == rect{ { 30, 11 }, { 40, 18 } };
    }

    auto test_default_layout_is_bounded_eighty_percent() -> bool
    {
        auto picker = make_file_picker({
            .title = "Open",
            .initial_dir = ".",
        });
        auto picker_dialog = std::dynamic_pointer_cast<dialog>(picker.widget);
        if (!picker_dialog) return faux;
        picker.widget->base::extend({ {}, { 120, 44 } });
        return picker_dialog->get_card_area() == rect{ { 15, 6 }, { 90, 32 } };
    }
}

int main()
{
    auto ok = true;
    auto check = [&](auto test, char const* name)
    {
        if (!test())
        {
            std::fprintf(stderr, "parvion file picker test failed: %s\n", name);
            ok = false;
        }
    };
    check(test_open_action_by_item_type, "Open action by item type");
    check(test_open_requires_one_actual_mark, "Open requires one actual mark");
    check(test_dialog_sizing_is_forwarded, "dialog sizing is forwarded");
    check(test_default_layout_is_bounded_eighty_percent, "default bounded eighty-percent layout");
    return ok ? 0 : 1;
}
