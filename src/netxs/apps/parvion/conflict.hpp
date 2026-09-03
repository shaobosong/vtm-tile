// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/conflict.hpp: the pure destination-conflict policy used by the
// enqueue path.  Filesystem and remote-listing probes deliberately live in
// session.hpp; keeping this module side-effect free makes the policy easy to
// test and prevents a worker from becoming part of the decision.

#include "../../desktopio/intmath.hpp"
#include "../../desktopio/utf.hpp"

#include <ctime>

namespace netxs::app::parvion
{
    enum conflict_policy_t : si32
    {
        conflict_overwrite = 0,
        conflict_skip,
        conflict_newer,
        conflict_resume,
        conflict_rename,
        conflict_ask,
        conflict_policy_count,
    };

    inline auto conflict_policy_label(si32 policy) -> view
    {
        static constexpr auto names = std::array<view, conflict_policy_count>{
            "Overwrite", "Skip", "Newer", "Resume", "Rename", "Ask" };
        return names[(size_t)std::clamp(policy, 0, conflict_policy_count - 1)];
    }

    inline auto conflict_policy_name(conflict_policy_t policy) -> view
    {
        static constexpr auto names = std::array<view, conflict_policy_count>{
            "overwrite", "skip", "newer", "resume", "rename", "ask" };
        return names[(size_t)std::clamp((si32)policy, 0, conflict_policy_count - 1)];
    }

    inline auto conflict_policy_from_name(view name) -> conflict_policy_t
    {
        for (auto i = si32{}; i < conflict_policy_count; ++i)
            if (name == conflict_policy_name((conflict_policy_t)i)) return (conflict_policy_t)i;
        return conflict_overwrite;
    }

    // The modal uses these choices to distinguish a one-file answer from the
    // corresponding answer for the remainder of the enqueue batch.
    enum class conflict_choice
    {
        overwrite,
        overwrite_all,
        skip,
        skip_all,
        rename,
        rename_all,
        cancel_rest,
    };

    struct conflict_inputs
    {
        bool download = true;
        text source_path;
        text dest_path;
        si64 source_size = 0;
        time_t source_mtime = 0; // 0 = unknown.
        bool dest_exists = false;
        si64 dest_size = -1;     // -1 = unknown.
        time_t dest_mtime = 0;   // 0 = unknown.
        bool state_file_usable = false;
    };

    enum conflict_action : si32
    {
        write_dest = 0,
        skip,
        resume_dest,
        rename_dest,
        ask_user,
    };

    inline auto classify(conflict_policy_t policy, conflict_inputs const& in) -> conflict_action
    {
        if (!in.dest_exists) return write_dest;
        switch (policy)
        {
            case conflict_overwrite:
                return write_dest;
            case conflict_skip:
                return skip;
            case conflict_newer:
                // Unknown timestamps must not turn a normal transfer into a
                // surprising skip.
                return !in.source_mtime || !in.dest_mtime || in.source_mtime > in.dest_mtime
                     ? write_dest : skip;
            case conflict_resume:
                if (in.state_file_usable) return resume_dest;
                return write_dest;
            case conflict_rename:
                return rename_dest;
            case conflict_ask:
                return ask_user;
            default:
                return write_dest;
        }
    }

    // Produce the nth numbered sibling while retaining the extension sequence
    // beginning at the first dot after the basename (archive.tar.gz becomes
    // archive (1).tar.gz).
    // The one-argument form is the public policy helper used by callers/tests.
    inline auto numbered_name(text dest_path, ui64 number) -> text
    {
        auto slash = dest_path.find_last_of("/\\");
        auto base = slash == text::npos ? size_t{} : slash + 1;
        auto dot = dest_path.find('.', base);
        if (dot == base) dot = text::npos; // Treat a leading dot as a name, not an extension.
        auto stem_end = dot == text::npos ? dest_path.size() : dot;
        return dest_path.substr(0, stem_end) + " (" + std::to_string(number) + ")"
             + (dot == text::npos ? text{} : dest_path.substr(dot));
    }

    inline auto uniquify_name(text dest_path) -> text
    {
        return numbered_name(std::move(dest_path), 1);
    }

    // Compare destinations that are still only queued. The explicit fold flag keeps the Windows
    // local-path rule testable on every host; production uses the platform-aware overload below.
    inline auto same_queued_dest(bool download, view a, view b, bool fold_local) -> bool
    {
        if (!download || !fold_local) return a == b;
        auto lhs = text{ a };
        auto rhs = text{ b };
        for (auto& c : lhs) if (c == '\\') c = '/';
        for (auto& c : rhs) if (c == '\\') c = '/';
        utf::to_lower(lhs);
        utf::to_lower(rhs);
        return lhs == rhs;
    }

    inline auto same_queued_dest(bool download, view a, view b) -> bool
    {
        #if defined(_WIN32)
        return same_queued_dest(download, a, b, download);
        #else
        return same_queued_dest(download, a, b, faux);
        #endif
    }
}
