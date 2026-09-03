// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/conflict_enqueue.hpp: a filesystem-free enqueue pump. Session code
// provides destination probing, name occupancy, queue commits, and UI asks;
// tests can drive this header without an SFTP connection or a queue model.

#include "conflict.hpp"

#include <ctime>
#include <deque>
#include <memory>
#include <optional>
#include <utility>

namespace netxs::app::parvion
{
    struct transfer_request
    {
        bool download = true;
        text local_path;
        text remote_path;
        text dest_dir;
        si64 size = 0;
        time_t source_mtime = 0;
    };

    struct enqueue_batch
    {
        conflict_policy_t policy = conflict_overwrite;
        std::deque<transfer_request> pending;
        std::optional<conflict_action> all_action;
        bool waiting = faux;
        bool cancelled = faux;
    };

    inline auto action_for_choice(conflict_choice choice) -> conflict_action
    {
        switch (choice)
        {
            case conflict_choice::skip:
            case conflict_choice::skip_all:     return skip;
            case conflict_choice::rename:
            case conflict_choice::rename_all:   return rename_dest;
            case conflict_choice::overwrite:
            case conflict_choice::overwrite_all:
            default:                            return write_dest;
        }
    }

    template<class Uniquify>
    inline auto take_enqueue_dest(conflict_action action, transfer_request request, Uniquify& uniquify)
        -> std::optional<transfer_request>
    {
        if (action != rename_dest) return request;
        return uniquify(std::move(request));
    }

    template<class Probe, class Uniquify, class Commit, class Ask>
    inline void pump_enqueue_batch(std::shared_ptr<enqueue_batch> const& batch,
                                   Probe probe, Uniquify uniquify, Commit commit, Ask ask)
    {
        if (!batch || batch->waiting || batch->cancelled) return;
        while (!batch->pending.empty())
        {
            auto request = std::move(batch->pending.front());
            batch->pending.pop_front();
            auto inputs = probe(request);
            auto action = classify(batch->policy, inputs);
            if (action == ask_user)
            {
                if (batch->all_action) action = *batch->all_action;
                else
                {
                    batch->waiting = true;
                    ask(inputs.dest_path,
                        [batch, request = std::move(request),
                         probe, uniquify, commit, ask](conflict_choice choice) mutable
                        {
                            if (!batch || batch->cancelled) return;
                            batch->waiting = faux;
                            if (choice == conflict_choice::cancel_rest)
                            {
                                batch->cancelled = true;
                                batch->pending.clear();
                                return;
                            }
                            if (choice == conflict_choice::overwrite_all
                             || choice == conflict_choice::skip_all
                             || choice == conflict_choice::rename_all)
                                batch->all_action = action_for_choice(choice);
                            auto action = action_for_choice(choice);
                            if (auto dest = take_enqueue_dest(action, std::move(request), uniquify))
                                commit(std::move(*dest), action);
                            pump_enqueue_batch(batch, probe, uniquify, commit, ask);
                        });
                    return;
                }
            }
            if (auto dest = take_enqueue_dest(action, std::move(request), uniquify))
                commit(std::move(*dest), action);
        }
    }
}
