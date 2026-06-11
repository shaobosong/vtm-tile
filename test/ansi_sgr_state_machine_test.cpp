// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#include "netxs/desktopio/ansivt.hpp"

using namespace netxs;

namespace
{
    struct probe
    {
        template<class T>
        using vt_parser = ansi::vt_parser<T>;

        ansi::deco style;
        ansi::mark brush;
        si32 decsg{};
        bool defer{};

        void task(ansi::rule const&) { }
        void post(auto const&) { }
        void ascii(view) { }
        void flush() { }
    };

    auto make_brush()
    {
        return cell{ '\0' }.fgc(tint::whitelt).bgc(tint::blackdk);
    }

    auto verify_full_parser_handles_multi_parameter_sgr() -> bool
    {
        auto state = probe{};
        state.brush.reset(make_brush());
        auto* ptr = &state;

        ansi::parse("\033[1;3;4:3;9:1;31;104m", ptr);

        return state.brush.bld()
            && state.brush.itc()
            && state.brush.und() == unln::wavy
            && state.brush.stk()
            && state.brush.fgc() == argb{ tint::reddk }
            && state.brush.bgc() == argb{ tint::bluelt };
    }

    auto verify_extended_subparams_support_iso_and_xterm_forms() -> bool
    {
        auto state = probe{};
        state.brush.reset(make_brush());
        auto* ptr = &state;

        ansi::parse("\033[38:2::1:2:3;48:2:4:5:6;58:2::7:8:9m", ptr);

        auto fg = state.brush.fgc();
        auto bg = state.brush.bgc();
        return fg.chan.r == 1
            && fg.chan.g == 2
            && fg.chan.b == 3
            && bg.chan.r == 4
            && bg.chan.g == 5
            && bg.chan.b == 6
            && state.brush.unc() == argb{ 7, 8, 9 }.to_256cube();
    }

    auto verify_sgr_22_clears_bold_and_faint() -> bool
    {
        auto state = probe{};
        state.brush.reset(make_brush());
        auto* ptr = &state;

        ansi::parse("\033[1;2;22m", ptr);

        return !state.brush.bld()
            && !state.brush.fnt();
    }
}

auto main() -> int
{
    if (!verify_full_parser_handles_multi_parameter_sgr()) return 1;
    if (!verify_extended_subparams_support_iso_and_xterm_forms()) return 2;
    if (!verify_sgr_22_clears_bold_and_faint()) return 3;
    return 0;
}
