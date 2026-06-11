// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/rate.hpp: byte-rate meter for one transfer run.
//
// The instantaneous speed must measure only the bytes moved by *this* run. A parallel transfer
// that resumes (e.g. after the user pauses and starts it again) carries over the bytes already on
// disk: the workers come back seeded with their resumed offsets, so `done` jumps from 0 to the
// resumed total on the first poll. Baselining the meter on that resumed amount (`base`) keeps the
// first sample from spiking to "resumed_total / 0.5 s", and keeps the run average from counting
// the carried-over bytes as this run's work.
//
// Self-contained (standard types only) so it can be unit-tested in isolation
// (test/parvion_rate_meter_test.cpp).

#include <chrono>
#include <cstdint>
#include <algorithm>

namespace netxs::app::parvion
{
    struct rate_meter
    {
        using clock = std::chrono::steady_clock;

        double            speed = 0.0; // Smoothed bytes/sec (0 until the first sample / while idle).
        std::int64_t      base  = 0;   // `done` already on disk when this run began (resume baseline).
        std::int64_t      mark  = 0;   // `done` captured at the last sample.
        clock::time_point tick{};      // Time of the last sample.

        // Begin a run with `done_now` bytes already transferred (0 for a fresh transfer; the resumed
        // total for a parallel resume). All later samples and the average are measured from here.
        void start(std::int64_t done_now, clock::time_point now)
        {
            speed = 0.0;
            base  = mark = done_now;
            tick  = now;
        }

        // Fold the current cumulative `done_now` into the EMA, but only once at least `min_dt`
        // seconds have passed since the last sample (a coarse window smooths the 50 ms poll jitter).
        // Returns whether a sample was actually taken.
        auto sample(std::int64_t done_now, clock::time_point now, double min_dt = 0.5) -> bool
        {
            auto dt = std::chrono::duration<double>(now - tick).count();
            if (dt < min_dt) return false;
            auto inst  = (double)(done_now - mark) / dt; // bytes/sec over this window
            auto alpha = 0.4;                            // EMA smoothing factor
            speed = speed <= 0.0 ? inst : alpha * inst + (1.0 - alpha) * speed;
            mark  = done_now;
            tick  = now;
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
