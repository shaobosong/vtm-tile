// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/components/checkbox.hpp: A reusable one-row checkbox control.

#include "ui.hpp"

#include <functional>

namespace netxs::app::parvion
{
    struct checkbox_palette
    {
        ui32 background          = theme::bg;
        ui32 foreground          = theme::text_fg;
        ui32 disabled_foreground = theme::subtext;
        ui32 focus_background    = theme::sel_bg;
    };

    struct checkbox_cfg
    {
        std::function<text()> label;
        std::function<bool()> checked;
        std::function<void(bool)> on_change;
        std::function<bool()> enabled; // Null means enabled.
        checkbox_palette palette{};
    };

    struct checkbox_state
    {
        bool focused = faux;
        bool hover = faux;
        bool press = faux;
    };

    inline auto checkbox_enabled(checkbox_cfg const& cfg) -> bool
    {
        return !cfg.enabled || cfg.enabled();
    }

    inline auto checkbox_value(checkbox_cfg const& cfg) -> bool
    {
        return cfg.checked && cfg.checked();
    }

    inline void checkbox_toggle(checkbox_cfg const& cfg)
    {
        if (checkbox_enabled(cfg) && cfg.on_change) cfg.on_change(!checkbox_value(cfg));
    }

    inline auto checkbox_natural_width(checkbox_cfg const& cfg) -> si32
    {
        auto label = cfg.label ? cfg.label() : text{};
        return 2 + cell_width(label); // Box glyph plus one separating cell.
    }

    inline void checkbox_render(checkbox_state& state, checkbox_cfg const& cfg,
                                auto& canvas, twod size)
    {
        if (size.x <= 0 || size.y <= 0) return;
        auto enabled = checkbox_enabled(cfg);
        if (!enabled) state.hover = state.press = faux;
        auto bg = enabled && state.focused ? cfg.palette.focus_background
                                           : cfg.palette.background;
        auto fg = enabled ? cfg.palette.foreground
                          : cfg.palette.disabled_foreground;
        canvas.fill(rect{ {}, size }, [&](cell& c){ c.bgc(bg).fgc(fg); });
        auto box = checkbox_value(cfg) ? "▣" : "□";
        put_str(canvas, 0, 0, box, fg, bg, std::min(1, size.x));
        if (size.x > 2)
        {
            auto label = cfg.label ? cfg.label() : text{};
            put_str(canvas, 2, 0, label, fg, bg, size.x - 2);
        }
        if      (enabled && state.press) canvas.fill(rect{ {}, size }, [](cell& c){ c.xlight(2); });
        else if (enabled && state.hover) canvas.fill(rect{ {}, size }, [](cell& c){ c.xlight(); });
    }

    inline auto make_checkbox(checkbox_cfg cfg) -> component
    {
        auto width = std::max(2, checkbox_natural_width(cfg));
        auto form = ui::mock::ctor()->active()
            ->plugin<pro::mouse>()
            ->plugin<pro::focus>(pro::focus::mode::focusable)
            ->plugin<pro::keybd>()
            ->limits({ width, 1 }, { width, 1 });
        form->invoke([cfgv = std::move(cfg)](auto& boss)
        {
            auto& state = boss.base::field(checkbox_state{});
            auto& cfg = boss.base::field(checkbox_cfg{ cfgv });
            boss.LISTEN(tier::release, e2::render::any, canvas)
            {
                checkbox_render(state, cfg, canvas, boss.base::size());
            };
            boss.LISTEN(tier::release, e2::form::state::focus::count, count)
            {
                auto focused = count != 0;
                if (state.focused != focused)
                {
                    state.focused = focused;
                    boss.base::deface();
                }
            };
            boss.on(tier::mouserelease, input::key::MouseMove, [&](hids&)
            {
                if (!state.hover && checkbox_enabled(cfg))
                {
                    state.hover = true;
                    boss.base::deface();
                }
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
                if (checkbox_enabled(cfg))
                {
                    pro::focus::set(boss.This(), gear.id, solo::on);
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
                checkbox_toggle(cfg);
                boss.base::deface();
                gear.dismiss();
            });
            boss.LISTEN(tier::preview, input::events::keybd::any, gear)
            {
                if (!state.focused || gear.keybd::handled
                 || gear.payload != input::keybd::type::keypress
                 || gear.keystat == input::key::released
                 || gear.keystat == input::key::interrupted) return;
                auto key = gear.keybd::generic();
                if (key == input::key::Space || key == input::key::KeyEnter)
                {
                    checkbox_toggle(cfg);
                    boss.base::deface();
                    gear.set_handled();
                }
            };
        });
        return { form };
    }
}
