// Copyright (c) Shaobo Song
// Licensed under the MIT license.

// Unit tests for the Parvion byte-rate meter (parvion/rate.hpp). The meter must measure only the
// bytes moved by the current run: a parallel transfer that resumes after a pause comes back with the
// already-transferred bytes seeded into `done`, and counting those as this run's work spikes the
// instantaneous speed and inflates the average. These tests pin that resume-aware behavior plus the
// sliding-window properties: a throughput change is fully reflected within one window, a stall reads
// 0 within one window, and the sample buffer stays bounded over a long run.

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

    // No rate is reported before the window spans `min_span` (the first polls after start are too
    // short to divide by).
    auto test_below_min_span_skipped() -> bool
    {
        auto t0 = clk::time_point{};
        auto r  = rate_meter{};
        r.start(0, t0);
        auto took = r.sample(1 * MiB, t0 + std::chrono::milliseconds{ 100 });
        return !took && r.speed == 0.0;
    }

    // A throughput step is fully reflected one window later: 2 MiB/s for 2 s, then 10 MiB/s; after
    // 1 s at the new rate the window holds only new-rate bytes, so the old rate leaves no residue
    // (the EMA this replaced would still read ~7 MiB/s here).
    auto test_window_tracks_step_change() -> bool
    {
        auto t0   = clk::time_point{};
        auto r    = rate_meter{};
        r.start(0, t0);
        auto done = std::int64_t{};
        auto when = t0;
        auto step = std::chrono::milliseconds{ 250 };
        for (auto i = 0; i < 8; ++i) { done +=  2 * MiB / 4; when += step; r.sample(done, when); } // 2 s at 2 MiB/s
        for (auto i = 0; i < 4; ++i) { done += 10 * MiB / 4; when += step; r.sample(done, when); } // 1 s at 10 MiB/s
        return approx(r.speed, (double)(10 * MiB));
    }

    // A stall reads 0 within one window: once no bytes moved inside the window, the speed is 0,
    // not a decaying remnant of the old rate.
    auto test_stall_drops_to_zero() -> bool
    {
        auto t0   = clk::time_point{};
        auto r    = rate_meter{};
        r.start(0, t0);
        auto done = std::int64_t{};
        auto when = t0;
        auto step = std::chrono::milliseconds{ 250 };
        for (auto i = 0; i < 8; ++i) { done += 2 * MiB / 4; when += step; r.sample(done, when); } // 2 s at 2 MiB/s
        for (auto i = 0; i < 5; ++i) {                      when += step; r.sample(done, when); } // 1.25 s stalled
        return r.speed == 0.0;
    }

    // The sample buffer holds ~one window of points (window / poll period + the straddler), no
    // matter how long the transfer runs.
    auto test_buffer_stays_bounded() -> bool
    {
        auto t0   = clk::time_point{};
        auto r    = rate_meter{};
        r.start(0, t0);
        auto done = std::int64_t{};
        auto when = t0;
        for (auto i = 0; i < 1000; ++i) { done += MiB / 64; when += std::chrono::milliseconds{ 50 }; r.sample(done, when); }
        return r.trail.size() <= 24; // 1 s window / 50 ms polls = 20 points, plus edge straddlers
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
        { "below_min_span_skipped",   test_below_min_span_skipped },
        { "window_tracks_step",       test_window_tracks_step_change },
        { "stall_drops_to_zero",      test_stall_drops_to_zero },
        { "buffer_stays_bounded",     test_buffer_stays_bounded },
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
