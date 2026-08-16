// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/components/dropdown.hpp: A retained, single-selection form dropdown.
//
// This is an independent, control-focused port of application.hpp's popup-menu
// interaction. It deliberately omits cascading/menu-bar features while keeping
// the selector behavior needed by forms: a radio-marked option list, keyboard
// navigation, one active popup, outside-click passthrough, and viewport-aware
// placement.

#include "ui.hpp"

#include <functional>
#include <limits>

namespace netxs::app::parvion
{
    struct dropdown_option
    {
        text label;
        bool enabled = true;
    };

    struct dropdown_palette
    {
        ui32 background          = theme::sel_bg;
        ui32 foreground          = theme::text_fg;
        ui32 disabled_foreground = theme::subtext;
        ui32 popup_background    = 0xFF3D3E50u;
        ui32 popup_hover         = 0xFF555668u;
    };

    struct dropdown_cfg
    {
        std::vector<dropdown_option> options;
        std::function<si32()> selected;
        std::function<void(si32)> on_change;
        std::function<bool()> enabled; // Null means enabled.
        si32 width = -1;               // Negative/zero means widest-option width.
        dropdown_palette palette{};
    };

    struct dropdown_state
    {
        bool focused = faux;
        bool hover = faux;
        bool press = faux;
        bool open = faux;
    };

    struct dropdown_model
    {
        dropdown_cfg config;
        dropdown_state state;
        netxs::wptr<ui::base> trigger;
    };

    struct dropdown_popup
    {
        netxs::sptr<dropdown_model> model;
        ui::sptr host;
        netxs::wptr<ui::base> host_shadow;
        netxs::wptr<ui::base> overlay;
        netxs::sptr<hook> keyboard_hook;
        netxs::sptr<hook> mouse_hook;
        netxs::sptr<si32> highlighted;
        netxs::sptr<rect> painted;
        // Stable link targets bind mouse activation to an option index without
        // depending on which retained coordinate space a host reports.
        std::vector<id_t> row_ids;
        rect trigger_area{};
        si32 width{};
        bool dismiss_queued = faux;
    };

    using dropdown_popup_ptr = netxs::sptr<dropdown_popup>;

    inline auto active_dropdown_popup() -> dropdown_popup_ptr&
    {
        static auto popup = dropdown_popup_ptr{};
        return popup;
    }

    inline auto dropdown_enabled(dropdown_cfg const& cfg) -> bool
    {
        return !cfg.enabled || cfg.enabled();
    }

    inline auto dropdown_selected(dropdown_cfg const& cfg) -> si32
    {
        auto selected = cfg.selected ? cfg.selected() : si32{};
        return selected >= 0 && selected < (si32)cfg.options.size() ? selected : -1;
    }

    inline auto dropdown_default_width(dropdown_cfg const& cfg) -> si32
    {
        auto widest = si32{};
        for (auto& option : cfg.options) widest = std::max(widest, cell_width(option.label));
        return std::max(4, widest + 4); // Leading/trailing pad, gap, and arrow.
    }

    inline auto dropdown_resolved_width(dropdown_cfg const& cfg) -> si32
    {
        return cfg.width > 0 ? std::max(1, cfg.width) : dropdown_default_width(cfg);
    }

    inline auto dropdown_trigger_area(ui::base& trigger, ui::sptr const& host) -> rect
    {
        auto coor = twod{};
        for (auto walk = ui::sptr{ trigger.This() }; walk && walk != host;
             walk = walk->base::parent())
        {
            coor += walk->base::region.coor;
        }
        return { coor, trigger.base::region.size };
    }

    inline auto dropdown_host(ui::base& trigger) -> ui::sptr
    {
        auto host = ui::sptr{};
        auto walk = ui::sptr{ trigger.This() };
        auto last_below_root = walk;
        while (walk)
        {
            if (walk->base::kind() == ui::base::reflow_root)
            {
                host = walk;
                break;
            }
            auto parent = walk->base::parent();
            if (!parent) break;
            last_below_root = walk;
            walk = parent;
        }
        if (!host) host = last_below_root;
        if (host.get() == static_cast<ui::base*>(&trigger)) return {};
        return host;
    }

    inline auto dropdown_visible_in_viewports(ui::base& trigger,
                                              ui::sptr const& host) -> bool
    {
        auto area = rect{ {}, trigger.base::region.size };
        auto walk = ui::sptr{ trigger.This() };
        while (auto parent = walk->base::parent())
        {
            area.coor += walk->base::region.coor;
            if (parent == host) break;

            // Viewport ownership is an event contract, not a concrete widget
            // dependency. An unchanged negative size means that this ancestor
            // does not impose a viewport.
            auto viewport = rect{ {}, { -1, -1 } };
            parent->base::signal(tier::request, e2::form::prop::viewport, viewport);
            if (viewport.size.x >= 0 && viewport.size.y >= 0
             && !area.overlap(viewport))
            {
                return faux;
            }
            walk = parent;
        }
        return true;
    }

    inline void dismiss_dropdown(dropdown_popup_ptr popup)
    {
        if (!popup) return;
        if (auto overlay = popup->overlay.lock()) overlay->base::detach();
        popup->overlay.reset();
        if (popup->keyboard_hook) popup->keyboard_hook->reset();
        if (popup->mouse_hook) popup->mouse_hook->reset();
        popup->keyboard_hook.reset();
        popup->mouse_hook.reset();
        if (popup->model)
        {
            popup->model->state.open = faux;
            if (auto trigger = popup->model->trigger.lock()) trigger->base::deface();
        }
        if (auto host = popup->host_shadow.lock()) host->base::deface();
        if (active_dropdown_popup() == popup) active_dropdown_popup().reset();
    }

    inline void defer_dropdown_dismiss(dropdown_popup_ptr const& popup)
    {
        if (!popup || std::exchange(popup->dismiss_queued, true)) return;
        auto weak = netxs::wptr<dropdown_popup>{ popup };
        popup->host->base::enqueue([weak](ui::base&)
        {
            if (auto current = weak.lock())
            {
                current->dismiss_queued = faux;
                if (active_dropdown_popup() == current) dismiss_dropdown(current);
            }
        });
    }

    inline auto dropdown_step(dropdown_cfg const& cfg, si32 from, si32 direction) -> si32
    {
        auto count = (si32)cfg.options.size();
        if (!count) return -1;
        auto start = from < 0 ? (direction > 0 ? count - 1 : 0) : from;
        for (auto step = si32{ 1 }; step <= count; ++step)
        {
            auto index = ((start + direction * step) % count + count) % count;
            if (cfg.options[(size_t)index].enabled) return index;
        }
        return -1;
    }

    inline void dropdown_activate(dropdown_popup_ptr const& popup, si32 index)
    {
        if (!popup || !popup->model) return;
        auto& cfg = popup->model->config;
        if (index < 0 || index >= (si32)cfg.options.size()
         || !cfg.options[(size_t)index].enabled) return;
        if (cfg.on_change) cfg.on_change(index);
        dismiss_dropdown(popup);
    }

    inline auto dropdown_popup_coord(rect area, twod point) -> twod
    {
        if (area.hittest(point)) return point;
        // Linked overlay cells can report coordinates relative to the
        // painted popup instead of the host. Normalize that form back into
        // the host space used by `painted`.
        auto translated = point + area.coor;
        return area.hittest(translated) ? translated : point;
    }

    inline void dropdown_render(dropdown_state& state, dropdown_cfg const& cfg,
                                auto& canvas, twod size)
    {
        if (size.x <= 0 || size.y <= 0) return;
        auto enabled = dropdown_enabled(cfg);
        if (!enabled) state.hover = state.press = faux;
        auto bg = cfg.palette.background;
        auto fg = enabled ? cfg.palette.foreground
                          : cfg.palette.disabled_foreground;
        canvas.fill(rect{ {}, size }, [&](cell& c){ c.bgc(bg).fgc(fg); });
        auto selected = dropdown_selected(cfg);
        auto label = selected >= 0 ? cfg.options[(size_t)selected].label : text{};
        auto label_width = std::max(0, size.x - 4);
        auto fitted = fit_ellipsis(label, label_width);
        if (size.x > 1) put_str(canvas, 1, 0, fitted, fg, bg, label_width);
        if (size.x > 2) put_str(canvas, size.x - 2, 0, "▾", fg, bg, 1);
        if      (enabled && state.press) canvas.fill(rect{ {}, size }, [](cell& c){ c.xlight(2); });
        else if (enabled && state.hover) canvas.fill(rect{ {}, size }, [](cell& c){ c.xlight(); });
    }

    inline void open_dropdown(netxs::sptr<dropdown_model> const& model, ui::base& trigger)
    {
        if (!model || model->config.options.empty() || !dropdown_enabled(model->config)) return;
        if (model->state.open)
        {
            dismiss_dropdown(active_dropdown_popup());
            return;
        }
        if (auto active = active_dropdown_popup()) dismiss_dropdown(active);
        auto host = dropdown_host(trigger);
        if (!host) return;

        auto popup = ptr::shared<dropdown_popup>();
        popup->model = model;
        popup->host = host;
        popup->host_shadow = ptr::shadow(host);
        popup->keyboard_hook = ptr::shared<hook>();
        popup->mouse_hook = ptr::shared<hook>();
        popup->highlighted = ptr::shared<si32>(dropdown_selected(model->config));
        if (*popup->highlighted < 0
         || !model->config.options[(size_t)*popup->highlighted].enabled)
            *popup->highlighted = dropdown_step(model->config, -1, +1);
        popup->painted = ptr::shared<rect>();
        popup->trigger_area = dropdown_trigger_area(trigger, host);
        popup->width = std::max(dropdown_resolved_width(model->config),
                                dropdown_default_width(model->config));
        model->state.open = true;
        model->trigger = ptr::shadow(ui::sptr{ trigger.This() });
        active_dropdown_popup() = popup;

        // A popup is a paint/event overlay, not a layout-focusable control.
        // Keeping it as a plain mock matches application.hpp: linked popup
        // cells route directly to its handlers in the host coordinate space.
        auto overlay = ui::mock::ctor();
        popup->overlay = ptr::shadow(overlay);
        auto popup_weak = netxs::wptr<dropdown_popup>{ popup };
        for (auto index = si32{}; index < (si32)model->config.options.size(); ++index)
        {
            auto row_target = ui::mock::ctor();
            auto activate = [popup_weak, index](hids& gear)
            {
                if (auto current = popup_weak.lock()) dropdown_activate(current, index);
                gear.dismiss();
            };
            row_target->on(tier::mouserelease, input::key::LeftDown, activate);
            row_target->on(tier::mouserelease, input::key::LeftClick, activate);
            popup->row_ids.push_back(row_target->bell::id);
            overlay->base::attach(row_target);
        }
        overlay->invoke([popup](auto& boss)
        {
            // LISTEN bodies capture local names by reference. Keep the shared
            // popup handle in the widget field rather than referring to the
            // temporary invoke closure's by-value capture.
            auto& popup_state = boss.base::field(dropdown_popup_ptr{ popup });
            auto overlay_id = boss.bell::id;
            boss.LISTEN(tier::release, e2::render::any, canvas)
            {
                if (!popup_state->model || !popup_state->host) return;
                auto& cfg = popup_state->model->config;
                auto host_size = popup_state->host->base::region.size;
                auto trigger = popup_state->model->trigger.lock();
                if (!trigger
                 || !dropdown_visible_in_viewports(*trigger, popup_state->host))
                {
                    defer_dropdown_dismiss(popup_state);
                    return;
                }
                popup_state->trigger_area = dropdown_trigger_area(*trigger, popup_state->host);
                auto count = (si32)cfg.options.size();
                auto width = std::min(popup_state->width, std::max(0, host_size.x));
                auto x = std::clamp(popup_state->trigger_area.coor.x, 0,
                                    std::max(0, host_size.x - width));
                auto below = popup_state->trigger_area.coor.y + popup_state->trigger_area.size.y + 1;
                auto above = popup_state->trigger_area.coor.y - count - 1;
                auto y = below + count <= host_size.y ? below
                       : above >= 0                    ? above
                       : std::clamp(below, 0, std::max(0, host_size.y - count));
                auto visible = std::min(count, std::max(0, host_size.y - y));
                *popup_state->painted = rect{ { x, y }, { width, visible } };

                for (auto row = si32{}; row < visible; ++row)
                {
                    auto& option = cfg.options[(size_t)row];
                    auto hover = row == *popup_state->highlighted && option.enabled;
                    auto bg = hover ? cfg.palette.popup_hover : cfg.palette.popup_background;
                    auto fg = option.enabled ? cfg.palette.foreground
                                             : cfg.palette.disabled_foreground;
                    auto row_id = popup_state->row_ids[(size_t)row];
                    canvas.fill(rect{ { x, y + row }, { width, 1 } }, [=](cell& c)
                    {
                        c.wipe();
                        c.bgc(bg).fgc(fg).txt(whitespace).link(row_id);
                    });
                    auto selected = row == dropdown_selected(cfg);
                    put_str(canvas, x + 1, y + row, selected ? "◉" : "○", fg, bg,
                            std::max(0, width - 1));
                    if (width > 4)
                    {
                        auto fitted = fit_ellipsis(option.label, width - 4);
                        put_str(canvas, x + 3, y + row, fitted, fg, bg, width - 4);
                    }
                }
                auto has_top = y > 0;
                auto has_bottom = y + visible < host_size.y;
                if (has_top)
                {
                    canvas.fill(rect{ { x, y - 1 }, { width, 1 } }, [=](cell& c)
                    {
                        c.st.wipe(); c.px.wipe();
                        c.fgc(cfg.palette.popup_background).txt("▄").link(overlay_id);
                    });
                }
                if (has_bottom)
                {
                    canvas.fill(rect{ { x, y + visible }, { width, 1 } }, [=](cell& c)
                    {
                        c.st.wipe(); c.px.wipe();
                        c.fgc(cfg.palette.popup_background).txt("▀").link(overlay_id);
                    });
                }
            };
            boss.on(tier::mouserelease, input::key::MouseMove, [popup, &boss](hids& gear)
            {
                auto area = *popup->painted;
                auto point = dropdown_popup_coord(area,
                    twod{ (si32)gear.coord.x, (si32)gear.coord.y });
                auto next = si32{ -1 };
                if (area.hittest(point))
                {
                    auto row = point.y - area.coor.y;
                    if (row >= 0 && row < (si32)popup->model->config.options.size()
                     && popup->model->config.options[(size_t)row].enabled) next = row;
                }
                if (*popup->highlighted != next)
                {
                    *popup->highlighted = next;
                    boss.base::deface();
                }
            });
            auto activate_at = [popup](hids& gear)
            {
                auto area = *popup->painted;
                auto point = dropdown_popup_coord(area,
                    twod{ (si32)gear.coord.x, (si32)gear.coord.y });
                if (area.hittest(point))
                {
                    dropdown_activate(popup, point.y - area.coor.y);
                    gear.dismiss();
                }
            };
            boss.on(tier::mouserelease, input::key::LeftDown, activate_at);
            boss.on(tier::mouserelease, input::key::LeftClick, activate_at);
        });
        // Popup overlays do not participate in host layout. base::attach()
        // resizes the complete reflow root; during that transient pass a
        // nested viewport can be measured unconstrained and clamp its saved
        // offset to zero. Register and size only the overlay instead.
        host->base::_attach(overlay);
        overlay->base::signal(tier::release, e2::form::upon::vtree::attached,
                              host->base::This());
        overlay->base::extend(rect{ {}, host->base::region.size });
        host->base::deface();

        host->on(tier::mousepreview, input::key::MouseAny, *popup->mouse_hook,
            [popup](hids& gear)
            {
                auto cause = gear.cause;
                if (cause != input::key::LeftClick
                 && cause != input::key::RightClick
                 && cause != input::key::MiddleClick
                 && cause != input::key::LeftDown
                 && cause != input::key::RightDown
                 && cause != input::key::MiddleDown) return;
                auto point = twod{ (si32)gear.coord.x, (si32)gear.coord.y };
                if (popup->trigger_area.hittest(point)) return;
                if (popup->painted->hittest(point))
                {
                    // The host preview is the stable coordinate-space seam for
                    // retained overlays. Activate on the press and consume it;
                    // the overlay's LeftClick handler remains as the normal
                    // linked-cell path for hosts that dispatch it directly.
                    if (cause == input::key::LeftDown || cause == input::key::LeftClick)
                    {
                        dropdown_activate(popup, point.y - popup->painted->coor.y);
                        gear.dismiss();
                    }
                    return;
                }
                dismiss_dropdown(popup); // Deliberately do not consume the underlying click.
            });

        host->bell::submit(tier::preview, input::events::keybd::any, *popup->keyboard_hook)
            = [popup](hids& gear)
            {
                if (gear.keybd::handled || gear.payload != input::keybd::type::keypress) return;
                if (gear.keystat != input::key::pressed
                 && gear.keystat != input::key::repeated)
                {
                    gear.set_handled(faux);
                    return;
                }
                auto key = gear.keybd::generic();
                if (key == input::key::Esc)
                {
                    dismiss_dropdown(popup);
                }
                else if (key == input::key::KeyDownArrow)
                {
                    auto next = dropdown_step(popup->model->config, *popup->highlighted, +1);
                    if (next >= 0) *popup->highlighted = next;
                    if (auto overlay = popup->overlay.lock()) overlay->base::deface();
                }
                else if (key == input::key::KeyUpArrow)
                {
                    auto next = dropdown_step(popup->model->config, *popup->highlighted, -1);
                    if (next >= 0) *popup->highlighted = next;
                    if (auto overlay = popup->overlay.lock()) overlay->base::deface();
                }
                else if (key == input::key::KeyEnter || key == input::key::Space)
                {
                    dropdown_activate(popup, *popup->highlighted);
                }
                gear.set_handled(faux); // The open popup owns all keyboard input.
            };
        trigger.base::deface();
    }

    inline auto make_dropdown(dropdown_cfg cfg) -> component
    {
        auto width = dropdown_resolved_width(cfg);
        auto model = ptr::shared<dropdown_model>();
        model->config = std::move(cfg);
        auto form = ui::mock::ctor()->active()
            ->plugin<pro::mouse>()
            ->plugin<pro::focus>(pro::focus::mode::focusable)
            ->plugin<pro::keybd>()
            ->limits({ width, 1 }, { width, 1 });
        model->trigger = ptr::shadow(form);
        form->invoke([model](auto& boss)
        {
            // Persist the model for LISTEN's reference captures; the invoke
            // closure itself is destroyed as soon as registration finishes.
            auto& runtime = boss.base::field(netxs::sptr<dropdown_model>{ model });
            boss.LISTEN(tier::release, e2::render::any, canvas)
            {
                dropdown_render(runtime->state, runtime->config, canvas, boss.base::size());
            };
            boss.LISTEN(tier::release, e2::form::state::focus::count, count)
            {
                auto focused = count != 0;
                if (runtime->state.focused != focused)
                {
                    runtime->state.focused = focused;
                    boss.base::deface();
                }
            };
            boss.on(tier::mouserelease, input::key::MouseMove, [&](hids&)
            {
                if (!runtime->state.hover && dropdown_enabled(runtime->config))
                {
                    runtime->state.hover = true;
                    boss.base::deface();
                }
            });
            boss.on(tier::mouserelease, input::key::MouseLeave, [&](hids&)
            {
                if (runtime->state.hover || runtime->state.press)
                {
                    runtime->state.hover = runtime->state.press = faux;
                    boss.base::deface();
                }
            });
            boss.on(tier::mouserelease, input::key::LeftDown, [&](hids& gear)
            {
                if (dropdown_enabled(runtime->config))
                {
                    pro::focus::set(boss.This(), gear.id, solo::on);
                    runtime->state.press = true;
                    boss.base::deface();
                }
                gear.dismiss();
            });
            boss.on(tier::mouserelease, input::key::LeftUp, [&](hids& gear)
            {
                if (runtime->state.press)
                {
                    runtime->state.press = faux;
                    boss.base::deface();
                }
                gear.dismiss();
            });
            boss.on(tier::mouserelease, input::key::LeftClick, [&](hids& gear)
            {
                open_dropdown(runtime, boss);
                gear.dismiss();
            });
            boss.LISTEN(tier::preview, input::events::keybd::any, gear)
            {
                if (!runtime->state.focused || runtime->state.open || gear.keybd::handled
                 || gear.payload != input::keybd::type::keypress
                 || gear.keystat == input::key::released
                 || gear.keystat == input::key::interrupted) return;
                auto key = gear.keybd::generic();
                if (key == input::key::Space || key == input::key::KeyEnter
                 || key == input::key::KeyDownArrow)
                {
                    open_dropdown(runtime, boss);
                    gear.set_handled();
                }
            };
        });
        return { form };
    }
}
