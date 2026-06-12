// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/rate.hpp: byte-rate meter for one transfer run.
//
// The instantaneous speed must measure only the bytes moved by *this* run. A parallel transfer
// that resumes (e.g. after the user pauses and starts it again) carries over the bytes already on
// disk: the workers come back seeded with their resumed offsets, so `done` jumps from 0 to the
// resumed total on the first poll. Baselining the meter on that resumed amount (`base`) keeps the
// first sample from spiking to "resumed_total / min_span", and keeps the run average from counting
// the carried-over bytes as this run's work.
//
// The speed is a sliding-window rate: every poll pushes a (time, done) point, and the speed is the
// byte delta across the buffered window (~1 s). Unlike an EMA there is no exponential tail — the
// shown speed never reflects anything older than the window, catches a throughput change within
// one window, and reads 0 within one window of a stall.
//
// Self-contained (standard types only) so it can be unit-tested in isolation
// (test/parvion_rate_meter_test.cpp).

#include <chrono>
#include <cstdint>
#include <algorithm>
#include <deque>

namespace netxs::app::parvion
{
    struct rate_meter
    {
        using clock = std::chrono::steady_clock;

        struct point
        {
            clock::time_point time;
            std::int64_t      done;
        };

        double            speed = 0.0; // Windowed bytes/sec (0 until the window has data / while stalled).
        std::int64_t      base  = 0;   // `done` already on disk when this run began (resume baseline).
        std::deque<point> trail;       // (time, done) points covering the last `window` seconds.

        // Begin a run with `done_now` bytes already transferred (0 for a fresh transfer; the resumed
        // total for a parallel resume). All later samples and the average are measured from here.
        void start(std::int64_t done_now, clock::time_point now)
        {
            speed = 0.0;
            base  = done_now;
            trail.clear();
            trail.push_back({ now, done_now });
        }

        // Push the current cumulative `done_now` and recompute the rate over the trailing `window`
        // seconds. The front point is kept straddling the window edge so the measured span covers
        // the full window once enough history exists. No rate is reported until the span reaches
        // `min_span` (the first polls after start are too short to divide by). Returns whether the
        // speed was updated.
        auto sample(std::int64_t done_now, clock::time_point now, double window = 1.0, double min_span = 0.25) -> bool
        {
            trail.push_back({ now, done_now });
            while (trail.size() > 2 && std::chrono::duration<double>(now - trail[1].time).count() >= window) trail.pop_front();
            auto span = std::chrono::duration<double>(trail.back().time - trail.front().time).count();
            if (span < min_span) return false;
            speed = (double)(trail.back().done - trail.front().done) / span;
            return true;
        }

        // Average bytes/sec over the run: bytes moved this run (`done_now` - baseline) / elapsed.
        auto average(std::int64_t done_now, std::int64_t elapsed_secs) const -> double
        {
            return (double)std::max<std::int64_t>(0, done_now - base)
                 / (double)std::max<std::int64_t>(1, elapsed_secs);
        }
    };
}
