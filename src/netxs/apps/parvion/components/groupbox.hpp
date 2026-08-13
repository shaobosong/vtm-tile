// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/components/groupbox.hpp: A titled, single-child group box matching
// the former hand-painted Parvion Settings sections.

#include "ui.hpp"

namespace netxs::app::parvion
{
    struct groupbox_cfg
    {
        text title;
        component content;
        dent padding{ 1, 1, 0, 0 };
        ui32 background = theme::bg;
        ui32 border = theme::subtext;
        ui32 title_foreground = theme::text_fg;
    };

    class groupbox
        : public ui::form<groupbox>
    {
        groupbox_cfg config;
        ui::sptr content;
        ui::face groupbox_canvas;
        ui::face content_canvas;
        rect content_viewport{};
        rect content_area{};

        auto horizontal_inset() const -> si32
        {
            return 2 + config.padding.l + config.padding.r;
        }

        auto vertical_inset() const -> si32
        {
            return 2 + config.padding.t + config.padding.b;
        }

    protected:
        void deform(rect& new_area) override
        {
            auto title_width = cell_width(config.title) + 6;
            auto requested_width = new_area.size.x;
            if (requested_width <= 0)
            {
                auto probe = rect{};
                content->base::recalc(probe);
                requested_width = std::max(title_width, probe.size.x + horizontal_inset());
            }
            new_area.size.x = std::max(2, requested_width);

            auto inner_width = std::max(0, new_area.size.x - horizontal_inset());
            auto natural = rect{ {}, { inner_width, 0 } };
            content->base::recalc(natural);
            auto natural_height = natural.size.y + vertical_inset();
            new_area.size.y = std::max(new_area.size.y, std::max(2, natural_height));

            content_viewport = {
                { 1 + config.padding.l, 1 + config.padding.t },
                { inner_width, std::max(0, new_area.size.y - vertical_inset()) },
            };
            // Keep the child's logical allocation independent of its paint
            // viewport. Fixed-width controls and grids are allowed to retain
            // their minimum width, but their rendering must not consume the
            // groupbox padding or frame when that width no longer fits.
            content_area = content_viewport;
            content->base::recalc(content_area);
        }

        void inform(rect new_area) override
        {
            auto area = content_area;
            area.coor += new_area.coor;
            content->base::notify(area);
        }

    public:
        static constexpr auto classname = basename::groupbox;

        groupbox(groupbox_cfg setup)
            : config{ std::move(setup) },
              content{ config.content.widget }
        {
            if (!content) content = ui::mock::ctor();
            config.padding.l = std::max(0, config.padding.l);
            config.padding.r = std::max(0, config.padding.r);
            config.padding.t = std::max(0, config.padding.t);
            config.padding.b = std::max(0, config.padding.b);

            LISTEN(tier::release, e2::render::any, parent_canvas)
            {
                auto size = base::size();
                if (size.x <= 0 || size.y <= 0) return;

                // Paint through physically bounded faces.  Narrowing face::clip()
                // alone does not constrain primitives such as face::fill(rect, ...),
                // so an oversized groupbox or child could otherwise overwrite its
                // frame, neighboring controls, or an ancestor's viewport.
                groupbox_canvas.area(rect{ {}, size });
                groupbox_canvas.fill(parent_canvas, cell::shaders::full);
                groupbox_canvas.fill(rect{ {}, size }, [&](cell& c)
                {
                    c.bgc(config.background).fgc(config.title_foreground).txt(whitespace);
                });
                if (size.x >= 2 && size.y >= 2)
                {
                    auto x1 = size.x - 1;
                    auto y1 = size.y - 1;
                    auto put = [&](si32 x, si32 y, view glyph, ui32 color)
                    {
                        put_str(groupbox_canvas, x, y, glyph, color, config.background, 1);
                    };
                    put(0, 0, "┌", config.border);
                    put(x1, 0, "┐", config.border);
                    put(0, y1, "└", config.border);
                    put(x1, y1, "┘", config.border);
                    for (auto x = si32{ 1 }; x < x1; ++x)
                    {
                        put(x, 0, "─", config.border);
                        put(x, y1, "─", config.border);
                    }
                    for (auto y = si32{ 1 }; y < y1; ++y)
                    {
                        put(0, y, "│", config.border);
                        put(x1, y, "│", config.border);
                    }
                    // Keep the leading top-border dash in the border color and
                    // overlay only the spacing and title with the caption color.
                    auto caption = text{ " " } + config.title + " ";
                    put_str(groupbox_canvas, 2, 0, caption, config.title_foreground, config.background,
                            std::max(0, size.x - 3));
                }

                if (content_viewport.size.x > 0 && content_viewport.size.y > 0)
                {
                    content_canvas.area(content_viewport);
                    content_canvas.fill(groupbox_canvas, cell::shaders::full);
                    content->render(content_canvas);
                    netxs::onclip(groupbox_canvas, content_canvas, cell::shaders::full);
                }
                netxs::onclip(parent_canvas, groupbox_canvas, cell::shaders::full);
            };
        }

        auto get_content_area() const -> rect { return content_area; }

        static auto ctor(groupbox_cfg setup)
        {
            auto box = ui::tui_domain().create<groupbox>(std::move(setup));
            box->ui::base::attach(box->content);
            return box;
        }
    };

    inline auto make_groupbox(groupbox_cfg cfg) -> component
    {
        auto retained = cfg.content;
        return {
            groupbox::ctor(std::move(cfg)),
            [retained]{ retained.on_activate(); },
            [retained]{ retained.on_deactivate(); },
        };
    }
}
