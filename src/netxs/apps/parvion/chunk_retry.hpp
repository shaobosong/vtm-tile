// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// Durable transfer-chunk state and pure retry policy. Process ownership and
// attachment-generation facts belong to the session layer, not this module.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace netxs::app::parvion
{
    using chunk_clock = std::chrono::steady_clock;

    enum class xfer_err_origin
    {
        none,
        connection,
        reject,
        local,
    };

    struct xfer_error_facts
    {
        bool local_failure = false; // Spawn/passphrase/shared-memory/local file I/O.
        bool done_seen = false;
        std::string_view done_code;
        bool alive = false;
    };

    inline auto classify_xfer_error(xfer_error_facts const& facts) -> xfer_err_origin
    {
        if (facts.local_failure) return xfer_err_origin::local;
        if (facts.done_seen && facts.done_code == "2") return xfer_err_origin::reject;
        if (facts.done_seen && facts.done_code != "1" && facts.alive) return xfer_err_origin::reject;
        return xfer_err_origin::connection;
    }

    inline auto should_auto_retry(xfer_err_origin origin, int attempts, int max_tries) -> bool
    {
        return origin == xfer_err_origin::connection
            && (max_tries == 0 || attempts < max_tries);
    }

    enum class chunk_phase
    {
        idle,
        running,
        retry_wait,
        succeeded,
        failed,
    };

    enum class retry_due
    {
        after_delay,
        next_pump,
        this_pump,
    };

    struct chunk_slot
    {
        std::int32_t index = 0;
        std::int64_t offset = 0;
        std::int64_t length = -1;
        std::int64_t done = 0;
        std::int64_t persisted = -1;
        bool opened = false;

        chunk_phase phase = chunk_phase::idle;
        xfer_err_origin origin = xfer_err_origin::none;
        std::string error;
        std::int32_t attempts = 0;
        std::int64_t attempt_base = 0;
        chunk_clock::time_point lost_at{};
        std::uint64_t armed_pump = 0;
        retry_due due = retry_due::after_delay;
    };

    inline auto slot_due(chunk_slot const& slot, chunk_clock::time_point now, int delay_sec) -> bool
    {
        return now >= slot.lost_at + std::chrono::seconds{ delay_sec };
    }

    inline auto slot_dispatchable(chunk_slot const& slot, std::uint64_t pump_id,
                                  chunk_clock::time_point now, int delay_sec) -> bool
    {
        if (slot.phase == chunk_phase::idle) return true;
        if (slot.phase != chunk_phase::retry_wait) return false;
        switch (slot.due)
        {
            case retry_due::this_pump:  return true;
            case retry_due::next_pump:  return slot.armed_pump < pump_id;
            case retry_due::after_delay:return slot.armed_pump < pump_id
                                              && slot_due(slot, now, delay_sec);
            default:                    return false;
        }
    }

    enum class arm_failure_result
    {
        stale,
        retry_wait,
        exhausted,
    };

    inline auto arm_failure(chunk_slot& slot, bool counted_attach, bool reused,
                            int max_tries, chunk_clock::time_point now,
                            std::uint64_t pump_id) -> arm_failure_result
    {
        if (reused && slot.origin == xfer_err_origin::connection && slot.done == slot.attempt_base)
        {
            if (counted_attach && slot.attempts > 0) --slot.attempts;
            slot.phase = counted_attach ? chunk_phase::retry_wait : chunk_phase::idle;
            if (counted_attach)
            {
                slot.armed_pump = pump_id;
                slot.due = retry_due::next_pump;
            }
            else
            {
                slot.origin = xfer_err_origin::none;
                slot.error.clear();
            }
            return arm_failure_result::stale;
        }
        if (should_auto_retry(slot.origin, slot.attempts, max_tries))
        {
            slot.phase = chunk_phase::retry_wait;
            slot.lost_at = now;
            slot.armed_pump = pump_id;
            slot.due = retry_due::after_delay;
            return arm_failure_result::retry_wait;
        }
        slot.phase = chunk_phase::failed;
        return arm_failure_result::exhausted;
    }

    inline void arm_manual_retry(chunk_slot& slot, std::uint64_t pump_id)
    {
        slot.attempts = 0;
        slot.phase = chunk_phase::retry_wait;
        slot.armed_pump = pump_id;
        slot.due = retry_due::this_pump;
    }

    inline auto slot_retryable_connection(chunk_slot const& slot) -> bool
    {
        return slot.origin == xfer_err_origin::connection
            && (slot.phase == chunk_phase::retry_wait || slot.phase == chunk_phase::failed);
    }

    enum class xfer_parent_decision
    {
        transferring,
        succeeded,
        failed,
    };

    struct xfer_job_counts
    {
        std::size_t ok = 0;
        std::size_t exhausted = 0;
        std::size_t active = 0; // Connecting, running, or retry-waiting.
        std::size_t parked = 0; // Not launched yet.
        bool parked_blocked = false; // Upload leader exhausted before followers became safe.
    };

    template<class Range, class Project>
    inline auto counts_from_slots(Range const& values, bool upload, bool resume,
                                  Project project) -> xfer_job_counts
    {
        auto counts = xfer_job_counts{};
        auto leader = static_cast<chunk_slot const*>(nullptr);
        for (auto const& value : values)
        {
            auto const& slot = std::invoke(project, value);
            if (!leader) leader = &slot;
            switch (slot.phase)
            {
                case chunk_phase::succeeded:  ++counts.ok; break;
                case chunk_phase::failed:     ++counts.exhausted; break;
                case chunk_phase::idle:       ++counts.parked; break;
                case chunk_phase::running:
                case chunk_phase::retry_wait: ++counts.active; break;
            }
        }
        counts.parked_blocked = upload && !resume && leader
                             && leader->phase == chunk_phase::failed
                             && !leader->opened;
        return counts;
    }

    inline auto counts_from_slots(std::span<chunk_slot const> slots,
                                  bool upload, bool resume) -> xfer_job_counts
    {
        return counts_from_slots(slots, upload, resume,
                                 [](chunk_slot const& slot) -> chunk_slot const& { return slot; });
    }

    inline auto decide_xfer_parent(xfer_job_counts const& counts) -> xfer_parent_decision
    {
        if (counts.active) return xfer_parent_decision::transferring;
        if (counts.parked && !counts.parked_blocked) return xfer_parent_decision::transferring;
        if (counts.exhausted) return xfer_parent_decision::failed;
        if (counts.ok && !counts.parked) return xfer_parent_decision::succeeded;
        return xfer_parent_decision::transferring;
    }

    inline auto leader_should_initialize(bool download, bool resume, std::size_t chunk,
                                         bool ready_written, bool leader_opened) -> bool
    {
        return !download && !resume && chunk == 0 && !ready_written && !leader_opened;
    }
}
