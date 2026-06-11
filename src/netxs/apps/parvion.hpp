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
        };

        auto build = [](eccc /*appcfg*/, settings& config)
        {
            auto ctrl = std::make_shared<sftp_remote>();
            configure_backend(*ctrl);
            seed_demo_queue(*ctrl);
            seed_demo_log(*ctrl);
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
                    root->attach(slot::_1, app::shared::menu::demo(config));
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
                            auto local_pane = panes->attach(slot::_1, make_file_pane("Local site", true, local_lister(), cwd(), true, nullptr, ctrl.get(), &local_st));
                            auto remote_pane = panes->attach(slot::_2, make_file_pane("Remote site", faux, lister_t{}, "/", faux, ctrl.get(), ctrl.get()));
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
                        auto queue_panel = workspace->attach(slot::_2, make_queue_panel(ctrl.get(), ptr::shadow(workspace)));
                            queue_panel->limits({ -1, min_queue_h });
            // Drive the SFTP controller from a periodic timer; repaint the remote pane on change.
            window->invoke([&, ctrl, local_pane, local_st, remote_pane, queue_panel](auto& boss)
            {
                boss.base::template plugin<pro::timer>().actify(std::chrono::milliseconds{ 50 },
                    [ctrl, lp = ptr::shadow(local_pane), local_st, rp = ptr::shadow(remote_pane), qp = ptr::shadow(queue_panel)](auto)
                    {
                        ctrl->poll();
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
