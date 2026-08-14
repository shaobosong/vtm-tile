// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#include "netxs/apps.hpp"
#include "netxs/apps/desk.hpp"
#include "vtm-common.hpp"
#include "netxs/apps/tile.hpp"

#include <cstdio>

using namespace netxs;
using namespace netxs::app;

namespace
{
    auto attribute_free(cell const& c) -> bool
    {
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
    }

    auto test_backdrop_dims_all_visible_channels() -> bool
    {
        auto original_fg = argb{ 0xff80a0c0 };
        auto original_bg = argb{ 0xff203040 };
        auto original_underline = argb{ 0xffd7875f };
        auto expected_fg = original_fg;
        auto expected_bg = original_bg;
        expected_fg.faint();
        expected_bg.faint();
        auto expected_underline = argb{ argb::vt256[original_underline.to_256cube()] };
        expected_underline.faint();

        auto c = cell{};
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

        tile::shade_overlay_backdrop(c, 4321);

        return c.txt() == "Z"
            && c.bgc() == expected_bg
            && c.fgc() == expected_fg
            && c.bld()
            && !c.fnt()
            && c.itc()
            && c.und() == unln::wavy
            && c.unc() == expected_underline.to_256cube()
            && c.inv()
            && c.ovr()
            && c.stk()
            && c.blk()
            && c.hid()
            && c.cur() == text_cursor::none
            && c.img() == 5678
            && c.link() == 4321;
    }

    auto test_backdrop_keeps_inherited_underline_and_ordinary_weight() -> bool
    {
        auto c = cell{};
        c.bgc(0xff203040)
         .fgc(0xff80a0c0)
         .txt("Z")
         .und(unln::line)
         .unc(0)
         .inv(faux)
         .fnt(faux);

        tile::shade_overlay_backdrop(c, 4321);

        return c.unc() == 0
            && !c.inv()
            && !c.fnt();
    }

    auto test_pane_index_cell_clears_underlying_attributes() -> bool
    {
        auto c = cell{};
        c.bgc(0xff203040)
         .fgc(0xff80a0c0)
         .txt("Z")
         .bld(true)
         .fnt(true)
         .itc(true)
         .und(unln::wavy)
         .unc(0xffd7875f)
         .inv(true)
         .ovr(true)
         .stk(true)
         .blk(true)
         .hid(true)
         .cur(text_cursor::block)
         .img(5678)
         .link(1234);

        auto bg = argb{ 0xff1e1e2e };
        auto fg = argb{ 0xff89b4fa };
        tile::paint_pane_index_cell(c, bg, fg, "0", 4321);

        return c.txt() == "0"
            && c.bgc() == bg
            && c.fgc() == fg
            && c.link() == 4321
            && attribute_free(c);
    }
}

int main()
{
    auto ok = true;
    auto check = [&](auto test, char const* name)
    {
        if (!test())
        {
            std::fprintf(stderr, "tile overlay cell test failed: %s\n", name);
            ok = false;
        }
    };
    check(test_backdrop_dims_all_visible_channels, "backdrop dims all visible channels");
    check(test_backdrop_keeps_inherited_underline_and_ordinary_weight,
          "backdrop keeps inherited underline and ordinary weight");
    check(test_pane_index_cell_clears_underlying_attributes,
          "pane-index cell clears underlying attributes");
    return ok ? 0 : 1;
}
