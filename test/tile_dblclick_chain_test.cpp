// Copyright (c) Shaobo Song
// Licensed under the MIT license.
//
// Unit test for the rapid double double-click bug on an in-pane applet's menu bar
// (the menu bar of term / parvion / etc. running inside a tile pane).
//
// Bug: double-clicking the applet's menu bar maximizes the pane; a second
// double-click restores it. The applet runs as a DirectVT child: it classifies the
// mouse clicks on its own gear and forwards the unhandled menu-bar double-click to
// the host, which performs the maximize/restore toggle. But two double-clicks at
// the *same* cell within the double-click timeout were folded into one continuous
// multi-click run on the applet's gear (single -> double -> triple -> quad), and a
// double-click is only forwarded when the running click-count is exactly 2. So the
// second double-click was classified as a triple/quad-click and never forwarded ->
// the host never restored -> the pane stayed maximized.
//
// Fix: when the applet's gate forwards a double-click outside (console.hpp, the
// direct-mode mouse forwarder), it now calls gear.break_click_chain() -- the same
// primitive the window-control buttons already use -- which resets the click run so
// the next pair of clicks forms a fresh double-click.
//
// This drives the real input::mouse click-counter (input.hpp) directly and asserts
// the run-count sequence with and without that reset.

#include "netxs/desktopio/input.hpp"

#include <vector>
#include <iostream>

using namespace netxs;

namespace
{
    using namespace netxs::input;

    // Minimal concrete mouse exposing the real click-counter (input::mouse is
    // abstract: it needs fire()/fire_fast()).
    struct test_mouse : public input::mouse
    {
        // hids::fire() resets nodbl at the start of every dispatched event; mimic
        // that here so the click-counter sees the same preconditions it does live.
        void fire(hint /*cause*/) override { mouse::nodbl = faux; }
        bool fire_fast() override { return faux; }

        // Exact replica of hids::break_click_chain() — it only touches mouse
        // members (nodbl + the per-button stamp), so reproducing it here exercises
        // the same state the real call mutates.
        void break_click_chain()
        {
            mouse::nodbl = true;
            if (auto button = mouse::bttn_id)
            {
                if (auto iter = stamp.find(button); iter != stamp.end())
                {
                    iter->second = {};
                }
            }
        }
    };

    constexpr auto left_button = si32{ 0b00001 };

    // Simulate one full left click (press + release) at cell xy at time t, the way
    // mouse::update() does on a button-down then button-up transition.
    void do_click(test_mouse& m, twod xy, netxs::time t)
    {
        m.bttn_id       = left_button;
        m.coord         = xy;
        m.dragged       = faux;
        m.m_sys.timecod = t;
        m.m2_push();   // MouseDown + m2_pressed(): advances the multi-click run.
        m.m2_click();  // MouseClick + single/double/multi decision: sets mouse::clicked.
    }

    // Four rapid clicks at the same cell, all inside the double-click timeout.
    // `reset_after_second` mimics the fix: break the click chain right after the
    // first double-click completes (as the menu-bar handler now does). Returns the
    // post-click run-count after each of the four clicks.
    std::vector<si32> rapid_four(bool reset_after_second)
    {
        using namespace std::chrono_literals;
        auto m = test_mouse{};
        m.delay = 500ms;                 // matches /config/timings/dblclick default.
        auto base = datetime::now();     // real (large) timestamps: a reset stamp's
        auto cell = twod{ 20, 20 };      // epoch fired-time then reads as "expired".
        auto seq  = std::vector<si32>{};

        do_click(m, cell, base);          seq.push_back(m.clicked);
        do_click(m, cell, base + 100ms);  seq.push_back(m.clicked); // first double-click
        if (reset_after_second) m.break_click_chain();
        do_click(m, cell, base + 200ms);  seq.push_back(m.clicked);
        do_click(m, cell, base + 300ms);  seq.push_back(m.clicked);
        return seq;
    }

    auto count_doubles(std::vector<si32> const& seq)
    {
        auto n = 0;
        for (auto c : seq) if (c == 2) n++;
        return n;
    }

    void show(char const* tag, std::vector<si32> const& seq)
    {
        std::cout << tag << " click-run counts:";
        for (auto c : seq) std::cout << ' ' << c;
        std::cout << "  (doubles=" << count_doubles(seq) << ")\n";
    }

    // Without the reset, the rapid second double-click is swallowed by the running
    // multi-click chain: counts run 1,2,3,4 and only one of them is a double-click.
    auto verify_without_reset_only_one_double() -> bool
    {
        auto seq = rapid_four(/*reset_after_second*/ faux);
        show("no-reset (bug)  ", seq);
        return seq == std::vector<si32>{ 1, 2, 3, 4 } && count_doubles(seq) == 1;
    }

    // With the reset (the fix), the chain restarts after the first double-click, so
    // the next pair is a fresh double-click: counts run 1,2,1,2 -> two double-clicks
    // -> maximize then restore.
    auto verify_break_click_chain_yields_second_double() -> bool
    {
        auto seq = rapid_four(/*reset_after_second*/ true);
        show("break_click_chain", seq);
        return seq == std::vector<si32>{ 1, 2, 1, 2 } && count_doubles(seq) == 2;
    }
}

auto main() -> int
{
    if (!verify_without_reset_only_one_double())          return 1;
    if (!verify_break_click_chain_yields_second_double()) return 2;
    std::cout << "PASS: break_click_chain restores the rapid second double-click\n";
    return 0;
}
