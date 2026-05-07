// Copyright (c) Dmitry Sapozhnikov
// Licensed under the MIT license.

#include "netxs/apps.hpp"
#include "netxs/apps/desk.hpp"
#include "vtm-common.hpp"
#include "netxs/apps/tile.hpp"

using namespace netxs;
using namespace netxs::app;

namespace
{
    auto verify_standalone_tile_shutdown() -> bool
    {
        auto tile_root = ui::cake::ctor();
        auto shutdown_seen = false;
        auto quit_seen = false;

        tile_root->LISTEN(tier::general, e2::shutdown, reason)
        {
            shutdown_seen = !reason.empty();
        };
        tile_root->LISTEN(tier::release, e2::form::proceed::quit::one, fast)
        {
            quit_seen = fast;
        };

        app::tile::close_tile_session(*tile_root, "regression standalone");
        return shutdown_seen && !quit_seen;
    }

    auto verify_embedded_tile_close_path() -> bool
    {
        auto desktop_root = ui::cake::ctor();
        auto tile_root = desktop_root->attach(ui::cake::ctor());
        auto shutdown_seen = false;
        auto quit_fast_seen = false;

        tile_root->LISTEN(tier::general, e2::config::creator, world_ptr)
        {
            world_ptr = tile_root;
        };
        tile_root->LISTEN(tier::general, e2::shutdown, reason)
        {
            shutdown_seen = !reason.empty();
        };
        desktop_root->LISTEN(tier::release, e2::form::proceed::quit::one, fast)
        {
            quit_fast_seen = fast;
        };

        app::tile::close_tile_session(*tile_root, "regression embedded");
        return !shutdown_seen && quit_fast_seen;
    }

    // Helper: register node_veer-equivalent quit and swap handlers on a veer.
    // These handlers mirror the logic in tile.hpp's node_veer lambda (quit::one
    // preview, quit::any release, swap release, and swap request on parent).
    void setup_node_veer_handlers(ui::veer& boss)
    {
        // quit::one preview handler: enqueue a deferred release (or forward to app).
        boss.LISTEN(tier::preview, e2::form::proceed::quit::one, fast)
        {
            if (boss.count() > 1 && boss.back()->base::root())
            {
                boss.back()->base::signal(tier::anycast, e2::form::proceed::quit::one, true);
            }
            else
            {
                // In real code this enqueues, but for sync tests we signal directly.
                // The test for the release handler covers the actual close logic.
                boss.base::signal(tier::release, e2::form::proceed::quit::one, fast);
            }
            boss.bell::expire(); // Fix: stop riseup at this node_veer.
        };
        // quit::any release handler: process close (pop app, or swap empty slot).
        boss.LISTEN(tier::release, e2::form::proceed::quit::any, fast)
        {
            if (auto parent = boss.base::parent())
            {
                if (boss.count() > 1 && boss.back()->base::kind() == base::client)
                {
                    auto deleted_item = boss.pop_back();
                }
                else if (boss.count() == 1)
                {
                    auto item_ptr = parent->base::signal(tier::request, e2::form::proceed::swap, boss.This());
                    if (item_ptr != boss.This())
                    {
                        parent->base::riseup(tier::release, e2::form::proceed::swap, item_ptr);
                    }
                }
            }
            boss.bell::expire(); // Fix: stop riseup at this node_veer.
        };
        // swap release handler: collapse fork into this veer.
        boss.LISTEN(tier::release, e2::form::proceed::swap, item_ptr)
        {
            if (boss.count() == 1)
            {
                // Only empty slot — nothing to swap.
            }
            else if (boss.count() == 2)
            {
                auto deleted_item = boss.pop_back();
                if (item_ptr) boss.attach(item_ptr);
                else          item_ptr = boss.This();
            }
        };
        // Register swap request handler on the parent (fork) when attached.
        boss.LISTEN(tier::release, e2::form::upon::vtree::attached, parent)
        {
            parent->LISTEN(tier::request, e2::form::proceed::swap, item_ptr, boss.relyon)
            {
                if (item_ptr != boss.This())
                {
                    if (boss.count() == 1)      item_ptr.reset();
                    else if (boss.count() == 2)  item_ptr = boss.pop_back();
                    if (auto p = boss.base::parent()) p->bell::expire();
                }
            };
        };
    }

    // Build the tree structure for two-empty-slot scenarios:
    //
    //   workspace_host (ui::veer)
    //     └── root_veer (ui::veer) — node_veer handlers
    //           ├── empty_slot_0 (ui::cake, placeholder) — root_veer's own
    //           └── fork (ui::fork)
    //                 ├── slot::_1 → slot_1_veer (ui::veer) — node_veer handlers
    //                 │                └── empty_slot_1 (ui::cake, placeholder)
    //                 └── slot::_2 → slot_2_veer (ui::veer) — node_veer handlers
    //                                   └── empty_slot_2 (ui::cake, placeholder)
    //
    struct two_empty_slots_fixture
    {
        netxs::sptr<ui::veer> workspace_host;
        netxs::sptr<ui::veer> root_veer;
        netxs::sptr<ui::fork> fork_node;
        netxs::sptr<ui::veer> slot_1_veer;
        netxs::sptr<ui::veer> slot_2_veer;
        netxs::sptr<ui::cake> empty_slot_0;  // root_veer's own empty slot
        netxs::sptr<ui::cake> empty_slot_1;
        netxs::sptr<ui::cake> empty_slot_2;
        bool           shutdown_seen = false;

        two_empty_slots_fixture()
        {
            workspace_host = ui::veer::ctor();
            root_veer      = ui::veer::ctor()->plugin<pro::focus>();
            setup_node_veer_handlers(*root_veer);

            // Root veer's own empty slot (created first, as in real node_veer).
            empty_slot_0 = ui::cake::ctor()->isroot(true, base::placeholder);
            root_veer->attach(empty_slot_0);

            // Fork with two child veers.
            fork_node = ui::fork::ctor(axis::X, 0, 1, 1);
            fork_node->isroot(faux, base::node);

            slot_1_veer = ui::veer::ctor()->plugin<pro::focus>();
            setup_node_veer_handlers(*slot_1_veer);
            empty_slot_1 = ui::cake::ctor()->isroot(true, base::placeholder);
            slot_1_veer->attach(empty_slot_1);

            slot_2_veer = ui::veer::ctor()->plugin<pro::focus>();
            setup_node_veer_handlers(*slot_2_veer);
            empty_slot_2 = ui::cake::ctor()->isroot(true, base::placeholder);
            slot_2_veer->attach(empty_slot_2);

            fork_node->attach(slot::_1, slot_1_veer);
            fork_node->attach(slot::_2, slot_2_veer);
            root_veer->attach(fork_node);

            // Attach root_veer to workspace_host (simulates workspace attachment).
            workspace_host->attach(root_veer);

            // Workspace_host intercepts swap request → triggers shutdown for last workspace.
            workspace_host->LISTEN(tier::request, e2::form::proceed::swap, item_ptr)
            {
                if (item_ptr) shutdown_seen = true;
            };
        }
    };

    // -----------------------------------------------------------------------
    // Regression test: close one of two empty slots must NOT trigger shutdown.
    //
    // Before the fix, closing one empty slot when its sibling was also empty
    // would cause the fork to collapse and the workspace-level swap handler
    // to fire, resulting in premature shutdown.
    // -----------------------------------------------------------------------
    auto verify_close_one_of_two_empty_slots_no_shutdown() -> bool
    {
        auto f = two_empty_slots_fixture{};

        // Simulate clicking "×" on slot_2's empty slot: fires quit::one release riseup.
        f.empty_slot_2->base::riseup(tier::release, e2::form::proceed::quit::one, true);

        // After closing one empty slot, root_veer should have 1 child (the surviving empty_slot_0).
        // The fork and both child veers should have been cleaned up.
        if (f.root_veer->count() != 1) return false;

        // No shutdown should have been triggered.
        if (f.shutdown_seen) return false;

        return true;
    }

    // -----------------------------------------------------------------------
    // Regression test: close the last remaining empty slot SHOULD shutdown.
    //
    // After collapsing two empty slots down to one, closing that last empty
    // slot should trigger workspace destruction (and shutdown for the last ws).
    // -----------------------------------------------------------------------
    auto verify_close_last_empty_slot_triggers_shutdown() -> bool
    {
        auto f = two_empty_slots_fixture{};

        // First: close slot_2 → collapses fork, root_veer left with empty_slot_0.
        f.empty_slot_2->base::riseup(tier::release, e2::form::proceed::quit::one, true);
        if (f.root_veer->count() != 1) return false;
        if (f.shutdown_seen) return false;

        // Second: close the last empty slot (empty_slot_0 in root_veer).
        f.empty_slot_0->base::riseup(tier::release, e2::form::proceed::quit::one, true);

        // Now shutdown SHOULD have been triggered.
        if (!f.shutdown_seen) return false;

        return true;
    }

    // -----------------------------------------------------------------------
    // Regression test: selectall + close on two empty slots must NOT shutdown.
    //
    // Before the fix, the quit::one preview riseup would propagate past the
    // child node_veers up to root_veer, enqueuing additional quit releases
    // that triggered shutdown.  Here we test the synchronous release path:
    // firing quit::one release on both slots sequentially should collapse
    // the fork but leave root_veer with one empty slot, not shutdown.
    // -----------------------------------------------------------------------
    auto verify_selectall_close_two_empty_slots_no_shutdown() -> bool
    {
        auto f = two_empty_slots_fixture{};

        // Simulate closing both empty slots (as selectall + close would do).
        // In real code these would be enqueued releases; here we fire sequentially.
        // The first release collapses the fork; the second operates on a detached
        // veer (parent gone or expired) and should be a no-op.
        f.empty_slot_1->base::riseup(tier::release, e2::form::proceed::quit::one, true);
        f.empty_slot_2->base::riseup(tier::release, e2::form::proceed::quit::one, true);

        // root_veer should retain one empty slot.
        if (f.root_veer->count() != 1) return false;

        // No shutdown should have been triggered.
        if (f.shutdown_seen) return false;

        return true;
    }

    // -----------------------------------------------------------------------
    // Verify quit riseup propagation is stopped by bell::expire().
    //
    // Without expire(), a quit::one riseup from a grandchild would propagate
    // through the child veer (which handles it) and continue to the parent
    // veer (which would incorrectly process it again).
    // -----------------------------------------------------------------------
    auto verify_quit_riseup_stopped_by_expire() -> bool
    {
        auto parent_veer = ui::veer::ctor();
        auto child_veer  = parent_veer->attach(ui::veer::ctor());
        auto grandchild  = child_veer->attach(ui::cake::ctor()->isroot(true, base::placeholder));

        auto parent_quit_seen = false;
        auto child_quit_seen  = false;

        // Child handles quit and calls expire to stop propagation.
        child_veer->LISTEN(tier::release, e2::form::proceed::quit::any, fast)
        {
            child_quit_seen = true;
            child_veer->bell::expire();
        };
        // Parent should NOT see the quit if expire works.
        parent_veer->LISTEN(tier::release, e2::form::proceed::quit::any, fast)
        {
            parent_quit_seen = true;
        };

        grandchild->base::riseup(tier::release, e2::form::proceed::quit::one, true);

        return child_quit_seen && !parent_quit_seen;
    }

    // Fixture: fork with one slot holding a fake pane (kind == base::client).
    //
    //   workspace_host (ui::veer)
    //     └── root_veer (ui::veer) — node_veer handlers
    //           ├── root_empty (ui::cake, placeholder)
    //           └── fork_node (ui::fork, node)
    //                 ├── slot::_1 → slot_1_veer (ui::veer) — node_veer handlers
    //                 │                ├── empty_1 (placeholder)
    //                 │                └── fake_pane (kind == client)
    //                 └── slot::_2 → slot_2_veer (ui::veer) — node_veer handlers
    //                                   └── empty_2 (placeholder)
    //
    struct slot_with_pane_fixture
    {
        netxs::sptr<ui::veer> workspace_host;
        netxs::sptr<ui::veer> root_veer;
        netxs::sptr<ui::fork> fork_node;
        netxs::sptr<ui::veer> slot_1_veer;
        netxs::sptr<ui::veer> slot_2_veer;
        netxs::sptr<ui::cake> root_empty;
        netxs::sptr<ui::cake> empty_1;
        netxs::sptr<ui::cake> fake_pane;  // simulates app_window: root=true, kind=client
        netxs::sptr<ui::cake> empty_2;
        bool shutdown_seen = false;

        slot_with_pane_fixture()
        {
            workspace_host = ui::veer::ctor();
            root_veer      = ui::veer::ctor()->plugin<pro::focus>();
            setup_node_veer_handlers(*root_veer);

            root_empty = ui::cake::ctor()->isroot(true, base::placeholder);
            root_veer->attach(root_empty);

            fork_node = ui::fork::ctor(axis::X, 0, 1, 1);
            fork_node->isroot(faux, base::node);

            slot_1_veer = ui::veer::ctor()->plugin<pro::focus>();
            setup_node_veer_handlers(*slot_1_veer);
            empty_1   = ui::cake::ctor()->isroot(true, base::placeholder);
            slot_1_veer->attach(empty_1);
            fake_pane = ui::cake::ctor()->isroot(true); // kind == base::client
            slot_1_veer->attach(fake_pane);

            slot_2_veer = ui::veer::ctor()->plugin<pro::focus>();
            setup_node_veer_handlers(*slot_2_veer);
            empty_2 = ui::cake::ctor()->isroot(true, base::placeholder);
            slot_2_veer->attach(empty_2);

            fork_node->attach(slot::_1, slot_1_veer);
            fork_node->attach(slot::_2, slot_2_veer);
            root_veer->attach(fork_node);
            workspace_host->attach(root_veer);

            workspace_host->LISTEN(tier::request, e2::form::proceed::swap, item_ptr)
            {
                if (item_ptr) shutdown_seen = true;
            };
        }
    };

    // -----------------------------------------------------------------------
    // CloseSlot on empty slot: preview quit::one rises through node_veer,
    // triggering the count==1 release path, which removes the slot.
    // -----------------------------------------------------------------------
    auto verify_close_slot_empty_slot() -> bool
    {
        auto f = two_empty_slots_fixture{};

        // Simulate CloseSlot empty-slot path (item_ptr->riseup tier::preview).
        // setup_node_veer_handlers fires the release synchronously (no enqueue).
        f.empty_slot_1->base::riseup(tier::preview, e2::form::proceed::quit::one, true);

        // Fork collapses; root_veer retains surviving empty_slot_0.
        if (f.root_veer->count() != 1) return false;
        if (f.shutdown_seen) return false;
        return true;
    }

    // -----------------------------------------------------------------------
    // CloseSlot on a slot with a pane: pane removed first (step 1), then the
    // now-empty slot is removed from the layout (step 2).
    // -----------------------------------------------------------------------
    auto verify_close_slot_with_pane() -> bool
    {
        auto f = slot_with_pane_fixture{};

        // Step 1: pane removal (release quit with count==2, kind==client → pop_back).
        f.slot_1_veer->base::signal(tier::release, e2::form::proceed::quit::one, true);
        if (f.slot_1_veer->count() != 1) return false; // Pane removed.

        // Step 2: slot removal (enqueued release with count==1 → reorganize fork).
        f.slot_1_veer->base::signal(tier::release, e2::form::proceed::quit::one, true);

        // Fork collapsed; root_veer retains root_empty.
        if (f.root_veer->count() != 1) return false;
        if (f.shutdown_seen) return false;
        return true;
    }
}

auto main() -> int
{
    if (!verify_standalone_tile_shutdown())                    return 1;
    if (!verify_embedded_tile_close_path())                   return 2;
    if (!verify_quit_riseup_stopped_by_expire())              return 3;
    if (!verify_close_one_of_two_empty_slots_no_shutdown())   return 4;
    if (!verify_close_last_empty_slot_triggers_shutdown())     return 5;
    if (!verify_selectall_close_two_empty_slots_no_shutdown()) return 6;
    if (!verify_close_slot_empty_slot())                       return 7;
    if (!verify_close_slot_with_pane())                        return 8;
    return 0;
}
