// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/components/dialog.hpp: A reusable modal popup shell.
//
// The caller supplies retained components for the one-row title bar, flexible
// content area, and a one-row button bar with one-cell horizontal padding. The
// shell owns the dimming overlay, placement, sizing, outside-click dismissal,
// and Escape fallback.

#include "grid.hpp"

#include <cmath>

namespace netxs::app::parvion
{
    enum class dialog_anchor
    {
        top_left,
        top,
        top_right,
        left,
        center,
        right,
        bottom_left,
        bottom,
        bottom_right,
    };

    enum class dialog_length_mode
    {
        fixed,
        proportional,
    };

    struct dialog_length
    {
        dialog_length_mode mode = dialog_length_mode::fixed;
        fp64 value = 1.0;

        static auto cells(si32 value) -> dialog_length
        {
            return { dialog_length_mode::fixed, (fp64)value };
        }

        static auto ratio(fp64 value) -> dialog_length
        {
            return { dialog_length_mode::proportional, value };
        }
    };

    struct dialog_size
    {
        dialog_length width  = dialog_length::cells(72);
        dialog_length height = dialog_length::cells(30);
    };

    struct dialog_cfg
    {
        component title;
        component content;
        component buttons;
        dialog_anchor position = dialog_anchor::center;
        twod offset{};
        dialog_size size{};
        twod minimum{ 3, 3 };
        twod maximum{ -1, -1 }; // Negative means bounded only by the host.
        bool fit_content_height = faux; // Resolve width first, then derive height from content.
        ui32 background   = theme::bg;
        ui32 border_color = theme::surface;
        std::function<bool()> can_dismiss; // Null means dismissal is always allowed.
        std::function<void()> on_cancel;
    };

    inline auto resolve_dialog_length(dialog_length length, si32 host) -> si32
    {
        host = std::max(0, host);
        if (length.mode == dialog_length_mode::proportional)
        {
            auto ratio = std::clamp(length.value, 0.0, 1.0);
            return (si32)std::lround((fp64)host * ratio);
        }
        return std::max(0, (si32)std::lround(length.value));
    }

    inline auto resolve_dialog_rect(dialog_cfg const& cfg, twod host) -> rect
    {
        host = std::max(host, dot_00);
        auto size = twod{
            resolve_dialog_length(cfg.size.width,  host.x),
            resolve_dialog_length(cfg.size.height, host.y),
        };
        auto minimum = std::min(std::max(cfg.minimum, dot_00), host);
        auto maximum = twod{
            cfg.maximum.x < 0 ? host.x : std::clamp(cfg.maximum.x, 0, host.x),
            cfg.maximum.y < 0 ? host.y : std::clamp(cfg.maximum.y, 0, host.y),
        };
        maximum = std::max(maximum, minimum);
        size = std::clamp(size, minimum, maximum);

        auto x = si32{};
        auto y = si32{};
        switch (cfg.position)
        {
            case dialog_anchor::top:          x = (host.x - size.x) / 2; break;
            case dialog_anchor::top_right:    x =  host.x - size.x;      break;
            case dialog_anchor::left:         y = (host.y - size.y) / 2; break;
            case dialog_anchor::center:       x = (host.x - size.x) / 2;
                                              y = (host.y - size.y) / 2; break;
            case dialog_anchor::right:        x =  host.x - size.x;
                                              y = (host.y - size.y) / 2; break;
            case dialog_anchor::bottom_left:  y =  host.y - size.y;      break;
            case dialog_anchor::bottom:       x = (host.x - size.x) / 2;
                                              y =  host.y - size.y;      break;
            case dialog_anchor::bottom_right: x =  host.x - size.x;
                                              y =  host.y - size.y;      break;
            case dialog_anchor::top_left:
            default: break;
        }
        auto coor = twod{ x, y } + cfg.offset;
        coor = std::clamp(coor, dot_00, host - size);
        return { coor, size };
    }

    class dialog
        : public ui::form<dialog>
    {
        dialog_cfg config;
        netxs::sptr<grid> card;
        rect card_area{};
        bool dismissed = faux;
        bool backdrop_armed = faux;

    protected:
        void inform(rect new_area) override
        {
            card_area = resolve_dialog_rect(config, new_area.size);
            if (config.fit_content_height)
            {
                // Wrapping content cannot report its natural height until its
                // final width is known.  Measure it at the already-clamped card
                // width, add the shell's title and button rows, then resolve the
                // rectangle again so height bounds and anchoring still apply.
                auto content_area = rect{ {}, { card_area.size.x, 0 } };
                config.content.widget->base::recalc(content_area);
                auto fitted = config;
                fitted.size.height = dialog_length::cells(content_area.size.y + 2);
                card_area = resolve_dialog_rect(fitted, new_area.size);
            }
            card_area.coor += new_area.coor;
            card->base::limits(card_area.size, card_area.size);
            auto area = card_area;
            card->base::recalc(area);
            card->base::notify(area);
            card_area = area;
        }

    public:
        static constexpr auto classname = basename::dialog;

        dialog(dialog_cfg setup)
            : config{ std::move(setup) },
              card{ grid::ctor({
                  .columns = { { .weight = 1 } },
                  .rows = {
                      { .weight = 0, .minimum = 1, .maximum = 1 },
                      { .weight = 1, .minimum = 1 },
                      { .weight = 0, .minimum = 1, .maximum = 1 },
                  },
                  .handle_mode = grid_handle_mode::hidden,
                  .border = faux,
              }) }
        {
            card->attach(config.title, {
                .column = 0,
                .row = 0,
                .padding = dent{ 1, 1, 0, 0 },
            });
            card->attach(config.content, { .column = 0, .row = 1 });
            card->attach(config.buttons, {
                .column = 0,
                .row = 2,
                .padding = dent{ 1, 1, 0, 0 },
            });

            // Shade the already-rendered window in the background pass.  Keep the
            // existing glyphs intact: the backdrop is a color/link transform, not
            // an opaque whitespace layer.
            LISTEN(tier::release, e2::render::background::any, parent_canvas)
            {
                auto myid = bell::id;
                parent_canvas.fill([myid](cell& c)
                {
                    c.bgc().faint();
                    c.fgc().faint();
                    if (auto underline = c.unc())
                    {
                        auto color = argb{ argb::vt256[underline] };
                        color.faint();
                        c.unc(color);
                    }
                    c.cur(text_cursor::none);
                    c.link(myid);
                });
            };
            LISTEN(tier::release, e2::render::any, parent_canvas)
            {
                auto myid = bell::id;
                // Reset every cell in the card before painting it so attributes
                // from the underlying terminal (underline, bold, cursor,
                // hyperlink/picture metadata, and grapheme remnants) cannot
                // bleed through the dialog's title, content, or button regions.
                parent_canvas.fill(card_area, [&](cell& c)
                {
                    c.wipe();
                    c.bgc(config.background)
                     .fgc(theme::text_fg)
                     .txt(whitespace)
                     .link(myid);
                });
                auto title_area = rect{
                    card_area.coor,
                    { card_area.size.x, 1 },
                };
                auto button_area = rect{
                    { card_area.coor.x, card_area.coor.y + card_area.size.y - 1 },
                    { card_area.size.x, 1 },
                };
                parent_canvas.fill(title_area,  [](cell& c){ c.bgc(theme::header); });
                parent_canvas.fill(button_area, [](cell& c){ c.bgc(theme::surface); });
                card->render(parent_canvas);
            };
        }

        auto get_card_area() const -> rect { return card_area; }
        auto get_card() const -> netxs::sptr<grid> { return card; }
        auto backdrop_is_armed() const -> bool { return backdrop_armed; }
        void arm_backdrop() { backdrop_armed = true; }

        auto contains(twod point) const -> bool
        {
            return point.x >= card_area.coor.x
                && point.y >= card_area.coor.y
                && point.x <  card_area.coor.x + card_area.size.x
                && point.y <  card_area.coor.y + card_area.size.y;
        }

        void dismiss()
        {
            if (config.can_dismiss && !config.can_dismiss()) return;
            if (std::exchange(dismissed, true)) return;
            auto callback = config.on_cancel;
            base::detach();
            if (callback) callback();
        }

        static auto ctor(dialog_cfg setup)
        {
            auto popup = ui::tui_domain().create<dialog>(std::move(setup));
            popup->ui::base::attach(popup->card);
            return popup;
        }
    };

    inline auto make_dialog(dialog_cfg cfg) -> component
    {
        auto children = std::array<component, 3>{ cfg.title, cfg.content, cfg.buttons };
        auto popup = dialog::ctor(std::move(cfg))
            ->plugin<pro::mouse>()
            ->plugin<pro::focus>(pro::focus::mode::focusable)
            ->plugin<pro::keybd>();

        popup->invoke([](auto& boss)
        {
            boss.LISTEN(tier::release, e2::form::upon::vtree::attached, parent)
            {
                if (!parent) return;
                auto weak = ptr::shadow(boss.This());
                parent->base::enqueue([weak](auto&)
                {
                    if (auto popup = weak.lock()) popup->arm_backdrop();
                });
            };
            // The dialog is an overlay sibling of the main Parvion body, so the
            // body's event boundary cannot stop extended click events raised in
            // the dialog subtree from reaching the surrounding tile applet.
            boss.on(tier::mouserelease, input::key::MouseDoubleClick, [](hids& gear)
            {
                gear.dismiss(true);
            });
            boss.on(tier::mouserelease, input::key::MouseMultiClick, [](hids& gear)
            {
                gear.dismiss(true);
            });
            boss.on(tier::mouserelease, input::key::LeftClick, [&](hids& gear)
            {
                if (boss.backdrop_is_armed() && !boss.contains(twod{ gear.coord }))
                {
                    boss.dismiss();
                    gear.dismiss();
                }
                if (gear.meta(hids::anyCtrl | hids::anyAlt | hids::anyShift))
                {
                    gear.dismiss(true);
                }
            });
            boss.on(tier::mouserelease, input::key::MouseWheel, [](hids& gear)
            {
                if (gear.meta(hids::anyCtrl | hids::anyAlt | hids::anyShift))
                {
                    gear.dismiss(true);
                }
            });
            boss.LISTEN(tier::release, input::events::keybd::any, gear)
            {
                if (gear.keybd::handled
                 || gear.payload != input::keybd::type::keypress
                 || gear.keystat == input::key::released
                 || gear.keystat == input::key::interrupted) return;
                if (gear.keybd::generic() == input::key::Esc)
                {
                    gear.set_handled();
                    boss.dismiss();
                }
            };
        });

        return {
            popup,
            [children]
            {
                for (auto& child : children) child.on_activate();
            },
            [children]
            {
                for (auto& child : children) child.on_deactivate();
            },
        };
    }
}
