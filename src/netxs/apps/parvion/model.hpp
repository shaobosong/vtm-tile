// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/model.hpp: Data model for the Parvion applet.
//   - direntry  : one directory entry (modeled on FileZilla's CDirentry).
//   - queue_item: one queued transfer (modeled on FileZilla's CFileItem).
//   - local FS browsing via std::filesystem + human-readable formatters.
// The remote side reuses `direntry`/`queue_item`; only the producer differs
// (std::filesystem here vs. the parvionsftp/fzprintf session in later phases).

#include "rate.hpp"

#include <filesystem>
#include <vector>
#include <array>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <system_error>

namespace netxs::app::parvion
{
    namespace fs = std::filesystem;

    // One entry in a directory listing (local or remote).
    struct direntry
    {
        text   name;            // File or directory name (no path).
        si64   size   = -1;     // Size in bytes; -1 = unknown (e.g. directory).
        text   perms;           // "rwxr-xr-x" (remote; empty if unknown).
        text   owner;           // "user:group" (remote; empty if unknown).
        time_t mtime  = 0;      // Modification time (epoch seconds; 0 = unknown).
        bool   is_dir = faux;   // Directory.
        bool   is_link = faux;  // Symbolic link.
        text   target;          // Symlink target (if is_link).
    };

    // One queued/active/finished transfer.
    struct queue_item
    {
        enum status_t { queued, transferring, succeeded, failed };
        struct table_ui_state
        {
            bool selected = faux;
            bool expanded = faux;
        };
        static constexpr auto table_count = size_t{ 3 }; // Transferring, Failed, Succeeded.

        bool     download = true;   // true: remote->local; false: local->remote.
        text     local_path;        // Absolute local path.
        text     remote_path;       // Absolute remote path.
        text     dest_dir;          // Destination directory at enqueue time (local_dir for a
                                    // download, remote path for an upload); used on completion
                                    // to refresh the destination pane only if it is still shown.
        si64     size = 0;          // Total bytes.
        si64     done = 0;          // Bytes transferred so far.
        si32     priority = 2;      // 0..4 (lowest..highest); 2 = normal.
        status_t status = queued;
        bool     paused = faux;     // Queued item held back from auto-start (user "Pause"); shown as "paused".
        text     error;             // Failure reason (when status == failed).
        std::time_t started = 0;    // Wall-clock start (for the FileZilla-style "transferred X in Y" summary).
        rate_meter rate;            // Live byte-rate: sliding-window instantaneous (rate.speed) + resume-aware baseline (see rate.hpp).
        // Parallel-transfer metadata (Phase 4): chunk index/count + state path.
        ui32     chunk_index = 0;
        ui32     chunk_count = 1;
        // Session-only presentation state is independent for the three transfer tables. An item can
        // move between statuses without carrying selection/expansion into the destination table,
        // while its previous state is remembered if it later returns.
        std::array<table_ui_state, table_count> table_ui{};
    };

    // One remembered Quick Connect target (the most-recent-first history persisted by
    // sftp_remote; mirrors FileZilla's CRecentServerList entries).
    struct recent_server
    {
        text host;
        text user;
        text pass;
        si32 port = 22;
        // Identity for de-duplication (the password is not part of the key).
        auto same_target(recent_server const& o) const
        {
            return host == o.host && user == o.user && port == o.port;
        }
    };

    // Queue-view server/source caption: keep transfer and checksum tables consistent.
    // The explicit port matches the Checksums Source convention, including the default :22.
    inline auto server_source(text const& host, text const& user, si32 port) -> text
    {
        if (host.empty()) return {};
        return (user.empty() ? text{} : user + "@") + host + ":" + std::to_string(port);
    }

    // Human-readable byte count, e.g. "1.2K", "410M". Empty for size < 0.
    inline auto human_size(si64 n) -> text
    {
        if (n < 0) return {};
        static constexpr auto unit = std::array{ "B", "K", "M", "G", "T", "P" };
        auto v = (double)n;
        auto i = size_t{ 0 };
        while (v >= 1024.0 && i + 1 < unit.size()) { v /= 1024.0; ++i; }
        auto buf = std::array<char, 32>{};
        if (i == 0) std::snprintf(buf.data(), buf.size(), "%lld B", (long long)n);
        else        std::snprintf(buf.data(), buf.size(), "%.1f %s", v, unit[i]);
        return text{ buf.data() };
    }

    // Format an epoch time as "YYYY-MM-DD HH:MM". Empty for t == 0.
    inline auto fmt_time(time_t t) -> text
    {
        if (!t) return {};
        auto tmv = std::tm{};
        #if defined(_WIN32)
            ::localtime_s(&tmv, &t);
        #else
            ::localtime_r(&t, &tmv);
        #endif
        auto buf = std::array<char, 32>{};
        std::strftime(buf.data(), buf.size(), "%Y-%m-%d %H:%M", &tmv);
        return text{ buf.data() };
    }

    // Convert a std::filesystem file_time_type to epoch seconds (portable across
    // libstdc++/MSVC where file_clock's epoch differs from system_clock's).
    inline auto to_epoch(fs::file_time_type ft) -> time_t
    {
        auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ft - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        return std::chrono::system_clock::to_time_t(sys);
    }

    // Parent of a path. is_local picks native (std::filesystem) vs POSIX (remote).
    inline auto parent_path(text const& path, bool is_local) -> text
    {
        if (is_local)
        {
        #if defined(_WIN32)
            if (path.empty()) return path;               // already at the drive list
            auto p = fs::path{ path };
            if (p.has_root_name() && p.relative_path().empty()) return {}; // C:\ -> drive list
        #endif
            auto up = fs::path{ path }.parent_path();
            return up.empty() ? path : up.string();
        }
        if (path == "/" || path.empty()) return "/";
        auto s = path;
        while (s.size() > 1 && s.back() == '/') s.pop_back();
        auto pos = s.find_last_of('/');
        if (pos == text::npos) return "/";
        return pos == 0 ? text{ "/" } : s.substr(0, pos);
    }
    // Child path (path + name).
    inline auto child_path(text const& path, text const& name, bool is_local) -> text
    {
        if (is_local) return (fs::path{ path } / name).string();
        if (path.empty() || path == "/") return "/" + name;
        return path.back() == '/' ? path + name : path + "/" + name;
    }
    // Resolve "."/".." segments in a POSIX path lexically (no symlink awareness — matches the
    // remote panes' logical navigation, like parent_path()). Collapses redundant slashes and any
    // trailing slash; ".." at an absolute root is clamped, a leading ".." in a relative path kept.
    inline auto normalize_posix(view path) -> text
    {
        auto abs   = !path.empty() && path.front() == '/';
        auto parts = std::vector<text>{};
        auto seg   = text{};
        auto flush = [&]
        {
            if      (seg.empty() || seg == ".") {}
            else if (seg == "..") { if (!parts.empty() && parts.back() != "..") parts.pop_back();
                                    else if (!abs) parts.push_back(".."); }
            else    parts.push_back(seg);
            seg.clear();
        };
        for (auto c : path) { if (c == '/') flush(); else seg += c; }
        flush();
        auto out = text{};
        for (auto& p : parts) { out += '/'; out += p; }
        if (abs) return out.empty() ? text{ "/" } : out;
        return out.empty() ? text{ "." } : text{ view{ out }.substr(1) }; // strip leading '/' (relative)
    }

#if defined(_WIN32)
    // Available drive roots (C:, D:, …) as directory entries, for the "drive list"
    // shown when going up from a drive root. Plain letters only (no volume labels).
    inline auto read_local_drives() -> std::vector<direntry>
    {
        auto out  = std::vector<direntry>{};
        auto mask = ::GetLogicalDrives(); // bit 0 = A:, bit 1 = B:, …
        for (auto i = 0; i < 26; ++i)
            if (mask & (1u << i))
            {
                auto e = direntry{};
                e.name   = text{ char('A' + i) } + ":"; // "C:"; root path is name + "\\"
                e.is_dir = true;
                out.push_back(std::move(e));
            }
        return out;
    }
#endif

    // Classify a path typed into a LOCAL file pane's address bar on Windows, so the pickers only
    // navigate to UNAMBIGUOUS targets (drive-letter + root-separator semantics are Windows-specific;
    // consulted only under _WIN32). Pure string analysis — no std::filesystem — so it behaves the same
    // everywhere (and is testable off-Windows). Cases:
    //   drive_list : ""            -> the drive list ("Computer").
    //                "/"  "\"      -> a bare root separator also opens the drive list.
    //   invalid    : "C:" "C:dir"  -> drive-relative: the *current* directory on a drive (hidden state).
    //                "/dir" "\dir" -> rooted but drive-less: the drive is unknown.
    //   navigate   : "C:\..." "C:/..."   -> absolute on a named drive.
    //                "\\srv\share"        -> UNC (let std::filesystem resolve / report it).
    //                "dir" ".." "a/b"     -> relative to the current directory.
    enum class win_addr { navigate, drive_list, invalid };
    inline auto classify_win_addr(view s) -> win_addr
    {
        auto is_sep   = [](char c){ return c == '/' || c == '\\'; };
        auto is_alpha = [](char c){ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
        if (s.empty()) return win_addr::drive_list;
        if (s.size() == 1 && is_sep(s[0])) return win_addr::drive_list;     // bare "/" or "\"
        if (s.size() >= 2 && is_alpha(s[0]) && s[1] == ':')                 // drive-letter prefix "X:"
        {
            if (s.size() == 2 || !is_sep(s[2])) return win_addr::invalid;   // "C:" / "C:dir" (drive-relative)
            return win_addr::navigate;                                      // "C:\..." / "C:/..."
        }
        if (is_sep(s[0]))
        {
            if (s.size() >= 2 && is_sep(s[1])) return win_addr::navigate;   // "\\..." / "//..." (UNC)
            return win_addr::invalid;                                       // "\dir" / "/dir" (drive-less rooted)
        }
        return win_addr::navigate;                                         // relative path
    }

    // Read a local directory into entries, sorted directories-first then by
    // case-insensitive name. Errors are swallowed (returns what was readable).
    inline auto read_local_dir(fs::path const& dir) -> std::vector<direntry>
    {
        auto out = std::vector<direntry>{};
        auto ec = std::error_code{};
        auto it = fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
        if (ec) return out;
        for (auto& de : it)
        {
            auto e = direntry{};
            e.name = de.path().filename().string();
            if (e.name.empty()) continue;
            auto se = std::error_code{};
            e.is_link = de.is_symlink(se);
            e.is_dir  = de.is_directory(se);
            if (!e.is_dir)
            {
                auto sz = de.file_size(se);
                e.size = se ? si64{ -1 } : (si64)sz;
            }
            auto ft = de.last_write_time(se);
            if (!se) e.mtime = to_epoch(ft);
            out.push_back(std::move(e));
        }
        std::sort(out.begin(), out.end(), [](direntry const& a, direntry const& b)
        {
            if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir; // Directories first.
            auto al = a.name; utf::to_lower(al);
            auto bl = b.name; utf::to_lower(bl);
            return al < bl;
        });
        return out;
    }
}
