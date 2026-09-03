// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/conflict_dialog.hpp: reusable conflict-policy modal for one enqueue
// batch. The controller owns the continuation; this dialog only collects one
// of the policy choices and tears itself down before returning it.

#include "conflict.hpp"
#include "components/button.hpp"
#include "components/dialog.hpp"
#include "components/dropdown.hpp"
#include "components/flex.hpp"
#include "components/label.hpp"

#include <memory>

namespace netxs::app::parvion
{
    struct conflict_dialog_cfg
    {
        netxs::wptr<ui::base> window_wp;
        text destination;
        std::function<void(conflict_choice)> on_choice;
    };

    struct conflict_dialog_state
    {
        si32 selected = 0;
        bool done = faux;
        netxs::wptr<ui::base> popup_wp;
        std::function<void(conflict_choice)> on_choice;
    };

    inline auto make_conflict_dialog(conflict_dialog_cfg cfg) -> ui::sptr
    {
        auto state = std::make_shared<conflict_dialog_state>();
        state->on_choice = std::move(cfg.on_choice);
        auto finish = [state, window_wp = cfg.window_wp](conflict_choice choice)
        {
            if (std::exchange(state->done, true)) return;
            auto popup_wp = state->popup_wp;
            auto on_choice = state->on_choice;
            auto close = [popup_wp, on_choice = std::move(on_choice), choice](auto&)
            {
                if (auto popup = popup_wp.lock()) popup->base::detach();
                if (on_choice) on_choice(choice);
            };
            if (auto window = window_wp.lock()) window->base::enqueue(std::move(close));
        };

        auto options = std::vector<dropdown_option>{
            { "Overwrite" }, { "Overwrite all" },
            { "Skip" },      { "Skip all" },
            { "Rename" },    { "Rename all" },
            { "Cancel rest" },
        };
        auto dropdown = make_dropdown({
            .options = std::move(options),
            .selected = [state]{ return state->selected; },
            .on_change = [state](si32 selected){ state->selected = selected; },
        });
        auto content = flex::ctor({
            .direction = flex_direction::column,
            .column_padding_width = 2,
            .row_padding_height = 1,
        });
        content->attach(make_label({
            .value = [destination = std::move(cfg.destination)]
                     { return text{ "Destination already exists:\n" } + destination; },
            .overflow = label_overflow::wrap,
        }), { .shrink = 0 });
        content->attach_separator(1);
        content->attach(make_label({
            .value = []{ return text{ "Choose how to handle this file and later conflicts:" }; },
            .overflow = label_overflow::wrap,
        }), { .shrink = 0 });
        content->attach(std::move(dropdown), { .shrink = 0, .basis = 1, .minimum = 1, .maximum = 1 });

        auto choose = [state, finish]
        {
            static constexpr auto choices = std::array<conflict_choice, 7>{
                conflict_choice::overwrite, conflict_choice::overwrite_all,
                conflict_choice::skip, conflict_choice::skip_all,
                conflict_choice::rename, conflict_choice::rename_all,
                conflict_choice::cancel_rest,
            };
            auto index = std::clamp(state->selected, 0, (si32)choices.size() - 1);
            finish(choices[(size_t)index]);
        };
        auto cancel = [finish]{ finish(conflict_choice::cancel_rest); };
        auto buttons = flex::ctor({
            .direction = flex_direction::row,
            .justify_content = flex_justify::end,
            .align_items = flex_align::stretch,
            .column_gap = 1,
        });
        buttons->attach(make_button({
            .label = []{ return text{ " OK " }; },
            .on_activate = [choose](hids&, ui::base&){ choose(); },
        }), { .basis = 6, .minimum = 6, .maximum = 6 });
        buttons->attach(make_button({
            .label = []{ return text{ " Cancel " }; },
            .on_activate = [cancel](hids&, ui::base&){ cancel(); },
        }), { .basis = 10, .minimum = 10, .maximum = 10 });

        auto popup = make_dialog({
            .title = make_label({
                .value = []{ return text{ "Transfer conflict" }; },
                .palette = { .text = theme::title_fg_act, .background = theme::header },
            }),
            .content = { content },
            .buttons = { buttons },
            .position = dialog_anchor::center,
            .size = { .width = dialog_length::cells(76) },
            .minimum = { 52, 3 },
            .maximum = { 96, -1 },
            .fit_content_height = true,
            .on_cancel = std::move(cancel),
        });
        state->popup_wp = ptr::shadow(popup.widget);
        return popup.widget;
    }
}
