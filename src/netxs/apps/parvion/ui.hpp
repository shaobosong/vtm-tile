// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/ui.hpp: Small, dependency-light UI primitives shared by the pane and
// reusable table widgets. Keeping these definitions outside panes.hpp lets the
// pane compose make_table() without creating an include cycle.

#include <algorithm>
#include <functional>

namespace netxs::app::parvion
{
    // Common retained-component handle used by every reusable Parvion widget.  The widget owns
    // rendering and input state; the optional lifecycle hooks let containers activate/deactivate
    // children without introducing a second page/widget hierarchy.
    struct component
    {
        ui::sptr widget;
        std::function<void()> activate;
        std::function<void()> deactivate;

        explicit operator bool() const { return !!widget; }
        void on_activate() const { if (activate) activate(); }
        void on_deactivate() const { if (deactivate) deactivate(); }
        void deface() const { if (widget) widget->base::deface(); }
    };

    // Shared palette (aligned with tile.hpp's command_bar tones).
    namespace theme
    {
        static constexpr auto bg           = 0xFF1E1E2Eu;
        static constexpr auto surface      = 0xFF313244u;
        static constexpr auto header       = 0xFF11111Bu;
        static constexpr auto text_fg      = 0xFFCDD6F4u;
        static constexpr auto subtext      = 0xFF6C7086u;
        static constexpr auto dir_fg       = 0xFF89B4FAu;
        static constexpr auto link_fg      = 0xFF94E2D5u;
        static constexpr auto sel_bg       = 0xFF45475Au;
        static constexpr auto sel_bg_act   = 0xFF89B4FAu;
        static constexpr auto sel_fg_act   = 0xFF1E1E2Eu;
        static constexpr auto title_fg     = 0xFFCDD6F4u;
        static constexpr auto title_fg_act = 0xFF89DCEBu;
        static constexpr auto sort_fg      = 0xFFFAB387u;
        static constexpr auto sb_track     = 0xFF2C3047u;
        static constexpr auto sb_thumb     = 0xFF3B4261u;
        static constexpr auto sb_hover     = 0xFF565F89u;
        static constexpr auto sb_drag      = 0xFF89B4FAu;
        static constexpr auto err_fg       = 0xFFF38BA8u;
        static constexpr auto trace_fg     = 0xFFCBA6F7u;
    }

    // Unicode helpers are grapheme-cluster and display-cell aware, so caret,
    // measurement, clipping, and painting agree for wide/combining glyphs.
    inline auto u8_step(view s, size_t i) -> size_t
    {
        auto c = (unsigned char)s[i];
        auto n = c < 0x80 ? 1u : (c & 0xE0) == 0xC0 ? 2u : (c & 0xF0) == 0xE0 ? 3u : 4u;
        return std::min((size_t)n, s.size() - i);
    }
    inline auto gc_cells(utf::frag const& frag) -> si32
    {
        auto m = utf::matrix::whxy(frag.attr.cmatrix);
        return std::max(1, (si32)m.w);
    }
    inline auto cell_width(view utf8) -> si32
    {
        auto cells = si32{};
        utf::decode_clusters(utf8, [&](view cl){ cells += gc_cells(utf::cluster(cl)); return true; });
        return cells;
    }
    inline auto cluster_count(view utf8) -> si32
    {
        auto n = si32{};
        utf::decode_clusters(utf8, [&](view){ ++n; return true; });
        return n;
    }
    inline auto cluster_to_byte(view s, si32 idx) -> size_t
    {
        auto i = size_t{};
        for (auto n = si32{}; n < idx && i < s.size(); ++n) i += utf::cluster(s.substr(i)).attr.utf8len;
        return std::min(i, s.size());
    }
    inline auto cell_to_cluster(view s, si32 col) -> si32
    {
        if (col <= 0) return 0;
        auto acc = si32{}, n = si32{}, stop = si32{};
        auto done = faux;
        utf::decode_clusters(s, [&](view cl)
        {
            auto cw = gc_cells(utf::cluster(cl));
            if (col < acc + cw) { stop = n + (col - acc >= (cw + 1) / 2 ? 1 : 0); done = true; return faux; }
            acc += cw;
            ++n;
            return true;
        });
        return done ? stop : n;
    }
    inline auto caret_cell(view s, si32 caret_idx) -> si32
    {
        return cell_width(s.substr(0, cluster_to_byte(s, caret_idx)));
    }
    inline auto byte_at_cell(view s, si32 col) -> size_t
    {
        return cluster_to_byte(s, cell_to_cluster(s, col));
    }
    inline auto put_str(auto& canvas, si32 x, si32 y, view utf8, ui32 fg, ui32 bg, si32 max_cells) -> si32
    {
        auto xi = si32{};
        utf::decode_clusters(utf8, [&](view cl) -> bool
        {
            auto frag = utf::cluster(cl);
            auto m = utf::matrix::whxy(frag.attr.cmatrix);
            auto cw = std::max(1, (si32)m.w);
            if (xi + cw > max_cells) return faux;
            if (cw == 1)
            {
                canvas.fill(rect{{ x + xi, y }, { 1, 1 }}, [&](cell& c){ c.bgc(bg).fgc(fg).txt(frag.text); });
            }
            else
            {
                canvas.fill(rect{{ x + xi,     y }, { 1, 1 }}, [&](cell& c){ c.bgc(bg).fgc(fg).txt(frag.text).wdt(m.w, m.h, 1, 1); });
                canvas.fill(rect{{ x + xi + 1, y }, { 1, 1 }}, [&](cell& c){ c.bgc(bg).fgc(fg).txt(frag.text).wdt(m.w, m.h, 2, 1); });
            }
            xi += cw;
            return true;
        });
        return xi;
    }
    inline auto fit_ellipsis(view utf8, si32 maxw) -> text
    {
        if (maxw <= 0) return {};
        if (cell_width(utf8) <= maxw) return text{ utf8 };
        if (maxw == 1) return text{ "\xE2\x80\xA6" };
        auto budget = maxw - 1;
        auto used = si32{};
        auto out = text{};
        utf::decode_clusters(utf8, [&](view cl) -> bool
        {
            auto cw = gc_cells(utf::cluster(cl));
            if (used + cw > budget) return faux;
            used += cw;
            out += text{ cl };
            return true;
        });
        out += "\xE2\x80\xA6";
        return out;
    }
}
