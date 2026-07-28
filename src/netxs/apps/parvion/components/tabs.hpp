// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/components/tabs.hpp: a generic, reusable multi-container — a bottom tab strip over a
// ui::veer that
// shows one component page at a time. Page-specific lifecycle is carried by the common component
// handle, so tabs can be nested without a separate inheritance hierarchy.
//
// Structure:  fork(axis::Y) { slot::_1 = veer (pages, z-stacked; back() is visible)
//                             slot::_2 = strip (1 row: labels + click-to-switch) }

#include "tab_page.hpp" // tab_page_cfg.

namespace netxs::app::parvion
{
    // Resolve current labels against the strip width. Compression reduces the widest title first;
    // ties advance one title at a time in stable rounds, so equally wide tabs lose width evenly.
    // Each page callback sees its full title plus the next target width and can preserve any
    // important suffix.
    inline auto resolve_tab_titles(std::vector<tab_page_cfg> const& pages, si32 strip_w)
        -> std::vector<text>
    {
        auto full = std::vector<text>{};
        auto labels = std::vector<text>{};
        auto widths = std::vector<si32>{};
        full.reserve(pages.size());
        labels.reserve(pages.size());
        widths.reserve(pages.size());
        auto total = si32{};
        for (auto& page : pages)
        {
            auto title = page.title ? page.title() : text{};
            auto width = cell_width(title);
            full.push_back(title);
            labels.push_back(std::move(title));
            widths.push_back(width);
            total += width + 2; // One padding cell on either side.
        }

        auto cursor = si32{};
        auto n = (si32)pages.size();
        while (n > 0 && total > std::max(0, strip_w))
        {
            auto target = si32{ -1 };
            auto target_w = si32{ -1 };
            auto target_next = text{};
            auto target_next_w = si32{};
            for (auto step = si32{}; step < n; ++step)
            {
                auto i = (cursor + step) % n;
                auto next = pages[(size_t)i].abbreviate
                          ? pages[(size_t)i].abbreviate(full[(size_t)i], widths[(size_t)i] - 1)
                          : tab_abbreviate_tail(full[(size_t)i], widths[(size_t)i] - 1);
                auto next_w = cell_width(next);
                if (next_w >= widths[(size_t)i]) continue; // This title reached its callback's floor.
                if (widths[(size_t)i] <= target_w) continue; // Keep the first tied title after cursor.
                target = i;
                target_w = widths[(size_t)i];
                target_next = std::move(next);
                target_next_w = next_w;
            }
            if (target < 0) break;
            total -= widths[(size_t)target] - target_next_w;
            widths[(size_t)target] = target_next_w;
            labels[(size_t)target] = std::move(target_next);
            cursor = (target + 1) % n;
        }
        return labels;
    }

    struct tabs_ctrl
    {
        std::vector<tab_page_cfg> pages;
        si32                      active = 0;   // Index (into `pages`) of the visible tab.
        netxs::wptr<ui::veer>     veer_wp;      // The page stack; back() is the visible page.
        netxs::wptr<ui::base>     strip_wp;     // The label strip (repainted on switch).
        ui::sptr                  root;         // fork(Y) [veer, strip] — the container widget.

        void on_activate()   { if (active >= 0 && active < (si32)pages.size()) pages[(size_t)active].content.on_activate(); }
        void on_deactivate() { if (active >= 0 && active < (si32)pages.size()) pages[(size_t)active].content.on_deactivate(); }

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
            pages[(size_t)active].content.on_deactivate();
            veer->base::reflow(); // The veer sizes only back(); settle the newly-visible page.
            if (gear) pro::focus::set(pages[(size_t)target].content.widget, gear->id, solo::on);
            pages[(size_t)target].content.on_activate();
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
                auto labels = resolve_tab_titles(tc->pages, w);
                for (auto i = si32{}; i < n; ++i)
                {
                    auto& label = labels[(size_t)i];
                    auto active = tc->active == i;
                    auto fg = active ? ui32{ theme::text_fg } : ui32{ theme::subtext };
                    auto bg = active ? ui32{ theme::bg }      : ui32{ theme::surface };
                    auto bw = cell_width(label) + 2;
                    auto visible_w = std::max(0, std::min(bw, w - x));
                    auto box = rect{{ x, 0 }, { visible_w, 1 }};
                    tabbox.push_back(box);
                    if (visible_w > 0)
                    {
                        canvas.fill(box, [&](cell& c){ c.bgc(bg); });
                        put_str(canvas, x + 1, 0, label, fg, bg, std::max(0, visible_w - 1));
                    }
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

    // Assemble a tabs component over `pages`, initially showing `active`.
    inline auto make_tabs(std::vector<tab_page_cfg> pages, si32 active = 0) -> component
    {
        auto tc = std::make_shared<tabs_ctrl>();
        tc->pages     = std::move(pages);
        auto n = (si32)tc->pages.size();

        auto root = ui::fork::ctor(axis::Y);
        auto veer = ui::veer::ctor();
        for (auto& p : tc->pages) veer->attach(p.content.widget); // Attach in tab order.
        auto strip = make_tab_strip(std::weak_ptr<tabs_ctrl>(tc));
        root->attach(slot::_1, veer);
        root->attach(slot::_2, strip)->limits({ -1, 1 }, { -1, 1 }); // Strip pinned to 1 row.

        tc->root     = root;
        tc->veer_wp  = veer;
        tc->strip_wp = strip;

        // Detect the initially-visible (back) page, then rotate to the requested `active` tab.
        auto cur = std::max(0, n - 1);
        for (auto i = si32{}; i < n; ++i) if (veer->back() == tc->pages[(size_t)i].content.widget) { cur = i; break; }
        tc->active = cur;
        tc->select(std::clamp(active, 0, std::max(0, n - 1)), nullptr);
        return {
            root,
            [tc]{ tc->on_activate(); },
            [tc]{ tc->on_deactivate(); },
        };
    }
}
