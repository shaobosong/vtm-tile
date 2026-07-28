// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/components/tab_page.hpp: Metadata that adapts any retained component to a tabs container.

#include "ui.hpp" // component, text.

namespace netxs::app::parvion
{
    // A page owns its abbreviation policy. The tabs container supplies a progressively smaller
    // cell budget, allowing callers to preserve semantic suffixes instead of blindly clipping them.
    using tab_abbreviate = std::function<text(view title, si32 max_cells)>;

    struct tab_page_cfg
    {
        component content;
        std::function<text()> title;
        tab_abbreviate abbreviate;
    };

    inline auto tab_abbreviate_tail(view title, si32 max_cells) -> text
    {
        return fit_ellipsis(title, max_cells);
    }

    // Shorten only the leading title and preserve a trailing count such as " (0)":
    // "Transferring (0)" -> "Transferri… (0)" -> ... -> "T… (0)".
    inline auto tab_abbreviate_count(view title, si32 max_cells) -> text
    {
        auto suffix_at = title.rfind(" (");
        auto counted = suffix_at != view::npos && title.size() > suffix_at + 2 && title.back() == ')';
        if (!counted) return tab_abbreviate_tail(title, max_cells);
        if (cell_width(title) <= max_cells) return text{ title };

        auto lead = title.substr(0, suffix_at);
        auto suffix = title.substr(suffix_at);
        auto suffix_w = cell_width(suffix);
        auto lead_w = std::max(2, max_cells - suffix_w); // Keep at least "T…".
        return fit_ellipsis(lead, lead_w) + text{ suffix };
    }

    inline auto make_tab_page(component content, std::function<text()> title,
                              tab_abbreviate abbreviate = tab_abbreviate_tail) -> tab_page_cfg
    {
        return { std::move(content), std::move(title), std::move(abbreviate) };
    }
}
