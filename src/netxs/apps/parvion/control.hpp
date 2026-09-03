// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/control.hpp: ownership and state for the single SFTP control-command
// stream.  The transport and host-side effects stay in sftp_remote; this file
// contains only the state that must die with the workflow that owns the wire.

#include "model.hpp"
#include "proto.hpp"

#include <ctime>
#include <type_traits>
#include <utility>
#include <variant>

namespace netxs::app::parvion
{
    enum class control_command
    {
        c_none,
        c_open,
        c_pwd,
        c_ls,
        c_cd,
        c_cd_rollback,
        c_op,
        c_rls,
        c_recop,
        c_keyfile,
    };

    enum class rec_op { mkdir, rm, rmdir };

    struct rec_command
    {
        rec_op op;
        text path;
    };

    inline auto rec_command_line(rec_command const& command) -> text
    {
        auto verb = command.op == rec_op::mkdir ? view{ "mkdir " }
                  : command.op == rec_op::rm    ? view{ "rm " }
                                                : view{ "rmdir " };
        return text{ verb } + quote_name(command.path);
    }

    // Exactly one alternative owns the helper command stream.  `command` is
    // wire occupancy only; each alternative's phase is its program counter.
    struct control_transaction
    {
        using steady_clock = std::chrono::steady_clock;

        struct connection
        {
            enum class phase_t { retry_wait, waiting_greeting, registering_key, opening, discovering_path, restoring_path, listing };
            phase_t phase = phase_t::waiting_greeting;
            bool reconnect = faux;
            text restore_path = "/";
            text target;
            size_t keyfile_i = 0;
            si32 attempts = 0;
            steady_clock::time_point retry_at{};

            connection() = default;
            connection(bool reconnect, text restore_path, si32 attempts = 0)
                : reconnect{ reconnect }, restore_path{ std::move(restore_path) }, attempts{ attempts } { }
        };

        struct navigation
        {
            enum class phase_t { changing_directory, waiting_to_list, listing_destination, rolling_back };
            phase_t phase = phase_t::changing_directory;
            text source;
            text target;
            text fallback_name;
            si64 fallback_size = -1;
            time_t fallback_mtime = 0;
            steady_clock::time_point list_due{};

            navigation() = default;
            navigation(text source, text target, text fallback_name = {}, si64 fallback_size = -1,
                       time_t fallback_mtime = 0)
                : source{ std::move(source) }, target{ std::move(target) },
                  fallback_name{ std::move(fallback_name) }, fallback_size{ fallback_size },
                  fallback_mtime{ fallback_mtime } { }
            auto has_fallback() const { return !fallback_name.empty(); }
        };

        struct refresh
        {
            enum class phase_t { listing };
            phase_t phase = phase_t::listing;
        };

        struct mutation
        {
            enum class phase_t { executing, listing_after_success, listing_after_failure };
            phase_t phase = phase_t::executing;
        };

        struct rec_dir
        {
            text remote;
            text local;
        };

        struct rec_download
        {
            enum class phase_t { collecting, listing };
            phase_t phase = phase_t::collecting;
            std::vector<rec_dir> stack;
            rec_dir current;
            size_t download_files = 0;
        };

        struct rec_delete
        {
            enum class phase_t { collecting, listing, executing, listing_result };
            phase_t phase = phase_t::collecting;
            std::vector<rec_dir> stack;
            rec_dir current;
            std::vector<text> files;
            std::vector<text> dirs;
            std::vector<rec_command> commands;
            size_t command_i = 0;
            bool commands_built = faux;
            size_t failures = 0;
            text completion_status;
        };

        struct rec_upload_file
        {
            text local_path;
            text remote_path;
            si64 size = 0;
            time_t mtime = 0;
        };

        struct rec_upload
        {
            enum class phase_t { collecting, executing, probing_mkdir, listing_after_abort, listing_result };
            phase_t phase = phase_t::collecting;
            std::vector<rec_command> commands;
            size_t command_i = 0;
            std::vector<rec_upload_file> uploads;
            text completion_status;
        };

        struct keepalive
        {
            enum class phase_t { waiting };
            phase_t phase = phase_t::waiting;
        };

        using state_t = std::variant<
            std::monostate,
            connection,
            navigation,
            refresh,
            mutation,
            rec_download,
            rec_delete,
            rec_upload,
            keepalive>;

        control_command command = control_command::c_none;
        std::vector<direntry> staged;
        state_t state;

        template<class Flow, class... Args>
        auto start(Args&&... args) -> Flow&
        {
            command = control_command::c_none;
            staged.clear();
            return state.template emplace<Flow>(std::forward<Args>(args)...);
        }

        void reset()
        {
            command = control_command::c_none;
            staged.clear();
            state.template emplace<std::monostate>();
        }

        template<class Flow> auto get_if()       -> Flow*       { return std::get_if<Flow>(&state); }
        template<class Flow> auto get_if() const -> Flow const* { return std::get_if<Flow>(&state); }
        template<class Flow> auto is() const { return std::holds_alternative<Flow>(state); }
        template<class Flow> auto is(typename Flow::phase_t phase) const
        {
            auto flow = get_if<Flow>();
            return flow && flow->phase == phase;
        }
        auto active() const { return !std::holds_alternative<std::monostate>(state); }
        auto wire_busy() const { return command != control_command::c_none; }

        template<class Flow> auto ensure_collecting() -> Flow*
        {
            if (auto flow = get_if<Flow>())
                return flow->phase == Flow::phase_t::collecting ? flow : nullptr;
            if (active()) return nullptr;
            return &start<Flow>();
        }

        auto recursive() const
        {
            return is<rec_download>() || is<rec_delete>() || is<rec_upload>();
        }

        auto accepts_listing() const
        {
            return std::visit([](auto const& flow)
            {
                using flow_t = std::decay_t<decltype(flow)>;
                if constexpr (std::is_same_v<flow_t, connection>)
                    return flow.phase == connection::phase_t::listing;
                else if constexpr (std::is_same_v<flow_t, navigation>)
                    return flow.phase == navigation::phase_t::listing_destination;
                else if constexpr (std::is_same_v<flow_t, refresh>)
                    return true;
                else if constexpr (std::is_same_v<flow_t, mutation>)
                    return flow.phase == mutation::phase_t::listing_after_success
                        || flow.phase == mutation::phase_t::listing_after_failure;
                else if constexpr (std::is_same_v<flow_t, rec_download>)
                    return flow.phase == rec_download::phase_t::listing;
                else if constexpr (std::is_same_v<flow_t, rec_delete>)
                    return flow.phase == rec_delete::phase_t::listing
                        || flow.phase == rec_delete::phase_t::listing_result;
                else if constexpr (std::is_same_v<flow_t, rec_upload>)
                    return flow.phase == rec_upload::phase_t::probing_mkdir
                        || flow.phase == rec_upload::phase_t::listing_after_abort
                        || flow.phase == rec_upload::phase_t::listing_result;
                else return false;
            }, state);
        }
    };

    static_assert(std::is_move_assignable_v<control_transaction>);
}
