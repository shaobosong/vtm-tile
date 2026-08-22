// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/components/scrollview.hpp: A retained, arbitrary-child viewport.
//
// Vertical-primary scrolling is enabled by default.  The primary direction
// can be horizontal instead, and the other direction can be enabled
// independently.  Scrollbar geometry, glyphs, colors, paging, wheel behavior
// and drag feedback intentionally match table.hpp/textbox.hpp.

#include "ui.hpp"

#include <cassert>

namespace netxs::app::parvion
{
    struct scrollview_palette
    {
        ui32 bg       = theme::bg;
        ui32 track    = theme::sb_track;
        ui32 thumb    = theme::sb_thumb;
        ui32 hover    = theme::sb_hover;
        ui32 dragging = theme::sb_drag;
    };

    struct scrollview_cfg
    {
        component content;
        axis primary_axis = axis::Y;
        axes scroll_axes = axes::Y_only;
        si32 vertical_wheel_step = 1;
        si32 horizontal_wheel_step = 4;
        scrollview_palette palette{};
    };

    struct scrollview_scrollbar
    {
        bool ok = faux;
        rect track{};
        rect thumb{};
        si32 maximum = 0;
    };

    class scrollview
        : public ui::form<scrollview>
    {
        enum drag_mode { drag_none, drag_vertical, drag_horizontal };

        scrollview_cfg config;
        ui::sptr content;
        ui::face viewport_canvas;
        rect viewport{};
        twod content_size{};
        twod offset{};
        bool vertical_bar = faux;
        bool horizontal_bar = faux;
        bool vertical_hover = faux;
        bool horizontal_hover = faux;
        bool vertical_press = faux;
        bool horizontal_press = faux;
        drag_mode dragging = drag_none;
        si32 drag_grab = 0;

        auto allows(axis dimension) const -> bool
        {
            return dimension == axis::X ? !!(config.scroll_axes & axes::X_only)
                                        : !!(config.scroll_axes & axes::Y_only);
        }

        auto secondary_axis() const -> axis
        {
            return config.primary_axis == axis::X ? axis::Y : axis::X;
        }

        auto maximum_offset() const -> twod
        {
            return std::max(dot_00, content_size - viewport.size);
        }

        auto measure(twod frame) -> rect
        {
            frame = std::max(frame, dot_00);
            auto request = frame;
            if (allows(axis::X)) request.x = std::max(request.x, content->base::min_sz.x);
            if (allows(axis::Y)) request.y = std::max(request.y, content->base::min_sz.y);
            auto area = rect{ {}, request };
            content->base::recalc(area);
            return area;
        }

        void move_content()
        {
            offset = std::clamp(offset, dot_00, maximum_offset());
            content->base::moveto(viewport.coor - offset);
            base::deface();
        }

        static auto scrollbar(si32 track_start, si32 track_length,
                              si32 viewport_length, si32 total_length,
                              si32 scroll, bool vertical, si32 cross) -> scrollview_scrollbar
        {
            auto result = scrollview_scrollbar{};
            if (track_length <= 0 || viewport_length <= 0 || total_length <= viewport_length) return result;
            result.ok = true;
            result.maximum = total_length - viewport_length;
            auto thumb_length = std::max(1, viewport_length * track_length / total_length);
            auto travel = track_length - thumb_length;
            auto thumb_start = track_start
                             + (travel * scroll + result.maximum / 2) / result.maximum;
            if (vertical)
            {
                result.track = { { cross, track_start }, { 1, track_length } };
                result.thumb = { { cross, thumb_start }, { 1, thumb_length } };
            }
            else
            {
                result.track = { { track_start, cross }, { track_length, 1 } };
                result.thumb = { { thumb_start, cross }, { thumb_length, 1 } };
            }
            return result;
        }

        static auto inside(rect const& area, twod point) -> bool
        {
            return point.x >= area.coor.x && point.y >= area.coor.y
                && point.x < area.coor.x + area.size.x
                && point.y < area.coor.y + area.size.y;
        }

        void scroll_to(axis dimension, si32 value)
        {
            auto maximum = maximum_offset();
            offset[dimension] = std::clamp(value, 0, maximum[dimension]);
            move_content();
        }

        void drag_to(axis dimension, si32 coordinate, scrollview_scrollbar const& bar)
        {
            if (!bar.ok) return;
            auto track_start = bar.track.coor[dimension];
            auto track_length = bar.track.size[dimension];
            auto thumb_length = bar.thumb.size[dimension];
            auto travel = track_length - thumb_length;
            if (travel <= 0) return;
            auto value = ((coordinate - drag_grab - track_start) * bar.maximum + travel / 2) / travel;
            scroll_to(dimension, value);
        }

    protected:
        void inform(rect new_area) override
        {
            auto show_x = faux;
            auto show_y = faux;
            auto measured = rect{};
            // Scrollbars can create overflow on the opposite axis.  Accumulate the result
            // until both axes settle, rather than letting the pair oscillate.
            for (auto pass = 0; pass < 3; ++pass)
            {
                auto frame = new_area.size - twod{ show_y ? 1 : 0, show_x ? 1 : 0 };
                measured = measure(frame);
                auto next_x = show_x || (allows(axis::X) && measured.size.x > frame.x);
                auto next_y = show_y || (allows(axis::Y) && measured.size.y > frame.y);
                if (next_x == show_x && next_y == show_y) break;
                show_x = next_x;
                show_y = next_y;
            }

            vertical_bar = show_y;
            horizontal_bar = show_x;
            viewport = {
                new_area.coor,
                std::max(dot_00, new_area.size - twod{ vertical_bar ? 1 : 0,
                                                       horizontal_bar ? 1 : 0 }),
            };
            measured = measure(viewport.size);
            content_size = measured.size;
            offset = std::clamp(offset, dot_00, maximum_offset());
            measured.coor = viewport.coor - offset;
            content->base::notify(measured);
        }

    public:
        static constexpr auto classname = basename::scrollview;

        scrollview(scrollview_cfg setup)
            : config{ std::move(setup) },
              content{ config.content.widget }
        {
            // The enabled-axis mask remains authoritative.  A primary axis
            // outside that mask is contradictory and would make unmodified
            // wheel/key input impossible to route consistently.
            assert(allows(config.primary_axis));
            if (!content) content = ui::mock::ctor();

            LISTEN(tier::release, e2::render::any, parent_canvas)
            {
                if (auto context2D = nested_2D_context(parent_canvas))
                {
                    // A narrowed face::clip() is not sufficient: child paint
                    // primitives can still address the parent face's larger
                    // backing buffer.  Render into a physically viewport-sized
                    // face, then copy only that bounded result back.  This clips
                    // vertical and horizontal overflow alike and preserves cell
                    // links and any ancestor-imposed clipping.
                    auto size = viewport.size;
                    if (size.x > 0 && size.y > 0)
                    {
                        viewport_canvas.area(rect{ {}, size });
                        viewport_canvas.fill(parent_canvas, cell::shaders::full);
                        content->render(viewport_canvas);
                        netxs::onclip(parent_canvas, viewport_canvas, cell::shaders::full);
                    }
                    paint_scrollbars(parent_canvas);
                }
            };
            LISTEN(tier::request, e2::form::prop::viewport, requested)
            {
                requested = viewport;
            };
        }

        auto get_viewport() const -> rect { return viewport; }
        auto get_content_size() const -> twod { return content_size; }
        auto get_offset() const -> twod { return offset; }
        auto has_vertical_scrollbar() const -> bool { return vertical_bar; }
        auto has_horizontal_scrollbar() const -> bool { return horizontal_bar; }

        auto vertical_scrollbar() const -> scrollview_scrollbar
        {
            return scrollbar(viewport.coor.y, viewport.size.y, viewport.size.y,
                             content_size.y, offset.y, true,
                             viewport.coor.x + viewport.size.x);
        }

        auto horizontal_scrollbar() const -> scrollview_scrollbar
        {
            return scrollbar(viewport.coor.x, viewport.size.x, viewport.size.x,
                             content_size.x, offset.x, faux,
                             viewport.coor.y + viewport.size.y);
        }

        void set_offset(twod value)
        {
            offset = value;
            move_content();
        }

        void paint_scrollbars(auto& canvas)
        {
            auto draw = [&](scrollview_scrollbar const& bar, bool vertical, bool hover, bool drag, bool press)
            {
                if (!bar.ok) return;
                auto mark = vertical ? (hover || drag ? "\xe2\x96\x88" : "\xe2\x96\x90")
                                     : (hover || drag ? "\xe2\x96\x84" : "\xe2\x96\x82");
                auto myid = bell::id;
                canvas.fill(bar.track, [&](cell& c)
                {
                    c.bgc(config.palette.bg).fgc(config.palette.track).txt(mark).link(myid);
                });
                auto color = drag ? config.palette.dragging
                           : hover ? config.palette.hover
                                   : config.palette.thumb;
                canvas.fill(bar.thumb, [&](cell& c)
                {
                    c.bgc(config.palette.bg).fgc(color).txt(mark).link(myid);
                });
                if (press || drag)
                {
                    if (vertical) canvas.fill(bar.thumb, [](cell& c){ c.xlight(2); });
                    else          canvas.fill(bar.thumb, [](cell& c){ c.fgc().xlight(2); });
                }
            };
            draw(vertical_scrollbar(), true, vertical_hover,
                 dragging == drag_vertical, vertical_press);
            draw(horizontal_scrollbar(), faux, horizontal_hover,
                 dragging == drag_horizontal, horizontal_press);
        }

        void update_hover(twod point)
        {
            auto next_v = inside(vertical_scrollbar().track, point);
            auto next_h = inside(horizontal_scrollbar().track, point);
            if (vertical_hover != next_v || horizontal_hover != next_h)
            {
                vertical_hover = next_v;
                horizontal_hover = next_h;
                base::deface();
            }
            if (vertical_press && !next_v) vertical_press = faux;
            if (horizontal_press && !next_h) horizontal_press = faux;
        }

        void leave()
        {
            if (vertical_hover || horizontal_hover || vertical_press || horizontal_press)
            {
                vertical_hover = horizontal_hover = faux;
                vertical_press = horizontal_press = faux;
                base::deface();
            }
        }

        auto press_at(twod point) -> bool
        {
            update_hover(point);
            vertical_press = vertical_scrollbar().ok && vertical_hover;
            horizontal_press = horizontal_scrollbar().ok && horizontal_hover;
            if (vertical_press || horizontal_press) base::deface();
            return vertical_press || horizontal_press;
        }

        auto page_at(twod point) -> bool
        {
            if (auto bar = vertical_scrollbar(); bar.ok && inside(bar.track, point))
            {
                auto page = std::max(1, viewport.size.y);
                if (point.y < bar.thumb.coor.y) scroll_to(axis::Y, offset.y - page);
                else if (point.y >= bar.thumb.coor.y + bar.thumb.size.y) scroll_to(axis::Y, offset.y + page);
                return true;
            }
            if (auto bar = horizontal_scrollbar(); bar.ok && inside(bar.track, point))
            {
                auto page = std::max(1, viewport.size.x);
                if (point.x < bar.thumb.coor.x) scroll_to(axis::X, offset.x - page);
                else if (point.x >= bar.thumb.coor.x + bar.thumb.size.x) scroll_to(axis::X, offset.x + page);
                return true;
            }
            return faux;
        }

        auto start_drag(twod point) -> bool
        {
            if (auto bar = vertical_scrollbar(); bar.ok && inside(bar.track, point))
            {
                dragging = drag_vertical;
                drag_grab = inside(bar.thumb, point) ? point.y - bar.thumb.coor.y
                                                     : bar.thumb.size.y / 2;
                if (!inside(bar.thumb, point)) drag_to(axis::Y, point.y, bar);
                vertical_press = faux;
                vertical_hover = true;
                return true;
            }
            if (auto bar = horizontal_scrollbar(); bar.ok && inside(bar.track, point))
            {
                dragging = drag_horizontal;
                drag_grab = inside(bar.thumb, point) ? point.x - bar.thumb.coor.x
                                                     : bar.thumb.size.x / 2;
                if (!inside(bar.thumb, point)) drag_to(axis::X, point.x, bar);
                horizontal_press = faux;
                horizontal_hover = true;
                return true;
            }
            return faux;
        }

        void pull_drag(twod point)
        {
            if      (dragging == drag_vertical)   drag_to(axis::Y, point.y, vertical_scrollbar());
            else if (dragging == drag_horizontal) drag_to(axis::X, point.x, horizontal_scrollbar());
        }

        void stop_drag()
        {
            if (dragging != drag_none)
            {
                dragging = drag_none;
                vertical_press = horizontal_press = faux;
                base::deface();
            }
        }

        auto wheel(hids& gear) -> bool
        {
            if (!gear.whlsi || gear.meta(hids::anyCtrl)) return faux;
            auto dimension = config.primary_axis;
            if      (horizontal_hover && allows(axis::X)) dimension = axis::X;
            else if (vertical_hover   && allows(axis::Y)) dimension = axis::Y;
            else if (gear.hzwhl       && allows(axis::X)) dimension = axis::X;
            else if (gear.ctlstat & (hids::anyShift | hids::anyAlt))
            {
                auto secondary = secondary_axis();
                if (allows(secondary)) dimension = secondary;
            }
            if (!allows(dimension)) return faux;
            auto step = dimension == axis::X ? config.horizontal_wheel_step
                                             : config.vertical_wheel_step;
            scroll_to(dimension, offset[dimension] - gear.whlsi * std::max(1, step));
            return true;
        }

        auto navigate(hids& gear) -> bool
        {
            if (gear.keybd::handled
             || gear.payload != input::keybd::type::keypress
             || gear.keystat == input::key::released
             || gear.keystat == input::key::interrupted) return faux;
            auto k = gear.keybd::generic();
            auto dimension = config.primary_axis;
            auto secondary = secondary_axis();
            if ((gear.ctlstat & hids::anyShift) && allows(secondary)) dimension = secondary;
            if (!allows(dimension)) return faux;
            auto page = std::max(1, viewport.size[dimension]);
            auto value = offset[dimension];
            auto handled = true;
                 if (dimension == axis::Y && k == input::key::KeyUpArrow)    value -= 1;
            else if (dimension == axis::Y && k == input::key::KeyDownArrow)  value += 1;
            else if (dimension == axis::X && k == input::key::KeyLeftArrow)  value -= 1;
            else if (dimension == axis::X && k == input::key::KeyRightArrow) value += 1;
            else if (k == input::key::KeyPageUp   || k == input::key::NumpadPageUp)   value -= page;
            else if (k == input::key::KeyPageDown || k == input::key::NumpadPageDown) value += page;
            else if (k == input::key::KeyHome     || k == input::key::NumpadHome)     value = 0;
            else if (k == input::key::KeyEnd      || k == input::key::NumpadEnd)      value = maximum_offset()[dimension];
            else handled = faux;
            if (!handled) return faux;
            scroll_to(dimension, value);
            return true;
        }

        static auto ctor(scrollview_cfg setup)
        {
            auto viewport = ui::tui_domain().create<scrollview>(std::move(setup));
            viewport->ui::base::attach(viewport->content);
            return viewport;
        }
    };

    inline auto make_scrollview(scrollview_cfg cfg) -> component
    {
        auto retained = cfg.content;
        auto viewport = scrollview::ctor(std::move(cfg))
            ->active()
            ->plugin<pro::mouse>()
            ->plugin<pro::focus>(pro::focus::mode::focusable)
            ->plugin<pro::keybd>();
        viewport->invoke([](auto& boss)
        {
            boss.on(tier::mouserelease, input::key::MouseMove, [&](hids& gear)
            {
                boss.update_hover(twod{ gear.coord });
            });
            boss.on(tier::mouserelease, input::key::MouseLeave, [&](hids&)
            {
                boss.leave();
            });
            boss.on(tier::mouserelease, input::key::LeftDown, [&](hids& gear)
            {
                if (boss.press_at(twod{ gear.coord })) gear.dismiss();
            });
            boss.on(tier::mouserelease, input::key::LeftClick, [&](hids& gear)
            {
                if (boss.page_at(twod{ gear.coord })) gear.dismiss();
            });
            boss.on(tier::mouserelease, input::key::MouseWheel, [&](hids& gear)
            {
                if (boss.wheel(gear)) gear.dismiss();
            });
            boss.base::signal(tier::release, e2::form::draggable::_<hids::buttons::left>, true);
            boss.LISTEN(tier::release, e2::form::drag::start::_<hids::buttons::left>, gear)
            {
                if (boss.start_drag(twod{ gear.click }))
                {
                    pro::focus::set(boss.This(), gear.id, solo::on);
                    boss.base::deface();
                }
            };
            boss.LISTEN(tier::release, e2::form::drag::pull::_<hids::buttons::left>, gear)
            {
                boss.pull_drag(twod{ gear.coord });
            };
            boss.LISTEN(tier::release, e2::form::drag::stop::_<hids::buttons::left>, gear)
            {
                boss.stop_drag();
            };
            boss.LISTEN(tier::release, e2::form::drag::cancel::_<hids::buttons::left>, gear)
            {
                boss.stop_drag();
            };
            // Release-tier fallback lets a focused descendant consume navigation first.
            boss.LISTEN(tier::release, input::events::keybd::any, gear)
            {
                if (boss.navigate(gear))
                {
                    gear.set_handled();
                    boss.base::deface();
                }
            };
        });
        return {
            viewport,
            [retained]{ retained.on_activate(); },
            [retained]{ retained.on_deactivate(); },
        };
    }
}
