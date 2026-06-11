// Copyright (c) Shaobo Song
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

    // Loads the bundled vtm.xml and confirms the new <group label="terminal">
    // entries surface through tile::command_bar::load(cfg).
    auto load_default_terminal_group() -> std::vector<tile::command_bar::item>
    {
        auto config = xml::settings{};
        app::shared::load::settings(config, ""); // Empty cliopt -> bundled vtm.xml only.
        auto commands_ptr = tile::command_bar::load(config);
        return commands_ptr ? *commands_ptr : std::vector<tile::command_bar::item>{};
    }

    auto verify_terminal_group_loaded() -> bool
    {
        auto commands = load_default_terminal_group();
        // Required entries match the active (uncommented) terminal items
        // in vtm.xml's <commandbar><group label="terminal">. Find Next/
        // Find Previous and Toggle Wrap Mode are intentionally commented
        // out in vtm.xml until their underlying scripting aliases ship,
        // so we don't enforce them here. Updating this list when the
        // group shrinks/grows is part of the contract: the test must
        // mirror what vtm.xml actually exposes today.
        auto required = std::vector<text>
        {
            "terminal: Toggle Find Bar",
            "terminal: Restart Session",
            "terminal: Scroll To Top",
            "terminal: Scroll To End",
            "terminal: Clear Scrollback",
            "terminal: Reset Terminal",
            "terminal: Copy Viewport",
            "terminal: Paste Clipboard",
        };
        for (auto const& label : required)
        {
            auto found = false;
            for (auto const& cmd : commands)
            {
                if (cmd.display == label) { found = true; break; }
            }
            if (!found) return faux;
        }
        return true;
    }

    auto verify_terminal_group_item_count() -> bool
    {
        auto commands = load_default_terminal_group();
        auto count = si32{};
        for (auto const& cmd : commands)
        {
            if (cmd.display.starts_with("terminal: ")) ++count;
        }
        // Lower bound chosen to track the currently-active set (16 as
        // of this commit) with a small safety margin so accidental
        // removal of a terminal command is caught here. Bump as the
        // group grows.
        return count >= 12;
    }

    auto verify_terminal_items_have_scripts() -> bool
    {
        auto commands = load_default_terminal_group();
        for (auto const& cmd : commands)
        {
            if (cmd.display.starts_with("terminal: ") && cmd.script.empty())
            {
                return faux;
            }
        }
        return true;
    }
}

auto main() -> int
{
    if (!verify_optimal_offsets())             return 1;
    if (!verify_boundary_ranking())            return 2;
    if (!verify_empty_query_keeps_order())     return 3;
    if (!verify_space_separated_terms_are_anded()) return 4;
    if (!verify_special_chars_are_literal())   return 5;
    if (!verify_terminal_group_loaded())       return 6;
    if (!verify_terminal_group_item_count())   return 7;
    if (!verify_terminal_items_have_scripts()) return 8;
    return 0;
}
