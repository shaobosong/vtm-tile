// Copyright (c) Shaobo Song
// Licensed under the MIT license.

// Boundary tests for saved-site Host syntax validation. Validation is deliberately syntactic:
// it must reject malformed input immediately without performing DNS or depending on the network.

#include "netxs/apps/parvion/settings.hpp"

#include <cstdio>
#include <string>
#include <vector>

using netxs::app::parvion::valid_site_host;

int main()
{
    auto label63 = std::string(63, 'a');
    auto label64 = std::string(64, 'a');
    auto max_dns = std::string(63, 'a') + "."
                 + std::string(63, 'b') + "."
                 + std::string(63, 'c') + "."
                 + std::string(61, 'd'); // 253 bytes, the DNS presentation limit without root dot.

    auto valid = std::vector<std::string>{
        "a.b",
        "example.com",
        "EXAMPLE.COM.",
        "a-b.example",
        "123.example",
        label63 + ".example",
        max_dns,
        max_dns + ".", // 254 bytes including the final DNS root dot.
        "0.0.0.0",
        "1.2.3.4",
        "255.255.255.255",
        "127.1",
        "127.0.1",
        "255.16777215", // a.b: 8-bit prefix + 24-bit tail.
        "255.255.65535", // a.b.c: two 8-bit prefixes + 16-bit tail.
        "::",
        "::1",
        "2001:db8::1",
        "1:2:3:4:5:6:7:8",
        "::ffff:192.0.2.128",
        "::ffff:127.1",
        "1:2:3:4:5:6:192.0.2.128",
        "fe80::1%eth0",
    };
    auto invalid = std::vector<std::string>{
        "",
        "abc",
        "123",
        "abc.",
        ".example",
        "example..com",
        "-bad.example",
        "bad-.example",
        "bad_example.com",
        "bad host.example",
        "https://example.com",
        "user@example.com",
        label64 + ".example",
        max_dns + "a", // 254 bytes without a final root dot.
        max_dns + ".x",
        "1.2.3.4.5",
        "1..2.3",
        "256.1.1.1",
        "256.1",
        "1.16777216",
        "1.2.65536",
        "192.168.001.1",
        "1.2.3.4.",
        ":",
        ":::",
        "2001:::1",
        "1:2:3:4:5:6:7",
        "1:2:3:4:5:6:7:8:9",
        "1:2:3:4:5:6:7::8",
        "12345::1",
        "2001::db8::1",
        "2001:db8::g",
        "::ffff:999.1.1.1",
        "::ffff:192.168.001.1",
        "::192.0.2.1:1",
        "fe80::1%",
        "fe80::1%eth0%2",
        "fe80::1%bad/zone",
        "[::1]",
    };

    auto failed = 0;
    for (auto const& host : valid)
    {
        auto ok = valid_site_host(host);
        std::printf("valid   %-48s %s\n", host.c_str(), ok ? "PASS" : "FAIL");
        if (!ok) ++failed;
    }
    for (auto const& host : invalid)
    {
        auto ok = !valid_site_host(host);
        std::printf("invalid %-48s %s\n", host.c_str(), ok ? "PASS" : "FAIL");
        if (!ok) ++failed;
    }
    std::printf("%s\n", failed ? "FAILED" : "OK");
    return failed ? 1 : 0;
}
