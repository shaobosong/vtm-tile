// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#include "netxs/apps/parvion/chunk_retry.hpp"

#include <cstdio>
#include <vector>

using namespace netxs::app::parvion;

namespace
{
    #define REQUIRE(expr) do { if (!(expr)) { std::fprintf(stderr, "  failed at line %d: %s\n", __LINE__, #expr); return false; } } while (false)

    auto test_origin_classification() -> bool
    {
        REQUIRE(classify_xfer_error({ true,  true,  "0", true  }) == xfer_err_origin::local);
        REQUIRE(classify_xfer_error({ false, true,  "0", true  }) == xfer_err_origin::reject);
        REQUIRE(classify_xfer_error({ false, true,  "2", false }) == xfer_err_origin::reject);
        REQUIRE(classify_xfer_error({ false, false, {},  false }) == xfer_err_origin::connection);
        REQUIRE(classify_xfer_error({ true,  false, {},  false }) == xfer_err_origin::local);
        REQUIRE(classify_xfer_error({ false, true,  "0", false }) == xfer_err_origin::connection);
        return true;
    }

    auto test_retry_policy() -> bool
    {
        REQUIRE(should_auto_retry(xfer_err_origin::connection, 0, 3));
        REQUIRE(should_auto_retry(xfer_err_origin::connection, 2, 3));
        REQUIRE(!should_auto_retry(xfer_err_origin::connection, 3, 3));
        REQUIRE(should_auto_retry(xfer_err_origin::connection, 300, 0));
        REQUIRE(!should_auto_retry(xfer_err_origin::local, 0, 0));
        REQUIRE(!should_auto_retry(xfer_err_origin::reject, 0, 0));
        return true;
    }

    auto test_connection_arm_and_dispatch() -> bool
    {
        auto now = chunk_clock::time_point{ std::chrono::seconds{ 100 } };
        auto slot = chunk_slot{};
        slot.phase = chunk_phase::running;
        slot.origin = xfer_err_origin::connection;
        slot.attempt_base = 100;
        slot.done = 123;

        REQUIRE(arm_failure(slot, false, false, 3, now, 7) == arm_failure_result::retry_wait);
        REQUIRE(slot.phase == chunk_phase::retry_wait && slot.done == 123 && slot.armed_pump == 7);
        REQUIRE(slot.due == retry_due::after_delay);
        REQUIRE(!slot_dispatchable(slot, 7, now, 0));
        REQUIRE(slot_dispatchable(slot, 8, now, 0));
        REQUIRE(!slot_dispatchable(slot, 8, now + std::chrono::seconds{ 4 }, 5));
        REQUIRE(slot_dispatchable(slot, 8, now + std::chrono::seconds{ 5 }, 5));

        // Live delay is not snapshotted: the same loss becomes due when 5 changes to 0,
        // but the next-pump gate remains mandatory.
        REQUIRE(!slot_dispatchable(slot, 7, now, 0));
        REQUIRE(slot_dispatchable(slot, 8, now, 0));
        return true;
    }

    auto test_non_connection_failures_exhaust() -> bool
    {
        auto now = chunk_clock::time_point{};
        for (auto origin : { xfer_err_origin::local, xfer_err_origin::reject })
        {
            auto slot = chunk_slot{};
            slot.phase = chunk_phase::running;
            slot.origin = origin;
            slot.error = "refused";
            REQUIRE(arm_failure(slot, false, false, 0, now, 1) == arm_failure_result::exhausted);
            REQUIRE(slot.phase == chunk_phase::failed);
            REQUIRE(!slot_retryable_connection(slot));
        }
        return true;
    }

    auto test_stale_pool_is_budget_neutral() -> bool
    {
        auto now = chunk_clock::time_point{ std::chrono::seconds{ 50 } };
        auto first = chunk_slot{};
        first.phase = chunk_phase::running;
        first.origin = xfer_err_origin::connection;
        first.done = first.attempt_base = 42;
        REQUIRE(arm_failure(first, false, true, 1, now, 4) == arm_failure_result::stale);
        REQUIRE(first.phase == chunk_phase::idle && first.attempts == 0);

        auto retry = chunk_slot{};
        retry.phase = chunk_phase::running;
        retry.origin = xfer_err_origin::connection;
        retry.done = retry.attempt_base = 42;
        retry.attempts = 2;
        REQUIRE(arm_failure(retry, true, true, 2, now, 4) == arm_failure_result::stale);
        REQUIRE(retry.phase == chunk_phase::retry_wait && retry.attempts == 1);
        REQUIRE(retry.due == retry_due::next_pump);
        REQUIRE(!slot_dispatchable(retry, 4, now, 999));
        REQUIRE(slot_dispatchable(retry, 5, now, 999));
        return true;
    }

    auto test_single_connection_progress_survives() -> bool
    {
        auto slot = chunk_slot{};
        slot.length = -1;
        slot.phase = chunk_phase::running;
        slot.origin = xfer_err_origin::connection;
        slot.done = 123;
        slot.attempt_base = 0;
        REQUIRE(arm_failure(slot, false, false, 3, {}, 9) == arm_failure_result::retry_wait);
        REQUIRE(slot.done == 123);
        return true;
    }

    auto test_job_fold_and_leader_block() -> bool
    {
        auto slots = std::vector<chunk_slot>(4);
        slots[0].phase = chunk_phase::failed;
        slots[0].opened = false;
        for (auto i = 1; i < 4; ++i) slots[(size_t)i].phase = chunk_phase::idle;
        auto blocked = counts_from_slots(slots, true, false);
        REQUIRE(blocked.parked_blocked);
        REQUIRE(decide_xfer_parent(blocked) == xfer_parent_decision::failed);

        slots[0].opened = true;
        auto ready = counts_from_slots(slots, true, false);
        REQUIRE(!ready.parked_blocked);
        REQUIRE(decide_xfer_parent(ready) == xfer_parent_decision::transferring);

        for (auto& slot : slots) slot.phase = chunk_phase::succeeded;
        REQUIRE(decide_xfer_parent(counts_from_slots(slots, false, false)) == xfer_parent_decision::succeeded);
        return true;
    }

    auto test_manual_retry_is_immediate_and_filtered() -> bool
    {
        auto now = chunk_clock::time_point{ std::chrono::seconds{ 20 } };
        auto connection = chunk_slot{};
        connection.phase = chunk_phase::failed;
        connection.origin = xfer_err_origin::connection;
        connection.attempts = 9;
        REQUIRE(slot_retryable_connection(connection));
        arm_manual_retry(connection, 12);
        REQUIRE(connection.attempts == 0);
        REQUIRE(connection.due == retry_due::this_pump);
        REQUIRE(slot_dispatchable(connection, 12, now, 5));

        auto local = chunk_slot{};
        local.phase = chunk_phase::failed;
        local.origin = xfer_err_origin::local;
        auto reject = chunk_slot{};
        reject.phase = chunk_phase::retry_wait;
        reject.origin = xfer_err_origin::reject;
        REQUIRE(!slot_retryable_connection(local));
        REQUIRE(!slot_retryable_connection(reject));
        return true;
    }

    auto test_leader_initialization_policy() -> bool
    {
        REQUIRE(leader_should_initialize(false, false, 0, false, false));
        REQUIRE(!leader_should_initialize(false, false, 0, false, true));
        REQUIRE(!leader_should_initialize(false, false, 0, true, false));
        REQUIRE(!leader_should_initialize(false, true, 0, false, false));
        REQUIRE(!leader_should_initialize(true, false, 0, false, false));
        return true;
    }
}

int main()
{
    struct test_case { char const* name; bool (*run)(); } tests[] =
    {
        { "origin classification", test_origin_classification },
        { "retry policy", test_retry_policy },
        { "connection arm and dispatch", test_connection_arm_and_dispatch },
        { "non-connection exhaustion", test_non_connection_failures_exhaust },
        { "stale pool budget", test_stale_pool_is_budget_neutral },
        { "single connection progress", test_single_connection_progress_survives },
        { "job fold and leader block", test_job_fold_and_leader_block },
        { "manual retry filtering", test_manual_retry_is_immediate_and_filtered },
        { "leader initialization", test_leader_initialization_policy },
    };
    auto failures = 0;
    for (auto const& test : tests)
    {
        auto ok = test.run();
        std::printf("%-32s %s\n", test.name, ok ? "PASS" : "FAIL");
        failures += !ok;
    }
    return failures ? 1 : 0;
}
