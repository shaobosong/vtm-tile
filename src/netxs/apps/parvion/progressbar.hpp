// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/progressbar.hpp: the reusable progress-bar core component.
//
// The retained widget uses the same geometry renderer as the unit-testable paint primitive.

#include "ui.hpp"

#include <cmath>
#include <unordered_map>

namespace netxs::app::parvion
{
    enum class progressbar_alignment
    {
        left,
        center_left,
        center_right,
        right,
    };

    inline constexpr auto progressbar_percentage_width = si32{ 7 }; // "100.00%".

    struct progressbar_palette
    {
        ui32 track      = theme::surface;
        ui32 fill       = theme::sel_bg;
        ui32 foreground = theme::text_fg;
    };

    struct progressbar_cfg
    {
        std::function<double()> fraction;
        std::function<text()> label;
        std::function<ui32()> foreground;
        progressbar_alignment alignment = progressbar_alignment::center_right;
        si32 label_field_width = progressbar_percentage_width;
        progressbar_palette palette{};
    };

    struct progressbar_value
    {
        double fraction = 0.0;
        text label;
        progressbar_alignment alignment = progressbar_alignment::center_right;
        si32 label_field_width = progressbar_percentage_width;
        progressbar_palette palette{};
    };

    inline auto progressbar_fraction(double value) -> double
    {
        return std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
    }

    inline auto progressbar_fill_width(si32 width, double fraction) -> si32
    {
        if (width <= 0) return 0;
        auto value = progressbar_fraction(fraction);
        return value >= 1.0 ? width : (si32)std::floor(value * width);
    }

    inline auto progressbar_intersect(rect area, rect clip) -> rect
    {
        auto x0 = std::max(area.coor.x, clip.coor.x);
        auto y0 = std::max(area.coor.y, clip.coor.y);
        auto x1 = std::min(area.coor.x + area.size.x, clip.coor.x + clip.size.x);
        auto y1 = std::min(area.coor.y + area.size.y, clip.coor.y + clip.size.y);
        return { { x0, y0 }, { std::max(0, x1 - x0), std::max(0, y1 - y0) } };
    }

    inline auto progressbar_label_x(rect area, si32 label_width,
                                    progressbar_alignment alignment,
                                    si32 label_field_width = progressbar_percentage_width) -> si32
    {
        auto area_width = std::max(0, area.size.x);
        auto width = std::clamp(label_width, 0, area_width);
        auto field = std::clamp(std::max(width, label_field_width), 0, area_width);
        auto free = area_width - field;
        // Center modes position a fixed-width field with any odd spare cell on the right,
        // then align the label within that field.
        auto offset = alignment == progressbar_alignment::left         ? 0
                    : alignment == progressbar_alignment::center_left  ? free / 2
                    : alignment == progressbar_alignment::center_right ? free / 2 + field - width
                                                                       : area_width - width;
        return area.coor.x + offset;
    }

    // Paint foreground glyphs without replacing the track/fill backgrounds already underneath.
    inline void progressbar_label(auto& canvas, si32 x, si32 y, view label, ui32 fg, rect clip)
    {
        auto cx = x;
        utf::decode_clusters(label, [&](view cluster)
        {
            auto frag = utf::cluster(cluster);
            auto size = utf::matrix::whxy(frag.attr.cmatrix);
            auto width = std::max(1, (si32)size.w);
            if (cx >= clip.coor.x + clip.size.x) return faux;
            if (y >= clip.coor.y && y < clip.coor.y + clip.size.y
             && cx >= clip.coor.x && cx + width <= clip.coor.x + clip.size.x)
            {
                if (width == 1)
                    canvas.fill(rect{{ cx, y }, { 1, 1 }}, [&](cell& c){ c.fgc(fg).txt(frag.text); });
                else
                {
                    canvas.fill(rect{{ cx,     y }, { 1, 1 }}, [&](cell& c){ c.fgc(fg).txt(frag.text).wdt(size.w, size.h, 1, 1); });
                    canvas.fill(rect{{ cx + 1, y }, { 1, 1 }}, [&](cell& c){ c.fgc(fg).txt(frag.text).wdt(size.w, size.h, 2, 1); });
                }
            }
            cx += width;
            return true;
        });
    }

    inline void progressbar_render(progressbar_value const& value, auto& canvas, rect area, rect clip)
    {
        if (area.size.x <= 0 || area.size.y <= 0 || clip.size.x <= 0 || clip.size.y <= 0) return;
        auto visible = progressbar_intersect(area, clip);
        if (visible.size.x <= 0 || visible.size.y <= 0) return;

        auto& pal = value.palette;
        canvas.fill(visible, [&](cell& c){ c.bgc(pal.track); });

        auto fill = area;
        fill.size.x = progressbar_fill_width(area.size.x, value.fraction);
        fill = progressbar_intersect(fill, clip);
        if (fill.size.x > 0 && fill.size.y > 0)
            canvas.fill(fill, [&](cell& c){ c.bgc(pal.fill); });

        auto label = fit_ellipsis(value.label, area.size.x);
        auto lx = progressbar_label_x(area, cell_width(label), value.alignment,
                                      value.label_field_width);
        auto ly = area.coor.y + std::max(0, (area.size.y - 1) / 2);
        progressbar_label(canvas, lx, ly, label, pal.foreground, clip);
    }

    inline void progressbar_render(progressbar_value const& value, auto& canvas, rect area)
    {
        progressbar_render(value, canvas, area, area);
    }

    inline auto make_progressbar(progressbar_cfg cfg) -> component
    {
        auto form = ui::mock::ctor();
        form->invoke([cfgv = std::move(cfg)](auto& boss)
        {
            auto& cfg = boss.base::field(progressbar_cfg{ cfgv });
            boss.LISTEN(tier::release, e2::render::any, canvas)
            {
                auto palette = cfg.palette;
                if (cfg.foreground) palette.foreground = cfg.foreground();
                auto value = progressbar_value{
                    .fraction = cfg.fraction ? cfg.fraction() : 0.0,
                    .label = cfg.label ? cfg.label() : text{},
                    .alignment = cfg.alignment,
                    .label_field_width = cfg.label_field_width,
                    .palette = palette,
                };
                progressbar_render(value, canvas, rect{ {}, boss.base::size() });
            };
        });
        return { std::move(form) };
    }

    // Stable retained progress-bar instances for virtualized containers such as tables. Callers
    // provide a model identity (not a transient sorted row number), update the live state, and keep
    // receiving the same component/widget while that model item exists.
    struct progressbar_live_state
    {
        double fraction = 0.0;
        text label;
        ui32 foreground = theme::text_fg;
    };
    struct progressbar_cache_entry
    {
        std::shared_ptr<progressbar_live_state> state;
        component content;
    };
    struct progressbar_cache
    {
        std::unordered_map<ui64, progressbar_cache_entry> entries;
    };
    inline auto cached_progressbar(progressbar_cache& cache, ui64 identity,
                                   double fraction, text label, ui32 foreground,
                                   progressbar_alignment alignment = progressbar_alignment::center_right,
                                   si32 label_field_width = progressbar_percentage_width) -> component
    {
        auto& entry = cache.entries[identity];
        if (!entry.state)
        {
            entry.state = std::make_shared<progressbar_live_state>();
            auto state = entry.state;
            entry.content = make_progressbar({
                .fraction = [state]{ return state->fraction; },
                .label = [state]{ return state->label; },
                .foreground = [state]{ return state->foreground; },
                .alignment = alignment,
                .label_field_width = label_field_width,
            });
        }
        entry.state->fraction = progressbar_fraction(fraction);
        entry.state->label = std::move(label);
        entry.state->foreground = foreground;
        return entry.content;
    }
}
