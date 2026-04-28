// Copyright (c) Dmitry Sapozhnikov
// Licensed under the MIT license.

#include "netxs/apps.hpp"
#include "netxs/apps/desk.hpp"
#include "vtm-common.hpp"
#include "netxs/apps/tile.hpp"

using namespace netxs;
using namespace netxs::app;

namespace
{
    auto verify_optimal_offsets() -> bool
    {
        auto offsets = tile::command_bar::match_offsets("abc", "a_____b__c__abc");
        return offsets == std::vector<size_t>{ 12, 13, 14 };
    }

    auto verify_boundary_ranking() -> bool
    {
        auto commands = std::vector<tile::command_bar::item>
        {
            { "foobar",  "", "" },
            { "foo bar", "", "" },
        };
        auto model = tile::command_bar::build_model(commands, "fb");
        return model.filtered.size() == 2
            && model.filtered[0] == 1
            && model.filtered[1] == 0;
    }

    auto verify_empty_query_keeps_order() -> bool
    {
        auto commands = std::vector<tile::command_bar::item>
        {
            { "zz", "", "" },
            { "a",  "", "" },
        };
        auto model = tile::command_bar::build_model(commands, "");
        return model.filtered.size() == 2
            && model.filtered[0] == 0
            && model.filtered[1] == 1;
    }

    auto verify_space_separated_terms_are_anded() -> bool
    {
        auto commands = std::vector<tile::command_bar::item>
        {
            { "layout: Split Horizontal", "", "" },
            { "layout: Split Vertical",   "", "" },
            { "layout: Rotate Split",     "", "" },
            { "layout: Swap Panes",       "", "" },
            { "layout: Equalize Splits",  "", "" },
        };
        auto model = tile::command_bar::build_model(commands, "hor sp");
        return model.filtered.size() == 1
            && model.filtered[0] == 0
            && tile::command_bar::fuzzy_match("hor sp", commands[0].display)
            && !tile::command_bar::fuzzy_match("hor sp", commands[1].display);
    }

    auto verify_special_chars_are_literal() -> bool
    {
        return  tile::command_bar::fuzzy_match("^", "caret ^ command")
            && !tile::command_bar::fuzzy_match("^", "caret command")
            &&  tile::command_bar::fuzzy_match("$", "dollar $ command")
            && !tile::command_bar::fuzzy_match("$", "dollar command");
    }
}

auto main() -> int
{
    if (!verify_optimal_offsets())             return 1;
    if (!verify_boundary_ranking())            return 2;
    if (!verify_empty_query_keeps_order())     return 3;
    if (!verify_space_separated_terms_are_anded()) return 4;
    if (!verify_special_chars_are_literal())   return 5;
    return 0;
}
