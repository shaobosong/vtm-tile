// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#include "netxs/desktopio/controls.hpp"

namespace netxs::app { using namespace netxs::ui; }

#include "netxs/apps/parvion/components/input.hpp"

#include <cstdio>

using namespace netxs;
using namespace netxs::app::parvion;

namespace
{
    constexpr auto palette = input_palette{
        .background = 0xFF010203u,
        .foreground = 0xFF112233u,
        .muted_foreground = 0xFF445566u,
        .ghost_foreground = 0xFF667788u,
        .focus = 0xFF778899u,
    };

    auto render(input_mode mode, si32 width, text value = "abcdef", text prefix = {},
                text ghost = {}, bool secret = faux) -> ui::face
    {
        auto input = make_input({
            .value = [value]{ return value; },
            .prefix = [prefix]{ return prefix; },
            .ghost = [ghost]{ return ghost; },
            .mode = [mode]{ return mode; },
            .secret = secret,
            .palette = palette,
        });
        input.widget->base::extend(rect{ {}, { width, 1 } });
        auto canvas = ui::face{};
        canvas.size({ width, 1 });
        input.widget->render(canvas);
        return canvas;
    }

    auto test_state_foregrounds() -> bool
    {
        return input_detail::value_foreground(input_mode::view, faux, palette) == palette.foreground
            && input_detail::value_foreground(input_mode::view, true, palette) == palette.foreground
            && input_detail::value_foreground(input_mode::edit, faux, palette) == palette.foreground
            && input_detail::value_foreground(input_mode::edit, true, palette) == palette.focus
            && input_detail::value_foreground(input_mode::disabled, faux, palette) == palette.muted_foreground
            && input_detail::value_foreground(input_mode::disabled, true, palette) == palette.muted_foreground;
    }

    auto test_view_and_disabled_value_colors() -> bool
    {
        auto view = render(input_mode::view, 7);
        auto disabled = render(input_mode::disabled, 7);
        return view[{ 0, 0 }].txt() == "a"
            && view[{ 0, 0 }].fgc() == argb{ palette.foreground }
            && disabled[{ 0, 0 }].txt() == "a"
            && disabled[{ 0, 0 }].fgc() == argb{ palette.muted_foreground };
    }

    auto test_truncated_value_colors() -> bool
    {
        auto view = render(input_mode::view, 4);
        auto disabled = render(input_mode::disabled, 4);
        return view[{ 0, 0 }].txt() == "\xE2\x80\xA6"
            && view[{ 0, 0 }].fgc() == argb{ palette.foreground }
            && view[{ 1, 0 }].fgc() == argb{ palette.foreground }
            && disabled[{ 0, 0 }].txt() == "\xE2\x80\xA6"
            && disabled[{ 0, 0 }].fgc() == argb{ palette.muted_foreground }
            && disabled[{ 1, 0 }].fgc() == argb{ palette.muted_foreground };
    }

    auto test_prefix_and_inactive_underline_stay_muted() -> bool
    {
        auto canvas = render(input_mode::view, 8, "value", ">");
        auto const& prefix = canvas[{ 0, 0 }];
        auto const& value = canvas[{ 1, 0 }];
        return prefix.txt() == ">"
            && prefix.fgc() == argb{ palette.muted_foreground }
            && value.txt() == "v"
            && value.fgc() == argb{ palette.foreground }
            && value.und() == unln::line
            && value.unc() == argb{ palette.muted_foreground }.to_256cube();
    }

    auto test_default_ghost_color() -> bool
    {
        return theme::ghost_fg == 0xFFA6ADC8u
            && input_palette{}.ghost_foreground == theme::ghost_fg;
    }

    auto test_empty_edit_renders_clipped_ghost_after_prefix() -> bool
    {
        auto canvas = render(input_mode::edit, 4, "", ">", "ghost");
        return canvas[{ 0, 0 }].txt() == ">"
            && canvas[{ 0, 0 }].fgc() == argb{ palette.muted_foreground }
            && canvas[{ 1, 0 }].txt() == "g"
            && canvas[{ 1, 0 }].fgc() == argb{ palette.ghost_foreground }
            && canvas[{ 2, 0 }].txt() == "h"
            && canvas[{ 2, 0 }].fgc() == argb{ palette.ghost_foreground }
            && canvas[{ 3, 0 }].txt() == "o"
            && canvas[{ 3, 0 }].fgc() == argb{ palette.ghost_foreground };
    }

    auto test_ghost_visibility() -> bool
    {
        auto populated = render(input_mode::edit, 6, "real", {}, "ghost");
        auto view = render(input_mode::view, 6, "", {}, "ghost");
        auto disabled = render(input_mode::disabled, 6, "", {}, "ghost");
        return populated[{ 0, 0 }].txt() == "r"
            && populated[{ 0, 0 }].fgc() == argb{ palette.foreground }
            && view[{ 0, 0 }].txt() == " "
            && disabled[{ 0, 0 }].txt() == " ";
    }

    auto test_secret_ghost_is_not_masked() -> bool
    {
        auto empty = render(input_mode::edit, 6, "", {}, "secret", true);
        auto populated = render(input_mode::edit, 6, "abc", {}, "secret", true);
        return empty[{ 0, 0 }].txt() == "s"
            && empty[{ 0, 0 }].fgc() == argb{ palette.ghost_foreground }
            && populated[{ 0, 0 }].txt() == "*"
            && populated[{ 0, 0 }].fgc() == argb{ palette.foreground };
    }
}

int main()
{
    auto check = [](bool result, char const* name)
    {
        if (!result) std::fprintf(stderr, "FAIL: %s\n", name);
        return result;
    };
    auto ok = true;
    ok &= check(test_state_foregrounds(), "state foregrounds");
    ok &= check(test_view_and_disabled_value_colors(), "view and disabled colors");
    ok &= check(test_truncated_value_colors(), "truncated colors");
    ok &= check(test_prefix_and_inactive_underline_stay_muted(), "prefix and underline colors");
    ok &= check(test_default_ghost_color(), "default ghost color");
    ok &= check(test_empty_edit_renders_clipped_ghost_after_prefix(), "empty edit ghost rendering");
    ok &= check(test_ghost_visibility(), "ghost visibility");
    ok &= check(test_secret_ghost_is_not_masked(), "secret ghost rendering");
    return ok ? 0 : 1;
}
