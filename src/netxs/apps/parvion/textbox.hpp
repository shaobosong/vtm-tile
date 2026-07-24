// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/textbox.hpp: the reusable read-only TEXT-VIEW core component.
//
// A scrolling, colour-coded, mouse-selectable (char / word / line) text view — a read-only text box
// (no editing). It owns ALL of its behavior: vertical/horizontal scrolling, tail-follow, text
// selection + clipboard copy, a right-click menu, and focus, in its OWN textbox_state — it shares
// nothing with the table core. A caller supplies a `textbox_cfg` (coloured line data + optional
// stable line identity for selection survival + menu/keys) and calls make_textbox(). The message-log
// tab is just one such config; the component knows nothing about logs.
//
// make_textbox() returns the common retained component handle. Containers can wrap it as needed.

#include "panes.hpp" // ui::sptr, theme, shared menu utilities.

namespace netxs::app::parvion
{
    // One coloured run within a line.
    struct textseg { text s; ui32 fg = 0; };

    // The complete text-view configuration. `line_count` / `line` are re-queried from live state.
    // `line_id` gives each line a stable opaque identity so a selection survives appends / scrolling
    // (null => selection anchors to indices only). `epoch` changes when the whole layout shifts (e.g.
    // a timestamp toggle), dropping the selection.
    struct textbox_cfg
    {
        std::function<si32()>                             line_count;
        std::function<std::vector<textseg>(si32)>         line;       // coloured segments of line i.
        std::function<const void*(si32)>                  line_id;    // stable identity of line i (null => index).
        std::function<ui64()>                             epoch;      // layout epoch; a change drops the selection.
        std::function<text()>                             empty_text; // shown when line_count()==0 (null => none).
        std::function<std::vector<app::shared::menu::item>(netxs::wptr<ui::base>,
                                                           app::shared::menu::item,
                                                           app::shared::menu::item)> menu; // App arranges core-supplied Copy/Select all rows.
        std::function<bool(hids&, netxs::wptr<ui::base>)> on_key;     // app keys (Delete clear-finished); null => none.
    };

    // The text view's mutable state. Body paints from row 0 down; `bottom` is the exclusive body end.
    struct textbox_state
    {
        enum selmode { sel_none, sel_char, sel_word, sel_line };
        enum dmode   { d_none, d_vsb, d_hsb };

        bool focused = faux;
        si32 scroll = 0, hscroll = 0;
        bool follow = true;
        bool sb_hover = faux,  sb_press = faux,  sb_drag = faux;  si32 sb_grab = 0;
        bool hsb_hover = faux, hsb_press = faux, hsb_drag = faux; si32 hsb_grab = 0;
        bool menu_hover = faux, menu_press = faux;
        si32 body_top = 0, body_rows = 0, bottom = 0;
        si32 content_w = 0, disp_w = 0, vsb_x = 0, hsb_y = 0, total = 0;
        bool has_vsb = faux, has_hsb = faux;
        dmode drag = d_none;

        selmode      selm = sel_none;
        bool         sel = faux, dragging = faux;
        si32         anchor_ln = 0, anchor_cl = 0, head_ln = 0, head_cl = 0;
        const void*  anchor_id = nullptr;
        const void*  head_id   = nullptr;
        si32         base_lo_ln = 0, base_lo_cl = 0, base_hi_ln = 0, base_hi_cl = 0;
        si32         drag_x = 0, drag_y = 0;
        ui64         seen_epoch = 0;
    };

    // A position within the selection: line index + grapheme-cluster index.
    struct tb_pos { si32 ln = 0; si32 cl = 0; };
    inline auto operator < (tb_pos a, tb_pos b) -> bool { return a.ln < b.ln || (a.ln == b.ln && a.cl < b.cl); }

    // The full rendered text of line i (segments concatenated) — used for selection columns + copy.
    inline auto tb_line_text(textbox_cfg const& cfg, si32 i) -> text
    {
        auto s = text{};
        if (i < 0 || (cfg.line_count && i >= cfg.line_count())) return s;
        for (auto& seg : cfg.line(i)) s += seg.s;
        return s;
    }
    // Character class for word selection: 0 space, 1 word (alnum / '_' / UTF-8 lead), 2 punctuation.
    inline auto tb_cclass(view cl) -> int
    {
        if (cl.empty()) return 0;
        auto c = (unsigned char)cl.front();
        if (c >= 0x80) return 1;
        if (c == ' ' || c == '\t') return 0;
        auto word = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        return word ? 1 : 2;
    }
    inline auto tb_word_bounds(view s, si32 idx) -> std::pair<si32, si32>
    {
        auto cls = std::vector<int>{};
        utf::decode_clusters(s, [&](view cl){ cls.push_back(tb_cclass(cl)); return true; });
        auto n = (si32)cls.size();
        if (n == 0) return { 0, 0 };
        if (idx >= n) return { n, n };
        auto k  = std::clamp(idx, 0, n - 1);
        auto cc = cls[(size_t)k];
        auto lo = k, hi = k + 1;
        while (lo > 0 && cls[(size_t)(lo - 1)] == cc) --lo;
        while (hi < n && cls[(size_t)hi]       == cc) ++hi;
        return { lo, hi };
    }
    inline auto tb_hit(textbox_state const& st, textbox_cfg const& cfg, si32 mx, si32 my) -> tb_pos
    {
        auto n = cfg.line_count ? cfg.line_count() : 0;
        auto last = n - 1;
        if (last < 0) return { 0, 0 };
        auto vrow = my - st.body_top;
        auto ln   = vrow < 0 ? st.scroll : st.scroll + std::clamp(vrow, 0, std::max(0, st.body_rows - 1));
        ln = std::clamp(ln, 0, last);
        auto s   = tb_line_text(cfg, ln);
        auto col = mx - 1 + st.hscroll; // Content-cell column from the line start (margin is 1).
        return { ln, cell_to_cluster(s, std::max(0, col)) };
    }
    inline auto tb_word_span(textbox_cfg const& cfg, tb_pos p) -> std::pair<tb_pos, tb_pos>
    {
        auto [lo, hi] = tb_word_bounds(tb_line_text(cfg, p.ln), p.cl);
        return { tb_pos{ p.ln, lo }, tb_pos{ p.ln, hi } };
    }
    inline auto tb_line_span(textbox_cfg const& cfg, tb_pos p) -> std::pair<tb_pos, tb_pos>
    {
        return { tb_pos{ p.ln, 0 }, tb_pos{ p.ln, cluster_count(tb_line_text(cfg, p.ln)) } };
    }
    inline auto tb_sel_bounds(textbox_state const& st, si32 n) -> std::pair<tb_pos, tb_pos>
    {
        auto a = tb_pos{ st.anchor_ln, st.anchor_cl }, h = tb_pos{ st.head_ln, st.head_cl };
        auto lo = a < h ? a : h, hi = a < h ? h : a;
        auto last = std::max(0, n - 1);
        lo.ln = std::clamp(lo.ln, 0, last); hi.ln = std::clamp(hi.ln, 0, last);
        return { lo, hi };
    }
    inline auto tb_has_selection(textbox_state const& st) -> bool
    {
        return st.sel && (st.anchor_ln != st.head_ln || st.anchor_cl != st.head_cl);
    }
    inline auto tb_selection_text(textbox_state const& st, textbox_cfg const& cfg) -> text
    {
        auto n = cfg.line_count ? cfg.line_count() : 0;
        if (n == 0) return {};
        auto [lo, hi] = tb_sel_bounds(st, n);
        auto out = text{};
        for (auto i = lo.ln; i <= hi.ln; ++i)
        {
            auto s = tb_line_text(cfg, i);
            auto a = i == lo.ln ? cluster_to_byte(s, lo.cl) : size_t{ 0 };
            auto b = i == hi.ln ? cluster_to_byte(s, hi.cl) : s.size();
            if (b > a) out += s.substr(a, b - a);
            if (i < hi.ln) out += '\n';
        }
        return out;
    }
    inline void tb_sel_clear(textbox_state& st)
    {
        st.sel = faux; st.dragging = faux; st.selm = textbox_state::sel_none;
        st.anchor_ln = st.anchor_cl = st.head_ln = st.head_cl = 0;
        st.anchor_id = st.head_id = nullptr;
    }
    inline auto tb_select_all(textbox_state& st, textbox_cfg const& cfg) -> bool
    {
        auto n = cfg.line_count ? cfg.line_count() : 0;
        if (n <= 0) return faux;
        auto last = n - 1;
        st.anchor_ln = 0;    st.anchor_cl = 0;
        st.head_ln   = last; st.head_cl   = cluster_count(tb_line_text(cfg, last));
        st.anchor_id = cfg.line_id ? cfg.line_id(0)    : nullptr;
        st.head_id   = cfg.line_id ? cfg.line_id(last) : nullptr;
        st.selm = textbox_state::sel_char; st.sel = true; st.dragging = faux;
        st.follow = faux; st.drag = textbox_state::d_none;
        st.seen_epoch = cfg.epoch ? cfg.epoch() : 0;
        return tb_has_selection(st);
    }
    inline auto tb_index_of(textbox_cfg const& cfg, si32 n, const void* id) -> si32
    {
        if (!id || !cfg.line_id) return -1;
        for (auto i = si32{}; i < n; ++i) if (cfg.line_id(i) == id) return i;
        return -1;
    }
    // Re-resolve the selection's line indices from its anchored ids against the fresh line list.
    inline auto tb_reanchor(textbox_state& st, textbox_cfg const& cfg, si32 n) -> bool
    {
        if (!st.sel) return true;
        auto epoch = cfg.epoch ? cfg.epoch() : 0;
        if (epoch != st.seen_epoch) return faux; // Layout shift invalidates the columns.
        if (!cfg.line_id) return true;            // Index-only anchoring: nothing to re-resolve.
        auto a = tb_index_of(cfg, n, st.anchor_id);
        auto h = tb_index_of(cfg, n, st.head_id);
        if (a < 0 || h < 0) return faux;
        st.anchor_ln = a; st.head_ln = h;
        return true;
    }

    // ---- Scroll layer ------------------------------------------------------------------------------
    inline void tb_layout(textbox_state& st, si32 w, si32 h, si32 total, si32 content_w)
    {
        st.total = total; st.bottom = h; st.content_w = content_w;
        auto avail = std::max(0, st.bottom - st.body_top);
        for (auto pass = si32{}; pass < 2; ++pass)
        {
            st.has_vsb   = avail > 0 && total > avail;
            st.disp_w    = w - (st.has_vsb ? 1 : 0);
            st.has_hsb   = st.content_w > st.disp_w;
            st.body_rows = std::max(0, avail - (st.has_hsb ? 1 : 0));
            st.has_vsb   = st.body_rows > 0 && total > st.body_rows;
            st.disp_w    = w - (st.has_vsb ? 1 : 0);
            st.has_hsb   = st.content_w > st.disp_w;
            st.body_rows = std::max(0, avail - (st.has_hsb ? 1 : 0));
        }
        st.vsb_x = w - 1; st.hsb_y = st.body_top + st.body_rows;
    }
    struct tb_sb { bool ok = faux; si32 x = 0, top = 0, track_h = 0, thumb_y = 0, thumb_h = 0, maxscroll = 0; };
    inline auto tb_vsb(textbox_state const& st) -> tb_sb
    {
        auto sb = tb_sb{};
        sb.ok = st.has_vsb && st.body_rows > 0 && st.total > st.body_rows;
        if (!sb.ok) return sb;
        sb.x = st.vsb_x; sb.top = st.body_top; sb.track_h = st.body_rows;
        sb.thumb_h = std::max(1, st.body_rows * st.body_rows / st.total);
        sb.maxscroll = st.total - st.body_rows;
        sb.thumb_y = sb.top + ((st.body_rows - sb.thumb_h) * st.scroll + sb.maxscroll / 2) / sb.maxscroll;
        return sb;
    }
    inline auto tb_hsb(textbox_state const& st) -> tb_sb
    {
        auto sb = tb_sb{};
        sb.ok = st.has_hsb && st.disp_w > 0 && st.content_w > st.disp_w;
        if (!sb.ok) return sb;
        sb.x = 0; sb.top = st.hsb_y; sb.track_h = st.disp_w;
        sb.thumb_h = std::max(1, st.disp_w * st.disp_w / st.content_w);
        sb.maxscroll = st.content_w - st.disp_w;
        sb.thumb_y = ((st.disp_w - sb.thumb_h) * st.hscroll + sb.maxscroll / 2) / sb.maxscroll;
        return sb;
    }
    inline void tb_vsb_to(textbox_state& st, si32 y, tb_sb const& sb) { auto t = sb.track_h - sb.thumb_h; if (t <= 0) return; st.scroll  = std::clamp(((y - st.sb_grab  - sb.top) * sb.maxscroll + t / 2) / t, 0, sb.maxscroll); }
    inline void tb_hsb_to(textbox_state& st, si32 x, tb_sb const& sb) { auto t = sb.track_h - sb.thumb_h; if (t <= 0) return; st.hscroll = std::clamp(((x - st.hsb_grab - sb.x)   * sb.maxscroll + t / 2) / t, 0, sb.maxscroll); }
    inline void tb_paint_scrollbars(textbox_state const& st, auto& canvas)
    {
        if (auto sb = tb_vsb(st); sb.ok)
        {
            auto mark = (st.sb_drag || st.sb_hover) ? "\xe2\x96\x88" : "\xe2\x96\x90";
            canvas.fill(rect{{ sb.x, sb.top }, { 1, sb.track_h }}, [&](cell& c){ c.bgc(theme::bg).fgc(theme::sb_track).txt(mark); });
            auto tc = st.sb_drag ? ui32{ theme::sb_drag } : st.sb_hover ? ui32{ theme::sb_hover } : ui32{ theme::sb_thumb };
            canvas.fill(rect{{ sb.x, sb.thumb_y }, { 1, sb.thumb_h }}, [&](cell& c){ c.bgc(theme::bg).fgc(tc).txt(mark); });
            if (st.sb_press || st.sb_drag)
                canvas.fill(rect{{ sb.x, sb.thumb_y }, { 1, sb.thumb_h }}, [](cell& c){ c.xlight(2); });
        }
        if (auto sb = tb_hsb(st); sb.ok)
        {
            auto mark = (st.hsb_drag || st.hsb_hover) ? "\xe2\x96\x84" : "\xe2\x96\x82";
            canvas.fill(rect{{ sb.x, sb.top }, { sb.track_h, 1 }}, [&](cell& c){ c.bgc(theme::bg).fgc(theme::sb_track).txt(mark); });
            auto tc = st.hsb_drag ? ui32{ theme::sb_drag } : st.hsb_hover ? ui32{ theme::sb_hover } : ui32{ theme::sb_thumb };
            canvas.fill(rect{{ sb.x + sb.thumb_y, sb.top }, { sb.thumb_h, 1 }}, [&](cell& c){ c.bgc(theme::bg).fgc(tc).txt(mark); });
            if (st.hsb_press || st.hsb_drag)
                canvas.fill(rect{{ sb.x + sb.thumb_y, sb.top }, { sb.thumb_h, 1 }}, [](cell& c){ c.fgc().xlight(2); });
        }
    }
    inline auto tb_over_scrollbar(textbox_state const& st, si32 mx, si32 my) -> bool
    {
        if (auto sb = tb_vsb(st); sb.ok && mx == sb.x && my >= sb.top && my < sb.top + sb.track_h) return true;
        if (auto sb = tb_hsb(st); sb.ok && my == sb.top && mx >= sb.x && mx < sb.x + sb.track_h) return true;
        return faux;
    }
    inline auto tb_menu_hit(textbox_state const& st, si32 mx, si32 my) -> bool
    {
        return st.disp_w > 0 && mx == 0 && my == st.body_top;
    }
    inline auto tb_drag_step(si32 distance) -> si32
    {
        return std::clamp(distance, si32{ 1 }, si32{ 8 });
    }
    inline auto tb_drag_head(textbox_state& st, textbox_cfg const& cfg, si32 mx, si32 my) -> bool
    {
        auto n = cfg.line_count ? cfg.line_count() : 0;
        if (!st.dragging || n == 0) return faux;
        auto p = tb_hit(st, cfg, mx, my);
        if (st.selm == textbox_state::sel_char)
        {
            st.head_ln = p.ln; st.head_cl = p.cl; st.head_id = cfg.line_id ? cfg.line_id(p.ln) : nullptr;
        }
        else
        {
            auto [elo, ehi] = st.selm == textbox_state::sel_word ? tb_word_span(cfg, p) : tb_line_span(cfg, p);
            auto blo = tb_pos{ st.base_lo_ln, st.base_lo_cl }, bhi = tb_pos{ st.base_hi_ln, st.base_hi_cl };
            auto lo = elo < blo ? elo : blo, hi = bhi < ehi ? ehi : bhi;
            st.anchor_ln = lo.ln; st.anchor_cl = lo.cl; st.head_ln = hi.ln; st.head_cl = hi.cl;
            st.anchor_id = cfg.line_id ? cfg.line_id(lo.ln) : nullptr;
            st.head_id   = cfg.line_id ? cfg.line_id(hi.ln) : nullptr;
        }
        return true;
    }
    inline auto tb_drag_autoscroll(textbox_state& st, textbox_cfg const& cfg) -> bool
    {
        if (!st.dragging) return faux;
        auto changed = faux;
        if (st.body_rows > 0)
        {
            auto maxv = std::max(0, st.total - st.body_rows);
            auto next = st.scroll;
            if      (st.drag_y <  st.body_top)                next -= tb_drag_step(st.body_top - st.drag_y);
            else if (st.drag_y >= st.body_top + st.body_rows) next += tb_drag_step(st.drag_y - (st.body_top + st.body_rows - 1));
            next = std::clamp(next, 0, maxv);
            if (next != st.scroll) { st.scroll = next; st.follow = faux; changed = true; }
        }
        if (st.disp_w > 0)
        {
            auto maxh = std::max(0, st.content_w - st.disp_w);
            auto next = st.hscroll;
            if      (st.drag_x <  1)         next -= tb_drag_step(1 - st.drag_x);
            else if (st.drag_x >= st.disp_w) next += tb_drag_step(st.drag_x - st.disp_w + 1);
            next = std::clamp(next, 0, maxh);
            if (next != st.hscroll) { st.hscroll = next; changed = true; }
        }
        if (changed) tb_drag_head(st, cfg, st.drag_x, st.drag_y);
        return changed;
    }

    // Build the same selection-aware menu for either a body right-click or the fixed menu button.
    inline auto tb_context_menu(textbox_state& st, textbox_cfg& cfg, netxs::wptr<ui::base> panel_wp)
        -> std::vector<app::shared::menu::item>
    {
        namespace m = app::shared::menu;
        auto has  = tb_has_selection(st);
        auto out  = has ? tb_selection_text(st, cfg) : text{};
        auto copy = m::item{ .alive = true, .label = "&Copy", .disabled = !has };
        copy.action = [out](hids& g){ if (!out.empty()) g.set_clipboard(dot_00, out, mime::textonly); };

        auto n = cfg.line_count ? cfg.line_count() : 0;
        auto select_all = m::item{ .alive = true, .label = "Select &All", .disabled = n == 0 };
        select_all.action = [stp = &st, cfgp = &cfg, panel_wp](hids&)
        {
            if (tb_select_all(*stp, *cfgp))
                if (auto panel = panel_wp.lock()) panel->base::deface();
        };

        if (cfg.menu) return cfg.menu(panel_wp, std::move(copy), std::move(select_all));
        auto items = std::vector<m::item>{};
        items.push_back(std::move(copy));
        items.push_back(m::item{ .alive = true, .type = m::kind::separator });
        items.push_back(std::move(select_all));
        return items;
    }
    inline void tb_open_context_menu(auto& boss, textbox_state& st, textbox_cfg& cfg, twod at)
    {
        app::shared::menu::open_dropdown_popup(
            boss, tb_context_menu(st, cfg, ptr::shadow(boss.This())),
            { .source = app::shared::menu::popup_source::context_menu, .cursor = at });
    }
    inline void tb_paint_menu_button(textbox_state const& st, auto& canvas)
    {
        if (st.disp_w <= 0) return;
        auto box = rect{{ 0, st.body_top }, { 1, 1 }};
        put_str(canvas, 0, st.body_top, "\xE2\x89\xA1", theme::subtext, theme::bg, 1); // ≡
        if      (st.menu_press) canvas.fill(box, [](cell& c){ c.xlight(2); });
        else if (st.menu_hover) canvas.fill(box, [](cell& c){ c.xlight(); });
    }

    // ---- Render ------------------------------------------------------------------------------------
    inline void textbox_render(textbox_state& st, textbox_cfg const& cfg, auto& canvas, twod size)
    {
        auto w = size.x, h = size.y;
        if (w <= 0 || h <= 0) return;
        canvas.fill(rect{{ 0, 0 }, { w, h }}, [&](cell& c){ c.bgc(theme::bg).fgc(theme::text_fg); });

        auto n = cfg.line_count ? cfg.line_count() : 0;
        if (st.sel && !tb_reanchor(st, cfg, n)) tb_sel_clear(st);
        st.seen_epoch = cfg.epoch ? cfg.epoch() : 0;

        auto content_w = si32{};
        for (auto i = si32{}; i < n; ++i) content_w = std::max(content_w, (si32)cell_width(tb_line_text(cfg, i)));
        if (content_w) content_w += 1; // Leading margin (lines render from x = 1).

        tb_layout(st, w, h, n, content_w);
        auto maxscroll = std::max(0, st.total - st.body_rows);
        if (st.follow) st.scroll = maxscroll;
        st.scroll  = std::clamp(st.scroll, 0, maxscroll);
        st.hscroll = std::clamp(st.hscroll, 0, std::max(0, st.content_w - st.disp_w));

        if (n == 0)
        {
            if (cfg.empty_text) put_str(canvas, 1, st.body_top, cfg.empty_text(), theme::subtext, theme::bg, std::max(1, st.disp_w - 1));
        }
        else if (st.body_rows > 0)
        {
            auto hs = st.hscroll, clipw = st.disp_w;
            auto seg = [&](si32 content_x, si32 y, view s, ui32 fg) -> si32
            {
                auto cw = cell_width(s), sx = content_x - hs, room = std::min((si32)cw, clipw - sx);
                if (room > 0 && sx < clipw) put_str(canvas, sx, y, s, fg, theme::bg, room);
                return (si32)cw;
            };
            auto first = st.scroll, last = std::min(n, st.scroll + st.body_rows);
            for (auto i = first; i < last; ++i)
            {
                auto y = st.body_top + (i - first), x = si32{ 1 };
                for (auto& sg : cfg.line(i)) x += seg(x, y, sg.s, sg.fg);
            }
            if (st.sel)
            {
                auto [lo, hi] = tb_sel_bounds(st, n);
                for (auto i = std::max(first, lo.ln); i <= std::min(last - 1, hi.ln); ++i)
                {
                    auto s  = tb_line_text(cfg, i);
                    auto nc = cluster_count(s);
                    auto c0 = std::clamp(i == lo.ln ? lo.cl : 0,  0, nc);
                    auto c1 = std::clamp(i == hi.ln ? hi.cl : nc, 0, nc);
                    auto px0 = caret_cell(s, c0) - hs + 1, px1 = caret_cell(s, c1) - hs + 1;
                    if (lo.ln != hi.ln && i < hi.ln) px1 += 1; // Extend past EOL to signal the newline.
                    auto x0 = std::clamp(px0, 0, clipw), x1 = std::clamp(px1, 0, clipw);
                    auto y  = st.body_top + (i - first);
                    if (x1 > x0) canvas.fill(rect{{ x0, y }, { x1 - x0, 1 }}, [&](cell& c){ c.bgc(theme::sel_bg); });
                }
            }
        }
        tb_paint_scrollbars(st, canvas);
        // Paint last so the fixed button stays above selection and horizontally-scrolled text.
        tb_paint_menu_button(st, canvas);
    }

    // ---- Widget ------------------------------------------------------------------------------------
    inline auto make_textbox(textbox_cfg cfg) -> component
    {
        auto state_ref = std::make_shared<textbox_state*>(nullptr);
        auto form = ui::mock::ctor()->active()
            ->plugin<pro::mouse>()->plugin<pro::focus>(pro::focus::mode::focusable)->plugin<pro::keybd>()->plugin<pro::timer>();
        form->invoke([&, cfgv = std::move(cfg), state_ref](auto& boss)
        {
            auto& st  = boss.base::field(textbox_state{});
            auto& cfg = boss.base::field(textbox_cfg{ cfgv });
            *state_ref = &st;
            boss.LISTEN(tier::release, e2::render::any, parent_canvas) { textbox_render(st, cfg, parent_canvas, boss.base::size()); };
            boss.LISTEN(tier::release, e2::form::state::focus::count, count) { st.focused = !!count; boss.base::deface(); };
            auto arm_autoscroll = [&boss, &st, &cfg]
            {
                auto& timer = boss.base::template plugin<pro::timer>();
                timer.pacify();
                if (!tb_drag_autoscroll(st, cfg)) return;
                boss.base::deface();
                timer.actify(ui::skin::globals().repeat_rate, [&boss, &st, &cfg](auto) -> bool
                {
                    if (!tb_drag_autoscroll(st, cfg)) return faux;
                    boss.base::deface();
                    return true;
                });
            };

            boss.on(tier::mouserelease, input::key::LeftDown, [&](hids& gear)
            {
                pro::focus::set(boss.This(), gear.id, solo::on);
                auto mx = (si32)gear.coord.x, my = (si32)gear.coord.y;
                if (tb_menu_hit(st, mx, my))
                {
                    if (!st.menu_press) { st.menu_press = true; boss.base::deface(); }
                    gear.dismiss();
                    return;
                }
                if (auto sb = tb_vsb(st); sb.ok && mx == sb.x && my >= sb.top && my < sb.top + sb.track_h)
                {
                    if (!st.sb_press || st.hsb_press)
                    {
                        st.sb_press = true;
                        st.hsb_press = faux;
                        boss.base::deface();
                    }
                    return;
                }
                if (auto sb = tb_hsb(st); sb.ok && my == sb.top && mx >= sb.x && mx < sb.x + sb.track_h)
                {
                    if (!st.hsb_press || st.sb_press)
                    {
                        st.hsb_press = true;
                        st.sb_press = faux;
                        boss.base::deface();
                    }
                    return;
                }
                boss.base::deface(); gear.dismiss();
            });
            boss.on(tier::mouserelease, input::key::LeftUp, [&](hids&)
            {
                if (st.menu_press || st.sb_press || st.hsb_press)
                {
                    st.menu_press = st.sb_press = st.hsb_press = faux;
                    boss.base::deface();
                }
            });
            boss.on(tier::mouserelease, input::key::LeftClick, [&](hids& gear)
            {
                pro::focus::set(boss.This(), gear.id, solo::on);
                auto mx = (si32)gear.coord.x, my = (si32)gear.coord.y;
                if (tb_menu_hit(st, mx, my))
                {
                    tb_open_context_menu(boss, st, cfg, twod{ mx, my + 2 });
                    st.menu_press = faux;
                    boss.base::deface();
                    gear.dismiss();
                    return;
                }
                if (auto sb = tb_vsb(st); sb.ok && mx == sb.x && my >= sb.top && my < sb.top + sb.track_h)
                {
                    auto page = std::max(1, st.body_rows);
                    if (my < sb.thumb_y)                    st.scroll = std::clamp(st.scroll - page, 0, sb.maxscroll);
                    else if (my >= sb.thumb_y + sb.thumb_h) st.scroll = std::clamp(st.scroll + page, 0, sb.maxscroll);
                    st.follow = st.scroll == sb.maxscroll; boss.base::deface(); gear.dismiss(); return;
                }
                if (auto sb = tb_hsb(st); sb.ok && my == sb.top && mx >= sb.x && mx < sb.x + sb.track_h)
                {
                    auto page = std::max(1, st.disp_w); auto tx = sb.x + sb.thumb_y;
                    if (mx < tx)                    st.hscroll = std::clamp(st.hscroll - page, 0, sb.maxscroll);
                    else if (mx >= tx + sb.thumb_h) st.hscroll = std::clamp(st.hscroll + page, 0, sb.maxscroll);
                    boss.base::deface(); gear.dismiss(); return;
                }
                if (tb_has_selection(st) && !(gear.ctlstat & hids::anyShift)) { tb_sel_clear(st); boss.base::deface(); gear.dismiss(); return; }
                gear.dismiss();
            });
            boss.on(tier::mouserelease, input::key::RightClick, [&](hids& gear)
            {
                pro::focus::set(boss.This(), gear.id, solo::on);
                tb_open_context_menu(boss, st, cfg,
                                     twod{ (si32)gear.coord.x, (si32)gear.coord.y });
                gear.dismiss();
            });
            boss.on(tier::mouserelease, input::key::MouseMove, [&](hids& gear)
            {
                auto mx = (si32)gear.coord.x, my = (si32)gear.coord.y;
                auto nmenu = tb_menu_hit(st, mx, my);
                if (st.menu_hover != nmenu) { st.menu_hover = nmenu; boss.base::deface(); }
                if (st.menu_press && !nmenu) { st.menu_press = faux; boss.base::deface(); }
                auto vsb = tb_vsb(st); auto nsb = !nmenu && vsb.ok && mx == vsb.x && my >= vsb.top && my < vsb.top + vsb.track_h;
                if (st.sb_hover != nsb) { st.sb_hover = nsb; boss.base::deface(); }
                if (st.sb_press && !nsb) { st.sb_press = faux; boss.base::deface(); }
                auto hsb = tb_hsb(st); auto nhsb = !nmenu && hsb.ok && my == hsb.top && mx >= hsb.x && mx < hsb.x + hsb.track_h;
                if (st.hsb_hover != nhsb) { st.hsb_hover = nhsb; boss.base::deface(); }
                if (st.hsb_press && !nhsb) { st.hsb_press = faux; boss.base::deface(); }
            });
            boss.on(tier::mouserelease, input::key::MouseLeave, [&](hids&)
            {
                if (st.sb_hover)  { st.sb_hover = faux;  boss.base::deface(); }
                if (st.hsb_hover) { st.hsb_hover = faux; boss.base::deface(); }
                if (st.sb_press)  { st.sb_press = faux;  boss.base::deface(); }
                if (st.hsb_press) { st.hsb_press = faux; boss.base::deface(); }
                if (st.menu_hover || st.menu_press)
                {
                    st.menu_hover = st.menu_press = faux;
                    boss.base::deface();
                }
            });
            boss.on(tier::mouserelease, input::key::MouseWheel, [&](hids& gear)
            {
                if (gear.hzwhl || st.hsb_hover) { auto maxh = std::max(0, st.content_w - st.disp_w); st.hscroll = std::clamp(st.hscroll - gear.whlsi * 4, 0, maxh); }
                else { auto maxv = std::max(0, st.total - st.body_rows); st.scroll = std::clamp(st.scroll - gear.whlsi, 0, maxv); st.follow = st.scroll == maxv; }
                boss.base::deface();
                gear.dismiss();
            });
            boss.base::signal(tier::release, e2::form::draggable::_<hids::buttons::left>, true);
            boss.LISTEN(tier::release, e2::form::drag::start::_<hids::buttons::left>, gear)
            {
                auto px = (si32)gear.click.x, py = (si32)gear.click.y;
                if (tb_menu_hit(st, px, py)) return;
                if (auto sb = tb_vsb(st); sb.ok && px == sb.x && py >= sb.top && py < sb.top + sb.track_h)
                {
                    pro::focus::set(boss.This(), gear.id, solo::on);
                    if (py >= sb.thumb_y && py < sb.thumb_y + sb.thumb_h) st.sb_grab = py - sb.thumb_y;
                    else { st.sb_grab = sb.thumb_h / 2; tb_vsb_to(st, py, sb); }
                    st.sb_press = faux;
                    st.sb_drag = st.sb_hover = true; st.drag = textbox_state::d_vsb;
                    st.follow = st.scroll == std::max(0, st.total - st.body_rows); boss.base::deface(); return;
                }
                if (auto sb = tb_hsb(st); sb.ok && py == sb.top && px >= sb.x && px < sb.x + sb.track_h)
                {
                    pro::focus::set(boss.This(), gear.id, solo::on);
                    auto tx = sb.x + sb.thumb_y;
                    if (px >= tx && px < tx + sb.thumb_h) st.hsb_grab = px - tx;
                    else { st.hsb_grab = sb.thumb_h / 2; tb_hsb_to(st, px, sb); }
                    st.hsb_press = faux;
                    st.hsb_drag = st.hsb_hover = true; st.drag = textbox_state::d_hsb; boss.base::deface(); return;
                }
                // Begin a character text selection (word/line modes are armed by double/triple-press).
                if (st.dragging && st.selm != textbox_state::sel_char) return;
                auto n = cfg.line_count ? cfg.line_count() : 0;
                if (n == 0) return;
                pro::focus::set(boss.This(), gear.id, solo::on);
                auto p = tb_hit(st, cfg, px, py);
                st.drag_x = px; st.drag_y = py;
                st.anchor_ln = st.head_ln = p.ln; st.anchor_cl = st.head_cl = p.cl;
                st.anchor_id = st.head_id = cfg.line_id ? cfg.line_id(p.ln) : nullptr;
                st.selm = textbox_state::sel_char; st.sel = true; st.dragging = true;
                st.follow = faux; st.drag = textbox_state::d_none;
                st.seen_epoch = cfg.epoch ? cfg.epoch() : 0;
                boss.base::deface();
            };
            boss.LISTEN(tier::release, e2::form::drag::pull::_<hids::buttons::left>, gear, -, (arm_autoscroll))
            {
                auto mx = (si32)gear.coord.x, my = (si32)gear.coord.y;
                if (st.dragging)
                {
                    st.drag_x = mx; st.drag_y = my;
                    if (!tb_drag_head(st, cfg, mx, my)) return;
                    arm_autoscroll();
                    boss.base::deface();
                    return;
                }
                if      (st.drag == textbox_state::d_vsb) { tb_vsb_to(st, my, tb_vsb(st)); st.follow = st.scroll == std::max(0, st.total - st.body_rows); boss.base::deface(); }
                else if (st.drag == textbox_state::d_hsb) { tb_hsb_to(st, mx, tb_hsb(st)); boss.base::deface(); }
            };
            // A drag ends: keep any text selection, else drop the scrollbar-drag flags (inlined into
            // both events — the LISTEN macro captures by reference, so a shared local would dangle).
            boss.LISTEN(tier::release, e2::form::drag::stop::_<hids::buttons::left>, gear)
            { if (st.dragging) { boss.base::template plugin<pro::timer>().pacify(); st.dragging = faux; boss.base::deface(); return; } auto was = st.drag; st.drag = textbox_state::d_none; st.sb_press = st.hsb_press = st.sb_drag = st.hsb_drag = faux; if (was != textbox_state::d_none) boss.base::deface(); };
            boss.LISTEN(tier::release, e2::form::drag::cancel::_<hids::buttons::left>, gear)
            { if (st.dragging) { boss.base::template plugin<pro::timer>().pacify(); st.dragging = faux; boss.base::deface(); return; } auto was = st.drag; st.drag = textbox_state::d_none; st.sb_press = st.hsb_press = st.sb_drag = st.hsb_drag = faux; if (was != textbox_state::d_none) boss.base::deface(); };

            auto span_select = [&](hids& gear, textbox_state::selmode mode, bool dragging)
            {
                auto mx = (si32)gear.coord.x, my = (si32)gear.coord.y;
                if (tb_menu_hit(st, mx, my)) return;
                if (tb_over_scrollbar(st, mx, my)) return;
                auto n = cfg.line_count ? cfg.line_count() : 0;
                if (n == 0) return;
                pro::focus::set(boss.This(), gear.id, solo::on);
                auto p = tb_hit(st, cfg, mx, my);
                auto [lo, hi] = mode == textbox_state::sel_word ? tb_word_span(cfg, p) : tb_line_span(cfg, p);
                st.drag_x = mx; st.drag_y = my;
                st.base_lo_ln = lo.ln; st.base_lo_cl = lo.cl; st.base_hi_ln = hi.ln; st.base_hi_cl = hi.cl;
                st.anchor_ln = lo.ln; st.anchor_cl = lo.cl; st.head_ln = hi.ln; st.head_cl = hi.cl;
                st.anchor_id = cfg.line_id ? cfg.line_id(lo.ln) : nullptr;
                st.head_id   = cfg.line_id ? cfg.line_id(hi.ln) : nullptr;
                st.selm = mode; st.sel = true; st.dragging = dragging; st.follow = faux; st.drag = textbox_state::d_none;
                st.seen_epoch = cfg.epoch ? cfg.epoch() : 0;
                gear.dismiss(); boss.base::deface();
            };
            boss.on(tier::mouserelease, input::key::LeftDoubleClick, [&, span_select](hids& gear) { span_select(gear, textbox_state::sel_word, faux); });
            boss.on(tier::mouserelease, input::key::LeftDoublePress, [&, span_select](hids& gear) { span_select(gear, textbox_state::sel_word, true); });
            boss.on(tier::mouserelease, input::key::LeftMultiPress, [&, span_select](hids& gear) { if (gear.clicked == 3) span_select(gear, textbox_state::sel_line, true); });
            boss.on(tier::mouserelease, input::key::LeftMultiClick, [&, span_select](hids& gear) { if (gear.clicked == 3) span_select(gear, textbox_state::sel_line, faux); });

            boss.LISTEN(tier::preview, input::events::keybd::any, gear)
            {
                if (!st.focused) return;
                if (gear.payload != input::keybd::type::keypress) return;
                if (gear.keystat == input::key::released || gear.keystat == input::key::interrupted) return;
                if (gear.keybd::handled) return;
                if (cfg.on_key) cfg.on_key(gear, ptr::shadow(boss.This()));
            };
        });
        auto deactivate = [state_ref, weak = ptr::shadow(form)]
        {
            if (*state_ref) tb_sel_clear(**state_ref);
            if (auto widget = weak.lock()) widget->base::deface();
        };
        return { std::move(form), {}, std::move(deactivate) };
    }
}
