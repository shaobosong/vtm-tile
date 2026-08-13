// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/secret_dialog.hpp: a reusable masked single-line input modal (make_secret_dialog),
// mirroring FileZilla's CInteractiveLoginNotification / CInputDialog. Used in two places:
//   - the Settings dialog's key-conversion flow (settings_dialog.hpp sd_begin_convert):
//     ask for an encrypted key's passphrase before converting it to .ppk via pvputtygen;
//   - the live SFTP login (session.hpp / parvion.hpp): when the backend asks for an SSH
//     key passphrase ("SSH key passphrase" preamble) or an account password.
//
// Rendered as a component-based centered modal, exactly like the Settings dialog.

#include "panes.hpp" // theme, put_str, sd_hit, and shared dialog helpers
#include "components/button.hpp"
#include "components/dialog.hpp"
#include "components/flex.hpp"
#include "components/input.hpp"
#include "components/label.hpp"

#include <thread>
#include <atomic>
#include <memory>

namespace netxs::app::parvion
{
    struct secret_dialog_work
    {
        text status;
        std::function<bool(text const&, std::vector<text>&)> run;
        std::function<void(std::vector<text>, bool)> on_done;
    };

    struct secret_dialog_cfg
    {
        netxs::wptr<ui::base> window_wp;
        netxs::wptr<ui::base> focus_back_wp;
        text title;
        text prompt;
        std::function<void(text)> on_submit;
        std::function<void()> on_cancel;
        bool is_retry = faux;
        bool secret = true;
        text initial;
        secret_dialog_work work;
    };

    struct secret_state
    {
        text value;
        bool done = faux;
        bool busy = faux;
        si32 spinner_frame = 0;
        std::function<void(text)> on_submit;
        std::function<void()>     on_cancel;
        secret_dialog_work work;
        netxs::wptr<ui::base> popup_wp;
        netxs::wptr<ui::base> retry_wp;
        netxs::wptr<ui::base> progress_wp;
        netxs::wptr<ui::base> status_separator_wp;
    };

    struct secret_job
    {
        std::atomic<bool> done{ faux };
        std::vector<text> replies; // Written by the worker before the release-store to done.
        bool ok = faux;
    };

    // A centered, dimming-overlay modal collecting one masked line. `on_submit(value)` runs on
    // OK/Enter, `on_cancel()` on Cancel/Esc/click-outside; both fire at most once and the overlay
    // is torn down (via window->enqueue, so it is safe to call from a key handler) before the
    // callback runs. `focus_back_wp` (when non-null) regains keyboard focus afterwards — the
    // settings card in the convert flow; left empty at login (the window hub refocuses).
    inline auto make_secret_dialog(secret_dialog_cfg cfg) -> ui::sptr
    {
        auto state = std::make_shared<secret_state>();
        state->value = std::move(cfg.initial);
        state->on_submit = std::move(cfg.on_submit);
        state->on_cancel = std::move(cfg.on_cancel);
        state->work = std::move(cfg.work);
        auto popup_ref = std::make_shared<netxs::wptr<ui::base>>();
        auto gear_id = id_t{};
        if (auto window = cfg.window_wp.lock())
            gear_id = window->bell::indexer.luafx.get_gear().id;

        // Tear down + refocus + run a follow-up, all deferred onto the (longer-lived) window so it
        // is safe from inside a mid-execution key/mouse handler (mirrors sd_open_key_picker).
        auto finish = [window_wp = cfg.window_wp,
                       focus_back_wp = cfg.focus_back_wp,
                       popup_ref,
                       gear_id](std::function<void()> after)
        {
            auto go = [popup_ref, focus_back_wp, after, gear_id](auto&)
            {
                if (auto popup = popup_ref->lock()) popup->base::detach();
                // Hand keyboard focus back, keyed to the gear that opened the prompt (real focus).
                if (auto c = focus_back_wp.lock()) { pro::focus::set(c, gear_id, solo::on); c->base::deface(); }
                if (after) after();
            };
            if (auto w = window_wp.lock()) w->base::enqueue(go);
        };

        // Switch the optional retry report to the live progress report. Both labels occupy the
        // same flex position, so the input and its separator retain the intended vertical rhythm.
        auto run_work = [state, finish]
        {
            if (state->done || std::exchange(state->busy, true)) return;
            if (auto retry = state->retry_wp.lock()) retry->base::hidden = true;
            if (auto progress = state->progress_wp.lock()) progress->base::hidden = faux;
            if (auto separator = state->status_separator_wp.lock()) separator->base::hidden = faux;
            if (auto popup = state->popup_wp.lock())
            {
                popup->base::deface();
                popup->base::reflow();
            }

            auto job = std::make_shared<secret_job>();
            auto value = state->value;
            auto work = state->work.run;
            std::thread{ [job, value = std::move(value), work = std::move(work)]
            {
                auto replies = std::vector<text>{};
                auto ok = work ? work(value, replies) : faux;
                job->replies = std::move(replies);
                job->ok = ok;
                job->done.store(true, std::memory_order_release);
            } }.detach();

            if (auto progress = state->progress_wp.lock())
            {
                progress->base::plugin<pro::timer>().actify(std::chrono::milliseconds{ 100 },
                    [job, state, finish](auto) -> bool
                    {
                        if (!job->done.load(std::memory_order_acquire))
                        {
                            ++state->spinner_frame;
                            if (auto label = state->progress_wp.lock()) label->base::deface();
                            return true;
                        }
                        state->done = true;
                        auto replies = std::move(job->replies);
                        auto okay = job->ok;
                        auto callback = state->work.on_done;
                        finish([callback, replies = std::move(replies), okay]() mutable
                        {
                            if (callback) callback(std::move(replies), okay);
                        });
                        return faux;
                    });
            }
        };
        auto submit = [state, finish, run_work]
        {
            if (state->done || state->busy) return;
            if (state->work.run)
            {
                run_work();
                return;
            }
            state->done = true;
            auto value = state->value;
            auto callback = state->on_submit;
            finish([callback, value = std::move(value)]{ if (callback) callback(value); });
        };
        auto cancel = [state, finish]
        {
            if (state->busy) return;
            if (std::exchange(state->done, true)) return;
            auto callback = state->on_cancel;
            finish([callback]{ if (callback) callback(); });
        };

        auto content = flex::ctor({
            .direction = flex_direction::column,
            .column_padding_width = 2,
            .row_padding_height = 1,
        });
        content->attach(make_label({
            .value = [prompt = std::move(cfg.prompt)]{ return prompt; },
            .wrap = true,
        }), { .shrink = 0 });

        content->attach_separator(1);

        auto retry = make_label({
            .value = []{ return text{ "Incorrect passphrase, try again." }; },
            .palette = { .text = theme::err_fg },
        });
        retry.widget->base::hidden = !cfg.is_retry;
        state->retry_wp = ptr::shadow(retry.widget);
        content->attach(std::move(retry), { .shrink = 0 });

        static constexpr auto spinner = std::array<view, 4>{ "|", "/", "-", "\\" };
        auto progress = make_label({
            .value = [state]
            {
                return text{ spinner[(size_t)(state->spinner_frame % spinner.size())] }
                     + "  " + state->work.status;
            },
        });
        progress.widget->base::hidden = true;
        progress.widget->base::plugin<pro::timer>();
        state->progress_wp = ptr::shadow(progress.widget);
        content->attach(std::move(progress), { .shrink = 0 });

        auto status_separator = content->attach_separator(1);
        status_separator->base::hidden = !cfg.is_retry;
        state->status_separator_wp = ptr::shadow(status_separator);

        auto input = make_input({
            .value = [state]{ return state->value; },
            .on_change = [state](text value){ state->value = std::move(value); },
            .mode = [state]{ return state->busy ? input_mode::disabled : input_mode::edit; },
            .on_submit = [submit](text){ submit(); },
            .on_cancel = cancel,
            .secret = cfg.secret,
            .focus_on_start = true,
            .palette = { .bg = theme::bg, .text_fg = theme::text_fg,
                         .muted_fg = theme::subtext, .active = theme::sel_bg_act },
        });
        auto input_wp = ptr::shadow(input.widget);
        content->attach(std::move(input), { .shrink = 0, .basis = 1, .minimum = 1, .maximum = 1 });

        auto buttons = flex::ctor({
            .direction = flex_direction::row,
            .justify_content = flex_justify::end,
            .align_items = flex_align::stretch,
            .column_gap = 1,
        });
        buttons->attach(make_button({
            .label = []{ return text{ " OK " }; },
            .on_activate = [submit](hids&, ui::base&){ submit(); },
            .enabled = [state]{ return !state->busy; },
        }), { .basis = 6, .minimum = 6, .maximum = 6 });
        buttons->attach(make_button({
            .label = []{ return text{ " Cancel " }; },
            .on_activate = [cancel](hids&, ui::base&){ cancel(); },
            .enabled = [state]{ return !state->busy; },
        }), { .basis = 10, .minimum = 10, .maximum = 10 });

        auto popup = make_dialog({
            .title = make_label({
                .value = [title = std::move(cfg.title)]{ return title; },
                .palette = {
                    .text = theme::title_fg_act,
                    .background = theme::header,
                },
            }),
            .content = { content },
            .buttons = { buttons },
            .position = dialog_anchor::center,
            .size = {
                .width = dialog_length::cells(64),
            },
            .minimum = { 44, 3 },
            .maximum = { 64, -1 },
            .fit_content_height = true,
            .can_dismiss = [state]{ return !state->busy; },
            .on_cancel = cancel,
        });
        *popup_ref = ptr::shadow(popup.widget);
        state->popup_wp = *popup_ref;

        // Take real keyboard focus on the card once the caller has attached the overlay, so Esc/Enter/
        // typing reach it without a click. Keyed to the active input gear (read now, while we are still
        // inside the triggering event — e.g. the key activation in the picker), then enqueued so it lands
        // after the attach (which is synchronous right after we return; mirrors sd_open_key_picker).
        if (auto w = cfg.window_wp.lock())
        {
            w->base::enqueue([input_wp, gear_id](auto&)
            {
                if (auto input = input_wp.lock()) pro::focus::set(input, gear_id, solo::on);
            });
        }
        return popup.widget;
    }
}
