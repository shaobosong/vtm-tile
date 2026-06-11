// Copyright (c) Shaobo Song
// Licensed under the MIT license.

// Unit tests for the Parvion "Pin to Top" reorder (parvion/reorder.hpp). The regression these
// pin down: pinning a selection that matches no *pending* item (e.g. a row that has just begun
// transferring) must be a no-op that leaves every element — and its contents — untouched. The old
// implementation moved everything into scratch vectors first and only then bailed, wiping the
// source rows (their Local/Remote Name strings went empty).

#include "netxs/apps/parvion/reorder.hpp"

#include <cstdio>
#include <string>
#include <vector>

using netxs::app::parvion::reorder_pin_to_front;

namespace
{
    // Stand-in for a queue row: a name (the content that was being lost) + selection / pending flags.
    struct row { std::string name; bool sel; bool queued; };

    auto take    = [](row const& r){ return r.sel; };
    auto movable = [](row const& r){ return r.queued; };

    auto names(std::vector<row> const& v) -> std::string
    {
        auto s = std::string{};
        for (auto& r : v) { s += r.name; s += ','; }
        return s;
    }

    // Pinning a selected NON-pending row (just started transferring) must not disturb the queue.
    auto test_noop_preserves_content() -> bool
    {
        auto v = std::vector<row>{ { "A", true, false }, { "B", false, true }, { "C", false, true } };
        auto changed = true;
        auto act = reorder_pin_to_front(v, 0, take, movable, changed);
        return !changed && act == 0 && names(v) == "A,B,C," && v[0].name == "A" && v[1].name == "B" && v[2].name == "C";
    }

    // A purely-empty match (nothing selected) is likewise a no-op.
    auto test_no_selection_noop() -> bool
    {
        auto v = std::vector<row>{ { "X", false, true }, { "Y", false, true } };
        auto changed = true;
        reorder_pin_to_front(v, -1, take, movable, changed);
        return !changed && names(v) == "X,Y,";
    }

    // A pinned queued row moves to the front of the pending group, ahead of an ongoing transfer.
    auto test_pin_moves_to_front_of_pending() -> bool
    {
        // [A transferring(active), B queued, C queued(selected)] -> C jumps ahead of B.
        auto v = std::vector<row>{ { "A", false, false }, { "B", false, true }, { "C", true, true } };
        auto changed = false;
        auto act = reorder_pin_to_front(v, 0, take, movable, changed);
        return changed && names(v) == "A,C,B," && act == 0; // active A still first, content intact.
    }

    // Newest pin lands ahead of an earlier pin (insert before the first still-movable element).
    auto test_newest_pin_first() -> bool
    {
        // [P (already pinned, queued), Q queued(selected)] -> Q before P.
        auto v = std::vector<row>{ { "P", false, true }, { "Q", true, true } };
        auto changed = false;
        reorder_pin_to_front(v, -1, take, movable, changed);
        return changed && names(v) == "Q,P,";
    }

    // Pinning ahead of an existing queued row shifts the active (transferring) row down; its index
    // is remapped through the +pinned branch.
    auto test_pin_remaps_active() -> bool
    {
        // [Q1 queued, A transferring(active=1), Q2 queued(sel)] -> Q2 to the front (before Q1):
        // [Q2,Q1,A]; active A is remapped from index 1 to 2.
        auto v = std::vector<row>{ { "Q1", false, true }, { "A", false, false }, { "Q2", true, true } };
        auto changed = false;
        auto act = reorder_pin_to_front(v, 1, take, movable, changed);
        return changed && names(v) == "Q2,Q1,A," && act == 2;
    }
}

int main()
{
    struct { char const* name; bool (*fn)(); } tests[] =
    {
        { "noop_preserves_content",     test_noop_preserves_content },
        { "no_selection_noop",          test_no_selection_noop },
        { "pin_moves_to_front",         test_pin_moves_to_front_of_pending },
        { "newest_pin_first",           test_newest_pin_first },
        { "pin_remaps_active",          test_pin_remaps_active },
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
