// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/components/button.hpp: the reusable button core component.
//
// The widget owns its mouse interaction and transient hover/press state. Callers provide live
// label/enabled data and an activation adapter, then position the returned widget in their layout.

#include "ui.hpp"

#include <functional>

namespace netxs::app::parvion
{
    struct button_palette
    {
        ui32 background          = theme::sel_bg;
        ui32 foreground          = theme::text_fg;
        ui32 disabled_foreground = theme::subtext;
    };

    struct button_cfg
    {
        std::function<text()> label;
        std::function<void(hids&, ui::base&)> on_activate;
        std::function<bool()> enabled; // Null means enabled.
        button_palette palette{};
    };

    struct button_state
    {
        bool hover = faux;
        bool press = faux;
    };

    inline auto button_hit(twod size, si32 mx, si32 my) -> bool
    {
        return mx >= 0 && mx < size.x && my >= 0 && my < size.y;
    }

    inline auto button_enabled(button_cfg const& cfg) -> bool
    {
        return !cfg.enabled || cfg.enabled();
    }

    inline void button_render(button_state& state, button_cfg const& cfg, auto& canvas, twod size)
    {
        if (size.x <= 0 || size.y <= 0) return;
        auto& pal = cfg.palette;
        auto enabled = button_enabled(cfg);
        if (!enabled) state.hover = state.press = faux;
        canvas.fill(rect{ {}, size }, [&](cell& c){ c.bgc(pal.background); });
        auto label = cfg.label ? cfg.label() : text{};
        auto width = cell_width(label);
        auto x = std::max(0, (size.x - width) / 2);
        auto y = std::max(0, (size.y - 1) / 2);
        put_str(canvas, x, y, label, enabled ? pal.foreground : pal.disabled_foreground,
                pal.background, std::max(0, size.x - x));
        if      (enabled && state.press) canvas.fill(rect{ {}, size }, [](cell& c){ c.xlight(2); });
        else if (enabled && state.hover) canvas.fill(rect{ {}, size }, [](cell& c){ c.xlight(); });
    }

    inline auto make_button(button_cfg cfg) -> component
    {
        auto form = ui::mock::ctor()->active()->plugin<pro::mouse>();
        form->invoke([cfgv = std::move(cfg)](auto& boss)
        {
            auto& state = boss.base::field(button_state{});
            auto& cfg   = boss.base::field(button_cfg{ cfgv });
            boss.LISTEN(tier::release, e2::render::any, parent_canvas)
            {
                button_render(state, cfg, parent_canvas, boss.base::size());
            };
            boss.on(tier::mouserelease, input::key::MouseMove, [&](hids& gear)
            {
                if (!button_enabled(cfg))
                {
                    if (state.hover || state.press)
                    {
                        state.hover = state.press = faux;
                        boss.base::deface();
                    }
                    return;
                }
                auto over = button_hit(boss.base::size(), (si32)gear.coord.x, (si32)gear.coord.y);
                auto dirty = faux;
                if (state.hover != over) { state.hover = over; dirty = true; }
                if (state.press && !over) { state.press = faux; dirty = true; }
                if (dirty) boss.base::deface();
            });
            boss.on(tier::mouserelease, input::key::MouseLeave, [&](hids&)
            {
                if (state.hover || state.press)
                {
                    state.hover = state.press = faux;
                    boss.base::deface();
                }
            });
            boss.on(tier::mouserelease, input::key::LeftDown, [&](hids& gear)
            {
                if (button_enabled(cfg) && !state.press)
                {
                    state.press = true;
                    boss.base::deface();
                }
                gear.dismiss();
            });
            boss.on(tier::mouserelease, input::key::LeftUp, [&](hids& gear)
            {
                if (state.press)
                {
                    state.press = faux;
                    boss.base::deface();
                }
                gear.dismiss();
            });
            boss.on(tier::mouserelease, input::key::LeftClick, [&](hids& gear)
            {
                if (button_enabled(cfg) && cfg.on_activate) cfg.on_activate(gear, boss);
                gear.dismiss();
            });
        });
        return { std::move(form) };
    }
}
