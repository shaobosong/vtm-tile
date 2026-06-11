// Copyright (c) Shaobo Song
// Licensed under the MIT license.

// Unit tests for the Parvion byte-rate meter (parvion/rate.hpp). The meter must measure only the
// bytes moved by the current run: a parallel transfer that resumes after a pause comes back with the
// already-transferred bytes seeded into `done`, and counting those as this run's work spikes the
// instantaneous speed and inflates the average. These tests pin the resume-aware behavior.

#include "netxs/apps/parvion/rate.hpp"

#include <cstdio>
#include <cmath>
#include <cstdint>

using netxs::app::parvion::rate_meter;
using clk = std::chrono::steady_clock;

namespace
{
    constexpr auto MiB = std::int64_t{ 1 } << 20;

    auto approx(double a, double b, double tol = 0.5) -> bool { return std::fabs(a - b) < tol; }

    // Fresh run: 1 MiB transferred over 1 s -> ~1 MiB/s.
    auto test_fresh_rate() -> bool
    {
        auto t0 = clk::time_point{};
        auto r  = rate_meter{};
        r.start(0, t0);
        auto took = r.sample(1 * MiB, t0 + std::chrono::seconds{ 1 });
        return took && approx(r.speed, (double)(1 * MiB));
    }

    // Resume after pause: 12 MiB already on disk, +1 MiB in 0.5 s -> ~2 MiB/s, NOT ~26 MiB/s.
    // This is the regression: baselining on the resumed amount keeps the first sample honest.
    auto test_resume_does_not_spike() -> bool
    {
        auto t0 = clk::time_point{};
        auto r  = rate_meter{};
        r.start(12 * MiB, t0);
        auto took = r.sample(13 * MiB, t0 + std::chrono::milliseconds{ 500 });
        return took && approx(r.speed, (double)(2 * MiB)) && r.speed < (double)(3 * MiB);
    }

    // No sample is taken before the coarse interval elapses (poll-jitter guard).
    auto test_below_min_interval_skipped() -> bool
    {
        auto t0 = clk::time_point{};
        auto r  = rate_meter{};
        r.start(0, t0);
        auto took = r.sample(1 * MiB, t0 + std::chrono::milliseconds{ 100 });
        return !took && r.speed == 0.0;
    }

    // EMA smooths toward a steady rate across successive windows.
    auto test_ema_converges() -> bool
    {
        auto t0 = clk::time_point{};
        auto r  = rate_meter{};
        r.start(0, t0);
        auto done = std::int64_t{};
        auto when = t0;
        for (auto i = 0; i < 8; ++i) { done += 2 * MiB; when += std::chrono::seconds{ 1 }; r.sample(done, when); }
        return approx(r.speed, (double)(2 * MiB), (double)(MiB) / 8.0);
    }

    // Average is measured from the resume baseline: (32 - 12) MiB over 10 s = 2 MiB/s, NOT 3.2.
    auto test_average_session_relative() -> bool
    {
        auto t0 = clk::time_point{};
        auto r  = rate_meter{};
        r.start(12 * MiB, t0);
        return approx(r.average(32 * MiB, 10), (double)(2 * MiB));
    }
}

int main()
{
    struct { char const* name; bool (*fn)(); } tests[] =
    {
        { "fresh_rate",               test_fresh_rate },
        { "resume_does_not_spike",    test_resume_does_not_spike },
        { "below_min_interval_skip",  test_below_min_interval_skipped },
        { "ema_converges",            test_ema_converges },
        { "average_session_relative", test_average_session_relative },
    };
    auto failed = 0;
    for (auto& t : tests)
    {
        auto ok = t.fn();
        std::printf("%-26s %s\n", t.name, ok ? "PASS" : "FAIL");
        if (!ok) ++failed;
    }
    std::printf("%s\n", failed ? "FAILED" : "OK");
    return failed ? 1 : 0;
}
