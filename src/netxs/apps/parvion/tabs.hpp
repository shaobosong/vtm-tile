// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/tabs.hpp: a generic, reusable multi-container — a bottom tab strip over a ui::veer that
// shows one page at a time. It knows nothing about its pages beyond the sealed tab_page interface
// (widget / title / activate lifecycle), so any tab_page (a table, the message log, or — in the
// future — a nested tabs / layout container) drops in unchanged. The container itself implements
// tab_page (Composite), so it can be nested inside another container later.
//
// Structure:  fork(axis::Y) { slot::_1 = veer (pages, z-stacked; back() is visible)
//                             slot::_2 = strip (1 row: labels + click-to-switch) }

#include "tab_page.hpp" // tab_page, tab_page_ptr.

namespace netxs::app::parvion
{
    struct tabs_ctrl : tab_page
    {
        std::vector<tab_page_ptr> pages;
        si32                      active = 0;   // Index (into `pages`) of the visible tab.
        netxs::wptr<ui::veer>     veer_wp;      // The page stack; back() is the visible page.
        netxs::wptr<ui::base>     strip_wp;     // The label strip (repainted on switch).
        ui::sptr                  root;         // fork(Y) [veer, strip] — the container widget.
        text                      title_str;    // This container's own tab label (used only when nested).

        auto widget() -> ui::sptr override { return root; }
        auto title() const -> text override { return title_str; }
        void on_activate()   override { if (active >= 0 && active < (si32)pages.size()) pages[(size_t)active]->on_activate(); }
        void on_deactivate() override { if (active >= 0 && active < (si32)pages.size()) pages[(size_t)active]->on_deactivate(); }

        // Bring page `target` to the front (visible): rotate the veer so its back() is that page,
        // run the deactivate/activate lifecycle, hand keyboard focus to the new page (when a gear is
        // available), and repaint the strip. Uses only the veer's public roll()/reflow() API.
        void select(si32 target, hids* gear)
        {
            auto n = (si32)pages.size();
            if (n == 0 || target < 0 || target >= n || target == active) return;
            auto veer = veer_wp.lock();
            if (!veer) return;
            // roll(1) moves back() -> front(), so the visible index steps back by 1 each roll; rotate
            // by (active - target) mod n to land `target` at back(). The veer subset stays in page
            // order, so this stays consistent across repeated switches.
            auto dt = ((active - target) % n + n) % n;
            veer->roll(dt);
            pages[(size_t)active]->on_deactivate();
            veer->base::reflow(); // The veer sizes only back(); settle the newly-visible page.
            if (gear) pro::focus::set(pages[(size_t)target]->widget(), gear->id, solo::on);
            pages[(size_t)target]->on_activate();
            active = target;
            if (auto s = strip_wp.lock()) s->base::deface();
        }
    };

    // The label strip: a 1-row widget that paints each page's title() and switches on click. It talks
    // to the container only through its public methods (weak-held to avoid a strong cycle).
    inline auto make_tab_strip(std::weak_ptr<tabs_ctrl> tcw) -> ui::sptr
    {
        auto strip = ui::mock::ctor()->active()->plugin<pro::mouse>();
        strip->invoke([tcw](auto& boss)
        {
            // Store the container back-reference in the widget's field so the render/click listeners
            // (which the LISTEN/on macros capture by reference) alias persistent storage rather than a
            // by-value member of this invoke closure (which is destroyed when invoke() returns).
            auto& tc_wp  = boss.base::field(std::weak_ptr<tabs_ctrl>{ tcw });
            auto& tabbox = boss.base::field(std::vector<rect>{}); // Cached per-tab hitboxes (rebuilt each render).
            boss.LISTEN(tier::release, e2::render::any, canvas)
            {
                auto tc = tc_wp.lock();
                if (!tc) return;
                auto size = boss.base::size();
                auto w = size.x;
                canvas.fill(rect{{ 0, 0 }, { w, 1 }}, [&](cell& c){ c.bgc(theme::surface); });
                tabbox.clear();
                auto x = si32{ 0 };
                auto n = (si32)tc->pages.size();
                for (auto i = si32{}; i < n; ++i)
                {
                    auto label  = tc->pages[(size_t)i]->title();
                    auto active = tc->active == i;
                    auto fg = active ? ui32{ theme::text_fg } : ui32{ theme::subtext };
                    auto bg = active ? ui32{ theme::bg }      : ui32{ theme::surface };
                    auto bw = (si32)label.size() + 2;
                    auto box = rect{{ x, 0 }, { bw, 1 }};
                    tabbox.push_back(box);
                    canvas.fill(box, [&](cell& c){ c.bgc(bg); });
                    put_str(canvas, x + 1, 0, label, fg, bg, bw);
                    x += bw;
                }
            };
            boss.on(tier::mouserelease, input::key::LeftDown, [&](hids& gear)
            {
                auto tc = tc_wp.lock();
                if (!tc) { gear.dismiss(); return; }
                auto mx = (si32)gear.coord.x;
                auto my = (si32)gear.coord.y;
                if (my == 0) for (auto i = si32{}; i < (si32)tabbox.size(); ++i)
                {
                    auto& b = tabbox[(size_t)i];
                    if (mx >= b.coor.x && mx < b.coor.x + b.size.x) { tc->select(i, &gear); break; }
                }
                gear.dismiss();
            });
        });
        return strip;
    }

    // Assemble a tabs container over `pages`, initially showing `active`. `title` is this container's
    // own tab label (used only when it is itself nested as a page in another container).
    inline auto make_tabs(std::vector<tab_page_ptr> pages, si32 active = 0, text title = {}) -> tab_page_ptr
    {
        auto tc = std::make_shared<tabs_ctrl>();
        tc->pages     = std::move(pages);
        tc->title_str = std::move(title);
        auto n = (si32)tc->pages.size();

        auto root = ui::fork::ctor(axis::Y);
        auto veer = ui::veer::ctor();
        for (auto& p : tc->pages) veer->attach(p->widget()); // Attach in tab order.
        auto strip = make_tab_strip(std::weak_ptr<tabs_ctrl>(tc));
        root->attach(slot::_1, veer);
        root->attach(slot::_2, strip)->limits({ -1, 1 }, { -1, 1 }); // Strip pinned to 1 row.

        tc->root     = root;
        tc->veer_wp  = veer;
        tc->strip_wp = strip;

        // Detect the initially-visible (back) page, then rotate to the requested `active` tab.
        auto cur = std::max(0, n - 1);
        for (auto i = si32{}; i < n; ++i) if (veer->back() == tc->pages[(size_t)i]->widget()) { cur = i; break; }
        tc->active = cur;
        tc->select(std::clamp(active, 0, std::max(0, n - 1)), nullptr);
        return tc;
    }
}
