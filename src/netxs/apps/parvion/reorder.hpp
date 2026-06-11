// Copyright (c) Shaobo Song
// Licensed under the MIT license.

// parvion/reorder.hpp: the "Pin to Top" queue reorder, factored out as a generic, side-effect-safe
// algorithm so it can be unit-tested in isolation (test/parvion_pin_top_test.cpp).
//
// The earlier in-place version moved every element into scratch vectors first and only then checked
// whether anything matched — so a call that pinned nothing (e.g. an item that had just begun
// transferring, hence no longer "movable") returned after the moves, leaving the source vector full
// of moved-from elements and silently wiping their contents. This version checks for a match BEFORE
// touching the vector, so a no-op pin never disturbs it.
//
// Self-contained (standard types only).

#include <vector>
#include <utility>

namespace netxs::app::parvion
{
    // Move every element with take(e) && movable(e) to the front of the "movable group" — i.e. just
    // before the first movable element that wasn't taken — preserving relative order (a newer pin
    // thus lands ahead of an earlier one). The element at index `active` is tracked by identity; its
    // new index is returned (or -1 if there was none). `changed` reports whether the vector was
    // reordered. When nothing is taken+movable the vector is left completely untouched.
    template<class T, class Take, class Movable>
    auto reorder_pin_to_front(std::vector<T>& v, int active, Take take, Movable movable, bool& changed) -> int
    {
        changed = false;
        auto any = false;
        for (auto& e : v) if (take(e) && movable(e)) { any = true; break; }
        if (!any) return active; // Nothing to pin: do NOT mutate the vector.
        changed = true;

        auto pinned = std::vector<T>{};
        auto rest   = std::vector<T>{};
        auto act    = (active >= 0 && active < (int)v.size()) ? &v[(size_t)active] : nullptr;
        auto act_in_rest = -1;
        for (auto& e : v)
        {
            if (take(e) && movable(e)) pinned.push_back(std::move(e));
            else { if (&e == act) act_in_rest = (int)rest.size(); rest.push_back(std::move(e)); }
        }
        auto pos = (int)rest.size(); // Insertion point: before the first still-movable element.
        for (auto i = 0; i < (int)rest.size(); ++i) if (movable(rest[(size_t)i])) { pos = i; break; }

        v.clear();
        v.reserve(rest.size() + pinned.size());
        for (auto i = 0; i < pos; ++i)                v.push_back(std::move(rest[(size_t)i]));
        for (auto& p : pinned)                        v.push_back(std::move(p));
        for (auto i = pos; i < (int)rest.size(); ++i) v.push_back(std::move(rest[(size_t)i]));

        return act_in_rest < 0 ? -1 : (act_in_rest < pos ? act_in_rest : act_in_rest + (int)pinned.size());
    }
}
