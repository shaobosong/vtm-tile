// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/resume.hpp: the resumable on-disk state for parallel transfers — the
// PARVIONC2 format (see engine/sftp/filetransfer.cpp in that codebase; only
// the 9-byte magic differs). One file per parallel transfer records the
// per-chunk progress so an interrupted transfer resumes the completed bytes
// of each chunk instead of restarting.
//
// On-disk layout (little-endian, serialized explicitly — no struct-packing
// reliance, so it is identical across gcc/MSVC):
//   [ 0.. 8] magic "PARVIONC2"
//   [ 9..15] zero pad
//   [16..23] total_size      u64
//   [24..27] part_count      u32
//   [28..31] reserved/status u32  (0 waiting, 1 ready, 2 aborted)
//   [32..35] metadata_size   u32
//   [36..67] metadata_sha256 32 bytes
//   [68..71] zero pad                         (header size = 72)
//   [72..]   metadata bytes (metadata_size)
//   [after]  part[count], each {start u64, size u64, transferred u64} = 24 B

#include "model.hpp"   // text, view, si64, ui64, ...
#include "proto.hpp"   // (quote_name etc. not needed, but keeps include order)

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace netxs::app::parvion
{
    // --- SHA-256 (FIPS 180-4), one-shot ------------------------------------
    namespace sha2
    {
        inline auto ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

        inline auto digest(view msg) -> std::array<uint8_t, 32>
        {
            static constexpr uint32_t k[64] = {
                0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
                0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
                0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
                0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
                0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
                0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
                0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
                0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };
            uint32_t h[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };

            auto data = text{ msg };
            auto const bitlen = (uint64_t)data.size() * 8;
            data.push_back('\x80'); // padding: leading 1-bit
            while (data.size() % 64 != 56) data.push_back('\0');
            for (auto i = 0; i < 8; ++i) data.push_back((char)((bitlen >> (56 - 8 * i)) & 0xFF));

            for (auto off = size_t{}; off < data.size(); off += 64)
            {
                uint32_t w[64];
                auto p = (uint8_t const*)data.data() + off;
                for (auto i = 0; i < 16; ++i)
                    w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
                for (auto i = 16; i < 64; ++i)
                {
                    auto s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
                    auto s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
                    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
                }
                uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
                for (auto i = 0; i < 64; ++i)
                {
                    auto S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25);
                    auto ch = (e & f) ^ (~e & g);
                    auto t1 = hh + S1 + ch + k[i] + w[i];
                    auto S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22);
                    auto maj = (a & b) ^ (a & c) ^ (b & c);
                    auto t2 = S0 + maj;
                    hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
                }
                h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
            }
            auto out = std::array<uint8_t, 32>{};
            for (auto i = 0; i < 8; ++i)
            {
                out[4 * i + 0] = (uint8_t)(h[i] >> 24);
                out[4 * i + 1] = (uint8_t)(h[i] >> 16);
                out[4 * i + 2] = (uint8_t)(h[i] >> 8);
                out[4 * i + 3] = (uint8_t)(h[i]);
            }
            return out;
        }

        inline auto hex(view msg) -> text
        {
            static constexpr auto digits = "0123456789abcdef";
            auto d = digest(msg);
            auto s = text{};
            for (auto b : d) { s.push_back(digits[b >> 4]); s.push_back(digits[b & 0xF]); }
            return s;
        }
    }

    // --- PARVIONC2 state file ----------------------------------------------
    inline constexpr size_t   state_header_size = 72;
    inline constexpr size_t   state_part_size   = 24;
    inline constexpr uint32_t state_waiting     = 0;
    inline constexpr uint32_t state_ready       = 1;
    inline constexpr uint32_t state_aborted     = 2;

    struct state_part { ui64 start = 0; ui64 size = 0; ui64 transferred = 0; };

    inline void put_u32le(text& b, uint32_t v) { for (auto i = 0; i < 4; ++i) b.push_back((char)((v >> (8 * i)) & 0xFF)); }
    inline void put_u64le(text& b, uint64_t v) { for (auto i = 0; i < 8; ++i) b.push_back((char)((v >> (8 * i)) & 0xFF)); }
    inline auto get_u32le(uint8_t const* p) -> uint32_t { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
    inline auto get_u64le(uint8_t const* p) -> uint64_t { auto v = uint64_t{}; for (auto i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (8 * i); return v; }

    // Byte offset of part[index].transferred within the file.
    inline auto part_xfer_offset(uint32_t metadata_size, uint32_t index) -> long
    {
        return (long)(state_header_size + metadata_size + state_part_size * index + 16);
    }

    inline auto write_state_file(text const& path, ui64 total, std::vector<state_part> const& parts,
                                 uint32_t status, text const& metadata) -> bool
    {
        auto body = text{};
        body.append("PARVIONC2", 9);
        body.append(7, '\0');                                  // pad -> 16
        put_u64le(body, total);                                // 16
        put_u32le(body, (uint32_t)parts.size());               // 24
        put_u32le(body, status);                               // 28
        put_u32le(body, (uint32_t)metadata.size());            // 32
        auto md = sha2::digest(metadata);
        body.append((char const*)md.data(), md.size());        // 36..67
        body.append(4, '\0');                                  // pad -> 72
        body.append(metadata);
        for (auto& p : parts) { put_u64le(body, p.start); put_u64le(body, p.size); put_u64le(body, p.transferred); }

        auto f = std::fopen(path.c_str(), "wb");
        if (!f) return faux;
        auto ok = std::fwrite(body.data(), 1, body.size(), f) == body.size();
        std::fclose(f);
        return ok;
    }

    // Read + validate (magic + metadata sha256). Fills total/status/metadata/parts.
    inline auto read_state_file(text const& path, ui64& total, uint32_t& status,
                                text& metadata, std::vector<state_part>& parts) -> bool
    {
        auto f = std::fopen(path.c_str(), "rb");
        if (!f) return faux;
        auto buf = text{};
        auto tmp = std::array<char, 8192>{};
        for (;;) { auto n = std::fread(tmp.data(), 1, tmp.size(), f); if (n) buf.append(tmp.data(), n); if (n < tmp.size()) break; }
        std::fclose(f);
        if (buf.size() < state_header_size) return faux;
        auto p = (uint8_t const*)buf.data();
        if (std::memcmp(p, "PARVIONC2", 9) != 0) return faux;
        total          = get_u64le(p + 16);
        auto count     = get_u32le(p + 24);
        status         = get_u32le(p + 28);
        auto md_size   = get_u32le(p + 32);
        if (state_header_size + (size_t)md_size + (size_t)count * state_part_size > buf.size()) return faux;
        metadata.assign(buf.data() + state_header_size, md_size);
        auto md = sha2::digest(metadata);
        if (std::memcmp(md.data(), p + 36, 32) != 0) return faux;     // metadata integrity
        parts.clear();
        auto base = state_header_size + md_size;
        for (auto i = uint32_t{}; i < count; ++i)
        {
            auto q = p + base + (size_t)i * state_part_size;
            parts.push_back({ get_u64le(q), get_u64le(q + 8), get_u64le(q + 16) });
        }
        return true;
    }

    // Live update of one chunk's transferred counter (fixed offset, no re-hash).
    inline auto write_state_transferred(text const& path, uint32_t metadata_size, uint32_t index, ui64 transferred) -> bool
    {
        auto f = std::fopen(path.c_str(), "r+b");
        if (!f) return faux;
        auto le = text{}; put_u64le(le, transferred);
        auto ok = std::fseek(f, part_xfer_offset(metadata_size, index), SEEK_SET) == 0
               && std::fwrite(le.data(), 1, le.size(), f) == le.size();
        std::fclose(f);
        return ok;
    }

    // Update the header status field (offset 28).
    inline auto write_state_status(text const& path, uint32_t status) -> bool
    {
        auto f = std::fopen(path.c_str(), "r+b");
        if (!f) return faux;
        auto le = text{}; put_u32le(le, status);
        auto ok = std::fseek(f, 28, SEEK_SET) == 0 && std::fwrite(le.data(), 1, le.size(), f) == le.size();
        std::fclose(f);
        return ok;
    }

    // --- Metadata / key / state-path (scheme inherited from Parvion) -
    // Server string per CServer::Format(with_user_and_optional_port): "user@host"
    // for the default SFTP port (22), else "sftp://user@host:port".
    inline auto fmt_server(text const& host, si32 port, text const& user) -> text
    {
        auto h = host.find(':') != text::npos ? "[" + host + "]" : host;     // bracket IPv6 literals
        auto default_port = port == 22;
        if (!default_port) h += ":" + std::to_string(port);
        if (!user.empty()) h = user + "@" + h;
        if (!default_port) h = "sftp://" + h;
        return h;
    }

    // CServerPath::GetSafePath for a unix path: "<type> <prefixlen> <seglen> <seg> ..."
    // (type 1 = unix, no prefix). Stable round-trip key, matches the GUI for unix hosts.
    inline auto safe_remote_path(text const& path) -> text
    {
        auto out = text{ "1 0" };
        auto i = size_t{};
        while (i < path.size())
        {
            while (i < path.size() && path[i] == '/') ++i;
            auto b = i;
            while (i < path.size() && path[i] != '/') ++i;
            if (i > b) { auto seg = path.substr(b, i - b); out += " " + std::to_string(seg.size()) + " " + seg; }
        }
        return out;
    }

    inline auto build_metadata(bool download, text const& server, text const& local_name,
                               text const& remote_path_safe, text const& remote_file) -> text
    {
        return text{ download ? "direction=download\n" : "direction=upload\n" }
             + "server="      + utf::base64(server)           + "\n"
             + "local_name="  + utf::base64(local_name)       + "\n"
             + "remote_path=" + utf::base64(remote_path_safe) + "\n"
             + "remote_file=" + utf::base64(remote_file)      + "\n";
    }

    inline auto state_key(bool download, text const& server, text const& local_dir, text const& local_file,
                          text const& remote_path_safe, text const& remote_file) -> text
    {
        return text{ download ? "download" : "upload" }
             + "|" + server + "|" + local_dir + "|" + local_file + "|" + remote_path_safe + "|" + remote_file;
    }

    // <local_dir>/<local_file>.parvion-{upload,download}-state.<sha256(key)[:16]>
    inline auto state_path(text const& local_dir, text const& local_file, bool download, text const& key) -> text
    {
        auto dir = local_dir;
        if (!dir.empty() && dir.back() != '/') dir.push_back('/');
        return dir + local_file + ".parvion-" + (download ? "download" : "upload") + "-state." + sha2::hex(key).substr(0, 16);
    }

    struct state_file_identity
    {
        text path;
        text metadata;
    };

    inline auto make_state_file_identity(bool download, text const& server,
                                         text const& local_path, text const& remote_path)
        -> state_file_identity
    {
        auto ls = local_path.find_last_of("/\\");
        auto ldir = ls == text::npos ? text{ "." } : (ls == 0 ? text{ "/" } : local_path.substr(0, ls));
        auto lfile = ls == text::npos ? local_path : local_path.substr(ls + 1);
        auto rs = remote_path.find_last_of('/');
        auto rdir = rs == text::npos ? text{ "/" } : (rs == 0 ? text{ "/" } : remote_path.substr(0, rs));
        auto rfile = rs == text::npos ? remote_path : remote_path.substr(rs + 1);
        auto rsafe = safe_remote_path(rdir);
        auto metadata = build_metadata(download, server, local_path, rsafe, rfile);
        auto key = state_key(download, server, ldir, lfile, rsafe, rfile);
        return { state_path(ldir, lfile, download, key), std::move(metadata) };
    }

    inline auto state_file_matches(state_file_identity const& identity, ui64 expected_total,
                                   std::vector<state_part> const& expected,
                                   std::vector<state_part>& found) -> bool
    {
        auto total = ui64{};
        auto status = uint32_t{};
        auto metadata = text{};
        auto parts = std::vector<state_part>{};
        if (!read_state_file(identity.path, total, status, metadata, parts)
         || status == state_aborted || total != expected_total || metadata != identity.metadata
         || parts.size() != expected.size())
            return faux;
        for (auto i = size_t{}; i < expected.size(); ++i)
            if (parts[i].start != expected[i].start || parts[i].size != expected[i].size
             || parts[i].transferred > expected[i].size)
                return faux;
        found = std::move(parts);
        return true;
    }
}
