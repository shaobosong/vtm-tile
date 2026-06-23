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
// Rendered command_bar-style (direct canvas paint + manual keyboard/mouse handling) as a
// centered modal card inside a dimming full-window overlay, exactly like the settings
// dialog and its file picker (settings_dialog.hpp make_settings_dialog / sd_open_key_picker).
// Reuse: the masked field is painted by the shared paint_field(secret=true) and buttons by
// paint_button (panes.hpp); sd_hit (panes.hpp) hit-tests the boxes.

#include "panes.hpp" // theme, put_str, paint_field, paint_button, edit_*, sd_hit, cluster_count

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
        input_field fld;          // The shared connect-bar input box (fld.secret masks a passphrase).
        rect ok_box{}, cancel_box{};
        bool hov_ok = faux, hov_cancel = faux, prs_ok = faux, prs_cancel = faux;
        bool focused = faux;
        bool dragging = faux;     // A left-drag that began in the field is scrubbing the caret.
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
        field_paint(canvas, st.fld, rect{ { 2, sz.y - 4 }, { inner, 1 } }, /*active*/true);
        auto by   = sz.y - 2;
        auto cnw  = si32{ 10 }, okw = si32{ 6 };
        st.cancel_box = rect{ { sz.x - 2 - cnw, by }, { cnw, 1 } };
        st.ok_box     = rect{ { st.cancel_box.coor.x - 2 - okw, by }, { okw, 1 } };
        paint_button(canvas, st.ok_box,     " OK ",     st.hov_ok,     st.prs_ok);
        paint_button(canvas, st.cancel_box, " Cancel ", st.hov_cancel, st.prs_cancel);
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
        auto card = overlay->attach(ui::mock::ctor())
            ->active()
            ->alignment({ snap::center, snap::center })
            ->limits({ 44, 10 }, { 64, 12 })
            ->plugin<pro::mouse>()
            ->plugin<pro::focus>(pro::focus::mode::focused)
            ->plugin<pro::keybd>();
        card->invoke([finish, title, prompt, is_retry, secret, initial, on_submit, on_cancel](auto& boss)
        {
            auto& st = boss.base::field(secret_state{});
            st.title = title; st.prompt = prompt; st.is_retry = is_retry;
            st.fld.secret = secret; st.fld.val = initial; st.fld.caret = cluster_count(initial);
            st.on_submit = on_submit; st.on_cancel = on_cancel;
            auto submit = [&st, finish]
            {
                if (st.done) return;
                st.done = true;
                auto v = st.fld.val; auto cb = st.on_submit;
                finish([cb, v]{ if (cb) cb(v); });
            };
            auto cancel = [&st, finish]
            {
                if (st.done) return;
                st.done = true;
                auto cb = st.on_cancel;
                finish([cb]{ if (cb) cb(); });
            };
            boss.LISTEN(tier::release, e2::render::any, parent_canvas)
            {
                secret_render(st, parent_canvas, boss.base::size());
            };
            boss.LISTEN(tier::release, e2::form::state::focus::count, count)
            {
                st.focused = !!count;
                boss.base::deface();
            };
            boss.on(tier::mouserelease, input::key::LeftDown, [&boss, &st](hids& gear)
            {
                pro::focus::set(boss.This(), gear.id, solo::on);
                auto mx = (si32)gear.coord.x, my = (si32)gear.coord.y;
                if (sd_hit(st.ok_box, mx, my))     st.prs_ok = true;
                if (sd_hit(st.cancel_box, mx, my)) st.prs_cancel = true;
                if (field_hit(st.fld, mx, my))     field_caret_to(st.fld, mx);
                boss.base::deface(); gear.dismiss();
            });
            boss.on(tier::mouserelease, input::key::MouseMove, [&boss, &st](hids& gear)
            {
                auto mx = (si32)gear.coord.x, my = (si32)gear.coord.y;
                auto o = sd_hit(st.ok_box, mx, my), c = sd_hit(st.cancel_box, mx, my);
                auto dirty = faux;
                if (st.hov_ok != o)     { st.hov_ok = o;     dirty = true; }
                if (st.hov_cancel != c) { st.hov_cancel = c; dirty = true; }
                if (st.prs_ok && !o)     { st.prs_ok = faux;     dirty = true; }
                if (st.prs_cancel && !c) { st.prs_cancel = faux; dirty = true; }
                if (dirty) boss.base::deface();
            });
            boss.on(tier::mouserelease, input::key::LeftUp, [&boss, &st](hids&)
            {
                if (st.prs_ok || st.prs_cancel) { st.prs_ok = st.prs_cancel = faux; boss.base::deface(); }
            });
            boss.on(tier::mouserelease, input::key::MouseLeave, [&boss, &st](hids&)
            {
                if (st.hov_ok || st.hov_cancel || st.prs_ok || st.prs_cancel) { st.hov_ok = st.hov_cancel = st.prs_ok = st.prs_cancel = faux; boss.base::deface(); }
            });
            boss.on(tier::mouserelease, input::key::LeftClick, [&st, submit, cancel](hids& gear)
            {
                auto mx = (si32)gear.coord.x, my = (si32)gear.coord.y;
                if      (sd_hit(st.ok_box, mx, my))     submit();
                else if (sd_hit(st.cancel_box, mx, my)) cancel();
                gear.dismiss();
            });
            // Connect-bar caret scrubbing: a left-drag that began in the field keeps the caret under the
            // cursor (the render's window clamp auto-scrolls at the edges); draggable adds pointer capture.
            boss.base::signal(tier::release, e2::form::draggable::_<hids::buttons::left>, true);
            boss.LISTEN(tier::release, e2::form::drag::start::_<hids::buttons::left>, gear) { st.dragging = field_hit(st.fld, (si32)gear.click.x, (si32)gear.click.y); };
            boss.LISTEN(tier::release, e2::form::drag::pull::_<hids::buttons::left>,  gear) { if (st.dragging) { field_caret_to(st.fld, (si32)gear.coord.x); boss.base::deface(); } };
            boss.LISTEN(tier::release, e2::form::drag::stop::_<hids::buttons::left>,   gear) { st.dragging = faux; };
            boss.LISTEN(tier::release, e2::form::drag::cancel::_<hids::buttons::left>, gear) { st.dragging = faux; };
            boss.LISTEN(tier::preview, input::events::keybd::any, gear, -, (submit, cancel))
            {
                if (!st.focused) return;
                if (gear.payload == input::keybd::type::keypaste)
                {
                    field_insert(st.fld, edit_filter(gear.cluster));
                    boss.base::deface(); gear.set_handled();
                    return;
                }
                if (gear.payload != input::keybd::type::keypress) return;
                if (gear.keystat == input::key::released || gear.keystat == input::key::interrupted) return;
                if (gear.keybd::handled) return;
                auto k = gear.keybd::generic();
                auto act = true;
                     if (k == input::key::Esc)      cancel();
                else if (k == input::key::KeyEnter)  submit();
                else if (field_key(st.fld, k, gear.cluster)) {} // Editing/navigation handled by the field.
                else act = faux;
                if (act) { gear.set_handled(); boss.base::deface(); }
            };
        });
        // Take real keyboard focus on the card once the caller has attached the overlay, so Esc/Enter/
        // typing reach it without a click. Keyed to the active input gear (read now, while we are still
        // inside the triggering event — e.g. the key activation in the picker), then enqueued so it lands
        // after the attach (which is synchronous right after we return; mirrors sd_open_key_picker).
        if (auto w = window_wp.lock())
        {
            auto gid = w->bell::indexer.luafx.get_gear().id;
            w->base::enqueue([cardw = ptr::shadow(card), gid](auto&){ if (auto c = cardw.lock()) pro::focus::set(c, gid, solo::on); });
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
