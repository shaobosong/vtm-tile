// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/settings.hpp: persisted Parvion settings — the SFTP-relevant subset of
// FileZilla's Connection and Connection/SFTP option pages (TLS options excluded):
//   Connection : Timeout, Reconnect count, Reconnect delay.
//   SFTP       : private key files, compression, parallel-transfer threshold + unit,
//                shared transfer-channel budget, queue allocation, and existing-file policy.
//   Site       : saved SFTP connection profiles.
//   Debug      : debug information level and raw directory listing.
// Stored as a flat `key<TAB>value` file next to the Quick Connect history
// (recent_servers), mirroring sftp_remote::load_recent / save_recent. The engine
// applies these onto its live fields (see sftp_remote::apply_settings).

#include "logging.hpp"
#include "model.hpp"
#include "conflict.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace netxs::app::parvion
{
    enum transfer_allocation_t : si32
    {
        allocation_strict = 0,
        allocation_new_file_first,
        allocation_count,
    };
    inline auto transfer_allocation_label(si32 policy) -> view
    {
        static constexpr auto names = std::array<view, allocation_count>{
            "Strict queue order",
            "New file first",
        };
        return names[(size_t)std::clamp(policy, 0, allocation_count - 1)];
    }

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

    // Syntactic Host validation for saved sites. Keep this independent of DNS so accepting the
    // dialog never blocks or depends on network availability. Supported forms are qualified
    // RFC-style ASCII hostnames (including a final root dot), dotted-decimal IPv4, and raw IPv6
    // literals (including an optional RFC 4007 zone id such as fe80::1%eth0).
    inline auto valid_site_ipv4(view host) -> bool
    {
        auto values = std::array<ui32, 4>{};
        auto count = si32{};
        auto pos = size_t{};
        while (pos < host.size())
        {
            if (count == (si32)values.size()) return faux;
            auto end = host.find('.', pos);
            if (end == view::npos) end = host.size();
            if (end == pos || end - pos > 8) return faux;
            // Keep decimal components unambiguous: multi-digit parts cannot have a leading zero.
            if (end - pos > 1 && host[pos] == '0') return faux;
            auto value = ui64{};
            for (auto i = pos; i < end; ++i)
            {
                auto c = host[i];
                if (c < '0' || c > '9') return faux;
                value = value * 10 + (c - '0');
                if (value > 0xFFFFFFu) return faux; // Largest legal part is the 24-bit tail of a.b.
            }
            values[(size_t)count++] = (ui32)value;
            if (end == host.size()) break;
            pos = end + 1;
            if (pos == host.size()) return faux;
        }
        // Do not accept the historical one-component form: it would make bare values such as
        // "123" valid again. For abbreviated dotted forms, every leading component is 8 bits and
        // the final component consumes the remaining address bits.
        if (count < 2 || count > 4) return faux;
        for (auto i = si32{}; i + 1 < count; ++i)
            if (values[(size_t)i] > 0xFFu) return faux;
        auto last_max = count == 2 ? 0xFFFFFFu
                      : count == 3 ? 0xFFFFu
                                   : 0xFFu;
        return values[(size_t)(count - 1)] <= last_max;
    }

    inline auto valid_site_ipv6_part(view part, bool ipv4_allowed, si32& groups) -> bool
    {
        if (part.empty()) return true;
        for (auto pos = size_t{}; pos < part.size();)
        {
            auto end = part.find(':', pos);
            if (end == view::npos) end = part.size();
            auto token = part.substr(pos, end - pos);
            if (token.empty()) return faux;
            if (token.find('.') != view::npos)
            {
                if (!ipv4_allowed || end != part.size() || !valid_site_ipv4(token)) return faux;
                groups += 2;
            }
            else
            {
                if (token.size() > 4) return faux;
                for (auto c : token)
                    if (!((c >= '0' && c <= '9')
                       || (c >= 'a' && c <= 'f')
                       || (c >= 'A' && c <= 'F'))) return faux;
                ++groups;
            }
            if (end == part.size()) break;
            pos = end + 1;
            if (pos == part.size()) return faux;
        }
        return true;
    }

    inline auto valid_site_ipv6(view host) -> bool
    {
        auto zone = host.find('%');
        if (zone != view::npos)
        {
            if (zone == 0 || zone + 1 == host.size() || host.find('%', zone + 1) != view::npos) return faux;
            for (auto c : host.substr(zone + 1))
                if (!((c >= '0' && c <= '9')
                   || (c >= 'a' && c <= 'z')
                   || (c >= 'A' && c <= 'Z')
                   || c == '_' || c == '-' || c == '.')) return faux;
            host = host.substr(0, zone);
        }
        auto compressed = host.find("::");
        auto groups = si32{};
        if (compressed == view::npos)
            return valid_site_ipv6_part(host, true, groups) && groups == 8;
        if (host.find("::", compressed + 2) != view::npos) return faux;
        auto left = host.substr(0, compressed);
        auto right = host.substr(compressed + 2);
        return valid_site_ipv6_part(left, false, groups)
            && valid_site_ipv6_part(right, true, groups)
            && groups < 8;
    }

    inline auto valid_site_hostname(view host) -> bool
    {
        if (host.empty() || host.size() > 254) return faux;
        auto root_dot = faux;
        if (host.back() == '.')
        {
            root_dot = true;
            host.remove_suffix(1);
            if (host.empty() || host.back() == '.') return faux;
        }
        if (host.size() > 253) return faux;
        // Saved-site DNS hosts must be qualified names. A bare word or number ("abc", "123")
        // is ambiguous and is rejected; IPv4/IPv6 literals remain supported separately.
        if (host.find('.') == view::npos) return faux;
        auto numeric = true;
        for (auto c : host) numeric = numeric && ((c >= '0' && c <= '9') || c == '.');
        if (numeric) return !root_dot && valid_site_ipv4(host);
        for (auto pos = size_t{}; pos < host.size();)
        {
            auto end = host.find('.', pos);
            if (end == view::npos) end = host.size();
            auto label = host.substr(pos, end - pos);
            if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return faux;
            for (auto c : label)
                if (!((c >= '0' && c <= '9')
                   || (c >= 'a' && c <= 'z')
                   || (c >= 'A' && c <= 'Z')
                   || c == '-')) return faux;
            if (end == host.size()) break;
            pos = end + 1;
        }
        return true;
    }

    inline auto valid_site_host(view host) -> bool
    {
        if (host.empty()) return faux;
        return host.find(':') != view::npos ? valid_site_ipv6(host)
                                            : valid_site_hostname(host);
    }

    // One SFTP connection profile. An omitted name is generated by the editor from User, Host,
    // and Port. Passwords are optional: an empty password lets the backend's normal interactive
    // authentication prompt take over. Values originate in shared single-line inputs, which reject
    // tabs/newlines, so a Site settings record can safely use tabs to delimit these five fields.
    struct saved_site
    {
        text name;
        text host;
        si32 port = 22;
        text user;
        text pass;
    };

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
        si32 max_connections = 4;    // Shared budget for all transfer connections: 1..10.
        si32 transfer_allocation = allocation_strict; // Slot allocation policy.
        conflict_policy_t conflict_policy = conflict_overwrite; // Existing destination policy.
        std::vector<text> keyfiles;  // OPTION_SFTP_KEYFILES (one private-key path per entry).
        // Hash verification page (Edit -> Settings -> SFTP -> "Hash verification").
        bool hash_on_transfer = faux; // Auto-hash the target of every completed transfer.
        si32 hash_algo        = 2;    // Algorithm index 0..4; SHA-256 default.
        // Site page.
        std::vector<saved_site> sites;
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
            transfer_allocation = std::clamp(transfer_allocation, 0, allocation_count - 1);
            conflict_policy = (conflict_policy_t)std::clamp((si32)conflict_policy,
                                                            0, conflict_policy_count - 1);
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
            sites.clear();
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
                else if (key == "SFTP transfer queue allocation")          transfer_allocation = std::atoi(val.c_str());
                else if (key == "SFTP existing file policy")              conflict_policy = (conflict_policy_t)std::atoi(val.c_str());
                else if (key == "Hash on transfer")                       hash_on_transfer = std::atoi(val.c_str()) != 0;
                else if (key == "Hash algorithm")                         hash_algo        = std::atoi(val.c_str());
                else if (key == "Logging Debug Level")                    log_debug_level  = std::atoi(val.c_str());
                else if (key == "Logging Raw Listing")                    log_raw_listing  = std::atoi(val.c_str()) != 0;
                else if (key == "SFTP keyfile")                           { if (!val.empty()) keyfiles.push_back(val); }
                else if (key == "Site")
                {
                    auto t1 = val.find('\t'); if (t1 == text::npos) continue;
                    auto t2 = val.find('\t', t1 + 1); if (t2 == text::npos) continue;
                    auto t3 = val.find('\t', t2 + 1); if (t3 == text::npos) continue;
                    auto t4 = val.find('\t', t3 + 1); if (t4 == text::npos) continue;
                    auto site = saved_site{};
                    site.name = val.substr(0, t1);
                    site.host = val.substr(t1 + 1, t2 - t1 - 1);
                    auto port = val.substr(t2 + 1, t3 - t2 - 1);
                    site.port = port.empty() ? 22 : std::atoi(port.c_str());
                    site.user = val.substr(t3 + 1, t4 - t3 - 1);
                    site.pass = val.substr(t4 + 1);
                    auto duplicate = std::ranges::any_of(sites, [&](auto const& s){ return s.name == site.name; });
                    if (!site.name.empty() && !site.host.empty()
                     && site.port > 0 && site.port <= 65535 && !duplicate)
                        sites.push_back(std::move(site));
                }
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
            put("SFTP transfer queue allocation", std::to_string(transfer_allocation));
            put("SFTP existing file policy", std::to_string((si32)conflict_policy));
            put("Hash on transfer", std::to_string(hash_on_transfer ? 1 : 0));
            put("Hash algorithm", std::to_string(hash_algo));
            put("Logging Debug Level", std::to_string(log_debug_level));
            put("Logging Raw Listing", std::to_string(log_raw_listing ? 1 : 0));
            for (auto& k : keyfiles) put("SFTP keyfile", k);
            for (auto& site : sites)
                put("Site", site.name + '\t' + site.host + '\t' + std::to_string(site.port)
                          + '\t' + site.user + '\t' + site.pass);
            std::fclose(f);
            #if !defined(_WIN32)
            auto ec = std::error_code{};
            fs::permissions(fpath, fs::perms::owner_read | fs::perms::owner_write, ec); // 0600.
            #endif
        }
    };
}
