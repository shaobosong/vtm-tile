// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/settings.hpp: persisted Parvion settings — the SFTP-relevant subset of
// FileZilla's Connection and Connection/SFTP option pages (TLS options excluded):
//   Connection : Timeout, Reconnect count, Reconnect delay.
//   SFTP       : private key files, compression, parallel-transfer threshold + unit,
//                max parallel connections per file.
//   Debug      : debug information level and raw directory listing.
// Stored as a flat `key<TAB>value` file next to the Quick Connect history
// (recent_servers), mirroring sftp_remote::load_recent / save_recent. The engine
// applies these onto its live fields (see sftp_remote::apply_settings).

#include "model.hpp"
#include "logging.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace netxs::app::parvion
{
    // Parallel-transfer threshold units, mirroring FileZilla's
    // OPTION_SFTP_PARALLEL_THRESHOLD_UNIT (0..4 = Byte/KiB/MiB/GiB/TiB).
    inline constexpr auto sftp_unit_count = si32{ 5 };
    inline auto sftp_unit_label(si32 u) -> view
    {
        static constexpr auto names = std::array<view, sftp_unit_count>{{ "Byte", "KiB", "MiB", "GiB", "TiB" }};
        return names[(size_t)std::clamp(u, 0, sftp_unit_count - 1)];
    }
    inline auto sftp_unit_mul(si32 u) -> si64
    {
        auto m = si64{ 1 };
        for (auto i = si32{}; i < std::clamp(u, 0, sftp_unit_count - 1); ++i) m <<= 10;
        return m;
    }

    // Hash-verification algorithms (Settings dropdown + right-click submenu). The index is
    // persisted (parvion_settings::hash_algo) and mapped to a coreutils-style name passed to
    // the `parvionhash` backend. Order is fixed; SHA-256 (index 2) is the default.
    inline constexpr auto hash_algo_count = si32{ 5 };
    inline auto hash_algo_label(si32 a) -> view // UI label
    {
        static constexpr auto names = std::array<view, hash_algo_count>{{ "MD5", "SHA-1", "SHA-256", "SHA-384", "SHA-512" }};
        return names[(size_t)std::clamp(a, 0, hash_algo_count - 1)];
    }
    inline auto hash_algo_name(si32 a) -> view // backend arg / hash.hpp make_hasher id
    {
        static constexpr auto names = std::array<view, hash_algo_count>{{ "md5", "sha1", "sha256", "sha384", "sha512" }};
        return names[(size_t)std::clamp(a, 0, hash_algo_count - 1)];
    }

    // <config>/parvion/ — $XDG_CONFIG_HOME (else $HOME/.config) on POSIX, %APPDATA%
    // on Windows; created on demand. Shared by recent_servers and settings.
    inline auto parvion_config_dir() -> fs::path
    {
        auto ec = std::error_code{};
        #if defined(_WIN32)
        auto base = std::getenv("APPDATA");
        auto cfg  = fs::path{ base && *base ? base : "." };
        #else
        auto xdg  = std::getenv("XDG_CONFIG_HOME");
        auto home = std::getenv("HOME");
        auto cfg  = xdg  && *xdg  ? fs::path{ xdg }
                  : home && *home ? fs::path{ home } / ".config"
                  :                 fs::path{ "." };
        #endif
        auto dir = cfg / "parvion";
        fs::create_directories(dir, ec);
        return dir;
    }
    inline auto parvion_settings_path() -> fs::path { return parvion_config_dir() / "settings"; }

    struct parvion_settings
    {
        // Connection page.
        si32 timeout         = 20;   // OPTION_TIMEOUT          : seconds; 0 (disabled) or 10..9999.
        si32 reconnect_count = 0;    // OPTION_RECONNECTCOUNT   : 0..99 (0 = unlimited).
        si32 reconnect_delay = 5;    // OPTION_RECONNECTDELAY   : 0..999 seconds.
        // Connection/SFTP page.
        bool compression     = faux; // OPTION_SFTP_COMPRESSION.
        si32 threshold_value = 64;   // OPTION_SFTP_PARALLEL_THRESHOLD_VALUE : 1..1048576.
        si32 threshold_unit  = 2;    // OPTION_SFTP_PARALLEL_THRESHOLD_UNIT  : 0..4 (default MiB).
        si32 max_connections = 4;    // OPTION_SFTP_PARALLEL_MAX_CONNECTIONS : 1..10.
        std::vector<text> keyfiles;  // OPTION_SFTP_KEYFILES (one private-key path per entry).
        // Hash verification page (Edit -> Settings -> SFTP -> "Hash verification").
        bool hash_on_transfer = faux; // Auto-hash the target of every completed transfer.
        si32 hash_algo        = 2;    // Algorithm index 0..4; SHA-256 default.
        // Debug page.
        si32 log_debug_level  = log_debug_none; // 0=None .. 4=Debug.
        bool log_raw_listing  = faux; // Show raw directory listing lines in the message log.

        // Bytes form of the "enable parallel transfers for files larger than" gate.
        auto threshold_bytes() const -> si64 { return (si64)threshold_value * sftp_unit_mul(threshold_unit); }

        // Clamp every numeric field to its FileZilla-documented range. Applied after
        // load and after any edit so out-of-range values can never reach the engine.
        void clamp()
        {
            timeout         = timeout == 0 ? 0 : std::clamp(timeout, 10, 9999);
            reconnect_count = std::clamp(reconnect_count, 0, 99);
            reconnect_delay = std::clamp(reconnect_delay, 0, 999);
            threshold_value = std::clamp(threshold_value, 1, 1024 * 1024);
            threshold_unit  = std::clamp(threshold_unit, 0, sftp_unit_count - 1);
            max_connections = std::clamp(max_connections, 1, 10);
            hash_algo       = std::clamp(hash_algo, 0, hash_algo_count - 1);
            log_debug_level = std::clamp(log_debug_level, 0, 4);
        }

        // Restore from <config>/parvion/settings; missing file leaves defaults.
        void load()
        {
            auto f = std::fopen(parvion_settings_path().string().c_str(), "rb");
            if (!f) return;
            auto buf = text{};
            auto tmp = std::array<char, 4096>{};
            for (auto n = size_t{}; (n = std::fread(tmp.data(), 1, tmp.size(), f)) > 0; ) buf.append(tmp.data(), n);
            std::fclose(f);
            keyfiles.clear();
            for (auto pos = size_t{}; pos < buf.size(); )
            {
                auto eol  = buf.find('\n', pos);
                auto line = buf.substr(pos, eol == text::npos ? text::npos : eol - pos);
                pos = eol == text::npos ? buf.size() : eol + 1;
                auto tab = line.find('\t');
                if (tab == text::npos) continue;
                auto key = line.substr(0, tab);
                auto val = line.substr(tab + 1);
                     if (key == "Timeout")                                timeout         = std::atoi(val.c_str());
                else if (key == "Reconnect count")                        reconnect_count = std::atoi(val.c_str());
                else if (key == "Reconnect delay")                        reconnect_delay = std::atoi(val.c_str());
                else if (key == "SFTP compression")                       compression     = std::atoi(val.c_str()) != 0;
                else if (key == "SFTP parallel transfer threshold value") threshold_value = std::atoi(val.c_str());
                else if (key == "SFTP parallel transfer threshold unit")  threshold_unit  = std::atoi(val.c_str());
                else if (key == "SFTP parallel max connections")          max_connections = std::atoi(val.c_str());
                else if (key == "Hash on transfer")                       hash_on_transfer = std::atoi(val.c_str()) != 0;
                else if (key == "Hash algorithm")                         hash_algo        = std::atoi(val.c_str());
                else if (key == "Logging Debug Level")                    log_debug_level  = std::atoi(val.c_str());
                else if (key == "Logging Raw Listing")                    log_raw_listing  = std::atoi(val.c_str()) != 0;
                else if (key == "SFTP keyfile")                           { if (!val.empty()) keyfiles.push_back(val); }
            }
            clamp();
        }

        // Persist to <config>/parvion/settings (0600: the file lists key paths).
        void save() const
        {
            auto fpath = parvion_settings_path();
            auto f = std::fopen(fpath.string().c_str(), "wb");
            if (!f) return;
            auto put = [&](view k, text v){ auto line = text{ k } + '\t' + v + '\n'; std::fwrite(line.data(), 1, line.size(), f); };
            put("Timeout", std::to_string(timeout));
            put("Reconnect count", std::to_string(reconnect_count));
            put("Reconnect delay", std::to_string(reconnect_delay));
            put("SFTP compression", std::to_string(compression ? 1 : 0));
            put("SFTP parallel transfer threshold value", std::to_string(threshold_value));
            put("SFTP parallel transfer threshold unit", std::to_string(threshold_unit));
            put("SFTP parallel max connections", std::to_string(max_connections));
            put("Hash on transfer", std::to_string(hash_on_transfer ? 1 : 0));
            put("Hash algorithm", std::to_string(hash_algo));
            put("Logging Debug Level", std::to_string(log_debug_level));
            put("Logging Raw Listing", std::to_string(log_raw_listing ? 1 : 0));
            for (auto& k : keyfiles) put("SFTP keyfile", k);
            std::fclose(f);
            #if !defined(_WIN32)
            auto ec = std::error_code{};
            fs::permissions(fpath, fs::perms::owner_read | fs::perms::owner_write, ec); // 0600.
            #endif
        }
    };
}
