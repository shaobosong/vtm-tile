// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/components/popup_menu.hpp: A retained cascading menu component.
//
// Callers describe live trigger data and menu rows through popup_menu_cfg.
// The implementation keeps one process-wide popup session, renders every
// level as an overlay on the applet host, and owns keyboard/mouse routing until
// the session is dismissed. The older application.hpp menu and the form
// selector in dropdown.hpp remain independent.

#include "ui.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace netxs::app::parvion
{
    enum class popup_menu_item_kind : si32
    {
        action,
        submenu,
        radio_submenu,
        separator,
        checkbox,
    };

    // Only menu_bar sessions participate in hover-switching between triggers.
    enum class popup_menu_source : si32
    {
        dropdown_menu,
        menu_bar,
        context_menu,
    };

    struct popup_menu_item
    {
        text label{};
        popup_menu_item_kind kind{ popup_menu_item_kind::action };
        bool checked{}; // Static radio/checkbox fallback when no live provider is set.
        bool enabled{ true };
        std::vector<popup_menu_item> children;
        std::function<si32()> selected_index; // radio_submenu live selection.
        std::function<bool()> is_checked;     // checkbox live state.
        std::function<void(hids&)> on_activate;
    };

    struct popup_menu_palette
    {
        ui32 background                = theme::sel_bg;
        ui32 foreground                = theme::text_fg;
        ui32 disabled_foreground       = theme::subtext;
        ui32 popup_background          = 0xFF3D3E50u;
        ui32 popup_background_alt      = 0xFF393A4Cu;
        ui32 popup_hover               = 0xFF555668u;
        ui32 popup_hover_alt           = 0xFF515264u;
        ui32 popup_foreground          = 0xFFCDD6F4u;
        ui32 popup_disabled_foreground = 0xFF6C7086u;
    };

    // Options for opening a popup directly from an existing trigger.
    struct popup_menu_open_cfg
    {
        popup_menu_source source{ popup_menu_source::dropdown_menu };
        bool radio_group{ faux };
        si32 selected_index{ -1 };
        twod anchor{ -1, -1 }; // Non-negative x: explicit point inside the trigger.
        popup_menu_palette palette{};
    };

    struct popup_menu_cfg
    {
        std::function<text()> label;
        std::function<std::vector<popup_menu_item>()> items;
        bool radio_group{ faux };
        std::function<si32()> selected_index;
        std::function<bool()> enabled; // Null means enabled.
        popup_menu_source source{ popup_menu_source::dropdown_menu };
        si32 padding{ 1 };
        popup_menu_palette palette{};
    };

    namespace popup_menu_detail
    {
        static constexpr auto radio_gutter = si32{ 2 };
        static constexpr auto check_gutter = si32{ 2 };
        static constexpr auto no_mouse_lock = twod{ -32768, -32768 };
        static constexpr auto padding_property = "menu.padding";
        static constexpr auto open_property = "menu.dropdown.open";

        struct parsed_label
        {
            text visible;
            size_t shortcut_offset{ text::npos };
            char shortcut{};
            si32 width{};
        };

        // A shortcut marker is the first '&' followed by an ASCII letter or digit.
        inline auto parse_label(text const& source) -> parsed_label
        {
            auto result = parsed_label{ .visible = source };
            auto marker = source.find('&');
            while (marker != text::npos && marker + 1 < source.size())
            {
                auto next = (unsigned char)source[marker + 1];
                if ((next >= 'A' && next <= 'Z')
                 || (next >= 'a' && next <= 'z')
                 || (next >= '0' && next <= '9'))
                {
                    result.shortcut_offset = marker;
                    result.shortcut = (char)std::tolower(next);
                    result.visible.erase(marker, 1);
                    break;
                }
                marker = source.find('&', marker + 1);
            }
            result.width = cell_width(result.visible);
            return result;
        }

        inline auto is_selectable(popup_menu_item const& item) -> bool
        {
            return item.kind != popup_menu_item_kind::separator;
        }

        inline auto is_interactive(popup_menu_item const& item) -> bool
        {
            return is_selectable(item) && item.enabled;
        }

        inline auto has_check_rows(std::vector<popup_menu_item> const& items) -> bool
        {
            return std::any_of(items.begin(), items.end(), [](auto const& item)
            {
                return item.kind == popup_menu_item_kind::checkbox;
            });
        }

        inline auto resolve_selected(std::vector<popup_menu_item> const& items,
                                     std::function<si32()> const& provider = {}) -> si32
        {
            if (provider)
            {
                auto index = provider();
                return index >= 0 && index < (si32)items.size() ? index : -1;
            }
            for (auto index = si32{}; index < (si32)items.size(); ++index)
            {
                if (items[(size_t)index].checked) return index;
            }
            return -1;
        }

        inline auto resolve_selected(popup_menu_item const& item) -> si32
        {
            return resolve_selected(item.children, item.selected_index);
        }

        inline auto snapshot_items(std::vector<popup_menu_item> items)
            -> std::vector<popup_menu_item>
        {
            for (auto& item : items)
            {
                if (item.kind == popup_menu_item_kind::checkbox && item.is_checked)
                    item.checked = item.is_checked();
            }
            return items;
        }

        inline auto next_interactive(std::vector<popup_menu_item> const& items,
                                     si32 from, si32 direction) -> si32
        {
            auto count = (si32)items.size();
            if (count <= 0) return -1;
            auto start = from < 0 ? (direction > 0 ? count - 1 : 0) : from;
            for (auto step = si32{ 1 }; step <= count; ++step)
            {
                auto index = ((start + direction * step) % count + count) % count;
                if (is_interactive(items[(size_t)index])) return index;
            }
            return -1;
        }

        inline auto shortcut_row(std::vector<popup_menu_item> const& items, char shortcut) -> si32
        {
            auto target = (char)std::tolower((unsigned char)shortcut);
            for (auto index = si32{}; index < (si32)items.size(); ++index)
            {
                auto const& item = items[(size_t)index];
                if (is_interactive(item) && parse_label(item.label).shortcut == target) return index;
            }
            return -1;
        }

        inline auto popup_dimensions(std::vector<popup_menu_item> const& items,
                                     si32 padding, bool radio_group = faux) -> twod
        {
            auto has_submenus = std::any_of(items.begin(), items.end(), [](auto const& item)
            {
                return !item.children.empty();
            });
            auto has_checks = has_check_rows(items);
            auto width = si32{ 2 };
            for (auto const& item : items)
            {
                if (!is_selectable(item)) continue;
                auto item_width = parse_label(item.label).width + 2 * std::max(0, padding)
                                + (has_submenus ? 2 : 0)
                                + (radio_group ? radio_gutter : 0)
                                + (has_checks ? check_gutter : 0);
                width = std::max(width, item_width);
            }
            return { width, (si32)items.size() };
        }

        inline auto constrain_popup(twod desired, twod popup_size, twod host_size) -> twod
        {
            if (desired.x + popup_size.x > host_size.x)
                desired.x = std::max(0, host_size.x - popup_size.x);
            if (desired.y + popup_size.y > host_size.y)
                desired.y = std::max(0, host_size.y - popup_size.y);
            desired.x = std::max(0, desired.x);
            desired.y = std::max(0, desired.y);
            return desired;
        }

        inline auto submenu_anchor(rect parent, si32 row, twod child_size, si32 host_width) -> twod
        {
            auto right = parent.coor.x + parent.size.x;
            auto left = parent.coor.x - child_size.x;
            auto x = right + child_size.x > host_width && left >= 0 ? left : right;
            return { x, parent.coor.y + row };
        }

        inline auto trigger_width(popup_menu_cfg const& cfg, text const& label) -> si32
        {
            auto width = parse_label(label).width
                       + 2 * std::max(0, cfg.padding)
                       + (label.empty() ? 1 : 2);
            return std::max(3, width);
        }

        inline auto enabled(popup_menu_cfg const& cfg) -> bool
        {
            return !cfg.enabled || cfg.enabled();
        }

        inline void paint_label(auto& canvas, si32 x, si32 y, text const& source,
                                ui32 fg, ui32 bg, si32 max_cells, id_t link_id = 0)
        {
            if (max_cells <= 0) return;
            auto label = parse_label(source);
            auto byte_offset = size_t{};
            auto used = si32{};
            utf::decode_clusters(label.visible, [&](view cluster) -> bool
            {
                auto frag = utf::cluster(cluster);
                auto matrix = utf::matrix::whxy(frag.attr.cmatrix);
                auto width = std::max(1, (si32)matrix.w);
                if (used + width > max_cells) return faux;
                auto underlined = byte_offset == label.shortcut_offset;
                for (auto part = si32{}; part < width; ++part)
                {
                    canvas.fill(rect{ { x + used + part, y }, { 1, 1 } }, [&](cell& c)
                    {
                        c.bgc(bg).fgc(fg).txt(frag.text);
                        if (width > 1) c.wdt(matrix.w, matrix.h, part + 1, 1);
                        if (link_id) c.link(link_id);
                        if (underlined) c.und(unln::line);
                    });
                }
                used += width;
                byte_offset += cluster.size();
                return true;
            });
        }

        struct trigger_state
        {
            bool focused = faux;
            bool hover = faux;
            bool press = faux;
        };

        struct popup_state;
        struct menu_session;
        using popup_ptr = netxs::sptr<popup_state>;
        using session_ptr = netxs::sptr<menu_session>;
        using popup_anchor = std::function<twod()>;

        struct popup_state
        {
            std::vector<popup_menu_item> items;
            popup_anchor anchor;
            netxs::wptr<ui::base> overlay;
            twod dimensions{};
            rect body{};
            rect painted_bounds{};
            twod keyboard_lock{ no_mouse_lock };
            si32 selected_row{ -1 };
            si32 child_row{ -1 };
            si32 radio_selected{ -1 };
            si32 depth{};
            bool radio_group{};
            bool has_submenus{};
            bool has_checks{};

            auto interactive(si32 index) const -> bool
            {
                return index >= 0 && index < (si32)items.size()
                    && is_interactive(items[(size_t)index]);
            }

            void deface() const
            {
                if (auto widget = overlay.lock()) widget->base::deface();
            }
        };

        inline auto active_session() -> session_ptr&
        {
            static auto session = session_ptr{};
            return session;
        }

        inline auto find_host(ui::base& trigger) -> ui::sptr
        {
            auto walk = ui::sptr{ trigger.This() };
            auto last_below_root = walk;
            while (walk)
            {
                if (walk->base::kind() == ui::base::reflow_root) return walk;
                auto parent = walk->base::parent();
                if (!parent) break;
                last_below_root = walk;
                walk = parent;
            }
            return last_below_root;
        }

        struct menu_session : std::enable_shared_from_this<menu_session>
        {
            ui::sptr host;
            netxs::wptr<ui::base> host_shadow;
            netxs::wptr<ui::base> trigger_shadow;
            netxs::sptr<hook> keyboard_hook;
            netxs::sptr<hook> mouse_hook;
            std::vector<popup_ptr> popups;
            popup_menu_palette palette{};
            popup_menu_source source{ popup_menu_source::dropdown_menu };
            si32 padding{ 1 };
            twod explicit_anchor{ -1, -1 };
            bool closing{};

            auto context_mode() const -> bool
            {
                return explicit_anchor.x >= 0;
            }

            auto trigger_rect() const -> rect
            {
                if (!host) return {};
                auto trigger = trigger_shadow.lock();
                if (!trigger) return {};
                auto coordinate = twod{};
                auto walk = trigger;
                while (walk && walk != host)
                {
                    coordinate += walk->base::region.coor;
                    walk = walk->base::parent();
                }
                return walk == host ? rect{ coordinate, trigger->base::region.size } : rect{};
            }

            void dismiss()
            {
                if (std::exchange(closing, true)) return;
                for (auto const& popup : popups)
                {
                    if (auto overlay = popup->overlay.lock()) overlay->base::detach();
                }
                popups.clear();
                if (keyboard_hook) keyboard_hook->reset();
                if (mouse_hook) mouse_hook->reset();
                keyboard_hook.reset();
                mouse_hook.reset();
                if (auto trigger = trigger_shadow.lock())
                {
                    trigger->base::property(open_property, faux) = faux;
                    trigger->base::deface();
                }
                if (auto current_host = host_shadow.lock()) current_host->base::deface();
                if (active_session().get() == this) active_session().reset();
            }

            void close_after(popup_ptr const& parent)
            {
                auto found = std::find(popups.begin(), popups.end(), parent);
                if (found == popups.end()) return;
                auto keep = (size_t)std::distance(popups.begin(), found) + 1;
                while (popups.size() > keep)
                {
                    auto child = popups.back();
                    if (auto overlay = child->overlay.lock()) overlay->base::detach();
                    popups.pop_back();
                }
            }

            void activate(popup_ptr const& popup, si32 index, hids& gear)
            {
                if (!popup || !popup->interactive(index)) return;
                auto const& item = popup->items[(size_t)index];
                if (!item.children.empty()) return;
                if (item.on_activate) item.on_activate(gear);
                dismiss();
            }

            void open_submenu(popup_ptr const& parent, si32 index)
            {
                if (!parent || parent->child_row == index) return;
                close_after(parent);
                parent->child_row = -1;
                if (!parent->interactive(index)) return;
                auto const& item = parent->items[(size_t)index];
                if (item.children.empty()) return;

                auto radio = item.kind == popup_menu_item_kind::radio_submenu;
                auto selected = radio ? resolve_selected(item) : -1;
                auto child_size = popup_dimensions(item.children, padding, radio);
                auto weak_session = netxs::wptr<menu_session>{ shared_from_this() };
                auto weak_parent = netxs::wptr<popup_state>{ parent };
                auto anchor = [weak_session, weak_parent, child_size, index]
                {
                    auto session = weak_session.lock();
                    auto popup = weak_parent.lock();
                    if (!session || !popup) return twod{};
                    auto host_width = session->host ? session->host->base::region.size.x : 0;
                    return submenu_anchor(popup->body, index, child_size, host_width);
                };
                attach_popup(std::move(anchor), item.children, radio, selected);
                parent->child_row = index;
            }

            void focus_child(popup_ptr const& parent, si32 index, twod mouse_coordinate)
            {
                auto old_size = popups.size();
                open_submenu(parent, index);
                if (popups.size() <= old_size) return;
                auto child = popups.back();
                child->selected_row = next_interactive(child->items, -1, +1);
                child->keyboard_lock = mouse_coordinate;
                child->deface();
            }

            void handle_keyboard(hids& gear)
            {
                if (gear.payload != input::keybd::type::keypress || gear.keybd::handled) return;
                if (gear.keystat != input::key::pressed && gear.keystat != input::key::repeated)
                {
                    gear.set_handled(faux);
                    return;
                }
                auto key = gear.keybd::generic();
                if (key == input::key::Esc)
                {
                    dismiss();
                    gear.set_handled(faux);
                    return;
                }
                if (popups.empty())
                {
                    gear.set_handled(faux);
                    return;
                }

                auto popup = popups.back();
                auto selected = popup->selected_row;
                auto stamp = [&]{ popup->keyboard_lock = gear.coord; };
                if (key == input::key::KeyDownArrow || key == input::key::KeyUpArrow)
                {
                    auto direction = key == input::key::KeyDownArrow ? +1 : -1;
                    auto next = next_interactive(popup->items, selected, direction);
                    if (next >= 0)
                    {
                        popup->selected_row = next;
                        popup->deface();
                        stamp();
                    }
                    gear.set_handled(faux);
                    return;
                }
                if (key == input::key::KeyRightArrow)
                {
                    if (popup->interactive(selected)
                     && !popup->items[(size_t)selected].children.empty())
                        focus_child(popup, selected, gear.coord);
                    stamp();
                    gear.set_handled(faux);
                    return;
                }
                if (key == input::key::KeyLeftArrow)
                {
                    if (popups.size() > 1)
                    {
                        auto parent = popups[popups.size() - 2];
                        close_after(parent);
                        parent->child_row = -1;
                        parent->keyboard_lock = gear.coord;
                        parent->deface();
                    }
                    gear.set_handled(faux);
                    return;
                }
                if (key == input::key::KeyEnter)
                {
                    if (popup->interactive(selected))
                    {
                        if (!popup->items[(size_t)selected].children.empty())
                            focus_child(popup, selected, gear.coord);
                        else
                            activate(popup, selected, gear);
                    }
                    if (!closing) stamp();
                    gear.set_handled(faux);
                    return;
                }
                if (!gear.keybd::cluster.empty())
                {
                    auto first = (unsigned char)gear.keybd::cluster.front();
                    if ((first >= 'A' && first <= 'Z')
                     || (first >= 'a' && first <= 'z')
                     || (first >= '0' && first <= '9'))
                    {
                        auto index = shortcut_row(popup->items, (char)first);
                        if (index >= 0)
                        {
                            popup->selected_row = index;
                            popup->deface();
                            stamp();
                            if (!popup->items[(size_t)index].children.empty())
                                focus_child(popup, index, gear.coord);
                            else
                                activate(popup, index, gear);
                        }
                    }
                }
                gear.set_handled(faux);
            }

            void handle_mouse_preview(hids& gear)
            {
                auto cause = gear.cause;
                if (cause != input::key::LeftClick
                 && cause != input::key::RightClick
                 && cause != input::key::MiddleClick
                 && cause != input::key::LeftDown
                 && cause != input::key::RightDown
                 && cause != input::key::MiddleDown) return;

                auto point = twod{ (si32)gear.coord.x, (si32)gear.coord.y };
                if (!context_mode() && trigger_rect().hittest(point)) return;
                for (auto const& popup : popups)
                {
                    if (popup->painted_bounds.hittest(point)) return;
                }
                dismiss(); // Outside clicks continue to their underlying target.
            }

            void handle_mouse_move(popup_ptr const& popup, hids& gear)
            {
                if (!popup) return;
                if (gear.coord == popup->keyboard_lock) return;
                popup->keyboard_lock = no_mouse_lock;
                auto point = twod{ (si32)gear.coord.x, (si32)gear.coord.y };
                auto row = popup->body.hittest(point) ? point.y - popup->body.coor.y : -1;
                auto next = popup->interactive(row) ? row : -1;
                if (popup->selected_row == next) return;
                popup->selected_row = next;
                open_submenu(popup, next);
                popup->deface();
            }

            void handle_popup_click(popup_ptr const& popup, hids& gear)
            {
                if (!popup) return;
                auto point = twod{ (si32)gear.coord.x, (si32)gear.coord.y };
                if (!popup->body.hittest(point))
                {
                    dismiss();
                    gear.dismiss();
                    return;
                }
                auto index = point.y - popup->body.coor.y;
                if (!popup->interactive(index))
                {
                    gear.dismiss();
                    return;
                }
                if (!popup->items[(size_t)index].children.empty()) open_submenu(popup, index);
                else activate(popup, index, gear);
                gear.dismiss();
            }

            auto covered_by_other(popup_ptr const& popup, twod point) const -> bool
            {
                for (auto const& other : popups)
                {
                    if (other != popup && other->painted_bounds.hittest(point)) return true;
                }
                return faux;
            }

            void render_popup(popup_ptr const& popup, auto& canvas, id_t overlay_id)
            {
                if (!popup) return;
                auto area = canvas.area();
                auto desired = popup->anchor ? popup->anchor() : twod{};
                auto position = constrain_popup(desired, popup->dimensions, area.size);
                auto width = popup->dimensions.x;
                auto height = popup->dimensions.y;
                popup->body = { position, popup->dimensions };

                auto top_edge = position.y > 0;
                auto bottom_edge = position.y + height < area.size.y;
                auto left_edge = position.x > 0;
                auto right_edge = position.x + width < area.size.x;
                auto bounds_y = top_edge ? position.y - 1 : position.y;
                auto bounds_h = height + (top_edge ? 1 : 0) + (bottom_edge ? 1 : 0);
                popup->painted_bounds = { { position.x, bounds_y }, { width, bounds_h } };

                auto level_bg = popup->depth & 1 ? palette.popup_background_alt
                                                 : palette.popup_background;
                auto popup_fg = palette.popup_foreground;
                for (auto row = si32{}; row < height; ++row)
                {
                    auto const& item = popup->items[(size_t)row];
                    if (!is_selectable(item))
                    {
                        canvas.fill(rect{ { position.x, position.y + row }, { width, 1 } }, [=](cell& c)
                        {
                            c.wipe();
                            c.bgc(level_bg).fgc(popup_fg).txt(whitespace).link(overlay_id);
                        });
                        canvas.fill(rect{ { position.x + 1, position.y + row }, { width - 2, 1 } }, [=](cell& c)
                        {
                            c.wipe();
                            c.bgc(level_bg).fgc(popup_fg).txt("─").link(overlay_id);
                        });
                        continue;
                    }

                    auto hovered = row == popup->selected_row && item.enabled;
                    auto hover_bg = popup->depth & 1 ? palette.popup_hover_alt
                                                     : palette.popup_hover;
                    auto bg = hovered ? hover_bg : level_bg;
                    auto fg = item.enabled ? palette.popup_foreground
                                           : palette.popup_disabled_foreground;
                    canvas.fill(rect{ { position.x, position.y + row }, { width, 1 } }, [=](cell& c)
                    {
                        c.wipe();
                        c.bgc(bg).fgc(fg).txt(whitespace).link(overlay_id);
                    });

                    auto gutter = (popup->radio_group ? radio_gutter : 0)
                                + (popup->has_checks ? check_gutter : 0);
                    if (popup->radio_group)
                    {
                        auto bullet = row == popup->radio_selected ? "◉" : "○";
                        put_str(canvas, position.x + padding, position.y + row,
                                bullet, fg, bg, 1);
                    }
                    if (popup->has_checks && item.kind == popup_menu_item_kind::checkbox)
                    {
                        auto box_x = position.x + padding + (popup->radio_group ? radio_gutter : 0);
                        put_str(canvas, box_x, position.y + row, item.checked ? "▣" : "□", fg, bg, 1);
                    }
                    auto right_reserve = popup->has_submenus ? 2 : 0;
                    paint_label(canvas, position.x + padding + gutter, position.y + row,
                                item.label, fg, bg,
                                width - 2 * padding - gutter - right_reserve, overlay_id);
                    if (!item.children.empty())
                    {
                        put_str(canvas, position.x + width - 2, position.y + row, "▸", fg, bg, 1);
                    }
                }

                auto paint_horizontal_edge = [&](si32 row, view glyph)
                {
                    canvas.fill(rect{ { position.x, row }, { width, 1 } }, [=](cell& c)
                    {
                        c.st.wipe();
                        c.px.wipe();
                        c.fgc(level_bg).txt(glyph).link(overlay_id);
                    });
                };
                if (top_edge) paint_horizontal_edge(position.y - 1, "▄");
                if (bottom_edge) paint_horizontal_edge(position.y + height, "▀");

                auto paint_margin = [&](si32 column)
                {
                    for (auto row = bounds_y; row < bounds_y + bounds_h; ++row)
                    {
                        if (covered_by_other(popup, { column, row })) continue;
                        canvas.fill(rect{ { column, row }, { 1, 1 } }, [=](cell& c)
                        {
                            c.st.wipe();
                            c.px.wipe();
                            c.txt(whitespace).link(overlay_id);
                        });
                    }
                };
                if (left_edge) paint_margin(position.x - 1);
                if (right_edge) paint_margin(position.x + width);
            }

            void attach_popup(popup_anchor anchor, std::vector<popup_menu_item> const& source_items,
                              bool radio_group, si32 radio_selected)
            {
                if (!host || source_items.empty()) return;
                auto popup = ptr::shared<popup_state>();
                popup->items = snapshot_items(source_items);
                popup->anchor = std::move(anchor);
                popup->radio_group = radio_group;
                popup->radio_selected = radio_selected >= 0
                                      && radio_selected < (si32)popup->items.size()
                                      ? radio_selected : -1;
                if (radio_group && popup->radio_selected < 0)
                    popup->radio_selected = resolve_selected(popup->items);
                popup->has_submenus = std::any_of(popup->items.begin(), popup->items.end(), [](auto const& item)
                {
                    return !item.children.empty();
                });
                popup->has_checks = has_check_rows(popup->items);
                popup->dimensions = popup_dimensions(popup->items, padding, radio_group);
                popup->depth = (si32)popups.size();

                auto overlay = ui::mock::ctor();
                popup->overlay = ptr::shadow(overlay);
                popups.push_back(popup);
                auto self = shared_from_this();
                overlay->invoke([self, popup](auto& boss)
                {
                    auto overlay_id = boss.bell::id;
                    boss.LISTEN(tier::release, e2::render::any, canvas, -, (self, popup, overlay_id))
                    {
                        self->render_popup(popup, canvas, overlay_id);
                    };
                    boss.on(tier::mouserelease, input::key::MouseMove,
                        [self, popup](hids& gear){ self->handle_mouse_move(popup, gear); });
                    boss.on(tier::mouserelease, input::key::LeftClick,
                        [self, popup](hids& gear){ self->handle_popup_click(popup, gear); });
                });
                host->attach(overlay);
                host->base::deface();
            }

            void install_hooks()
            {
                auto self = shared_from_this();
                host->on(tier::mousepreview, input::key::MouseAny, *mouse_hook,
                    [self](hids& gear){ self->handle_mouse_preview(gear); });
                host->bell::submit(tier::preview, input::events::keybd::any, *keyboard_hook)
                    = [self](hids& gear){ self->handle_keyboard(gear); };
            }
        };

        inline void render_trigger(trigger_state& state, popup_menu_cfg const& cfg,
                                   auto& canvas, twod size)
        {
            if (size.x <= 0 || size.y <= 0) return;
            auto available = enabled(cfg);
            if (!available) state.hover = state.press = faux;
            auto bg = cfg.palette.background;
            auto fg = available ? cfg.palette.foreground : cfg.palette.disabled_foreground;
            canvas.fill(rect{ {}, size }, [&](cell& c){ c.bgc(bg).fgc(fg); });
            auto padding = std::max(0, cfg.padding);
            auto label = cfg.label ? cfg.label() : text{};
            if (label.empty())
            {
                put_str(canvas, padding, 0, "▾", fg, bg, std::max(0, size.x - padding));
            }
            else
            {
                paint_label(canvas, padding, 0, label, fg, bg,
                            std::max(0, size.x - 2 * padding - 2));
                auto arrow_x = size.x - padding - 2;
                if (arrow_x >= 0 && size.x > 1) put_str(canvas, arrow_x, 0, " ▾", fg, bg, 2);
            }
            if (available && state.press) canvas.fill(rect{ {}, size }, [](cell& c){ c.xlight(2); });
            else if (available && state.hover) canvas.fill(rect{ {}, size }, [](cell& c){ c.xlight(); });
        }
    }

    inline void dismiss_popup_menu()
    {
        if (auto session = popup_menu_detail::active_session()) session->dismiss();
    }

    inline void open_popup_menu(ui::base& trigger,
                                   std::vector<popup_menu_item> const& items,
                                   popup_menu_open_cfg const& options = {})
    {
        using namespace popup_menu_detail;
        if (items.empty()) return;
        auto& open = trigger.base::property(open_property, faux);
        if (open)
        {
            dismiss_popup_menu();
            return;
        }
        dismiss_popup_menu();

        auto host = find_host(trigger);
        if (!host || host.get() == static_cast<ui::base*>(&trigger)) return;
        auto session = ptr::shared<menu_session>();
        session->host = host;
        session->host_shadow = ptr::shadow(host);
        session->trigger_shadow = ptr::shadow(ui::sptr{ trigger.This() });
        session->keyboard_hook = ptr::shared<hook>();
        session->mouse_hook = ptr::shared<hook>();
        session->source = options.source;
        session->palette = options.palette;
        session->padding = std::max(0, trigger.base::property(padding_property, si32{ 1 }));
        session->explicit_anchor = options.anchor;
        open = true;
        active_session() = session;
        session->install_hooks();

        auto weak_session = netxs::wptr<menu_session>{ session };
        auto anchor = [weak_session]
        {
            auto current = weak_session.lock();
            if (!current) return twod{};
            auto area = current->trigger_rect();
            return current->context_mode()
                 ? area.coor + current->explicit_anchor
                 : area.coor + twod{ 0, area.size.y + 1 };
        };
        session->attach_popup(std::move(anchor), items,
                              options.radio_group, options.selected_index);
        trigger.base::deface();
    }

    namespace popup_menu_detail
    {
        inline auto make_trigger(popup_menu_cfg cfg) -> component
        {
            auto initial_label = cfg.label ? cfg.label() : text{};
            auto width = trigger_width(cfg, initial_label);
            auto form = ui::mock::ctor()->active()
                ->plugin<pro::mouse>()
                ->plugin<pro::focus>(pro::focus::mode::focusable)
                ->plugin<pro::keybd>()
                ->limits({ width, 1 }, { width, 1 });
            form->invoke([cfgv = std::move(cfg)](auto& boss)
            {
                auto& cfg = boss.base::field(popup_menu_cfg{ cfgv });
                auto& state = boss.base::field(trigger_state{});
                auto& open = boss.base::field(std::function<void()>{});
                boss.base::property(padding_property, si32{ 1 }) = cfg.padding;
                open = [&]
                {
                    if (!enabled(cfg)) return;
                    auto items = cfg.items ? cfg.items() : std::vector<popup_menu_item>{};
                    if (items.empty()) return;
                    auto selected = cfg.selected_index ? cfg.selected_index() : -1;
                    open_popup_menu(boss, items,
                        { .source = cfg.source,
                          .radio_group = cfg.radio_group,
                          .selected_index = selected,
                          .palette = cfg.palette });
                };
                boss.LISTEN(tier::release, e2::render::any, canvas)
                {
                    render_trigger(state, cfg, canvas, boss.base::size());
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
                    if (!state.hover && enabled(cfg))
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
                    if (enabled(cfg))
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
                    open();
                    gear.dismiss();
                });
                boss.on(tier::mouserelease, input::key::MouseEnter, [&](hids&)
                {
                    if (cfg.source != popup_menu_source::menu_bar || !enabled(cfg)) return;
                    auto session = active_session();
                    if (!session || session->source != popup_menu_source::menu_bar) return;
                    if (boss.base::property(open_property, faux)) return;
                    if (auto trigger = session->trigger_shadow.lock())
                    {
                        if (trigger.get() == static_cast<ui::base*>(&boss)) return;
                    }
                    auto items = cfg.items ? cfg.items() : std::vector<popup_menu_item>{};
                    if (items.empty()) return;
                    auto selected = cfg.selected_index ? cfg.selected_index() : -1;
                    open_popup_menu(boss, items,
                        { .source = cfg.source,
                          .radio_group = cfg.radio_group,
                          .selected_index = selected,
                          .palette = cfg.palette });
                });
                boss.LISTEN(tier::preview, input::events::keybd::any, gear)
                {
                    if (!state.focused || gear.keybd::handled
                     || gear.payload != input::keybd::type::keypress
                     || gear.keystat == input::key::released
                     || gear.keystat == input::key::interrupted) return;
                    auto key = gear.keybd::generic();
                    if (key == input::key::Space || key == input::key::KeyEnter
                     || key == input::key::KeyDownArrow)
                    {
                        open();
                        gear.set_handled();
                    }
                };
            });
            return { form };
        }
    } // namespace popup_menu_detail

    // Public component factories: each pins the trigger's popup source so
    // callers never set it by hand; the core factory stays in detail.
    inline auto make_dropdown_menu(popup_menu_cfg cfg) -> component
    {
        cfg.source = popup_menu_source::dropdown_menu;
        return popup_menu_detail::make_trigger(std::move(cfg));
    }

    inline auto make_context_menu(popup_menu_cfg cfg) -> component
    {
        cfg.source = popup_menu_source::context_menu;
        return popup_menu_detail::make_trigger(std::move(cfg));
    }

    inline auto make_menu_bar(popup_menu_cfg cfg) -> component
    {
        cfg.source = popup_menu_source::menu_bar;
        return popup_menu_detail::make_trigger(std::move(cfg));
    }
}
