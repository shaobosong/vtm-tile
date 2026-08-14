// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/components/input.hpp: the reusable single-line INPUT component.
//
// The widget owns every transient editor detail: focus, caret, horizontal
// scrolling, click/drag caret placement, filtering, paste, and keyboard input.
// Callers bind a live value and lifecycle callbacks, position the returned
// widget, and otherwise stay out of its event handling.
//
// make_input() is the component's only callable public API.

#include "ui.hpp"

#include <functional>

namespace netxs::app::parvion
{
    enum class input_mode { view, edit, disabled };
    enum class input_blur { keep, submit, cancel };

    struct input_palette
    {
        ui32 background       = theme::surface;
        ui32 foreground       = theme::text_fg;
        ui32 muted_foreground = theme::subtext;
        ui32 focus            = theme::sel_bg_act;
    };

    struct input_cfg
    {
        std::function<text()>       value;
        std::function<void(text)>   on_change;
        std::function<text()>       prefix;
        std::function<input_mode()> mode;
        std::function<void()>       on_activate;
        std::function<void(text)>   on_submit;
        std::function<void()>       on_cancel;
        input_blur                  blur = input_blur::keep;
        bool                        secret = faux;
        bool                        digits_only = faux;
        bool                        focus_on_start = faux;
        input_palette               palette{};
    };

    namespace input_detail
    {
        inline auto value_foreground(input_mode mode, bool focused,
                                     input_palette const& palette) -> ui32
        {
            if (mode == input_mode::disabled) return palette.muted_foreground;
            return mode == input_mode::edit && focused ? palette.focus
                                                       : palette.foreground;
        }
    }

    inline auto make_input(input_cfg cfg) -> component
    {
        auto focus_on_activate = cfg.focus_on_start;
        struct state
        {
            si32 caret = 0;
            si32 off = 0;
            bool focused = faux;
            bool dragging = faux;
            text seen;
            text original;

            auto mode(input_cfg const& cfg) const
            {
                return cfg.mode ? cfg.mode() : input_mode::edit;
            }
            auto current(input_cfg const& cfg) const
            {
                return cfg.value ? cfg.value() : text{};
            }
            void set(input_cfg& cfg, text value)
            {
                seen = value;
                caret = std::clamp(caret, 0, cluster_count(value));
                if (cfg.on_change) cfg.on_change(std::move(value));
            }
            auto sync(input_cfg const& cfg)
            {
                auto value = current(cfg);
                if (value != seen)
                {
                    seen = value;
                    caret = cluster_count(value);
                    off = 0;
                }
                else caret = std::clamp(caret, 0, cluster_count(value));
                return value;
            }
            auto display(input_cfg const& cfg, view value) const
            {
                return cfg.secret ? text((size_t)cluster_count(value), '*') : text{ value };
            }
            void caret_to(input_cfg const& cfg, si32 mx)
            {
                auto value = sync(cfg);
                auto disp = display(cfg, value);
                caret = std::min(cell_to_cluster(disp, off + mx), cluster_count(value));
            }
            auto filter(input_cfg const& cfg, view utf8) const
            {
                auto out = text{};
                out.reserve(utf8.size());
                for (auto i = size_t{}; i < utf8.size();)
                {
                    auto c = (unsigned char)utf8[i];
                    if (c < 0x20 || c == 0x7f) { ++i; continue; }
                    auto n = u8_step(utf8, i);
                    if (!cfg.digits_only || (n == 1 && c >= '0' && c <= '9'))
                        out.append(utf8.data() + i, n);
                    i += n;
                }
                return out;
            }
            auto insert(input_cfg& cfg, view utf8)
            {
                auto ins = filter(cfg, utf8);
                if (ins.empty()) return faux;
                auto value = sync(cfg);
                value.insert(cluster_to_byte(value, caret), ins);
                caret += cluster_count(ins);
                set(cfg, std::move(value));
                return true;
            }
            void restore(input_cfg& cfg)
            {
                caret = cluster_count(original);
                off = 0;
                set(cfg, original);
            }
            void submit(input_cfg& cfg)
            {
                auto value = sync(cfg);
                original = value;
                if (cfg.on_submit) cfg.on_submit(std::move(value));
            }
            void cancel(input_cfg& cfg)
            {
                restore(cfg);
                if (cfg.on_cancel) cfg.on_cancel();
            }
        };

        auto form = ui::mock::ctor()->active()
            ->plugin<pro::mouse>()
            ->plugin<pro::focus>(cfg.focus_on_start ? pro::focus::mode::focused
                                                    : pro::focus::mode::focusable)
            ->plugin<pro::keybd>();
        form->invoke([cfgv = std::move(cfg)](auto& boss)
        {
            auto& st  = boss.base::field(state{});
            auto& cfg = boss.base::field(input_cfg{ cfgv });

            st.seen = st.current(cfg);
            st.original = st.seen;
            st.caret = cluster_count(st.seen);

            boss.LISTEN(tier::release, e2::render::any, canvas)
            {
                auto size = boss.base::size();
                if (size.x <= 0 || size.y <= 0) return;
                auto mode = st.mode(cfg);
                auto value = st.sync(cfg);
                auto disp = st.display(cfg, value);
                auto prefix = cfg.prefix ? cfg.prefix() : text{};
                auto prefix_w = std::min(size.x, cell_width(prefix));
                auto active = mode == input_mode::edit && st.focused;
                auto value_fg = input_detail::value_foreground(mode, st.focused, cfg.palette);
                auto und = active ? cfg.palette.focus : cfg.palette.muted_foreground;
                canvas.fill(rect{ {}, size }, [&](cell& c)
                {
                    c.bgc(cfg.palette.background).und(unln::line).unc(argb{ und });
                });

                if (prefix_w > 0)
                    put_str(canvas, 0, 0, prefix, cfg.palette.muted_foreground,
                            cfg.palette.background, prefix_w);
                auto field_w = std::max(0, size.x - prefix_w);
                if (field_w <= 0) return;

                if (mode == input_mode::view || mode == input_mode::disabled)
                {
                    auto width = cell_width(disp);
                    if (width >= field_w)
                    {
                        // Keep the final cell empty as a direct click target for the end of
                        // the value.  `off` is the source column hidden under the ellipsis;
                        // if this press changes a view field to edit mode, caret_to() can
                        // therefore interpret every mouse column against what was painted.
                        st.off = width - field_w + 1;
                        if (field_w > 1)
                        {
                            put_str(canvas, prefix_w, 0, "\xE2\x80\xA6", value_fg,
                                    cfg.palette.background, 1);
                            put_str(canvas, prefix_w + 1, 0, view{ disp }.substr(byte_at_cell(disp, st.off + 1)),
                                    value_fg, cfg.palette.background, field_w - 2);
                        }
                    }
                    else
                    {
                        st.off = 0;
                        put_str(canvas, prefix_w, 0, disp, value_fg,
                                cfg.palette.background, field_w);
                    }
                    return;
                }

                auto total = cell_width(disp);
                auto ccell = cfg.secret ? st.caret : caret_cell(disp, st.caret);
                if (st.off > ccell)             st.off = ccell;
                if (ccell - st.off >= field_w)  st.off = ccell - field_w + 1;
                st.off = std::clamp(st.off, 0, std::max(0, total - field_w + 1));
                auto shown = view{ disp }.substr(byte_at_cell(disp, st.off));
                put_str(canvas, prefix_w, 0, shown, value_fg,
                        cfg.palette.background, field_w);
                if (active)
                {
                    auto cx = prefix_w + ccell - st.off;
                    if (cx >= prefix_w && cx < size.x)
                        canvas.fill(rect{{ cx, 0 }, { 1, 1 }}, [&](cell& c)
                        {
                            c.bgc(cfg.palette.focus).fgc(cfg.palette.background);
                        });
                }
            };
            boss.LISTEN(tier::release, e2::form::state::focus::count, count)
            {
                auto was = st.focused;
                st.focused = !!count;
                if (!was && st.focused && st.mode(cfg) == input_mode::edit)
                {
                    st.original = st.sync(cfg);
                }
                else if (was && !st.focused && st.mode(cfg) == input_mode::edit)
                {
                    if      (cfg.blur == input_blur::submit && cfg.on_submit) st.submit(cfg);
                    else if (cfg.blur == input_blur::cancel && cfg.on_cancel) st.cancel(cfg);
                }
                boss.base::deface();
            };
            boss.on(tier::mouserelease, input::key::LeftDown, [&](hids& gear)
            {
                auto mode = st.mode(cfg);
                if (mode == input_mode::disabled) { gear.dismiss(); return; }
                if (mode == input_mode::view && cfg.on_activate) cfg.on_activate();
                if (st.mode(cfg) != input_mode::edit) { gear.dismiss(); return; }
                pro::focus::set(boss.This(), gear.id, solo::on);
                st.original = st.sync(cfg);
                auto prefix_w = cfg.prefix ? cell_width(cfg.prefix()) : 0;
                st.caret_to(cfg, std::max(0, (si32)gear.coord.x - prefix_w));
                boss.base::deface();
                gear.dismiss();
            });
            boss.on(tier::mouserelease, input::key::LeftClick, [&](hids& gear)
            {
                if (st.mode(cfg) == input_mode::edit)
                    pro::focus::set(boss.This(), gear.id, solo::on);
                gear.dismiss();
            });
            boss.base::signal(tier::release, e2::form::draggable::_<hids::buttons::left>, true);
            boss.LISTEN(tier::release, e2::form::drag::start::_<hids::buttons::left>, gear)
            {
                st.dragging = st.mode(cfg) == input_mode::edit;
            };
            boss.LISTEN(tier::release, e2::form::drag::pull::_<hids::buttons::left>, gear)
            {
                if (!st.dragging) return;
                auto prefix_w = cfg.prefix ? cell_width(cfg.prefix()) : 0;
                st.caret_to(cfg, std::max(0, (si32)gear.coord.x - prefix_w));
                boss.base::deface();
            };
            boss.LISTEN(tier::release, e2::form::drag::stop::_<hids::buttons::left>, gear)
            {
                st.dragging = faux;
            };
            boss.LISTEN(tier::release, e2::form::drag::cancel::_<hids::buttons::left>, gear)
            {
                st.dragging = faux;
            };
            boss.LISTEN(tier::preview, input::events::keybd::any, gear)
            {
                if (!st.focused || st.mode(cfg) != input_mode::edit || gear.keybd::handled) return;
                if (gear.payload == input::keybd::type::keypaste)
                {
                    if (st.insert(cfg, gear.cluster))
                    {
                        gear.set_handled();
                        boss.base::deface();
                    }
                    return;
                }
                if (gear.payload != input::keybd::type::keypress
                 || gear.keystat == input::key::released
                 || gear.keystat == input::key::interrupted) return;

                auto k = gear.keybd::generic();
                auto ctrl = !!(gear.ctlstat & hids::anyCtrl);
                auto alt  = !!(gear.ctlstat & hids::anyAlt);
                auto value = st.sync(cfg);
                auto handled = true;
                     if (k == input::key::Tab && !ctrl && !alt) {} // Deliberately consumed: no field traversal.
                else if (k == input::key::Esc && cfg.on_cancel) st.cancel(cfg);
                else if (k == input::key::KeyEnter && cfg.on_submit) st.submit(cfg);
                else if (k == input::key::Backspace)
                {
                    if (st.caret > 0)
                    {
                        auto end = cluster_to_byte(value, st.caret);
                        auto beg = cluster_to_byte(value, st.caret - 1);
                        value.erase(beg, end - beg);
                        --st.caret;
                        st.set(cfg, std::move(value));
                    }
                }
                else if (k == input::key::KeyDelete)
                {
                    if (st.caret < cluster_count(value))
                    {
                        auto beg = cluster_to_byte(value, st.caret);
                        auto end = cluster_to_byte(value, st.caret + 1);
                        value.erase(beg, end - beg);
                        st.set(cfg, std::move(value));
                    }
                }
                else if (k == input::key::KeyLeftArrow)  st.caret = std::max(0, st.caret - 1);
                else if (k == input::key::KeyRightArrow) st.caret = std::min(cluster_count(value), st.caret + 1);
                else if (k == input::key::KeyHome)       st.caret = 0;
                else if (k == input::key::KeyEnd)        st.caret = cluster_count(value);
                else handled = st.insert(cfg, gear.cluster);

                if (handled)
                {
                    gear.set_handled();
                    boss.base::deface();
                }
            };
        });
        auto activate = focus_on_activate ? std::function<void()>{ [weak = ptr::shadow(form)]
        {
            if (auto input = weak.lock())
            {
                auto gear_id = input->bell::indexer.luafx.get_gear().id;
                input->base::enqueue([weak, gear_id](auto&)
                {
                    if (auto target = weak.lock()) pro::focus::set(target, gear_id, solo::on);
                });
            }
        } } : std::function<void()>{};
        return { std::move(form), std::move(activate), {} };
    }
}
