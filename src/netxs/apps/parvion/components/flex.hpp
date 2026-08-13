// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/components/flex.hpp: A retained, terminal-cell flex layout.
//
// The container supports row/column flow, optional wrapping, main/cross-axis
// alignment, per-item grow/shrink/basis constraints and padding, stable
// ordering, gaps, independent container padding, and an optional outer border.
// It deliberately has no draggable handles.

#include "ui.hpp"

#include <limits>
#include <numeric>

namespace netxs::app::parvion
{
    enum class flex_direction
    {
        row,
        column,
    };

    enum class flex_wrap
    {
        no_wrap,
        wrap,
    };

    enum class flex_justify
    {
        start,
        end,
        center,
        space_between,
        space_around,
        space_evenly,
    };

    enum class flex_align
    {
        start,
        end,
        center,
        stretch,
    };

    enum class flex_content_align
    {
        start,
        end,
        center,
        stretch,
        space_between,
        space_around,
        space_evenly,
    };

    enum class flex_align_self
    {
        automatic,
        start,
        end,
        center,
        stretch,
    };

    struct flex_item
    {
        si32 grow       = 0;
        si32 shrink     = 1;
        si32 basis      = -1; // Negative means use the child's intrinsic main size.
        si32 minimum    = 0;
        si32 maximum    = -1; // Negative means unbounded.
        si32 order      = 0;
        flex_align_self align_self = flex_align_self::automatic;
        dent padding    = {}; // Inner item padding: left, right, top, bottom.
    };

    struct flex_cfg
    {
        flex_direction direction = flex_direction::row;
        flex_wrap wrapping = flex_wrap::no_wrap;
        flex_justify justify_content = flex_justify::start;
        flex_align align_items = flex_align::stretch;
        flex_content_align align_content = flex_content_align::stretch;
        si32 column_gap = 0;
        si32 row_gap = 0;
        si32 column_border_width = 2; // Outer border thickness: vertical sides.
        si32 row_border_height = 1;   // Outer border thickness: horizontal sides.
        si32 column_padding_width = 0; // Inner padding: left and right sides.
        si32 row_padding_height = 0;   // Inner padding: top and bottom sides.
        ui32 border_color = theme::surface;
        bool border = faux;
    };

    class flex
        : public ui::form<flex>
    {
        struct item_entry
        {
            ui::sptr widget;
            flex_item config;
            rect area;
            twod intrinsic;
            size_t sequence;
        };

        struct line_entry
        {
            std::vector<size_t> items;
            std::vector<si32> sizes;
            si32 cross_size{};
            si32 cross_minimum{};
            si32 cross_position{};
        };

        flex_cfg config;
        std::vector<item_entry> items;
        std::vector<component> retained_components;
        std::vector<size_t> visual_order;
        size_t next_sequence{};

        auto vertical() const -> bool
        {
            return config.direction == flex_direction::column;
        }

        auto main_axis() const -> axis
        {
            return vertical() ? axis::Y : axis::X;
        }

        auto cross_axis() const -> axis
        {
            return vertical() ? axis::X : axis::Y;
        }

        auto main_gap() const -> si32
        {
            return std::max(0, vertical() ? config.row_gap : config.column_gap);
        }

        auto cross_gap() const -> si32
        {
            return std::max(0, vertical() ? config.column_gap : config.row_gap);
        }

        auto border_x() const -> si32
        {
            return config.border ? std::max(0, config.column_border_width) : 0;
        }

        auto border_y() const -> si32
        {
            return config.border ? std::max(0, config.row_border_height) : 0;
        }

        auto padding_x() const -> si32
        {
            return std::max(0, config.column_padding_width);
        }

        auto padding_y() const -> si32
        {
            return std::max(0, config.row_padding_height);
        }

        auto main_inset() const -> si32
        {
            return vertical() ? border_y() + padding_y()
                              : border_x() + padding_x();
        }

        auto cross_inset() const -> si32
        {
            return vertical() ? border_x() + padding_x()
                              : border_y() + padding_y();
        }

        static auto value(twod const& point, axis dimension) -> si32
        {
            return dimension == axis::X ? point.x : point.y;
        }

        static void value(twod& point, axis dimension, si32 next)
        {
            (dimension == axis::X ? point.x : point.y) = next;
        }

        static auto item_maximum(item_entry const& entry, axis dimension) -> si32
        {
            auto own_min = std::max(0, value(entry.widget->base::min_sz, dimension));
            auto own_max = value(entry.widget->base::max_sz, dimension);
            auto configured = entry.config.maximum < 0
                            ? std::numeric_limits<si32>::max()
                            : std::max(std::max(0, entry.config.minimum), entry.config.maximum);
            if (own_max >= 0) configured = std::min(configured, std::max(own_min, own_max));
            return configured;
        }

        static auto item_minimum(item_entry const& entry, axis dimension) -> si32
        {
            auto child_minimum = std::max(0, value(entry.widget->base::min_sz, dimension));
            return std::max(child_minimum, std::max(0, entry.config.minimum));
        }

        static auto cross_minimum(item_entry const& entry, axis dimension) -> si32
        {
            return std::max(0, value(entry.widget->base::min_sz, dimension));
        }

        static auto cross_maximum(item_entry const& entry, axis dimension) -> si32
        {
            return std::max(0, value(entry.widget->base::max_sz, dimension));
        }

        void rebuild_visual_order()
        {
            visual_order.resize(items.size());
            std::iota(visual_order.begin(), visual_order.end(), size_t{});
            std::stable_sort(visual_order.begin(), visual_order.end(), [&](auto left, auto right)
            {
                if (items[left].config.order != items[right].config.order)
                    return items[left].config.order < items[right].config.order;
                return items[left].sequence < items[right].sequence;
            });
        }

        static auto distribute_spaces(std::vector<si32> const& weights, si32 free_space)
            -> std::vector<si32>
        {
            auto spaces = std::vector<si32>(weights.size());
            if (free_space <= 0 || weights.empty()) return spaces;
            auto total = std::accumulate(weights.begin(), weights.end(), si64{},
                [](si64 sum, si32 weight){ return sum + std::max(0, weight); });
            if (!total) return spaces;
            auto used = si32{};
            for (auto i = size_t{}; i < weights.size(); ++i)
            {
                spaces[i] = (si32)((si64)free_space * std::max(0, weights[i]) / total);
                used += spaces[i];
            }
            for (auto i = size_t{}; used < free_space; i = (i + 1) % weights.size())
            {
                if (weights[i] <= 0) continue;
                ++spaces[i];
                ++used;
            }
            return spaces;
        }

        static auto justify_spaces(flex_justify mode, size_t count, si32 free_space)
            -> std::vector<si32>
        {
            auto spaces = std::vector<si32>(count + 1);
            if (!count || free_space <= 0) return spaces;
            switch (mode)
            {
                case flex_justify::end:
                    spaces.front() = free_space;
                    break;
                case flex_justify::center:
                    spaces.front() = free_space / 2;
                    spaces.back() = free_space - spaces.front();
                    break;
                case flex_justify::space_between:
                    if (count > 1)
                    {
                        auto weights = std::vector<si32>(count - 1, 1);
                        auto inner = distribute_spaces(weights, free_space);
                        for (auto i = size_t{}; i < inner.size(); ++i) spaces[i + 1] = inner[i];
                    }
                    break;
                case flex_justify::space_around:
                {
                    auto weights = std::vector<si32>(count + 1, 2);
                    weights.front() = weights.back() = 1;
                    spaces = distribute_spaces(weights, free_space);
                    break;
                }
                case flex_justify::space_evenly:
                    spaces = distribute_spaces(std::vector<si32>(count + 1, 1), free_space);
                    break;
                case flex_justify::start:
                default:
                    spaces.back() = free_space;
                    break;
            }
            return spaces;
        }

        static auto content_spaces(flex_content_align mode, size_t count, si32 free_space)
            -> std::vector<si32>
        {
            auto spaces = std::vector<si32>(count + 1);
            if (!count || free_space <= 0) return spaces;
            switch (mode)
            {
                case flex_content_align::end:
                    spaces.front() = free_space;
                    break;
                case flex_content_align::center:
                    spaces.front() = free_space / 2;
                    spaces.back() = free_space - spaces.front();
                    break;
                case flex_content_align::space_between:
                    if (count > 1)
                    {
                        auto inner = distribute_spaces(std::vector<si32>(count - 1, 1), free_space);
                        for (auto i = size_t{}; i < inner.size(); ++i) spaces[i + 1] = inner[i];
                    }
                    break;
                case flex_content_align::space_around:
                {
                    auto weights = std::vector<si32>(count + 1, 2);
                    weights.front() = weights.back() = 1;
                    spaces = distribute_spaces(weights, free_space);
                    break;
                }
                case flex_content_align::space_evenly:
                    spaces = distribute_spaces(std::vector<si32>(count + 1, 1), free_space);
                    break;
                case flex_content_align::start:
                default:
                    spaces.back() = free_space;
                    break;
            }
            return spaces;
        }

        auto resolve_line(line_entry& line, si32 extent) const -> void
        {
            auto dimension = main_axis();
            auto gap_total = main_gap() * std::max(0, (si32)line.items.size() - 1);
            auto available = std::max(0, extent - gap_total);
            auto used = std::accumulate(line.sizes.begin(), line.sizes.end(), si32{});
            if (used < available)
            {
                auto extra = available - used;
                auto active = std::vector<bool>(line.items.size());
                for (auto i = size_t{}; i < line.items.size(); ++i)
                {
                    auto& entry = items[line.items[i]];
                    active[i] = std::max(0, entry.config.grow) > 0
                             && line.sizes[i] < item_maximum(entry, dimension);
                }
                while (extra > 0)
                {
                    auto total = si64{};
                    for (auto i = size_t{}; i < active.size(); ++i)
                        if (active[i]) total += std::max(0, items[line.items[i]].config.grow);
                    if (!total) break;
                    auto shares = std::vector<si32>(active.size());
                    auto assigned = si32{};
                    for (auto i = size_t{}; i < active.size(); ++i)
                    {
                        if (!active[i]) continue;
                        auto weight = std::max(0, items[line.items[i]].config.grow);
                        shares[i] = (si32)((si64)extra * weight / total);
                        assigned += shares[i];
                    }
                    for (auto i = size_t{}; assigned < extra; ++i)
                    {
                        if (i == active.size()) i = 0;
                        if (!active[i]) continue;
                        ++shares[i];
                        ++assigned;
                    }
                    auto capped = faux;
                    for (auto i = size_t{}; i < active.size(); ++i)
                    {
                        if (!active[i]) continue;
                        auto maximum = item_maximum(items[line.items[i]], dimension);
                        auto capacity = maximum - line.sizes[i];
                        if (shares[i] >= capacity)
                        {
                            line.sizes[i] += capacity;
                            extra -= capacity;
                            active[i] = faux;
                            capped = true;
                        }
                    }
                    if (capped) continue;
                    for (auto i = size_t{}; i < active.size(); ++i)
                        if (active[i]) line.sizes[i] += shares[i];
                    extra = 0;
                }
            }
            else if (used > available)
            {
                auto deficit = used - available;
                auto active = std::vector<bool>(line.items.size());
                for (auto i = size_t{}; i < line.items.size(); ++i)
                {
                    auto& entry = items[line.items[i]];
                    active[i] = std::max(0, entry.config.shrink) > 0
                             && line.sizes[i] > item_minimum(entry, dimension);
                }
                while (deficit > 0)
                {
                    auto total = si64{};
                    for (auto i = size_t{}; i < active.size(); ++i)
                    {
                        if (!active[i]) continue;
                        total += (si64)std::max(0, items[line.items[i]].config.shrink)
                               * std::max(1, line.sizes[i]);
                    }
                    if (!total) break;
                    auto shares = std::vector<si32>(active.size());
                    auto assigned = si32{};
                    for (auto i = size_t{}; i < active.size(); ++i)
                    {
                        if (!active[i]) continue;
                        auto factor = (si64)std::max(0, items[line.items[i]].config.shrink)
                                    * std::max(1, line.sizes[i]);
                        shares[i] = (si32)((si64)deficit * factor / total);
                        assigned += shares[i];
                    }
                    for (auto i = size_t{}; assigned < deficit; ++i)
                    {
                        if (i == active.size()) i = 0;
                        if (!active[i]) continue;
                        ++shares[i];
                        ++assigned;
                    }
                    auto capped = faux;
                    for (auto i = size_t{}; i < active.size(); ++i)
                    {
                        if (!active[i]) continue;
                        auto minimum = item_minimum(items[line.items[i]], dimension);
                        auto capacity = line.sizes[i] - minimum;
                        if (shares[i] >= capacity)
                        {
                            line.sizes[i] -= capacity;
                            deficit -= capacity;
                            active[i] = faux;
                            capped = true;
                        }
                    }
                    if (capped) continue;
                    for (auto i = size_t{}; i < active.size(); ++i)
                        if (active[i]) line.sizes[i] -= shares[i];
                    deficit = 0;
                }
            }
        }

        auto effective_alignment(item_entry const& entry) const -> flex_align
        {
            switch (entry.config.align_self)
            {
                case flex_align_self::start:   return flex_align::start;
                case flex_align_self::end:     return flex_align::end;
                case flex_align_self::center:  return flex_align::center;
                case flex_align_self::stretch: return flex_align::stretch;
                default:                       return config.align_items;
            }
        }

    protected:
        void deform(rect& new_area) override
        {
            rebuild_visual_order();
            auto main = main_axis();
            auto cross = cross_axis();
            auto minset = main_inset();
            auto cinset = cross_inset();
            auto available_cross = std::max(0, value(new_area.size, cross) - cinset * 2);

            for (auto& entry : items)
            {
                entry.area = {};
                entry.intrinsic = {};
                if (entry.widget->base::hidden) continue;
                auto probe = rect{};
                // A vertical layout already knows the width it will offer its
                // children.  Probe at that width so wrapping labels and titled
                // containers can report their width-dependent natural height.
                if (vertical() && effective_alignment(entry) == flex_align::stretch)
                {
                    auto padding = entry.config.padding.l + entry.config.padding.r;
                    value(probe.size, cross, std::max(0, available_cross - padding));
                }
                entry.widget->base::recalc(probe);
                entry.intrinsic = probe.size;
            }

            auto visible = std::vector<size_t>{};
            for (auto index : visual_order)
                if (!items[index].widget->base::hidden) visible.push_back(index);

            auto preferred_basis = [&](item_entry const& entry)
            {
                auto main_padding = vertical()
                                  ? entry.config.padding.t + entry.config.padding.b
                                  : entry.config.padding.l + entry.config.padding.r;
                auto basis = entry.config.basis < 0 ? value(entry.intrinsic, main) + main_padding
                                                    : std::max(0, entry.config.basis);
                return std::clamp(basis,
                                  item_minimum(entry, main),
                                  std::max(item_minimum(entry, main), item_maximum(entry, main)));
            };

            auto main_minimum = si32{};
            if (!visible.empty())
            {
                if (config.wrapping == flex_wrap::no_wrap)
                {
                    main_minimum = main_gap() * ((si32)visible.size() - 1);
                    for (auto index : visible) main_minimum += item_minimum(items[index], main);
                }
                else
                {
                    for (auto index : visible)
                        main_minimum = std::max(main_minimum, item_minimum(items[index], main));
                }
            }
            auto natural_main = main_gap() * std::max(0, (si32)visible.size() - 1);
            for (auto index : visible) natural_main += preferred_basis(items[index]);
            value(new_area.size, main,
                  std::max(value(new_area.size, main),
                           (value(new_area.size, main) == 0 ? natural_main : main_minimum) + minset * 2));
            auto inner_main = std::max(0, value(new_area.size, main) - minset * 2);

            auto lines = std::vector<line_entry>{};
            for (auto index : visible)
            {
                auto& entry = items[index];
                auto minimum = item_minimum(entry, main);
                auto maximum = std::max(minimum, item_maximum(entry, main));
                auto basis = std::clamp(preferred_basis(entry), minimum, maximum);
                auto needs_line = lines.empty();
                if (!needs_line && config.wrapping == flex_wrap::wrap)
                {
                    auto& line = lines.back();
                    auto occupied = std::accumulate(line.sizes.begin(), line.sizes.end(), si32{})
                                  + main_gap() * (si32)line.sizes.size();
                    needs_line = occupied + basis > inner_main;
                }
                if (needs_line) lines.emplace_back();
                auto& line = lines.back();
                line.items.push_back(index);
                line.sizes.push_back(basis);
                line.cross_size = std::max(line.cross_size, value(entry.intrinsic, cross));
                line.cross_minimum = std::max(line.cross_minimum, cross_minimum(entry, cross));
            }

            auto cross_minimum_total = cross_gap() * std::max(0, (si32)lines.size() - 1);
            for (auto& line : lines) cross_minimum_total += line.cross_minimum;
            value(new_area.size, cross,
                  std::max(value(new_area.size, cross), cross_minimum_total + cinset * 2));
            auto inner_cross = std::max(0, value(new_area.size, cross) - cinset * 2);

            for (auto& line : lines) resolve_line(line, inner_main);

            // A non-wrapping run of non-shrinkable items defines a hard natural
            // extent.  Let the container grow so scrollviews can observe the
            // overflow instead of allowing children to overlap outside it.
            if (config.wrapping == flex_wrap::no_wrap && lines.size() == 1)
            {
                auto resolved = main_gap() * std::max(0, (si32)lines.front().sizes.size() - 1)
                              + std::accumulate(lines.front().sizes.begin(), lines.front().sizes.end(), si32{});
                if (resolved > inner_main)
                {
                    inner_main = resolved;
                    value(new_area.size, main, inner_main + minset * 2);
                }
            }

            auto base_cross = cross_gap() * std::max(0, (si32)lines.size() - 1);
            for (auto& line : lines) base_cross += line.cross_size;
            auto cross_free = std::max(0, inner_cross - base_cross);
            auto line_spaces = std::vector<si32>(lines.size() + 1);
            if (config.wrapping == flex_wrap::no_wrap && lines.size() == 1)
            {
                lines.front().cross_size = std::max(lines.front().cross_size, inner_cross);
            }
            else if (config.align_content == flex_content_align::stretch && !lines.empty())
            {
                auto additions = distribute_spaces(std::vector<si32>(lines.size(), 1), cross_free);
                for (auto i = size_t{}; i < lines.size(); ++i) lines[i].cross_size += additions[i];
            }
            else
            {
                line_spaces = content_spaces(config.align_content, lines.size(), cross_free);
            }

            auto cross_cursor = cinset + (line_spaces.empty() ? 0 : line_spaces.front());
            for (auto line_index = size_t{}; line_index < lines.size(); ++line_index)
            {
                auto& line = lines[line_index];
                line.cross_position = cross_cursor;
                auto occupied = main_gap() * std::max(0, (si32)line.items.size() - 1)
                              + std::accumulate(line.sizes.begin(), line.sizes.end(), si32{});
                auto free = std::max(0, inner_main - occupied);
                auto spaces = justify_spaces(config.justify_content, line.items.size(), free);
                auto main_cursor = minset + spaces.front();
                for (auto i = size_t{}; i < line.items.size(); ++i)
                {
                    auto& entry = items[line.items[i]];
                    auto alignment = effective_alignment(entry);
                    auto natural_cross = std::clamp(value(entry.intrinsic, cross),
                                                    cross_minimum(entry, cross),
                                                    cross_maximum(entry, cross));
                    auto requested_cross = alignment == flex_align::stretch
                                         ? std::clamp(line.cross_size,
                                                      cross_minimum(entry, cross),
                                                      cross_maximum(entry, cross))
                                         : natural_cross;
                    auto area = rect{};
                    value(area.coor, main, main_cursor);
                    value(area.coor, cross, line.cross_position);
                    value(area.size, main, line.sizes[i]);
                    value(area.size, cross, requested_cross);
                    area = entry.config.padding.area(area);
                    entry.widget->base::recalc(area);
                    auto actual_cross = value(area.size, cross);
                    auto cross_padding = vertical()
                                       ? entry.config.padding.l + entry.config.padding.r
                                       : entry.config.padding.t + entry.config.padding.b;
                    auto leading_padding = vertical()
                                         ? entry.config.padding.l
                                         : entry.config.padding.t;
                    auto cross_offset = si32{};
                    auto actual_outer_cross = actual_cross + cross_padding;
                    if (alignment == flex_align::end) cross_offset = line.cross_size - actual_outer_cross;
                    else if (alignment == flex_align::center) cross_offset = (line.cross_size - actual_outer_cross) / 2;
                    value(area.coor, cross,
                          line.cross_position + std::max(0, cross_offset) + leading_padding);
                    entry.area = area;
                    main_cursor += line.sizes[i] + main_gap() + spaces[i + 1];
                }
                cross_cursor += line.cross_size + cross_gap() + line_spaces[line_index + 1];
            }
        }

        void inform(rect new_area) override
        {
            for (auto index : visual_order)
            {
                auto& entry = items[index];
                if (entry.widget->base::hidden) continue;
                auto area = entry.area;
                area.coor += new_area.coor;
                entry.widget->base::notify(area);
            }
        }

    public:
        static constexpr auto classname = basename::flex;

        flex(flex_cfg setup)
            : config{ std::move(setup) }
        {
            LISTEN(tier::release, e2::render::any, parent_canvas)
            {
                if (auto context2D = nested_2D_context(parent_canvas))
                {
                    if (config.border)
                    {
                        auto full = rect{ {}, base::size() };
                        auto color = config.border_color;
                        parent_canvas.cage(full, dent{ border_x(), border_x(), border_y(), border_y() },
                                           [color](cell& c){ c.bgc(color); });
                    }
                    for (auto index : visual_order)
                    {
                        auto& entry = items[index];
                        if (!entry.widget->base::hidden) entry.widget->render(parent_canvas);
                    }
                }
            };
        }

        template<class Item>
        auto attach(netxs::sptr<Item> item, flex_item setup = {}) -> netxs::sptr<Item>
        {
            if (!item) return item;
            setup.grow = std::max(0, setup.grow);
            setup.shrink = std::max(0, setup.shrink);
            setup.minimum = std::max(0, setup.minimum);
            if (setup.maximum >= 0) setup.maximum = std::max(setup.minimum, setup.maximum);
            if (setup.basis < 0) setup.basis = -1;
            setup.padding.l = std::max(0, setup.padding.l);
            setup.padding.r = std::max(0, setup.padding.r);
            setup.padding.t = std::max(0, setup.padding.t);
            setup.padding.b = std::max(0, setup.padding.b);
            items.push_back({ item, setup, {}, {}, next_sequence++ });
            ui::base::attach(item);
            rebuild_visual_order();
            return item;
        }

        auto attach(component item, flex_item setup = {}) -> ui::sptr
        {
            auto widget = item.widget;
            if (!widget) return widget;
            retained_components.push_back(std::move(item));
            return attach(widget, setup);
        }

        // Insert a fixed spacer between the previously attached item and the next
        // one.  In row flow the spacer is `count` columns wide; in column flow it is
        // `count` rows tall.  The gap never grows or shrinks.
        auto attach_separator(si32 count) -> ui::sptr
        {
            return attach(ui::mock::ctor(), { .grow = 0, .shrink = 0, .basis = std::max(0, count) });
        }

        void remove(ui::sptr item) override
        {
            if (!item) return;
            std::erase_if(items, [&](auto const& entry){ return entry.widget == item; });
            std::erase_if(retained_components, [&](auto const& entry){ return entry.widget == item; });
            ui::base::remove(item);
            rebuild_visual_order();
        }

        auto get_item_area(ui::sptr const& item) const -> rect
        {
            for (auto& entry : items) if (entry.widget == item) return entry.area;
            return {};
        }

        auto get_item_count() const -> size_t { return items.size(); }

        static auto ctor(flex_cfg setup = {})
        {
            return ui::tui_domain().create<flex>(std::move(setup));
        }
    };
}
