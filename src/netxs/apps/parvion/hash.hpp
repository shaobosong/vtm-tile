// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/hash.hpp: self-contained MD5 / SHA-1 / SHA-256 / SHA-512 digests behind a
// single streaming interface, used by the `parvionhash` backend (vtm-tile.cpp) to
// compute file/stream checksums. Public-domain-style reference implementations — kept
// here (rather than reusing the vendored PuTTY crypto, which is exposed via ssh_hash
// vtables + hardware-accel dispatch + PuTTY globals) so the multi-call binary needs no
// extra link wiring and the algorithms stay portable and unit-testable.
//
// Output hex is lowercase and matches the coreutils md5sum/sha1sum/sha256sum/sha512sum
// tools, so a Parvion digest can be compared directly against `sha256sum <file>`.

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>
#include <memory>

namespace netxs::app::parvion::hashing
{
    // Lowercase hex of n raw digest bytes.
    inline auto to_hex(uint8_t const* p, size_t n) -> std::string
    {
        static constexpr char d[] = "0123456789abcdef";
        auto s = std::string{};
        s.resize(n * 2);
        for (size_t i = 0; i < n; ++i) { s[2 * i] = d[p[i] >> 4]; s[2 * i + 1] = d[p[i] & 0xF]; }
        return s;
    }

    // Feed `len` bytes through `compress` (which consumes exactly BS bytes per call),
    // buffering any remainder in `buf` (carry across update() calls). The caller tracks
    // the running total length separately (needed for the final length padding).
    template<size_t BS, class Compress>
    inline void absorb(uint8_t* buf, size_t& buflen, uint8_t const* data, size_t len, Compress&& compress)
    {
        if (buflen)
        {
            auto need = BS - buflen;
            auto take = len < need ? len : need;
            std::memcpy(buf + buflen, data, take);
            buflen += take; data += take; len -= take;
            if (buflen == BS) { compress(buf); buflen = 0; }
        }
        while (len >= BS) { compress(data); data += BS; len -= BS; }
        if (len) { std::memcpy(buf + buflen, data, len); buflen += len; }
    }

    inline auto rotl32(uint32_t x, int c) -> uint32_t { return (x << c) | (x >> (32 - c)); }
    inline auto rotr32(uint32_t x, int c) -> uint32_t { return (x >> c) | (x << (32 - c)); }
    inline auto rotr64(uint64_t x, int c) -> uint64_t { return (x >> c) | (x << (64 - c)); }

    // --- MD5 (RFC 1321) -- little-endian -----------------------------------------------
    struct md5_algo
    {
        static constexpr size_t block_size = 64;
        static constexpr size_t digest_size = 16;
        uint32_t h[4]; uint64_t total; uint8_t buf[64]; size_t buflen;

        static void init(md5_algo& s)
        {
            s.h[0] = 0x67452301u; s.h[1] = 0xefcdab89u; s.h[2] = 0x98badcfeu; s.h[3] = 0x10325476u;
            s.total = 0; s.buflen = 0;
        }
        static void block(md5_algo& s, uint8_t const* p)
        {
            static constexpr uint32_t K[64] = {
                0xd76aa478u,0xe8c7b756u,0x242070dbu,0xc1bdceeeu, 0xf57c0fafu,0x4787c62au,0xa8304613u,0xfd469501u,
                0x698098d8u,0x8b44f7afu,0xffff5bb1u,0x895cd7beu, 0x6b901122u,0xfd987193u,0xa679438eu,0x49b40821u,
                0xf61e2562u,0xc040b340u,0x265e5a51u,0xe9b6c7aau, 0xd62f105du,0x02441453u,0xd8a1e681u,0xe7d3fbc8u,
                0x21e1cde6u,0xc33707d6u,0xf4d50d87u,0x455a14edu, 0xa9e3e905u,0xfcefa3f8u,0x676f02d9u,0x8d2a4c8au,
                0xfffa3942u,0x8771f681u,0x6d9d6122u,0xfde5380cu, 0xa4beea44u,0x4bdecfa9u,0xf6bb4b60u,0xbebfbc70u,
                0x289b7ec6u,0xeaa127fau,0xd4ef3085u,0x04881d05u, 0xd9d4d039u,0xe6db99e5u,0x1fa27cf8u,0xc4ac5665u,
                0xf4292244u,0x432aff97u,0xab9423a7u,0xfc93a039u, 0x655b59c3u,0x8f0ccc92u,0xffeff47du,0x85845dd1u,
                0x6fa87e4fu,0xfe2ce6e0u,0xa3014314u,0x4e0811a1u, 0xf7537e82u,0xbd3af235u,0x2ad7d2bbu,0xeb86d391u };
            static constexpr int S[64] = {
                7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
                5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
                4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
                6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21 };
            uint32_t M[16];
            for (int i = 0; i < 16; ++i)
                M[i] = (uint32_t)p[i*4] | ((uint32_t)p[i*4+1] << 8) | ((uint32_t)p[i*4+2] << 16) | ((uint32_t)p[i*4+3] << 24);
            auto A = s.h[0], B = s.h[1], C = s.h[2], D = s.h[3];
            for (int i = 0; i < 64; ++i)
            {
                uint32_t F; int g;
                if      (i < 16) { F = (B & C) | (~B & D);           g = i; }
                else if (i < 32) { F = (D & B) | (~D & C);           g = (5*i + 1) & 15; }
                else if (i < 48) { F = B ^ C ^ D;                    g = (3*i + 5) & 15; }
                else             { F = C ^ (B | ~D);                 g = (7*i)     & 15; }
                F = F + A + K[i] + M[g];
                A = D; D = C; C = B;
                B = B + rotl32(F, S[i]);
            }
            s.h[0] += A; s.h[1] += B; s.h[2] += C; s.h[3] += D;
        }
        static void update(md5_algo& s, uint8_t const* data, size_t len)
        {
            s.total += len;
            absorb<64>(s.buf, s.buflen, data, len, [&](uint8_t const* blk){ block(s, blk); });
        }
        static void final(md5_algo& s, uint8_t* out)
        {
            auto bits = s.total * 8;
            uint8_t pad = 0x80;
            update(s, &pad, 1);
            uint8_t zero = 0;
            while (s.buflen != 56) update(s, &zero, 1);
            uint8_t lenbuf[8];
            for (int i = 0; i < 8; ++i) lenbuf[i] = (uint8_t)(bits >> (8 * i)); // little-endian
            update(s, lenbuf, 8);
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j) out[i*4 + j] = (uint8_t)(s.h[i] >> (8 * j));
        }
    };

    // --- SHA-1 (RFC 3174) -- big-endian ------------------------------------------------
    struct sha1_algo
    {
        static constexpr size_t block_size = 64;
        static constexpr size_t digest_size = 20;
        uint32_t h[5]; uint64_t total; uint8_t buf[64]; size_t buflen;

        static void init(sha1_algo& s)
        {
            s.h[0]=0x67452301u; s.h[1]=0xEFCDAB89u; s.h[2]=0x98BADCFEu; s.h[3]=0x10325476u; s.h[4]=0xC3D2E1F0u;
            s.total = 0; s.buflen = 0;
        }
        static void block(sha1_algo& s, uint8_t const* p)
        {
            uint32_t w[80];
            for (int i = 0; i < 16; ++i)
                w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) | ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
            for (int i = 16; i < 80; ++i) w[i] = rotl32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
            auto a=s.h[0], b=s.h[1], c=s.h[2], d=s.h[3], e=s.h[4];
            for (int i = 0; i < 80; ++i)
            {
                uint32_t f, k;
                if      (i < 20) { f = (b & c) | (~b & d);           k = 0x5A827999u; }
                else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
                else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
                else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
                auto t = rotl32(a, 5) + f + e + k + w[i];
                e = d; d = c; c = rotl32(b, 30); b = a; a = t;
            }
            s.h[0]+=a; s.h[1]+=b; s.h[2]+=c; s.h[3]+=d; s.h[4]+=e;
        }
        static void update(sha1_algo& s, uint8_t const* data, size_t len)
        {
            s.total += len;
            absorb<64>(s.buf, s.buflen, data, len, [&](uint8_t const* blk){ block(s, blk); });
        }
        static void final(sha1_algo& s, uint8_t* out)
        {
            auto bits = s.total * 8;
            uint8_t pad = 0x80; update(s, &pad, 1);
            uint8_t zero = 0; while (s.buflen != 56) update(s, &zero, 1);
            uint8_t lenbuf[8];
            for (int i = 0; i < 8; ++i) lenbuf[i] = (uint8_t)(bits >> (8 * (7 - i))); // big-endian
            update(s, lenbuf, 8);
            for (int i = 0; i < 5; ++i)
                for (int j = 0; j < 4; ++j) out[i*4 + j] = (uint8_t)(s.h[i] >> (8 * (3 - j)));
        }
    };

    // --- SHA-256 (FIPS 180-4) -- big-endian --------------------------------------------
    struct sha256_algo
    {
        static constexpr size_t block_size = 64;
        static constexpr size_t digest_size = 32;
        uint32_t h[8]; uint64_t total; uint8_t buf[64]; size_t buflen;

        static void init(sha256_algo& s)
        {
            static constexpr uint32_t iv[8] = {
                0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u };
            for (int i = 0; i < 8; ++i) s.h[i] = iv[i];
            s.total = 0; s.buflen = 0;
        }
        static void block(sha256_algo& s, uint8_t const* p)
        {
            static constexpr uint32_t K[64] = {
                0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
                0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
                0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
                0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
                0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
                0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
                0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
                0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u };
            uint32_t w[64];
            for (int i = 0; i < 16; ++i)
                w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) | ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
            for (int i = 16; i < 64; ++i)
            {
                auto s0 = rotr32(w[i-15], 7) ^ rotr32(w[i-15], 18) ^ (w[i-15] >> 3);
                auto s1 = rotr32(w[i-2], 17) ^ rotr32(w[i-2], 19) ^ (w[i-2] >> 10);
                w[i] = w[i-16] + s0 + w[i-7] + s1;
            }
            auto a=s.h[0],b=s.h[1],c=s.h[2],d=s.h[3],e=s.h[4],f=s.h[5],g=s.h[6],hh=s.h[7];
            for (int i = 0; i < 64; ++i)
            {
                auto S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
                auto ch = (e & f) ^ (~e & g);
                auto t1 = hh + S1 + ch + K[i] + w[i];
                auto S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
                auto maj = (a & b) ^ (a & c) ^ (b & c);
                auto t2 = S0 + maj;
                hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
            }
            s.h[0]+=a; s.h[1]+=b; s.h[2]+=c; s.h[3]+=d; s.h[4]+=e; s.h[5]+=f; s.h[6]+=g; s.h[7]+=hh;
        }
        static void update(sha256_algo& s, uint8_t const* data, size_t len)
        {
            s.total += len;
            absorb<64>(s.buf, s.buflen, data, len, [&](uint8_t const* blk){ block(s, blk); });
        }
        static void final(sha256_algo& s, uint8_t* out)
        {
            auto bits = s.total * 8;
            uint8_t pad = 0x80; update(s, &pad, 1);
            uint8_t zero = 0; while (s.buflen != 56) update(s, &zero, 1);
            uint8_t lenbuf[8];
            for (int i = 0; i < 8; ++i) lenbuf[i] = (uint8_t)(bits >> (8 * (7 - i)));
            update(s, lenbuf, 8);
            for (int i = 0; i < 8; ++i)
                for (int j = 0; j < 4; ++j) out[i*4 + j] = (uint8_t)(s.h[i] >> (8 * (3 - j)));
        }
    };

    // --- SHA-512 (FIPS 180-4) -- big-endian, 64-bit words ------------------------------
    struct sha512_algo
    {
        static constexpr size_t block_size = 128;
        static constexpr size_t digest_size = 64;
        uint64_t h[8]; uint64_t total; uint8_t buf[128]; size_t buflen; // total < 2^64 bytes is plenty.

        static void init(sha512_algo& s)
        {
            static constexpr uint64_t iv[8] = {
                0x6a09e667f3bcc908ull,0xbb67ae8584caa73bull,0x3c6ef372fe94f82bull,0xa54ff53a5f1d36f1ull,
                0x510e527fade682d1ull,0x9b05688c2b3e6c1full,0x1f83d9abfb41bd6bull,0x5be0cd19137e2179ull };
            for (int i = 0; i < 8; ++i) s.h[i] = iv[i];
            s.total = 0; s.buflen = 0;
        }
        static void block(sha512_algo& s, uint8_t const* p)
        {
            static constexpr uint64_t K[80] = {
                0x428a2f98d728ae22ull,0x7137449123ef65cdull,0xb5c0fbcfec4d3b2full,0xe9b5dba58189dbbcull,
                0x3956c25bf348b538ull,0x59f111f1b605d019ull,0x923f82a4af194f9bull,0xab1c5ed5da6d8118ull,
                0xd807aa98a3030242ull,0x12835b0145706fbeull,0x243185be4ee4b28cull,0x550c7dc3d5ffb4e2ull,
                0x72be5d74f27b896full,0x80deb1fe3b1696b1ull,0x9bdc06a725c71235ull,0xc19bf174cf692694ull,
                0xe49b69c19ef14ad2ull,0xefbe4786384f25e3ull,0x0fc19dc68b8cd5b5ull,0x240ca1cc77ac9c65ull,
                0x2de92c6f592b0275ull,0x4a7484aa6ea6e483ull,0x5cb0a9dcbd41fbd4ull,0x76f988da831153b5ull,
                0x983e5152ee66dfabull,0xa831c66d2db43210ull,0xb00327c898fb213full,0xbf597fc7beef0ee4ull,
                0xc6e00bf33da88fc2ull,0xd5a79147930aa725ull,0x06ca6351e003826full,0x142929670a0e6e70ull,
                0x27b70a8546d22ffcull,0x2e1b21385c26c926ull,0x4d2c6dfc5ac42aedull,0x53380d139d95b3dfull,
                0x650a73548baf63deull,0x766a0abb3c77b2a8ull,0x81c2c92e47edaee6ull,0x92722c851482353bull,
                0xa2bfe8a14cf10364ull,0xa81a664bbc423001ull,0xc24b8b70d0f89791ull,0xc76c51a30654be30ull,
                0xd192e819d6ef5218ull,0xd69906245565a910ull,0xf40e35855771202aull,0x106aa07032bbd1b8ull,
                0x19a4c116b8d2d0c8ull,0x1e376c085141ab53ull,0x2748774cdf8eeb99ull,0x34b0bcb5e19b48a8ull,
                0x391c0cb3c5c95a63ull,0x4ed8aa4ae3418acbull,0x5b9cca4f7763e373ull,0x682e6ff3d6b2b8a3ull,
                0x748f82ee5defb2fcull,0x78a5636f43172f60ull,0x84c87814a1f0ab72ull,0x8cc702081a6439ecull,
                0x90befffa23631e28ull,0xa4506cebde82bde9ull,0xbef9a3f7b2c67915ull,0xc67178f2e372532bull,
                0xca273eceea26619cull,0xd186b8c721c0c207ull,0xeada7dd6cde0eb1eull,0xf57d4f7fee6ed178ull,
                0x06f067aa72176fbaull,0x0a637dc5a2c898a6ull,0x113f9804bef90daeull,0x1b710b35131c471bull,
                0x28db77f523047d84ull,0x32caab7b40c72493ull,0x3c9ebe0a15c9bebcull,0x431d67c49c100d4cull,
                0x4cc5d4becb3e42b6ull,0x597f299cfc657e2aull,0x5fcb6fab3ad6faecull,0x6c44198c4a475817ull };
            uint64_t w[80];
            for (int i = 0; i < 16; ++i)
            {
                w[i] = 0;
                for (int j = 0; j < 8; ++j) w[i] = (w[i] << 8) | (uint64_t)p[i*8 + j];
            }
            for (int i = 16; i < 80; ++i)
            {
                auto s0 = rotr64(w[i-15], 1) ^ rotr64(w[i-15], 8) ^ (w[i-15] >> 7);
                auto s1 = rotr64(w[i-2], 19) ^ rotr64(w[i-2], 61) ^ (w[i-2] >> 6);
                w[i] = w[i-16] + s0 + w[i-7] + s1;
            }
            auto a=s.h[0],b=s.h[1],c=s.h[2],d=s.h[3],e=s.h[4],f=s.h[5],g=s.h[6],hh=s.h[7];
            for (int i = 0; i < 80; ++i)
            {
                auto S1 = rotr64(e, 14) ^ rotr64(e, 18) ^ rotr64(e, 41);
                auto ch = (e & f) ^ (~e & g);
                auto t1 = hh + S1 + ch + K[i] + w[i];
                auto S0 = rotr64(a, 28) ^ rotr64(a, 34) ^ rotr64(a, 39);
                auto maj = (a & b) ^ (a & c) ^ (b & c);
                auto t2 = S0 + maj;
                hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
            }
            s.h[0]+=a; s.h[1]+=b; s.h[2]+=c; s.h[3]+=d; s.h[4]+=e; s.h[5]+=f; s.h[6]+=g; s.h[7]+=hh;
        }
        static void update(sha512_algo& s, uint8_t const* data, size_t len)
        {
            s.total += len;
            absorb<128>(s.buf, s.buflen, data, len, [&](uint8_t const* blk){ block(s, blk); });
        }
        static void final(sha512_algo& s, uint8_t* out)
        {
            auto bits = s.total * 8; // high 64 bits of the 128-bit length are 0 for our sizes.
            uint8_t pad = 0x80; update(s, &pad, 1);
            uint8_t zero = 0; while (s.buflen != 112) update(s, &zero, 1);
            uint8_t lenbuf[16] = {};
            for (int i = 0; i < 8; ++i) lenbuf[8 + i] = (uint8_t)(bits >> (8 * (7 - i))); // low 64 bits, big-endian
            update(s, lenbuf, 16);
            for (int i = 0; i < 8; ++i)
                for (int j = 0; j < 8; ++j) out[i*8 + j] = (uint8_t)(s.h[i] >> (8 * (7 - j)));
        }
    };

    // Streaming hash interface: update() with arbitrary chunks, then hex() to finalize.
    struct hasher
    {
        virtual void update(void const* data, size_t len) = 0;
        virtual auto hex() -> std::string = 0; // finalize -> lowercase hex digest
        virtual ~hasher() = default;
    };

    template<class Algo>
    struct hasher_impl : hasher
    {
        Algo st;
        hasher_impl() { Algo::init(st); }
        void update(void const* data, size_t len) override { Algo::update(st, (uint8_t const*)data, len); }
        auto hex() -> std::string override
        {
            uint8_t out[Algo::digest_size];
            Algo::final(st, out);
            return to_hex(out, Algo::digest_size);
        }
    };

    // Construct a streaming hasher for "md5" | "sha1" | "sha256" | "sha512"; nullptr if unknown.
    inline auto make_hasher(std::string_view algo) -> std::unique_ptr<hasher>
    {
        if (algo == "md5")    return std::make_unique<hasher_impl<md5_algo>>();
        if (algo == "sha1")   return std::make_unique<hasher_impl<sha1_algo>>();
        if (algo == "sha256") return std::make_unique<hasher_impl<sha256_algo>>();
        if (algo == "sha512") return std::make_unique<hasher_impl<sha512_algo>>();
        return nullptr;
    }
}
