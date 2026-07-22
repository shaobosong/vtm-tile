// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/tab_page.hpp: Metadata that adapts any retained component to a tabs container.

#include "ui.hpp" // component, text.

namespace netxs::app::parvion
{
    struct tab_page_cfg
    {
        component content;
        std::function<text()> title;
    };

    inline auto make_tab_page(component content, std::function<text()> title) -> tab_page_cfg
    {
        return { std::move(content), std::move(title) };
    }
}
