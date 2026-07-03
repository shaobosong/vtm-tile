// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/tab_page.hpp: the single container-facing interface every tab page exposes.
//
// A tab page adapts a retained ui sub-widget to the tabs container with a dynamic tab-strip label and
// activation lifecycle hooks. Core widgets do not depend on this contract; concrete views wrap them
// with make_tab_page() when they need to live inside tabs.

#include "panes.hpp" // ui::sptr, text.

namespace netxs::app::parvion
{
    struct tab_page
    {
        virtual ~tab_page() = default;
        // The retained sub-widget added to a veer / layout container.
        virtual auto widget() -> ui::sptr = 0;
        // The live tab-strip label; may include a running count, e.g. "Transferring (3)".
        virtual auto title() const -> text = 0;
        // This page became the visible tab (page-specific side effects; focus is driven by the container).
        virtual void on_activate() {}
        // Another tab took over (e.g. the message log drops its text selection).
        virtual void on_deactivate() {}
    };
    using tab_page_ptr = std::shared_ptr<tab_page>;

    struct widget_tab_page final : tab_page
    {
        widget_tab_page(ui::sptr widget,
                        std::function<text()> title,
                        std::function<void()> on_activate = {},
                        std::function<void()> on_deactivate = {})
            : w{ std::move(widget) },
              title_fn{ std::move(title) },
              activate_fn{ std::move(on_activate) },
              deactivate_fn{ std::move(on_deactivate) }
        { }

        auto widget() -> ui::sptr override { return w; }
        auto title() const -> text override { return title_fn ? title_fn() : text{}; }
        void on_activate() override { if (activate_fn) activate_fn(); }
        void on_deactivate() override { if (deactivate_fn) deactivate_fn(); }

    private:
        ui::sptr              w;
        std::function<text()> title_fn;
        std::function<void()> activate_fn;
        std::function<void()> deactivate_fn;
    };

    inline auto make_tab_page(ui::sptr widget,
                              std::function<text()> title,
                              std::function<void()> on_activate = {},
                              std::function<void()> on_deactivate = {}) -> tab_page_ptr
    {
        return std::make_shared<widget_tab_page>(std::move(widget),
                                                 std::move(title),
                                                 std::move(on_activate),
                                                 std::move(on_deactivate));
    }
}
