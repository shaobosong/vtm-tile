// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/prompts.hpp: a reusable masked single-line input modal (make_secret_dialog),
// mirroring FileZilla's CInteractiveLoginNotification / CInputDialog. Used in two places:
//   - the Settings dialog's key-conversion flow (settings_dialog.hpp sd_begin_convert):
//     ask for an encrypted key's passphrase before converting it to .ppk via pvputtygen;
//   - the live SFTP login (session.hpp / parvion.hpp): when the backend asks for an SSH
//     key passphrase ("SSH key passphrase" preamble) or an account password.
//
// Rendered command_bar-style as a
// centered modal card inside a dimming full-window overlay, exactly like the settings
// dialog and its file picker (settings_dialog.hpp make_settings_dialog / sd_open_key_picker).
// Reuse: the masked value is a shared make_input child, and OK/Cancel are independent
// make_button() widgets layered over the card.

#include "panes.hpp" // theme, put_str, sd_hit, and shared dialog helpers
#include "components/button.hpp"

#include <thread>
#include <atomic>
#include <memory>

namespace netxs::app::parvion
{
    // Greedy word-wrap `s` to lines no wider than `maxw` cells (best-effort; an over-long
    // word is hard-clipped by put_str at render time).
    inline auto secret_wrap(view s, si32 maxw) -> std::vector<text>
    {
        auto out = std::vector<text>{};
        if (maxw < 1) maxw = 1;
        auto line = text{};
        auto flush = [&]{ out.push_back(line); line.clear(); };
        auto words = std::vector<text>{};
        auto cur = text{};
        for (auto c : s) { if (c == ' ') { words.push_back(cur); cur.clear(); } else cur.push_back(c); }
        words.push_back(cur);
        for (auto& w : words)
        {
            if (line.empty()) line = w;
            else if ((si32)cell_width(line) + 1 + (si32)cell_width(w) <= maxw) { line += ' '; line += w; }
            else flush(), line = w;
        }
        flush();
        return out;
    }

    struct secret_state
    {
        text title, prompt;
        bool is_retry = faux;
        text value;               // Bound to the shared make_input child.
        rect ok_box{}, cancel_box{};
        netxs::wptr<ui::base> input_wp, ok_button_wp, cancel_button_wp;
        bool done = faux;         // Submit/cancel fire exactly once.
        std::function<void(text)> on_submit;
        std::function<void()>     on_cancel;
    };

    inline void secret_render(secret_state& st, auto& canvas, twod sz)
    {
        canvas.fill(rect{ {}, sz }, [](cell& c){ c.bgc(theme::bg).fgc(theme::text_fg); });
        // Title strip.
        canvas.fill(rect{ {}, { sz.x, 1 } }, [](cell& c){ c.bgc(theme::header); });
        put_str(canvas, 2, 0, st.title, theme::title_fg_act, theme::header, sz.x - 4);
        auto inner = std::max(1, sz.x - 4);
        // Prompt text (wrapped, up to 3 lines), then the optional retry note.
        auto y = si32{ 2 };
        auto lines = secret_wrap(st.prompt, inner);
        for (auto i = size_t{}; i < lines.size() && i < 3; ++i) put_str(canvas, 2, y++, lines[i], theme::text_fg, theme::bg, inner);
        if (st.is_retry) put_str(canvas, 2, y++, "Incorrect passphrase, try again.", theme::err_fg, theme::bg, inner);
        // Field on the row two above the button row; buttons on the last-but-one row.
        if (auto input = st.input_wp.lock())
            input->base::extend(rect{ { 2, sz.y - 4 }, { inner, 1 } });
        auto by   = sz.y - 2;
        auto cnw  = si32{ 10 }, okw = si32{ 6 };
        st.cancel_box = rect{ { sz.x - 2 - cnw, by }, { cnw, 1 } };
        st.ok_box     = rect{ { st.cancel_box.coor.x - 2 - okw, by }, { okw, 1 } };
        if (auto button = st.ok_button_wp.lock())     button->base::extend(st.ok_box);
        if (auto button = st.cancel_button_wp.lock()) button->base::extend(st.cancel_box);
    }

    // A centered, dimming-overlay modal collecting one masked line. `on_submit(value)` runs on
    // OK/Enter, `on_cancel()` on Cancel/Esc/click-outside; both fire at most once and the overlay
    // is torn down (via window->enqueue, so it is safe to call from a key handler) before the
    // callback runs. `focus_back_wp` (when non-null) regains keyboard focus afterwards — the
    // settings card in the convert flow; left empty at login (the window hub refocuses).
    inline auto make_secret_dialog(netxs::wptr<ui::base> window_wp, netxs::wptr<ui::base> focus_back_wp,
                                   text title, text prompt,
                                   std::function<void(text)> on_submit, std::function<void()> on_cancel,
                                   bool is_retry = faux, bool secret = true, text initial = {}) -> ui::sptr
    {
        auto overlay = ui::cake::ctor()->alignment({ snap::both, snap::both });
        auto overlay_wp = ptr::shadow(overlay);
        // Tear down + refocus + run a follow-up, all deferred onto the (longer-lived) window so it
        // is safe from inside a mid-execution key/mouse handler (mirrors sd_open_key_picker).
        auto finish = [window_wp, overlay_wp, focus_back_wp](std::function<void()> after)
        {
            auto go = [overlay_wp, focus_back_wp, after](auto& win)
            {
                if (auto o = overlay_wp.lock()) o->base::detach();
                // Hand keyboard focus back, keyed to the gear that submitted/cancelled (real focus).
                if (auto c = focus_back_wp.lock()) { pro::focus::set(c, win.bell::indexer.luafx.get_gear().id, solo::on); c->base::deface(); }
                if (after) after();
            };
            if (auto w = window_wp.lock()) w->base::enqueue(go);
        };
        // Dimming backdrop (click outside cancels).
        overlay->attach(ui::mock::ctor())->invoke([finish](auto& boss)
        {
            auto myid = boss.bell::id;
            boss.LISTEN(tier::release, e2::render::background::any, parent_canvas, -, (myid))
            {
                parent_canvas.fill([myid](cell& c){ c.bgc().faint(); c.fgc().faint(); c.link(myid); });
            };
            boss.on(tier::mouserelease, input::key::LeftClick, [finish](hids& gear){ finish(nullptr); gear.dismiss(); });
        });
        auto card_layer = overlay->attach(ui::cake::ctor())
            ->alignment({ snap::center, snap::center })
            ->limits({ 44, 10 }, { 64, 12 });
        auto card_layer_wp = ptr::shadow(card_layer);
        auto input_ref = std::make_shared<netxs::wptr<ui::base>>();
        auto card = card_layer->attach(ui::mock::ctor())
            ->active()
            ->plugin<pro::mouse>()
            ->plugin<pro::focus>(pro::focus::mode::focusable)
            ->plugin<pro::keybd>();
        card->invoke([finish, title, prompt, is_retry, secret, initial, on_submit, on_cancel, card_layer_wp, input_ref](auto& boss)
        {
            auto& st = boss.base::field(secret_state{});
            st.title = title; st.prompt = prompt; st.is_retry = is_retry;
            st.value = initial;
            st.on_submit = on_submit; st.on_cancel = on_cancel;
            auto submit = [&st, finish]
            {
                if (st.done) return;
                st.done = true;
                auto v = st.value; auto cb = st.on_submit;
                finish([cb, v]{ if (cb) cb(v); });
            };
            auto cancel = [&st, finish]
            {
                if (st.done) return;
                st.done = true;
                auto cb = st.on_cancel;
                finish([cb]{ if (cb) cb(); });
            };
            if (auto layer = card_layer_wp.lock())
            {
                auto input = make_input({
                    .value = [&st]{ return st.value; },
                    .on_change = [&st](text value){ st.value = std::move(value); },
                    .on_submit = [submit](text){ submit(); },
                    .on_cancel = [cancel]{ cancel(); },
                    .secret = secret,
                    .focus_on_start = true,
                    .palette = { .bg = theme::bg, .text_fg = theme::text_fg,
                                 .muted_fg = theme::subtext, .active = theme::sel_bg_act },
                });
                st.input_wp = ptr::shadow(input.widget);
                *input_ref = st.input_wp;
                layer->base::attach(input.widget);
                auto ok = make_button({
                    .label = []{ return text{ " OK " }; },
                    .on_activate = [submit](hids&, ui::base&){ submit(); },
                });
                st.ok_button_wp = ptr::shadow(ok.widget);
                layer->base::attach(ok.widget);
                auto cancel_button = make_button({
                    .label = []{ return text{ " Cancel " }; },
                    .on_activate = [cancel](hids&, ui::base&){ cancel(); },
                });
                st.cancel_button_wp = ptr::shadow(cancel_button.widget);
                layer->base::attach(cancel_button.widget);
            }
            boss.LISTEN(tier::release, e2::render::any, parent_canvas)
            {
                secret_render(st, parent_canvas, boss.base::size());
            };
            boss.LISTEN(tier::preview, input::events::keybd::any, gear, -, (submit, cancel))
            {
                if (gear.payload != input::keybd::type::keypress) return;
                if (gear.keystat == input::key::released || gear.keystat == input::key::interrupted) return;
                if (gear.keybd::handled) return;
                auto k = gear.keybd::generic();
                if      (k == input::key::Esc)      { gear.set_handled(); cancel(); }
                else if (k == input::key::KeyEnter) { gear.set_handled(); submit(); }
            };
        });
        // Take real keyboard focus on the card once the caller has attached the overlay, so Esc/Enter/
        // typing reach it without a click. Keyed to the active input gear (read now, while we are still
        // inside the triggering event — e.g. the key activation in the picker), then enqueued so it lands
        // after the attach (which is synchronous right after we return; mirrors sd_open_key_picker).
        if (auto w = window_wp.lock())
        {
            auto gid = w->bell::indexer.luafx.get_gear().id;
            w->base::enqueue([cardw = ptr::shadow(card), input_ref, gid](auto&)
            {
                if (auto input = input_ref->lock()) pro::focus::set(input, gid, solo::on);
                else if (auto c = cardw.lock()) pro::focus::set(c, gid, solo::on);
            });
        }
        return overlay;
    }

    // --- Async "busy" runner (keeps the TUI responsive during slow work) --------------------------
    // Run a slow, BLOCKING `work` (e.g. pvputtygen key decryption / Argon2 .ppk write, ~1s) on a
    // background thread behind a centered spinner modal, then deliver its result to `on_done` ON THE
    // UI THREAD. The crypto never runs on the UI thread, so the terminal stays live (the spinner
    // animates) instead of freezing. Mirrors the proven sftp/ldel pattern: a detached std::thread
    // writes the result + sets an atomic flag; a pro::timer on the modal polls it and finishes here.
    // (FileZilla blocks its UI for the same work; this is the graceful alternative.)
    struct progress_job
    {
        std::atomic<bool> done{ faux };
        std::vector<text> replies;  // Written by the worker BEFORE done; read by the UI AFTER.
        bool ok = faux;
    };
    inline void run_with_progress(netxs::wptr<ui::base> window_wp, netxs::wptr<ui::base> focus_back_wp, id_t gear_id,
                                  text title,
                                  std::function<bool(std::vector<text>&)> work,        // worker thread: fills replies, returns ok.
                                  std::function<void(std::vector<text>, bool)> on_done) // UI thread: the result.
    {
        auto window = window_wp.lock();
        if (!window) return;
        if (!gear_id) gear_id = window->bell::indexer.luafx.get_gear().id;
        auto job = std::make_shared<progress_job>();
        std::thread{ [job, work]
        {
            auto reps = std::vector<text>{};
            auto ok = work ? work(reps) : faux;
            job->replies = std::move(reps);
            job->ok = ok;
            job->done.store(true, std::memory_order_release); // Release: results are visible before the flag.
        } }.detach();
        auto overlay = ui::cake::ctor()->alignment({ snap::both, snap::both });
        auto overlay_wp = ptr::shadow(overlay);
        // Dimming backdrop with NO click handler: a running conversion can't be cancelled mid-write.
        overlay->attach(ui::mock::ctor())->invoke([](auto& boss)
        {
            auto myid = boss.bell::id;
            boss.LISTEN(tier::release, e2::render::background::any, parent_canvas, -, (myid))
            {
                parent_canvas.fill([myid](cell& c){ c.bgc().faint(); c.fgc().faint(); c.link(myid); });
            };
        });
        auto card = overlay->attach(ui::mock::ctor())
            ->active()
            ->alignment({ snap::center, snap::center })
            ->limits({ 40, 5 }, { 60, 5 })
            ->plugin<pro::mouse>()
            ->plugin<pro::focus>(pro::focus::mode::focused)
            ->plugin<pro::keybd>() // Grabs focus to swallow input while the work runs.
            ->plugin<pro::timer>();
        card->invoke([job, title, window_wp, overlay_wp, focus_back_wp, gear_id, on_done](auto& boss)
        {
            auto frame = ptr::shared(si32{ 0 });
            boss.LISTEN(tier::release, e2::render::any, parent_canvas, -, (title, frame))
            {
                auto sz = boss.base::size();
                parent_canvas.fill(rect{ {}, sz }, [](cell& c){ c.bgc(theme::bg).fgc(theme::text_fg); });
                parent_canvas.fill(rect{ {}, { sz.x, 1 } }, [](cell& c){ c.bgc(theme::header); });
                put_str(parent_canvas, 2, 0, "Please wait", theme::title_fg_act, theme::header, std::max(0, sz.x - 4));
                static constexpr auto spin = std::array<view, 4>{ "|", "/", "-", "\\" };
                put_str(parent_canvas, 2, sz.y / 2, text{ spin[(size_t)(*frame % 4)] } + "  " + title, theme::text_fg, theme::bg, std::max(0, sz.x - 4));
            };
            boss.base::template plugin<pro::timer>().actify(std::chrono::milliseconds{ 100 },
                [job, &boss, frame, window_wp, overlay_wp, focus_back_wp, gear_id, on_done](auto) -> bool
                {
                    if (!job->done.load(std::memory_order_acquire)) { ++*frame; boss.base::deface(); return true; } // Spin.
                    // Done. Tear down the spinner FIRST (deferred so we don't destroy this very timer
                    // mid-callback, and so any modal on_done opens grabs focus AFTER our refocus); then
                    // deliver the result here on the UI thread.
                    auto reps = std::move(job->replies);
                    auto okay = job->ok;
                    if (auto w = window_wp.lock()) w->base::enqueue([overlay_wp, focus_back_wp, gear_id](auto&)
                    {
                        if (auto o = overlay_wp.lock()) o->base::detach();
                        if (auto c = focus_back_wp.lock()) { pro::focus::set(c, gear_id, solo::on); c->base::deface(); }
                    });
                    if (on_done) on_done(std::move(reps), okay);
                    return faux; // Stop the timer (the hook is erased after we return; the card is still alive).
                });
        });
        window->base::attach(overlay);
        // Grab keyboard focus so input is blocked while the work runs.
        window->base::enqueue([cardw = ptr::shadow(card), gear_id](auto&){ if (auto c = cardw.lock()) pro::focus::set(c, gear_id, solo::on); });
    }
}
