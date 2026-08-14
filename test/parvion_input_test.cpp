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
        .focus = 0xFF778899u,
    };

    auto render(input_mode mode, si32 width, text value = "abcdef", text prefix = {}) -> ui::face
    {
        auto input = make_input({
            .value = [value]{ return value; },
            .prefix = [prefix]{ return prefix; },
            .mode = [mode]{ return mode; },
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
    return ok ? 0 : 1;
}
