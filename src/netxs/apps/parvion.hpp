// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion: Parvion — a parallel striped SSH-based transfer system: a FileZilla-style SFTP client applet.
//      Launch standalone with: vtm-tile -r parvion
//
//      Phase 1 lays out the faithful FileZilla-style frame:
//        ┌───────────────────────────────────────────────┐
//        │ menu bar                                       │
//        ├───────────────────────────────────────────────┤
//        │ quick-connect: Host/User/Pass/Port [Connect]   │
//        ├──────────────────────┬────────────────────────┤
//        │ Local site           │ Remote site            │
//        │ (real local FS)      │ (filled once connected) │
//        ├──────────────────────┴────────────────────────┤
//        │ ····· (empty handle bar: drag to resize) ····· │
//        │ queue / message-log body                       │
//        │ [Transferring][Failed][Succeeded][Message log] │
//        └───────────────────────────────────────────────┘
//      Later phases wire the real parvionsftp helper + fzprintf protocol + the
//      parallel chunked-transfer engine behind this frame.

#include "parvion/model.hpp"
#include "parvion/proto.hpp"
#include "parvion/resume.hpp"
#include "parvion/session.hpp"
#include "parvion/panes.hpp"
#include "parvion/connectbar.hpp"
#include "parvion/queue.hpp"
#include "parvion/settings_dialog.hpp"
#include "parvion/prompts.hpp" // make_secret_dialog (live passphrase / password login modal)

namespace netxs::app::parvion
{
    static constexpr auto id   = "parvion";
    static constexpr auto name = "Parvion (Parallel SFTP)";

    namespace
    {
        // Palette (aligned with tile.hpp's command_bar tones for a consistent look).
        static constexpr auto col_bg      = 0xFF1E1E2Eu; // Window background.
        static constexpr auto col_surface = 0xFF313244u; // Bars/headers surface.
        static constexpr auto col_header  = 0xFF11111Bu; // Pane header strip.
        static constexpr auto col_text    = 0xFFCDD6F4u; // Primary text.
        static constexpr auto col_subtext = 0xFF6C7086u; // Secondary text.
        static constexpr auto col_accent  = 0xFF89B4FAu; // Accent / focus.
        static constexpr auto col_ok      = 0xFFA6E3A1u; // Success.
        static constexpr auto col_err     = 0xFFF38BA8u; // Failure.

        // Minimum region heights: the panes (which host the divider) never collapse under the
        // queue panel, and the queue panel always keeps room for its top handle bar, a body
        // row, and the bottom tab strip — so the handle and the tabs never land on one row.
        static constexpr auto min_panes_h = si32{ 3 };
        static constexpr auto min_queue_h = si32{ 4 }; // handle + column header + ≥1 row + tab strip.

        // Initial local directory (current working directory; falls back to "/").
        auto cwd = []
        {
            auto ec = std::error_code{};
            auto p = fs::current_path(ec);
            return ec ? text{ "/" } : p.string();
        };
        // Point the controller at the parvionsftp backend. By default the backend is
        // vtm-tile itself (multi-call binary): we spawn this very executable with
        // `-r parvionsftp`, which main() dispatches to the in-process FileZilla engine.
        // $PARVION_SFTP_BIN overrides this with an external standalone parvionsftp (invoked
        // directly, no run-prefix) for debugging / bring-your-own-backend.
        auto configure_backend = [](sftp_remote& ctrl)
        {
            // The checksum backend is always a mode of the multi-call self (`-r parvionhash`),
            // independent of $PARVION_SFTP_BIN (which only overrides the inner SFTP helper; the
            // remote-hash path reads that env itself when it spawns parvionsftp).
            ctrl.hash_exe = os::process::binary();
            ctrl.hash_runargs = { "-r", "parvionhash" };
            if (auto e = std::getenv("PARVION_SFTP_BIN")) if (*e)
            {
                ctrl.exe = text{ e };
                ctrl.runargs.clear();
                return;
            }
            ctrl.exe = os::process::binary(); // Absolute path to this running vtm-tile.
            ctrl.runargs = { "-r", "parvionsftp" };
        };

        // Test/demo seam: when $PARVION_DEMO_QUEUE is set, seed the transfer queue with a
        // fixed set of synthetic items (no backend, no network) so the queue panel's
        // fold/unfold subtask UI can be driven by the Python regression tests. Items are
        // left in non-queued states (transferring/succeeded/failed) on purpose so the
        // poll loop's auto-start (which only fires on `queued`) never spawns a real
        // transfer. The mix covers both a parallel item (enabled "+") and a single-
        // connection item (disabled "+") on every tab.
        auto seed_demo_queue = [](sftp_remote& ctrl)
        {
            auto e = std::getenv("PARVION_DEMO_QUEUE");
            if (!(e && *e && *e != '0')) return;
            ctrl.no_autostart = true; // Demo/tests: never spawn a real backend, even for `queued` items.
            auto mk = [](bool dl, text path, si64 size, si64 done, ui32 chunks,
                         queue_item::status_t st, text err = {}) -> queue_item
            {
                auto it = queue_item{};
                it.download    = dl;
                it.remote_path = dl ? path : text{ "/remote/" + path };
                it.local_path  = dl ? text{ "/local/" + path } : path;
                it.size        = size;
                it.done        = done;
                it.chunk_count = chunks;
                it.status      = st;
                it.error       = std::move(err);
                return it;
            };
            // Transferring tab: parallel (enabled "+") + single connection (disabled "+").
            ctrl.queue.push_back(mk(true,  "bigfile.iso", 200ll << 20,  90ll << 20, 4, queue_item::transferring));
            ctrl.queue.push_back(mk(false, "notes.txt",     2ll << 20,   1ll << 20, 1, queue_item::transferring));
            // Succeeded tab: a finished parallel item (its children read 100.00%).
            ctrl.queue.push_back(mk(true,  "archive.tar",  48ll << 20,  48ll << 20, 3, queue_item::succeeded));
            // Failed tab: a parallel item that failed partway.
            ctrl.queue.push_back(mk(false, "upload.bin",   32ll << 20,  12ll << 20, 2, queue_item::failed, "Permission denied"));
            // Overflow seam ($PARVION_DEMO_QUEUE_N=k): append k extra single-connection
            // transferring items (numbered names) to force the queue panel's vertical
            // scrollbar, plus one very-long-named item to force the horizontal scrollbar.
            // Gated separately from the base seed so existing regression tests are unaffected.
            if (auto n = std::getenv("PARVION_DEMO_QUEUE_N"); n && *n)
            {
                auto k = std::atoi(n);
                for (auto i = 0; i < k; ++i)
                {
                    auto nm = "file_" + (i < 10 ? text{ "0" } : text{}) + std::to_string(i) + ".dat";
                    ctrl.queue.push_back(mk(true, nm, (8ll + i) << 20, (i % 7) << 20, 1, queue_item::transferring));
                }
                ctrl.queue.push_back(mk(true,
                    "a_file_with_a_very_long_name_that_overflows_the_name_column_for_hscroll.bin",
                    16ll << 20, 4ll << 20, 1, queue_item::transferring));
                // A failed item whose error is far longer than the old 22-cell Reason cap, to
                // exercise the now-uncapped Reason column (sized to the widest message).
                ctrl.queue.push_back(mk(false, "broken_upload.bin", 64ll << 20, 8ll << 20, 1,
                    queue_item::failed,
                    "Connection reset by peer while writing remote chunk: the server closed the session unexpectedly"));
            }
            // Pending seam ($PARVION_DEMO_QUEUED_N=k): append k `queued` items with distinct names
            // (queued_00, queued_01, …) on the Transferring tab. They never auto-start (no_autostart
            // is set above), so the queue context-menu tests can exercise Pin-to-Top ordering and the
            // paused/queued Progress label without a backend.
            if (auto n = std::getenv("PARVION_DEMO_QUEUED_N"); n && *n)
            {
                auto k = std::atoi(n);
                for (auto i = 0; i < k; ++i)
                {
                    auto nm = "queued_" + (i < 10 ? text{ "0" } : text{}) + std::to_string(i) + ".dat";
                    ctrl.queue.push_back(mk(true, nm, (4ll + i) << 20, 0, 1, queue_item::queued));
                }
            }
            ctrl.dirty = true;
        };

        // Test/demo seam: when $PARVION_DEMO_LOG_N=k is set, seed the message log with k numbered
        // Status lines plus one very long line — enough to overflow the Message-log tab and force
        // its vertical and horizontal scrollbars. Independent of the queue seed so it can be
        // driven on its own; existing regression tests (which never set it) are unaffected.
        auto seed_demo_log = [](sftp_remote& ctrl)
        {
            auto n = std::getenv("PARVION_DEMO_LOG_N");
            if (!(n && *n)) return;
            auto k = std::atoi(n);
            for (auto i = 0; i < k; ++i)
                ctrl.log_line(logtype::status, "log line " + (i < 10 ? text{ "0" } : text{}) + std::to_string(i));
            ctrl.log_line(logtype::status,
                "a very long status line that overflows the message-log width to force the horizontal scrollbar for testing");
            if (auto d = std::getenv("PARVION_DEMO_LOG_DETAIL"); d && *d && *d != '0')
            {
                ctrl.log_line(logtype::command, "demo hidden command");
                ctrl.log_line(logtype::response, "demo hidden response");
            }
        };

        // Test/demo seam: when $PARVION_DEMO_HASH is set, seed the Checksums tab with synthetic
        // hash_items spanning every state (queued / hashing / succeeded / failed), so the new tab,
        // its progress/result rendering, and its right-click menu can be driven without a backend
        // or network. no_autostart is set so pump_hash never spawns a real parvionhash child.
        auto seed_demo_hash = [](sftp_remote& ctrl)
        {
            auto e = std::getenv("PARVION_DEMO_HASH");
            if (!(e && *e && *e != '0')) return;
            ctrl.no_autostart = true;
            // host empty = local origin; else the Source column shows "user@host:port".
            auto mk = [&](text user, text host, si32 port, text path, si32 algo, si64 size, si64 done,
                          hash_item::status_t status, text digest = {}, text err = {}) -> hash_item
            {
                auto it = hash_item{};
                it.id     = ++ctrl.hash_id_seq;
                it.remote = !host.empty();
                it.user   = user;
                it.host   = host;
                it.port   = port;
                it.path   = path;                 // Full path (shown verbatim in the Path column).
                it.algo   = algo;
                it.size   = size;
                it.done   = done;
                it.status = status;
                it.digest = std::move(digest);
                it.error  = std::move(err);
                return it;
            };
            ctrl.hash_queue.push_back(mk("", "", 0,                       "/home/user/report.pdf",       2, 4ll << 20,   4ll << 20, hash_item::succeeded,
                                         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
            ctrl.hash_queue.push_back(mk("", "", 0,                       "/home/user/notes.txt",        0, 12ll << 10,  12ll << 10, hash_item::succeeded,
                                         "d41d8cd98f00b204e9800998ecf8427e"));
            ctrl.hash_queue.push_back(mk("deploy", "192.168.0.5", 22,     "/srv/backup/backup.tar.gz",   2, 200ll << 20, 90ll << 20, hash_item::hashing));
            ctrl.hash_queue.push_back(mk("", "", 0,                       "/home/user/images/image.iso", 0, 700ll << 20, 0,          hash_item::queued));
            ctrl.hash_queue.push_back(mk("admin", "files.example.com", 2222, "/data/missing.bin",        3, -1,          0,          hash_item::failed, {}, "No such file or directory"));
            ctrl.dirty = true;
        };

        auto build = [](eccc /*appcfg*/, settings& config)
        {
            auto ctrl = std::make_shared<sftp_remote>();
            configure_backend(*ctrl);
            seed_demo_queue(*ctrl);
            seed_demo_log(*ctrl);
            seed_demo_hash(*ctrl);
            auto window = ui::cake::ctor();
            window->plugin<pro::focus>(pro::focus::mode::hub)
                  ->plugin<pro::keybd>()
                  ->plugin<pro::cache>()
                  ->plugin<pro::timer>()
                  ->invoke([&](auto& boss)
                  {
                      boss.LISTEN(tier::anycast, e2::form::proceed::quit::any, fast)
                      {
                          boss.base::riseup(tier::release, e2::form::proceed::quit::one, fast);
                      };
                      boss.LISTEN(tier::release, e2::form::upon::vtree::attached, parent)
                      {
                          auto title = ansi::jet(bias::left).add("Parvion — Parallel SFTP");
                          boss.base::riseup(tier::preview, e2::form::prop::ui::header, title);
                      };
                  });
            auto root = window->attach(ui::fork::ctor(axis::Y))
                ->colors(col_text, col_bg);
                // Menu bar. Scope to /config/parvion/ so menu::create reads /config/parvion/menu/*
                // (padding, slim, autohide) just like the other applets do.
                {
                    auto parvion_context = config.settings::push_context("/config/parvion/");
                    auto [menu_block, cover, menu_data] = app::shared::menu::load(config);
                    root->attach(slot::_1, menu_block);
                }
                auto body = root->attach(slot::_2, ui::fork::ctor(axis::Y));
                    // Quick-connect bar: interactive Host/User/Pass/Port + Connect.
                    body->attach(slot::_1, make_connect_bar(ctrl.get()))
                        ->limits({ 1, 1 }, { -1, 1 });
                    auto workspace = body->attach(slot::_2, ui::fork::ctor(axis::Y, 0, 3, 2));
                        // Local | Remote file-browser panes with a draggable divider.
                        auto panes = workspace->attach(slot::_1, ui::fork::ctor(axis::X, 2, 1, 1));
                            panes->limits({ -1, min_panes_h }); // Panes (and the divider) can't collapse under the queue bar.
                            pane_state* local_st = nullptr; // Stable handle to the local pane's state so the timer can re-list it after a download.
                            auto local_pane = panes->attach(slot::_1, make_file_pane("Local site", true, local_lister(), cwd(), true, nullptr, ctrl.get(), &local_st, window));
                            auto remote_pane = panes->attach(slot::_2, make_file_pane("Remote site", faux, lister_t{}, "/", faux, ctrl.get(), ctrl.get(), nullptr, window));
                            // Draggable Local|Remote divider: pro::mover feeds the fork's split ratio; pro::shade lightens on hover.
                            panes->attach(slot::_I, ui::mock::ctor()
                                ->active()
                                ->plugin<pro::mouse>()
                                ->plugin<pro::mover>()
                                ->plugin<pro::shade<cell::shaders::xlight>>()
                                ->invoke([&](auto& boss)
                                {
                                    boss.LISTEN(tier::release, e2::render::any, parent_canvas)
                                    {
                                        parent_canvas.fill([](cell& c){ c.bgc(col_surface); });
                                    };
                                    // Double-click the divider to restore the default 1:1 Local|Remote split.
                                    attach_dblclick_reset(boss, ptr::shadow(panes), 1, 1);
                                }));
                        // Transfer queue + message log, merged into one bottom-pinned tabbed panel.
                        // Its empty top handle bar drags the panes/queue split.
                        auto queue_panel = workspace->attach(slot::_2, make_queue_panel(ctrl.get(), ptr::shadow(workspace), window));
                            queue_panel->limits({ -1, min_queue_h });
            // Edit -> Settings: register the OpenSettingsDialog lua method on the window so the
            // menu's ParvionOpenSettings script (vtm.xml) opens the settings overlay. ctrl is
            // captured by value (a shared_ptr copy) so it outlives this build() call.
            window->invoke([ctrl](auto& boss)
            {
                auto& luafx = boss.bell::indexer.luafx;
                boss.base::add_methods(basename::parvion,
                {
                    { "OpenSettingsDialog", [&, ctrl]
                    {
                        auto& active = boss.base::property("parvion.settings.active", faux);
                        if (!active) // Toggle guard: don't stack multiple settings overlays.
                        {
                            active = true;
                            auto card = ui::sptr{};
                            boss.base::attach(make_settings_dialog(ctrl.get(), ptr::shadow(boss.This()), &card));
                            boss.base::reflow();
                            // Grab keyboard focus on the dialog so Esc closes it immediately (without a
                            // click to focus first). Focus is keyed to the activating gear (the same
                            // gear.id the card's own click handler uses) and enqueued so it runs AFTER
                            // the menu dropdown that launched us finishes dismissing — grabbing during
                            // the menu script races the popup's focus teardown and the grab is lost.
                            auto gid = luafx.get_gear().id;
                            boss.base::enqueue([card_wp = ptr::shadow(card), gid](auto&)
                            {
                                if (auto c = card_wp.lock()) pro::focus::set(c, gid, solo::on);
                            });
                        }
                        luafx.set_return();
                    }},
                });
                // Live login prompts: when the SFTP controller needs an SSH key passphrase or the
                // account password, pop the shared masked-input modal over the window (enqueued so it
                // attaches outside the poll/timer tick). OK feeds the secret back to the waiting backend
                // (caching a passphrase for reconnects + parallel workers); Cancel/Esc aborts the connect.
                // cp is a raw pointer (not a shared_ptr) to avoid a controller<->callback ownership cycle;
                // the controller outlives the window's timer/panes, which keep it alive.
                ctrl->on_prompt_secret = [wp = ptr::shadow(boss.This()), cp = ctrl.get()](sftp_remote::secret_req_t const& req)
                {
                    auto w = wp.lock();
                    if (!w) return;
                    w->base::enqueue([wp, cp, req](auto& win)
                    {
                        auto title  = req.is_passphrase ? text{ "SSH key passphrase" } : text{ "Password" };
                        auto submit = [cp](text v){ cp->provide_secret(std::move(v)); };
                        auto cancel = [cp]{ cp->cancel_secret(); };
                        win.base::attach(make_secret_dialog(wp, {}, title, req.prompt, submit, cancel, req.is_retry, /*secret*/true));
                    });
                };
            });
            // Test/demo seam ($PARVION_DEMO_LOG_TICK=N): append a Status line every N polls (each ~50ms)
            // so the Python regression tests can verify the Message-log selection survives live log
            // updates. Off (0) unless the env var is set, so normal runs are unaffected.
            auto log_tick = []{ auto e = std::getenv("PARVION_DEMO_LOG_TICK"); return e && *e ? std::atoi(e) : 0; }();
            // Drive the SFTP controller from a periodic timer; repaint the remote pane on change.
            window->invoke([&, ctrl, local_pane, local_st, remote_pane, queue_panel, log_tick](auto& boss)
            {
                boss.base::template plugin<pro::timer>().actify(std::chrono::milliseconds{ 50 },
                    [ctrl, lp = ptr::shadow(local_pane), local_st, rp = ptr::shadow(remote_pane), qp = ptr::shadow(queue_panel),
                     log_tick, tickc = ptr::shared(si32{ 0 })](auto)
                    {
                        ctrl->poll();
                        if (log_tick > 0 && (++*tickc % log_tick == 0))
                        {
                            ctrl->log_line(logtype::status, "tick " + std::to_string(*tickc / log_tick));
                            ctrl->dirty = true;
                        }
                        // A completed download into the displayed local dir bumped local_gen:
                        // re-list the local pane. Lock the shadow before touching local_st,
                        // which points into the (still-alive) widget's field storage.
                        if (local_st) if (auto p = lp.lock()) if (ctrl->local_gen != local_st->seen_local_gen)
                        {
                            local_st->seen_local_gen = ctrl->local_gen;
                            pane_refresh(*local_st);
                            p->base::deface();
                        }
                        if (ctrl->dirty)
                        {
                            ctrl->dirty = faux;
                            if (auto p = rp.lock()) p->base::deface();
                            if (auto p = qp.lock()) p->base::deface();
                        }
                        return true;
                    });
            });
            return window;
        };
    }

    app::shared::initialize builder{ app::parvion::id, build };
}
