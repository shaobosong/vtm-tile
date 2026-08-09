// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/components/grid.hpp: A reusable row/column layout for retained Parvion
// components. Handle behavior is selected through grid_cfg:
//
//   enabled  — draggable handles with hover feedback and double-click reset.
//   disabled — inert, solid boundary lines.
//   hidden   — no handle objects, paint, hit targets, or reserved cells.
//
// Cells may span tracks.  A spanning child is painted after the handles, so it
// naturally covers internal handles crossed by the span (the bottom tabs in the
// main Parvion layout use this to span both pane columns).
//
// Independent inner padding can inset the tracks from the component edges or
// from an optional outer border.  The border's vertical sides take
// column_border_width cells and its horizontal sides row_border_height.

#include "ui.hpp"

#include <limits>
#include <numeric>

namespace netxs::app::parvion
{
    enum class grid_handle_mode
    {
        enabled,
        disabled,
        hidden,
    };

    struct grid_track
    {
        si32 weight  = 1;
        si32 minimum = 0;
        si32 maximum = -1; // Negative means unbounded.
    };

    struct grid_cell
    {
        si32 column     = 0;
        si32 row        = 0;
        si32 column_span = 1;
        si32 row_span    = 1;
    };

    struct grid_cfg
    {
        std::vector<grid_track> columns;
        std::vector<grid_track> rows;
        grid_handle_mode handle_mode = grid_handle_mode::enabled;
        si32 column_handle_width = 2;
        si32 row_handle_height   = 1;
        si32 column_border_width = 2; // Outer border thickness: vertical sides.
        si32 row_border_height   = 1; // Outer border thickness: horizontal sides.
        si32 column_padding_width = 0; // Inner padding: left and right sides.
        si32 row_padding_height   = 0; // Inner padding: top and bottom sides.
        ui32 handle_color        = theme::surface;
        ui32 border_color        = theme::surface;
        bool border              = faux; // Paint an outer border using border_color
                                         // with column_border_width/row_border_height.
    };

    inline auto grid_track_max(grid_track const& track) -> si32
    {
        return track.maximum < 0 ? std::numeric_limits<si32>::max()
                                 : std::max(track.minimum, track.maximum);
    }

    // Resolve an extent against weighted tracks while honoring hard minima and
    // maxima.  Constraints are fixed first, then the remaining tracks share the
    // rest in proportion to their weights.  Remainder cells are handed out in
    // stable track order, making the result deterministic.
    inline auto resolve_grid_tracks(std::vector<grid_track> const& source, si32 extent)
        -> std::vector<si32>
    {
        auto count = source.size();
        auto sizes = std::vector<si32>(count);
        if (!count) return sizes;

        auto minimum_sum = si32{};
        for (auto& track : source) minimum_sum += std::max(0, track.minimum);
        extent = std::max(extent, minimum_sum);

        auto active = std::vector<bool>(count, true);
        auto left = extent;
        auto active_count = (si32)count;
        while (active_count)
        {
            auto weight_sum = si64{};
            for (auto i = size_t{}; i < count; ++i)
                if (active[i]) weight_sum += std::max(0, source[i].weight);
            auto equal = weight_sum == 0;
            if (equal) weight_sum = active_count;

            auto constrained = faux;
            for (auto i = size_t{}; i < count; ++i)
            {
                if (!active[i]) continue;
                auto weight = equal ? si64{ 1 } : (si64)std::max(0, source[i].weight);
                auto proposed = weight_sum ? (si32)(left * weight / weight_sum) : 0;
                auto low = std::max(0, source[i].minimum);
                auto high = grid_track_max(source[i]);
                if (proposed < low || proposed > high)
                {
                    sizes[i] = std::clamp(proposed, low, high);
                    left -= sizes[i];
                    active[i] = faux;
                    --active_count;
                    constrained = true;
                }
            }
            if (constrained) continue;

            auto used = si32{};
            for (auto i = size_t{}; i < count; ++i)
            {
                if (!active[i]) continue;
                auto weight = equal ? si64{ 1 } : (si64)std::max(0, source[i].weight);
                sizes[i] = weight_sum ? (si32)(left * weight / weight_sum) : 0;
                used += sizes[i];
            }
            auto remainder = left - used;
            while (remainder > 0)
            {
                auto progressed = faux;
                for (auto i = size_t{}; i < count && remainder > 0; ++i)
                {
                    if (!active[i] || sizes[i] >= grid_track_max(source[i])) continue;
                    ++sizes[i];
                    --remainder;
                    progressed = true;
                }
                if (!progressed) break; // Every active track is capped.
            }
            break;
        }
        return sizes;
    }

    class grid
        : public ui::form<grid>
    {
        struct cell_entry
        {
            ui::sptr widget;
            grid_cell placement;
            rect area;
        };

        struct handle_entry
        {
            ui::sptr widget;
            axis dimension;
            si32 boundary;
            rect area;
        };

        grid_handle_mode mode;
        grid_cfg defaults;
        std::vector<grid_track> columns;
        std::vector<grid_track> rows;
        std::vector<si32> column_sizes;
        std::vector<si32> row_sizes;
        std::vector<cell_entry> cells;
        std::vector<handle_entry> handles;
        std::vector<component> retained_components;

        auto column_gap() const -> si32
        {
            return mode != grid_handle_mode::hidden
                 ? std::max(0, defaults.column_handle_width)
                 : 0;
        }

        auto row_gap() const -> si32
        {
            return mode != grid_handle_mode::hidden
                 ? std::max(0, defaults.row_handle_height)
                 : 0;
        }

        // Vertical (left/right) border thickness.
        auto border_x() const -> si32
        {
            return defaults.border ? std::max(0, defaults.column_border_width) : 0;
        }

        // Horizontal (top/bottom) border thickness.
        auto border_y() const -> si32
        {
            return defaults.border ? std::max(0, defaults.row_border_height) : 0;
        }

        auto padding_x() const -> si32
        {
            return std::max(0, defaults.column_padding_width);
        }

        auto padding_y() const -> si32
        {
            return std::max(0, defaults.row_padding_height);
        }

        auto content_x() const -> si32
        {
            return border_x() + padding_x();
        }

        auto content_y() const -> si32
        {
            return border_y() + padding_y();
        }

        static auto track_minimum(std::vector<grid_track> const& tracks) -> si32
        {
            return std::accumulate(tracks.begin(), tracks.end(), si32{},
                [](si32 sum, grid_track const& track)
                {
                    return sum + std::max(0, track.minimum);
                });
        }

        static auto track_offsets(std::vector<si32> const& sizes, si32 gap, si32 start = 0)
            -> std::vector<si32>
        {
            auto offsets = std::vector<si32>(sizes.size());
            auto cursor = start;
            for (auto i = size_t{}; i < sizes.size(); ++i)
            {
                offsets[i] = cursor;
                cursor += sizes[i];
                if (i + 1 < sizes.size()) cursor += gap;
            }
            return offsets;
        }

        auto normalized(grid_cell cell) const -> grid_cell
        {
            cell.column = std::clamp(cell.column, 0, (si32)columns.size() - 1);
            cell.row = std::clamp(cell.row, 0, (si32)rows.size() - 1);
            cell.column_span = std::clamp(cell.column_span, 1, (si32)columns.size() - cell.column);
            cell.row_span = std::clamp(cell.row_span, 1, (si32)rows.size() - cell.row);
            return cell;
        }

        auto cell_rect(grid_cell const& cell,
                       std::vector<si32> const& x,
                       std::vector<si32> const& y) const -> rect
        {
            auto last_column = cell.column + cell.column_span - 1;
            auto last_row = cell.row + cell.row_span - 1;
            auto x0 = x[(size_t)cell.column];
            auto y0 = y[(size_t)cell.row];
            auto x1 = x[(size_t)last_column] + column_sizes[(size_t)last_column];
            auto y1 = y[(size_t)last_row] + row_sizes[(size_t)last_row];
            return { { x0, y0 }, { x1 - x0, y1 - y0 } };
        }

    protected:
        void deform(rect& new_area) override
        {
            auto xgap = column_gap();
            auto ygap = row_gap();
            auto inset_x = content_x();
            auto inset_y = content_y();
            auto slot_x = inset_x * 2 + xgap * std::max(0, (si32)columns.size() - 1);
            auto slot_y = inset_y * 2 + ygap * std::max(0, (si32)rows.size() - 1);
            auto min_width = track_minimum(columns) + slot_x;
            auto min_height = track_minimum(rows) + slot_y;
            new_area.size = std::max(new_area.size, twod{ min_width, min_height });

            auto track_width = std::max(0, new_area.size.x - slot_x);
            auto track_height = std::max(0, new_area.size.y - slot_y);
            column_sizes = resolve_grid_tracks(columns, track_width);
            row_sizes = resolve_grid_tracks(rows, track_height);
            auto x = track_offsets(column_sizes, xgap, inset_x);
            auto y = track_offsets(row_sizes, ygap, inset_y);

            for (auto& handle : handles)
            {
                if (handle.dimension == axis::X)
                {
                    auto bx = x[(size_t)handle.boundary] - xgap;
                    handle.area = { { bx, inset_y }, { xgap, std::max(0, new_area.size.y - inset_y * 2) } };
                }
                else
                {
                    auto by = y[(size_t)handle.boundary] - ygap;
                    handle.area = { { inset_x, by }, { std::max(0, new_area.size.x - inset_x * 2), ygap } };
                }
                auto area = handle.area;
                handle.widget->base::recalc(area);
                handle.area = area;
            }

            for (auto& cell : cells)
            {
                cell.area = cell_rect(cell.placement, x, y);
                auto area = cell.area;
                cell.widget->base::recalc(area);
                // Respect the child's growth/crop alignment just as ui::fork does.
                // Tables use this to keep their natural content width instead of
                // stretching the final column across all unused grid space.
                cell.area = area;
            }
        }

        void inform(rect new_area) override
        {
            for (auto& handle : handles)
            {
                auto area = handle.area;
                area.coor += new_area.coor;
                handle.widget->base::notify(area);
            }
            for (auto& cell : cells)
            {
                auto area = cell.area;
                area.coor += new_area.coor;
                cell.widget->base::notify(area);
            }
        }

    public:
        static constexpr auto classname = basename::grid;

        grid(grid_cfg config)
            : mode{ config.handle_mode },
              defaults{ std::move(config) },
              columns{ defaults.columns },
              rows{ defaults.rows }
        {
            if (columns.empty()) columns.push_back({});
            if (rows.empty()) rows.push_back({});
            defaults.columns = columns;
            defaults.rows = rows;

            LISTEN(tier::release, e2::render::any, parent_canvas)
            {
                if (auto context2D = nested_2D_context(parent_canvas))
                {
                    if (defaults.border)
                    {
                        auto full = rect{ {}, base::size() };
                        auto color = defaults.border_color;
                        parent_canvas.cage(full, dent{ border_x(), border_x(), border_y(), border_y() },
                                           [color](cell& c){ c.bgc(color); });
                    }
                    // Handles are behind cells.  A spanning cell therefore hides both
                    // the paint and the hit link of a crossed internal boundary.
                    for (auto& handle : handles) handle.widget->render(parent_canvas);
                    for (auto& cell : cells) cell.widget->render(parent_canvas);
                }
            };
        }

        void install_handles()
        {
            if (mode == grid_handle_mode::hidden || !handles.empty()) return;
            auto enabled = mode == grid_handle_mode::enabled;
            auto target = ptr::shadow(This());
            auto add_handle = [&](axis dimension, si32 boundary)
            {
                auto handle = ui::mock::ctor();
                if (enabled)
                {
                    handle->active()
                           ->plugin<pro::mouse>()
                           ->plugin<pro::mover>();
                }
                handle->invoke([target, dimension, boundary, enabled, color = defaults.handle_color](auto& boss)
                {
                    auto& hover = boss.base::field(bool{ faux });
                    boss.LISTEN(tier::release, e2::render::any, canvas, -, (color))
                    {
                        canvas.fill(rect{ {}, boss.base::size() }, [&](cell& c){ c.bgc(color); });
                        if (hover) canvas.fill(rect{ {}, boss.base::size() }, [](cell& c){ c.xlight(); });
                    };
                    if (!enabled) return;
                    boss.on(tier::mouserelease, input::key::MouseMove, [&](hids&)
                    {
                        if (!hover) { hover = true; boss.base::deface(); }
                    });
                    boss.on(tier::mouserelease, input::key::MouseLeave, [&](hids&)
                    {
                        if (hover) { hover = faux; boss.base::deface(); }
                    });
                    // pro::mover provides the framework's established captured-drag
                    // path and reports each actual move through upon::changed.
                    boss.LISTEN(tier::preview, e2::form::upon::changed, delta, -, (target, dimension, boundary))
                    {
                        if (auto grid = target.lock())
                        {
                            auto step = dimension == axis::X ? delta.x : delta.y;
                            grid->move_handle(dimension, boundary, step);
                        }
                    };
                    boss.on(tier::mouserelease, input::key::LeftDoubleClick, [target, dimension, boundary](hids& gear)
                    {
                        if (auto grid = target.lock()) grid->reset_handle(dimension, boundary);
                        gear.dismiss();
                    });
                });
                handles.push_back({ handle, dimension, boundary, {} });
                ui::base::attach(handle);
            };
            for (auto boundary = si32{ 1 }; boundary < (si32)columns.size(); ++boundary)
                add_handle(axis::X, boundary);
            for (auto boundary = si32{ 1 }; boundary < (si32)rows.size(); ++boundary)
                add_handle(axis::Y, boundary);
        }

        template<class Item>
        auto attach(netxs::sptr<Item> item, grid_cell placement = {}) -> netxs::sptr<Item>
        {
            if (!item) return item;
            cells.push_back({ item, normalized(placement), {} });
            ui::base::attach(item);
            return item;
        }

        auto attach(component item, grid_cell placement = {}) -> ui::sptr
        {
            auto widget = item.widget;
            if (!widget) return widget;
            retained_components.push_back(std::move(item));
            return attach(widget, placement);
        }

        void remove(ui::sptr item) override
        {
            if (!item) return;
            std::erase_if(cells, [&](auto const& entry){ return entry.widget == item; });
            std::erase_if(handles, [&](auto const& entry){ return entry.widget == item; });
            std::erase_if(retained_components, [&](auto const& entry){ return entry.widget == item; });
            ui::base::remove(item);
        }

        void move_handle(axis dimension, si32 boundary, si32 delta)
        {
            if (mode != grid_handle_mode::enabled) return;
            auto& tracks = dimension == axis::X ? columns : rows;
            auto& sizes = dimension == axis::X ? column_sizes : row_sizes;
            if (boundary <= 0 || boundary >= (si32)tracks.size()
             || sizes.size() != tracks.size()) return;
            auto before_index = (size_t)boundary - 1;
            auto after_index = (size_t)boundary;
            auto pair_size = sizes[before_index] + sizes[after_index];
            auto low = std::max(std::max(0, tracks[before_index].minimum),
                                pair_size - grid_track_max(tracks[after_index]));
            auto high = std::min(grid_track_max(tracks[before_index]),
                                 pair_size - std::max(0, tracks[after_index].minimum));
            if (high < low) high = low;
            auto before = std::clamp(sizes[before_index] + delta, low, high);
            tracks[before_index].weight = before;
            tracks[after_index].weight = pair_size - before;
            base::reflow();
        }

        void reset_handle(axis dimension, si32 boundary)
        {
            if (mode != grid_handle_mode::enabled) return;
            auto& tracks = dimension == axis::X ? columns : rows;
            auto const& initial = dimension == axis::X ? defaults.columns : defaults.rows;
            if (boundary <= 0 || boundary >= (si32)tracks.size()) return;
            tracks[(size_t)boundary - 1].weight = initial[(size_t)boundary - 1].weight;
            tracks[(size_t)boundary].weight = initial[(size_t)boundary].weight;
            base::reflow();
        }

        auto get_column_sizes() const -> std::vector<si32> const& { return column_sizes; }
        auto get_row_sizes() const -> std::vector<si32> const& { return row_sizes; }
        auto get_handle_count() const -> size_t { return handles.size(); }
        auto get_handle_mode() const -> grid_handle_mode { return mode; }
        auto handles_visible() const -> bool { return mode != grid_handle_mode::hidden; }

        auto get_cell_area(ui::sptr const& item) const -> rect
        {
            for (auto& entry : cells) if (entry.widget == item) return entry.area;
            return {};
        }

        static auto ctor(grid_cfg config)
        {
            auto item = ui::tui_domain().create<grid>(std::move(config));
            item->install_handles();
            return item;
        }
    };
}
