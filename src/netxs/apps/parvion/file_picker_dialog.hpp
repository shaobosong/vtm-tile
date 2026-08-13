// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/file_picker_dialog.hpp: reusable local Open / Save picker dialog.
//
// The picker composes the shared dialog, pane, input, flex, label, and button
// components.  Its requested dimensions use dialog_size directly, so callers
// may choose fixed terminal cells or proportions of the host window without a
// second picker-specific layout policy.

#include "panes.hpp"
#include "components/button.hpp"
#include "components/dialog.hpp"
#include "components/flex.hpp"
#include "components/input.hpp"
#include "components/label.hpp"

namespace netxs::app::parvion
{
    enum class file_picker_mode
    {
        open,
        save,
    };

    enum class file_picker_selection
    {
        files,
        files_and_directories,
    };

    struct file_picker_cfg
    {
        netxs::wptr<ui::base> window_wp;
        netxs::wptr<ui::base> focus_back_wp;
        id_t gear_id{};
        file_picker_mode mode = file_picker_mode::open;
        file_picker_selection selection = file_picker_selection::files;
        text title;
        text initial_dir;
        text name;
        std::function<void(text const&)> on_accept;
        std::function<void()> on_cancel;

        // Shared dialog layout contract. Each dimension can be either
        // dialog_length::cells(...) or dialog_length::ratio(...).
        dialog_anchor position = dialog_anchor::center;
        twod offset{};
        dialog_size size{
            .width  = dialog_length::ratio(0.80),
            .height = dialog_length::ratio(0.80),
        };
        twod minimum{ 50, 14 };
        twod maximum{ 90, 32 };
    };

    enum class file_picker_item_kind
    {
        none,
        parent,
        directory,
        file,
    };

    struct file_picker_target
    {
        file_picker_item_kind kind = file_picker_item_kind::none;
        si32 row = -1;
        text path;
    };

    enum class file_picker_open_action
    {
        none,
        navigate,
        accept,
    };

    struct file_picker_open_result
    {
        file_picker_open_action action = file_picker_open_action::none;
        file_picker_target target;
    };

    // Resolve from the marked set, not pane_state::sel: the navigation cursor
    // intentionally survives Esc after the visible selection has been cleared.
    inline auto file_picker_selected_target(pane_state const& pane) -> file_picker_target
    {
        if (pane.marked.size() != 1) return {};
        auto row = *pane.marked.begin();
        if (row < 0 || row >= pane.total()) return {};
        if (row == 0)
        {
            return {
                .kind = file_picker_item_kind::parent,
                .row = row,
                .path = parent_path(pane.cur_path(), pane.is_local),
            };
        }
        auto& item = pane.cur_items()[(size_t)(row - 1)];
        return {
            .kind = item.is_dir ? file_picker_item_kind::directory
                                : file_picker_item_kind::file,
            .row = row,
            .path = child_path(pane.cur_path(), item.name, pane.is_local),
        };
    }

    inline auto file_picker_resolve_open(pane_state const& pane,
                                         file_picker_selection selection)
        -> file_picker_open_result
    {
        auto target = file_picker_selected_target(pane);
        auto action = file_picker_open_action::none;
        switch (target.kind)
        {
            case file_picker_item_kind::parent:
                action = file_picker_open_action::navigate;
                break;
            case file_picker_item_kind::directory:
                action = selection == file_picker_selection::files_and_directories
                       ? file_picker_open_action::accept
                       : file_picker_open_action::navigate;
                break;
            case file_picker_item_kind::file:
                action = file_picker_open_action::accept;
                break;
            case file_picker_item_kind::none:
            default:
                break;
        }
        return { action, std::move(target) };
    }

    inline auto file_picker_open_enabled(pane_state const& pane,
                                         file_picker_selection selection) -> bool
    {
        return file_picker_resolve_open(pane, selection).action
            != file_picker_open_action::none;
    }

    struct file_picker_state
    {
        pane_state* pane = nullptr;
        text name;
        bool done = faux;
        bool confirming = faux;
        netxs::wptr<ui::base> popup_wp;
        netxs::wptr<ui::base> accept_button_wp;
        netxs::wptr<ui::base> name_input_wp;
    };

    // Returns a standalone modal component. The caller owns attachment to the
    // configured host window (normally `window->base::attach(picker.widget)`).
    inline auto make_file_picker(file_picker_cfg cfg) -> component
    {
        auto state = std::make_shared<file_picker_state>();
        state->name = std::move(cfg.name);

        auto gear_id = cfg.gear_id;
        if (!gear_id)
        {
            if (auto window = cfg.window_wp.lock())
                gear_id = window->bell::indexer.luafx.get_gear().id;
        }

        auto popup_ref = std::make_shared<netxs::wptr<ui::base>>();
        auto finish = [state,
                       window_wp = cfg.window_wp,
                       focus_back_wp = cfg.focus_back_wp,
                       popup_ref,
                       gear_id](std::function<void()> after)
        {
            auto complete = [popup_ref, focus_back_wp, gear_id, after]
            {
                if (auto popup = popup_ref->lock()) popup->base::detach();
                if (auto focus = focus_back_wp.lock())
                {
                    pro::focus::set(focus, gear_id, solo::on);
                    focus->base::deface();
                }
                if (after) after();
            };
            if (auto window = window_wp.lock())
                window->base::enqueue([complete](auto&){ complete(); });
            else
                complete();
        };

        auto accept_path = [state, finish, callback = cfg.on_accept](text path)
        {
            if (std::exchange(state->done, true)) return;
            finish([callback, path = std::move(path)]
            {
                if (callback) callback(path);
            });
        };
        auto cancel = [state, finish, callback = cfg.on_cancel]
        {
            if (std::exchange(state->done, true)) return;
            finish([callback]{ if (callback) callback(); });
        };

        auto saving = cfg.mode == file_picker_mode::save;
        auto pane_widget = make_file_pane("Local site", true, local_lister(),
                                          cfg.initial_dir, !saving, nullptr, nullptr,
                                          &state->pane, cfg.window_wp);

        auto accept = std::make_shared<std::function<void()>>();
        *accept = [state, saving, selection = cfg.selection,
                   window_wp = cfg.window_wp, accept_path]
        {
            if (state->done || !state->pane) return;
            if (saving)
            {
                if (state->name.empty()) return;
                auto path = child_path(state->pane->cur_path(), state->name, true);
                auto ec = std::error_code{};
                if (fs::is_regular_file(fs::path{ path }, ec))
                {
                    if (state->confirming) return;
                    auto window = window_wp.lock();
                    if (!window) return;
                    state->confirming = true;
                    auto filename = fs::path{ path }.filename().string();
                    auto texts = app::shared::confirm_dialog_text{
                        "The file \"" + filename + "\" already exists. Overwrite it?",
                        "Overwrite",
                        "Cancel",
                    };
                    app::shared::show_close_confirmation(*window,
                        [state, accept_path, path]
                        {
                            state->confirming = faux;
                            accept_path(path);
                        },
                        [state]{ state->confirming = faux; },
                        texts);
                    return;
                }
                accept_path(std::move(path));
                return;
            }

            auto result = file_picker_resolve_open(*state->pane, selection);
            if (result.action == file_picker_open_action::accept)
            {
                accept_path(std::move(result.target.path));
            }
            else if (result.action == file_picker_open_action::navigate)
            {
                state->pane->sel = result.target.row;
                state->pane->marked = { result.target.row };
                pane_activate(*state->pane);
                if (auto button = state->accept_button_wp.lock()) button->base::deface();
            }
        };

        auto buttons = flex::ctor({
            .direction = flex_direction::row,
            .justify_content = flex_justify::end,
            .align_items = flex_align::stretch,
            .column_gap = 1,
        });
        if (saving)
        {
            buttons->attach(make_label({
                .value = []{ return text{ "Name:" }; },
                .palette = { .background = theme::surface },
            }), { .shrink = 0, .basis = 5, .minimum = 5, .maximum = 5 });

            auto name_input = make_input({
                .value = [state]{ return state->name; },
                .on_change = [state](text value){ state->name = std::move(value); },
                .on_submit = [accept](text){ (*accept)(); },
                .focus_on_start = true,
                .palette = {
                    .bg = theme::surface,
                    .text_fg = theme::text_fg,
                    .muted_fg = theme::subtext,
                    .active = theme::sel_bg_act,
                },
            });
            state->name_input_wp = ptr::shadow(name_input.widget);
            buttons->attach(std::move(name_input), { .grow = 1, .basis = 1, .minimum = 1 });
        }

        auto accept_button = make_button({
            .label = [saving]{ return saving ? text{ " Save " } : text{ " Open " }; },
            .on_activate = [accept](hids&, ui::base&){ (*accept)(); },
            .enabled = [state, saving, selection = cfg.selection]
            {
                return saving || (state->pane && file_picker_open_enabled(*state->pane, selection));
            },
        });
        state->accept_button_wp = ptr::shadow(accept_button.widget);
        buttons->attach(std::move(accept_button), { .shrink = 0, .basis = 6, .minimum = 6, .maximum = 6 });
        buttons->attach(make_button({
            .label = []{ return text{ " Cancel " }; },
            .on_activate = [cancel](hids&, ui::base&){ cancel(); },
        }), { .shrink = 0, .basis = 8, .minimum = 8, .maximum = 8 });

        if (state->pane)
        {
            state->pane->on_selection_change = [button_wp = state->accept_button_wp]
            {
                if (auto button = button_wp.lock()) button->base::deface();
            };
            if (saving)
            {
                auto fill = [state](text const& path)
                {
                    state->name = fs::path{ path }.filename().string();
                    if (auto input = state->name_input_wp.lock()) input->base::deface();
                };
                state->pane->on_select = fill;
                state->pane->on_pick = [fill, accept](text const& path)
                {
                    fill(path);
                    (*accept)();
                };
            }
            else
            {
                // Row activation remains browser-like: pane_activate handles
                // directories first, so this callback is reached only by files.
                state->pane->on_pick = [accept_path](text const& path){ accept_path(path); };
            }
        }

        auto popup = make_dialog({
            .title = make_label({
                .value = [title = std::move(cfg.title)]{ return title; },
                .palette = {
                    .text = theme::title_fg_act,
                    .background = theme::header,
                },
            }),
            .content = { pane_widget },
            .buttons = { buttons },
            .position = cfg.position,
            .offset = cfg.offset,
            .size = cfg.size,
            .minimum = cfg.minimum,
            .maximum = cfg.maximum,
            .on_cancel = cancel,
        });
        *popup_ref = ptr::shadow(popup.widget);
        state->popup_wp = *popup_ref;

        auto focus_target = saving ? state->name_input_wp
                                   : state->pane ? state->pane->table_wp
                                                 : netxs::wptr<ui::base>{};
        if (auto window = cfg.window_wp.lock())
        {
            window->base::enqueue([focus_target, gear_id](auto&)
            {
                if (auto target = focus_target.lock())
                    pro::focus::set(target, gear_id, solo::on);
            });
        }
        return popup;
    }
}
