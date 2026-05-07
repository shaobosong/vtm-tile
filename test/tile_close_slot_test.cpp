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
    // Mirror of setup_node_veer_handlers from tile_session_shutdown_test.cpp.
    // Registers quit/swap handlers that replicate node_veer logic.
    void setup_node_veer_handlers(ui::veer& boss)
    {
        boss.LISTEN(tier::preview, e2::form::proceed::quit::one, fast)
        {
            if (boss.count() > 1 && boss.back()->base::root())
            {
                boss.back()->base::signal(tier::anycast, e2::form::proceed::quit::one, true);
            }
            else
            {
                boss.base::signal(tier::release, e2::form::proceed::quit::one, fast);
            }
            boss.bell::expire();
        };
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
            boss.bell::expire();
        };
        boss.LISTEN(tier::release, e2::form::proceed::swap, item_ptr)
        {
            if (boss.count() == 2)
            {
                auto deleted_item = boss.pop_back();
                if (item_ptr) boss.attach(item_ptr);
                else          item_ptr = boss.This();
            }
        };
        boss.LISTEN(tier::release, e2::form::upon::vtree::attached, parent)
        {
            parent->LISTEN(tier::request, e2::form::proceed::swap, item_ptr, boss.relyon)
            {
                if (item_ptr != boss.This())
                {
                    if (boss.count() == 1)     item_ptr.reset();
                    else if (boss.count() == 2) item_ptr = boss.pop_back();
                    if (auto p = boss.base::parent()) p->bell::expire();
                }
            };
        };
    }

    // Build fixture:
    //   workspace_host (ui::veer)
    //     └── root_veer (ui::veer)
    //           ├── empty_slot_0
    //           └── fork (ui::fork)
    //                 ├── slot::_1 → slot_1_veer
    //                 │               └── empty_slot_1
    //                 └── slot::_2 → slot_2_veer
    //                                 └── empty_slot_2
    struct two_slot_fixture
    {
        netxs::sptr<ui::veer> workspace_host;
        netxs::sptr<ui::veer> root_veer;
        netxs::sptr<ui::fork> fork_node;
        netxs::sptr<ui::veer> slot_1_veer;
        netxs::sptr<ui::veer> slot_2_veer;
        netxs::sptr<ui::cake> empty_slot_0;
        netxs::sptr<ui::cake> empty_slot_1;
        netxs::sptr<ui::cake> empty_slot_2;
        bool swap_seen = false;

        two_slot_fixture()
        {
            workspace_host = ui::veer::ctor();
            root_veer      = ui::veer::ctor()->plugin<pro::focus>();
            setup_node_veer_handlers(*root_veer);

            empty_slot_0 = ui::cake::ctor()->isroot(true, base::placeholder);
            root_veer->attach(empty_slot_0);

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
            workspace_host->attach(root_veer);

            workspace_host->LISTEN(tier::request, e2::form::proceed::swap, item_ptr)
            {
                if (item_ptr) swap_seen = true;
            };
        }
    };

    // -----------------------------------------------------------------------
    // CloseSlot on an empty slot removes the slot (collapses fork).
    // For empty slots the handler reuses the close-pane path:
    // riseup(preview, quit::one) → node_veer enqueues release → slot removed.
    // -----------------------------------------------------------------------
    auto verify_closeslot_empty_slot_collapses_fork() -> bool
    {
        auto f = two_slot_fixture{};

        // Simulate CloseSlot empty-slot path: riseup(preview, quit::one) from placeholder.
        f.empty_slot_2->base::riseup(tier::preview, e2::form::proceed::quit::one, true);

        if (f.root_veer->count() != 1) return false; // Fork must be collapsed.
        if (f.swap_seen) return false;                // No workspace swap (sibling was empty too).
        return true;
    }

    // -----------------------------------------------------------------------
    // CloseSlot two-step: close pane (step 1) then close slot (step 2).
    // Step 1: signal(release, quit::one) on the veer → app popped (count 2→1).
    // Step 2: signal(release, quit::one) on the veer → slot removed (fork collapses).
    // -----------------------------------------------------------------------
    auto verify_closeslot_two_step_removes_pane_and_slot() -> bool
    {
        auto f = two_slot_fixture{};

        // Attach a fake client pane to slot_1_veer.
        auto pane = f.slot_1_veer->attach(ui::cake::ctor()->isroot(true, base::client));
        if (f.slot_1_veer->count() != 2) return false;

        // Step 1: close pane (mirrors what preview-quit::one → release-quit::any does).
        f.slot_1_veer->base::signal(tier::release, e2::form::proceed::quit::one, true);
        if (f.slot_1_veer->count() != 1) return false; // Pane must be gone.

        // Step 2: close slot (mirrors the enqueued task in the closeslot handler).
        f.slot_1_veer->base::signal(tier::release, e2::form::proceed::quit::one, true);

        if (f.root_veer->count() != 1) return false; // Fork must be collapsed.
        if (f.swap_seen) return false;                // No workspace swap (slot_2 was empty).
        return true;
    }

    // -----------------------------------------------------------------------
    // CloseSlot on the only remaining slot triggers workspace swap.
    // After collapsing the fork in step 1, closing root_veer's last empty slot
    // must propagate up to the workspace host.
    // -----------------------------------------------------------------------
    auto verify_closeslot_last_slot_triggers_workspace_swap() -> bool
    {
        auto f = two_slot_fixture{};

        // Collapse fork first (close slot_2).
        f.empty_slot_2->base::riseup(tier::preview, e2::form::proceed::quit::one, true);
        if (f.root_veer->count() != 1) return false;
        if (f.swap_seen) return false;

        // Now close the last slot (empty_slot_0 in root_veer).
        f.empty_slot_0->base::riseup(tier::preview, e2::form::proceed::quit::one, true);

        if (!f.swap_seen) return false; // Workspace swap must fire.
        return true;
    }
}

auto main() -> int
{
    if (!verify_closeslot_empty_slot_collapses_fork())        return 1;
    if (!verify_closeslot_two_step_removes_pane_and_slot())   return 2;
    if (!verify_closeslot_last_slot_triggers_workspace_swap()) return 3;
    return 0;
}
