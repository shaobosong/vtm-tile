// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/panes.hpp: Interactive file-browser pane, rendered in the command_bar
// style (direct canvas paint + manual keyboard/mouse handling, per tile.hpp).
// One pane drives both the Local site (std::filesystem) and the Remote site
// (SFTP session, wired in later phases) via a pluggable directory lister.
//
// Pane logic lives in free functions operating on pane_state& so it is safe to
// call from deferred event handlers (which capture only the stable `st`/`boss`
// references via the LISTEN/on macros' `[&]` capture).

#include "session.hpp" // brings model.hpp + proto.hpp + sftp_remote

#include <functional>
#include <set>

namespace netxs::app::parvion
{
    // Shared palette (aligned with tile.hpp's command_bar tones).
    namespace theme
    {
        static constexpr auto bg          = 0xFF1E1E2Eu; // Pane background.
        static constexpr auto surface     = 0xFF313244u; // Column-header strip.
        static constexpr auto header      = 0xFF11111Bu; // Title strip.
        static constexpr auto text_fg     = 0xFFCDD6F4u; // File names.
        static constexpr auto subtext     = 0xFF6C7086u; // Size/time/secondary.
        static constexpr auto dir_fg      = 0xFF89B4FAu; // Directory names.
        static constexpr auto link_fg     = 0xFF94E2D5u; // Symlinks.
        static constexpr auto sel_bg      = 0xFF45475Au; // Selection (unfocused).
        static constexpr auto sel_bg_act  = 0xFF89B4FAu; // Selection (focused pane).
        static constexpr auto sel_fg_act  = 0xFF1E1E2Eu; // Selection text (focused).
        static constexpr auto title_fg    = 0xFFCDD6F4u;
        static constexpr auto title_fg_act = 0xFF89DCEBu; // Focused pane title.
        static constexpr auto sb_track    = 0xFF2C3047u; // Scrollbar track.
        static constexpr auto sb_thumb    = 0xFF3B4261u; // Scrollbar thumb.
        static constexpr auto sb_hover    = 0xFF565F89u; // Scrollbar thumb (hover).
        static constexpr auto sb_drag     = 0xFF89B4FAu; // Scrollbar thumb (dragging).
        static constexpr auto err_fg      = 0xFFF38BA8u;
        static constexpr auto trace_fg    = 0xFFCBA6F7u; // Message-log Trace/debug lines (mauve).
    }

    // A directory lister: fill `out` with the entries of `path`; on failure set
    // `err` and return faux. Local panes use this with read_local_dir; remote
    // panes plug the SFTP session in later.
    using lister_t = std::function<bool(text const& path, std::vector<direntry>& out, text& err)>;

    // The built-in local lister (std::filesystem).
    inline auto local_lister() -> lister_t
    {
        return [](text const& path, std::vector<direntry>& out, text& err) -> bool
        {
        #if defined(_WIN32)
            if (path.empty()) { out = read_local_drives(); return true; } // drive list
        #endif
            auto ec = std::error_code{};
            auto p = fs::path{ path };
            if (!fs::is_directory(p, ec)) { err = "Not a directory: " + path; return faux; }
            // Probe the directory open: read_local_dir silently lists an unreadable dir as
            // empty, but navigation (the address bar) must see the EACCES to fall back.
            auto probe = fs::directory_iterator{ p, ec };
            if (ec) { err = "Cannot access: " + path + " (" + ec.message() + ")"; return faux; }
            out = read_local_dir(p);
            return true;
        };
    }

    // Per-pane state, stored inside the widget via base::field.
    struct pane_state
    {
        text                  label;            // "Local site" / "Remote site".
        text                  path;             // Current directory.
        bool                  is_local = true;  // POSIX vs. native path math.
        lister_t              lister;           // Directory producer.
        std::vector<direntry> items;            // Listing (excludes synthetic "..").
        text                  error;            // Last listing error.
        si32                  sel = 0;          // Cursor / activation target / shift anchor (0 == "..").
        // Multi-selection (mirrors the queue table): `marked` is the highlighted set of logical rows;
        // `sel_anchor` is the shift-range anchor; the rubber_* fields track a live left-drag band.
        std::set<si32>        marked{ 0 };
        si32                  sel_anchor = 0;
        bool                  rubber = faux;
        si32                  rubber_a = -1, rubber_b = -1;
        // Ctrl+drag rubber-band (additive select / deselect): `drag_base` is the selection captured at
        // press time (before the click mutates it) that the band is merged onto; `rubber_ctrl` marks a
        // Ctrl-modified band; `rubber_add` is its mode (true = select swept rows, false = deselect them,
        // decided from the anchor row's pre-drag state).
        std::set<si32>        drag_base;
        bool                  rubber_ctrl = faux;
        bool                  rubber_add  = true;
        // Inline name entry for the right-click "Create Directory" / "Rename" actions.
        si32                  input_mode = 0;   // 0 = none, 1 = create-directory, 2 = rename.
        text                  input_buf;        // The name being typed.
        // Address-bar path entry (the row-0 path field, connect-bar style).
        bool                  addr_edit  = faux; // Editing the path in place.
        text                  addr_buf;          // The path being typed.
        si32                  addr_caret = 0;    // Caret grapheme-cluster index.
        si32                  addr_off   = 0;    // Horizontal scroll offset (display cells).
        rect                  addr_box;          // Cached field box (render→mouse), like connect_state::box.
        bool                  addr_drag  = faux; // Left-drag from the field scrubs the caret.
        si32                  scroll = 0;       // First visible logical row.
        si32                  hscroll = 0;      // Horizontal cell offset (long names overflow).
        // Transfer-table-style columns (Name, Size, Modified): session-only widths + visibility,
        // resizable by dragging the inter-column border (which runs down the list) and toggled from
        // the column-header right-click menu. Mirrors parvion/queue.hpp.
        std::array<si32, 3>   col_w{ 24, 10, 17 }; // Name; Size (right-aligned); Modified ("YYYY-MM-DD HH:MM" + border cell).
        std::array<bool, 3>   col_shown{ true, true, true };
        si32                  hover_border = -1; // Column border under the cursor (-1 = none).
        si32                  col_drag = -1;     // Column border being width-dragged (-1 = none).
        si32                  div_bottom = 1;    // Exclusive bottom row of the column dividers (header + visible rows).
        bool                  focused = faux;   // Pane has keyboard focus.
        rect                  area;             // Cached widget area (render→mouse).
        si32                  rows = 0;         // Cached visible list rows.
        si32                  content_w = 0;    // Natural width of the columns (Name+Size+Modified).
        si32                  disp_w = 0;       // Visible width (pane width minus the VSB column).
        si32                  hsb_y = 0;        // Row of the horizontal scrollbar.
        bool                  has_vsb = faux, has_hsb = faux; // Which scrollbars the layout needs.
        bool                  sb_hover = faux;  // Cursor is over the vertical scrollbar.
        bool                  sb_drag  = faux;  // Dragging the vertical scrollbar thumb.
        si32                  sb_grab  = 0;     // V-thumb-drag grab offset (press row - thumb top).
        bool                  hsb_hover = faux; // Cursor is over the horizontal scrollbar.
        bool                  hsb_drag  = faux; // Dragging the horizontal scrollbar thumb.
        si32                  hsb_grab  = 0;    // H-thumb-drag grab offset (press col - thumb left).
        sftp_remote*          remote = nullptr; // Non-null: remote pane backed by the SFTP controller.
        sftp_remote*          ctrl = nullptr;   // SFTP controller (both panes) for enqueueing transfers.
        netxs::wptr<ui::base> window_wp;        // App top-level window cake: anchor for confirm dialogs.
        ui64                  seen_gen = ~0ull; // Last remote listing generation seen (selection reset).
        ui64                  seen_local_gen = 0; // Last local refresh generation seen (post-download re-list); matches ctrl->local_gen's initial 0.

        // Effective data source: the SFTP controller for a remote pane, else self.
        auto const& cur_items() const { return remote ? remote->items : items; }
        auto const& cur_path()  const { return remote ? remote->path  : path;  }
        auto cur_msg() const -> text  { return remote ? (remote->connected() ? text{} : remote->status) : error; }
        // Logical row count = ".." + items.
        auto total() const { return (si32)cur_items().size() + 1; }
    };

    // --- Unicode helpers (grapheme-cluster + display-cell aware) -----------------
    // Earlier revisions rolled their own codepoint-counting helpers that assumed one
    // codepoint == one cell; that mangled East-Asian wide glyphs (2 cells) and combining
    // clusters (many codepoints, 1 cell). These delegate to the netxs utf:: machinery so
    // measurement, caret math and painting all agree on display cells / grapheme clusters.

    // Byte step of a single UTF-8 codepoint (used for byte-level filtering, not display).
    inline auto u8_step(view s, size_t i) -> size_t
    {
        auto c = (unsigned char)s[i];
        auto n = c < 0x80 ? 1u : (c & 0xE0) == 0xC0 ? 2u : (c & 0xF0) == 0xE0 ? 3u : 4u;
        return std::min((size_t)n, s.size() - i);
    }
    // Display width of one grapheme cluster (1 for narrow, 2 for wide; >=1 defensively).
    inline auto gc_cells(utf::frag const& frag) -> si32
    {
        auto m = utf::matrix::whxy(frag.attr.cmatrix);
        return std::max(1, (si32)m.w);
    }
    // Total display-cell width of utf8 (grapheme + East-Asian aware; controls skipped).
    inline auto cell_width(view utf8) -> si32
    {
        auto cells = si32{};
        utf::decode_clusters(utf8, [&](view cl){ cells += gc_cells(utf::cluster(cl)); return true; });
        return cells;
    }
    // Number of grapheme clusters in utf8.
    inline auto cluster_count(view utf8) -> si32
    {
        auto n = si32{};
        utf::decode_clusters(utf8, [&](view){ ++n; return true; });
        return n;
    }
    // Byte offset of the start of cluster #idx (idx >= count -> s.size()). Steps raw bytes
    // by cluster length so it stays exact regardless of embedded controls.
    inline auto cluster_to_byte(view s, si32 idx) -> size_t
    {
        auto i = size_t{};
        for (auto n = si32{}; n < idx && i < s.size(); ++n) i += utf::cluster(s.substr(i)).attr.utf8len;
        return std::min(i, s.size());
    }
    // Cluster index for a target cell column `col` (from the start of s). A click landing on
    // the right half of a wide glyph snaps to the following boundary (caret after the glyph).
    inline auto cell_to_cluster(view s, si32 col) -> si32
    {
        if (col <= 0) return 0;
        auto acc  = si32{}; // cells consumed
        auto n    = si32{}; // clusters consumed
        auto stop = si32{};
        auto done = faux;
        utf::decode_clusters(s, [&](view cl)
        {
            auto cw = gc_cells(utf::cluster(cl));
            if (col < acc + cw) { stop = n + (col - acc >= (cw + 1) / 2 ? 1 : 0); done = true; return faux; }
            acc += cw; ++n;
            return true;
        });
        return done ? stop : n; // past end -> caret at end.
    }
    // Display-cell column of the caret (cluster index) within s.
    inline auto caret_cell(view s, si32 caret_idx) -> si32
    {
        return cell_width(s.substr(0, cluster_to_byte(s, caret_idx)));
    }
    // Byte offset of the leftmost cluster whose cell column is >= col (window start).
    inline auto byte_at_cell(view s, si32 col) -> size_t
    {
        return cluster_to_byte(s, cell_to_cluster(s, col));
    }
    // Paint up to max_cells display cells of utf8 at (x,y), advancing by each cluster's display
    // width. Wide (w==2) clusters fill two cells: left half (x=1) and right half (x=2), matching
    // rich::forward_fill_proc. A wide glyph that would only half-fit is dropped (trailing blank).
    // Returns cells written.
    inline auto put_str(auto& canvas, si32 x, si32 y, view utf8, ui32 fg, ui32 bg, si32 max_cells) -> si32
    {
        auto xi = si32{};
        utf::decode_clusters(utf8, [&](view cl) -> bool
        {
            auto frag    = utf::cluster(cl);
            auto m       = utf::matrix::whxy(frag.attr.cmatrix);
            auto cw      = std::max(1, (si32)m.w);
            if (xi + cw > max_cells) return faux; // No partial wide glyph at the right edge.
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

    // Fit utf8 into at most `maxw` display cells. When it overflows, keep the leading clusters that
    // fit in maxw-1 cells and append '…' (U+2026, 1 cell) so the tail signals the truncation. Used
    // by the queue table so a too-narrow column shows "longna…" instead of a hard clip.
    inline auto fit_ellipsis(view utf8, si32 maxw) -> text
    {
        if (maxw <= 0) return {};
        if (cell_width(utf8) <= maxw) return text{ utf8 };
        if (maxw == 1) return text{ "\xE2\x80\xA6" }; // Only room for the ellipsis itself.
        auto budget = maxw - 1; // Cells available before the trailing '…'.
        auto used   = si32{};
        auto out    = text{};
        utf::decode_clusters(utf8, [&](view cl) -> bool
        {
            auto cw = gc_cells(utf::cluster(cl));
            if (used + cw > budget) return faux;
            used += cw;
            out += text{ cl };
            return true;
        });
        out += "\xE2\x80\xA6"; // '…'
        return out;
    }

    // --- single-line editor core (shared by the connect bar's fields and the panes'
    // address bar): a text value + a grapheme-cluster caret index. ---------------

    // Strip control characters from pasted/typed input.
    inline auto edit_filter(view utf8) -> text
    {
        auto out = text{};
        out.reserve(utf8.size());
        for (auto i = size_t{}; i < utf8.size();)
        {
            auto c = (unsigned char)utf8[i];
            if (c < 0x20 || c == 0x7f) { ++i; continue; }
            auto n = u8_step(utf8, i);
            out.append(utf8.data() + i, n);
            i += n;
        }
        return out;
    }
    inline void edit_insert(text& v, si32& caret, view ins)
    {
        if (ins.empty()) return;
        v.insert(cluster_to_byte(v, caret), ins);
        caret += cluster_count(ins);
    }
    inline void edit_backspace(text& v, si32& caret)
    {
        if (caret <= 0) return;
        auto e = cluster_to_byte(v, caret);
        auto b = cluster_to_byte(v, caret - 1);
        v.erase(b, e - b);
        --caret;
    }
    inline void edit_delete(text& v, si32& caret)
    {
        if (caret >= cluster_count(v)) return;
        auto b = cluster_to_byte(v, caret);
        auto e = cluster_to_byte(v, caret + 1);
        v.erase(b, e - b);
    }

    // Make `boss` a vertical drag-resize handle for `target` fork's split: dragging the
    // widget up/down moves the boundary (drag down → the region above grows). When
    // `row_gate >= 0`, only a press landing on that local row begins a resize, so the rest
    // of the widget keeps its own click behavior (e.g. the queue tab strip). Reuses the
    // fork's public ratio API; the offset of the cursor within the handle (gear.coord.y) is
    // exactly the desired change in split position (grip width cancels), re-read each pull
    // so it self-corrects as the handle moves with the split.
    template<class Boss>
    void attach_vsplit_resize(Boss& boss, netxs::wptr<ui::fork> target, si32 row_gate = -1)
    {
        // Enable dragging. Once draggable is on, pro::mouse captures the pointer and
        // re-emits high-level e2::form::drag::* events while consuming the low-level
        // input::key::*Drag* ones, so we must subscribe to the high-level events here
        // (same extension point pro::mover uses). Capture keeps pulls coming even when the
        // cursor leaves the handle as the split moves under it.
        boss.base::signal(tier::release, e2::form::draggable::_<hids::buttons::left>, true);
        auto dragging = ptr::shared(faux);
        boss.LISTEN(tier::release, e2::form::drag::start::_<hids::buttons::left>, gear, -, (dragging, row_gate))
        {
            // gear.click is the press position localized to this widget (gear.pressxy
            // stays in global coords - it is not offset per widget in hids::pass()).
            *dragging = row_gate < 0 || (si32)gear.click.y == row_gate;
        };
        boss.LISTEN(tier::release, e2::form::drag::pull::_<hids::buttons::left>, gear, -, (dragging, target))
        {
            if (!*dragging) return;
            if (auto fork_ptr = target.lock())
            {
                auto [orientation, griparea, ratio] = fork_ptr->get_config();
                auto limit = std::max(1, fork_ptr->base::size().y - griparea.size.y);
                auto split = std::clamp(griparea.coor.y + (si32)gear.coord.y, 0, limit);
                fork_ptr->set_ratio(netxs::divround(ui::fork::max_ratio * split, limit));
                fork_ptr->base::reflow();
            }
        };
        boss.LISTEN(tier::release, e2::form::drag::stop::_<hids::buttons::left>,   gear, -, (dragging)) { *dragging = faux; };
        boss.LISTEN(tier::release, e2::form::drag::cancel::_<hids::buttons::left>, gear, -, (dragging)) { *dragging = faux; };
    }

    // Double-click a divider handle to restore the fork's default split ratio.
    // row_gate >= 0 limits the reset to a press landing on that local row (so the
    // queue tab strip keeps its own behavior); -1 means the whole widget is a handle.
    template<class Boss>
    void attach_dblclick_reset(Boss& boss, netxs::wptr<ui::fork> target, si32 def_s1, si32 def_s2, si32 row_gate = -1)
    {
        boss.on(tier::mouserelease, input::key::LeftDoubleClick, [target, def_s1, def_s2, row_gate](hids& gear)
        {
            if (row_gate >= 0 && (si32)gear.coord.y != row_gate) return;
            if (auto fork_ptr = target.lock()) fork_ptr->config(def_s1, def_s2);
            gear.dismiss();
        });
    }

    // --- pane logic (free functions; safe to call from deferred handlers) --------
    inline void pane_relist(pane_state& st, text const& newpath)
    {
        st.error.clear();
        st.items.clear();
        st.sel = 0;
        st.sel_anchor = 0;
        st.marked = { 0 };
        st.scroll = 0;
        st.hscroll = 0;
        if (st.lister) { if (!st.lister(newpath, st.items, st.error)) st.items.clear(); }
        else st.error = "Not connected.";
        st.path = newpath;
        if (st.is_local && st.ctrl && !newpath.empty()) st.ctrl->local_dir = newpath; // Download target follows the local pane (skip the drive-list sentinel).
    }
    inline void pane_clamp(pane_state& st)
    {
        auto n = st.total();
        if (st.sel < 0)  st.sel = 0;
        if (st.sel >= n) st.sel = n - 1;
        if (st.rows > 0)
        {
            if (st.sel < st.scroll)            st.scroll = st.sel;
            if (st.sel >= st.scroll + st.rows) st.scroll = st.sel - st.rows + 1;
            auto maxscroll = std::max(0, n - st.rows);
            if (st.scroll > maxscroll) st.scroll = maxscroll;
            if (st.scroll < 0)         st.scroll = 0;
        }
    }
    // Reload the current directory in place, preserving the selection/scroll (a completed
    // transfer only adds a row). Used to refresh the destination pane after a transfer.
    inline void pane_refresh(pane_state& st)
    {
        if (!st.lister) return;
        st.error.clear();
        auto items = std::vector<direntry>{};
        if (st.lister(st.path, items, st.error)) st.items = std::move(items);
        pane_clamp(st); // Keep sel/scroll valid against the (possibly larger) listing.
    }
    inline void pane_goparent(pane_state& st)
    {
        if (st.remote) { st.remote->cdup(); return; }
        pane_relist(st, parent_path(st.path, st.is_local));
    }
    inline void pane_activate(pane_state& st)
    {
        if (st.remote) // Remote pane: dir -> descend; file -> enqueue download.
        {
            if (st.sel == 0) { st.remote->cdup(); return; }
            auto idx = st.sel - 1;
            auto& its = st.remote->items;
            if (idx < 0 || idx >= (si32)its.size()) return;
            auto& e = its[idx];
            if (e.is_dir)          st.remote->chdir(e.name);
            else if (st.ctrl)      st.ctrl->enqueue_download(e.name, e.size);
            return;
        }
        // Local pane: dir -> descend; file -> enqueue upload.
        if (st.sel == 0) { pane_relist(st, parent_path(st.path, st.is_local)); return; }
        auto idx = st.sel - 1;
        if (idx < 0 || idx >= (si32)st.items.size()) return;
        auto& e = st.items[idx];
        if (e.is_dir)
        {
        #if defined(_WIN32)
            if (st.path.empty()) { pane_relist(st, e.name + "\\"); return; } // drive list -> enter drive root
        #endif
            pane_relist(st, child_path(st.path, e.name, st.is_local));
        }
        else if (st.ctrl) st.ctrl->enqueue_upload(child_path(st.path, e.name, true), e.name, e.size);
    }
    // Display name of a logical row (row 0 == "..", else items[row-1].name).
    inline auto pane_row_name(pane_state const& st, si32 row) -> view
    {
        if (row == 0) return view{ ".." };
        auto& its = st.cur_items();
        auto idx = row - 1;
        return idx >= 0 && idx < (si32)its.size() ? view{ its[idx].name } : view{};
    }
    // Type-ahead: select the next logical row whose name's first byte matches `ch`
    // (case-insensitive ASCII), searching forward from the current selection and
    // wrapping, so repeated presses cycle through every match. No-op if no match.
    inline void pane_typeahead(pane_state& st, char ch)
    {
        auto n = st.total();
        if (n <= 0) return;
        auto lc   = [](char c){ return (char)std::tolower((unsigned char)c); };
        auto want = lc(ch);
        for (auto step = 1; step <= n; ++step) // sel+1 .. wrap .. sel
        {
            auto row = (st.sel + step) % n;
            auto nm  = pane_row_name(st, row);
            if (!nm.empty() && lc(nm.front()) == want) { st.sel = row; return; }
        }
    }

    // --- address-bar editing (the row-0 path field, connect-bar style) -----------
    inline void pane_addr_begin(pane_state& st)
    {
        st.input_mode = 0; // The two inline editors are mutually exclusive.
        st.input_buf.clear();
        st.addr_buf   = st.cur_path();
        st.addr_caret = cluster_count(st.addr_buf);
        // Open scrolled to the tail (where the resting view is anchored), caret at the end.
        st.addr_off   = std::max(0, cell_width(st.addr_buf) - st.addr_box.size.x);
        st.addr_edit  = true;
    }
    inline void pane_addr_cancel(pane_state& st)
    {
        st.addr_edit = faux;
        st.addr_drag = faux;
        st.addr_buf.clear();
        st.addr_caret = 0;
        st.addr_off = 0;
    }
    // Enter: navigate to the typed path. Edit mode ends before navigating, so a failed
    // remote cd leaves the field showing the unchanged remote->path.
    inline void pane_addr_commit(pane_state& st)
    {
        auto p = view{ st.addr_buf };
        utf::trim_front(p);
        utf::trim_back(p);
        auto dest = text{ p };
        pane_addr_cancel(st);
        if (st.remote)
        {
            if (dest.empty()) return;
            while (dest.size() > 1 && dest.back() == '/') dest.pop_back();
            st.remote->chdir_abs(dest);
            return;
        }
    #if defined(_WIN32)
        if (dest.empty()) { pane_relist(st, {}); return; } // The drive list ("Computer").
    #else
        if (dest.empty()) return;
    #endif
        auto fp = fs::path{ dest };
        if (fp.is_relative() && !st.path.empty()) // `..` / a subdir name resolves against the current dir.
        {
            dest = (fs::path{ st.path } / fp).lexically_normal().string();
        }
        auto oldpath = st.path;
        pane_relist(st, dest);
        // Nonexistent / inaccessible target: fall back to the previous directory (the
        // reverted address is the feedback). Mirrors the remote pane, whose failed cd
        // keeps the old path server-side.
        if (!st.error.empty() && dest != oldpath) pane_relist(st, oldpath);
    }

    // local_y is the widget-local mouse row (gear.coord is rebased per-widget).
    inline auto pane_hit_row(pane_state const& st, si32 local_y) -> si32
    {
        auto vis = local_y - 2; // Skip title + column header.
        if (vis < 0 || vis >= st.rows) return -1;
        auto row = st.scroll + vis;
        return row < st.total() ? row : -1;
    }

    // --- transfer-table-style columns (Name / Size / Modified), mirroring parvion/queue.hpp ----------
    static constexpr auto p_ncol    = si32{ 3 };
    static constexpr auto p_name_x  = si32{ 1 };  // One-cell left margin before the first column.
    static constexpr auto p_col_min = si32{ 2 };  // Minimum resizable-column width.
    static constexpr auto p_col_max = si32{ 200 };
    static constexpr auto p_headers = std::array<view, p_ncol>{ "Name", "Size", "Modified" };

    inline auto p_col_visible(pane_state const& st, si32 i) -> bool { return i >= 0 && i < p_ncol && st.col_shown[(size_t)i]; }
    // Content-x of the left edge of column `i`, skipping hidden columns; -1 when `i` is hidden.
    inline auto p_col_x(pane_state const& st, si32 i) -> si32
    {
        if (!p_col_visible(st, i)) return -1;
        auto x = p_name_x;
        for (auto j = si32{}; j < i; ++j) if (p_col_visible(st, j)) x += st.col_w[(size_t)j];
        return x;
    }
    // Content-x of the right edge (border cell) of column `i`; -1 when hidden.
    inline auto p_border_cx(pane_state const& st, si32 i) -> si32
    {
        auto x = p_col_x(st, i);
        return x < 0 ? -1 : x + st.col_w[(size_t)i] - 1;
    }
    // Right edge of the last visible column (content width), for layout / HSB / row highlight.
    inline auto pane_content_w(pane_state const& st) -> si32
    {
        auto x = p_name_x;
        for (auto i = si32{}; i < p_ncol; ++i) if (p_col_visible(st, i)) x += st.col_w[(size_t)i];
        return x;
    }
    // Screen width of a row's selection highlight / hit-box: the visible columns only.
    inline auto pane_row_w(pane_state const& st) -> si32 { return std::clamp(st.content_w - st.hscroll, 0, st.disp_w); }
    // The text column `col` shows for logical `row` (row 0 == "..", else items[row-1]); mirrors render.
    inline auto pane_cell_text(pane_state const& st, si32 col, si32 row) -> text
    {
        if (row == 0) return col == 0 ? text{ "/.." } : text{}; // ".." is a directory; no size/time.
        auto& its = st.cur_items();
        auto  idx = row - 1;
        if (idx < 0 || idx >= (si32)its.size()) return {};
        auto& e = its[(size_t)idx];
        if (col == 0) return (e.is_dir ? text{ "/" } : text{ " " }) + e.name;
        if (col == 1) return e.is_dir ? text{} : human_size(e.size);
        return e.mtime ? fmt_time(e.mtime) : text{};
    }
    // Display-cell width of the widest content in column `col` (floored by the header) — drives the
    // double-click auto-fit. The Modified header renders with a leading space, so floor it likewise.
    inline auto pane_col_content_w(pane_state const& st, si32 col) -> si32
    {
        auto w = cell_width(p_headers[(size_t)col]);
        for (auto row = si32{}; row < st.total(); ++row) w = std::max(w, cell_width(pane_cell_text(st, col, row)));
        return w;
    }
    // Build the column-header right-click menu: one kind::check show-hide toggle (▣/□) per column
    // (mirrors the message log's "Show detailed log"). At least one column stays visible. Toggling
    // defaces the pane; the action only touches `st` after locking the pane so a torn-down pane is
    // safe.
    inline auto build_pane_columns_menu(pane_state& st, netxs::wptr<ui::base> panel_wp) -> std::vector<app::shared::menu::item>
    {
        namespace m = app::shared::menu;
        auto items = std::vector<m::item>{};
        for (auto i = si32{}; i < p_ncol; ++i)
        {
            auto shown = st.col_shown[(size_t)i];
            auto row = m::item{ .alive = true, .label = text{ p_headers[(size_t)i] },
                                .type = m::kind::check, .checked = shown };
            row.action = [&st, panel_wp, i](hids&)
            {
                if (auto p = panel_wp.lock())
                {
                    if (st.col_shown[(size_t)i]) // Hiding: keep at least one column visible.
                    {
                        auto cnt = si32{};
                        for (auto j = si32{}; j < p_ncol; ++j) if (st.col_shown[(size_t)j]) ++cnt;
                        if (cnt <= 1) return;
                    }
                    st.col_shown[(size_t)i] = !st.col_shown[(size_t)i];
                    p->base::deface();
                }
            };
            items.push_back(std::move(row));
        }
        return items;
    }

    // Hit-test a body row WITHIN the columns: a press to the right of the last column (mx past the
    // columns width) or below the list returns -1, so a click there never selects an item.
    inline auto pane_hit_item(pane_state const& st, si32 mx, si32 my) -> si32
    {
        if (mx >= pane_row_w(st)) return -1;
        return pane_hit_row(st, my);
    }

    // --- file operations (local: std::filesystem, cross-platform; remote: backend verbs) ----------
    inline void pane_reload(pane_state& st)
    {
        if (st.remote) st.remote->request_refresh();
        else           pane_refresh(st);
    }
    inline void pane_create_dir(pane_state& st) // Uses st.input_buf as the new directory name.
    {
        auto name = st.input_buf;
        if (name.empty()) return;
        if (st.remote) { st.remote->remote_mkdir(name); return; }
        auto ec = std::error_code{};
        fs::create_directory(fs::path{ child_path(st.path, name, true) }, ec);
        pane_refresh(st);
    }
    inline void pane_rename_sel(pane_state& st) // Renames the cursor item to st.input_buf.
    {
        auto newname = st.input_buf;
        auto& its = st.cur_items();
        auto idx = st.sel - 1;
        if (newname.empty() || st.sel <= 0 || idx < 0 || idx >= (si32)its.size()) return;
        auto oldname = text{ its[(size_t)idx].name };
        if (oldname == newname) return;
        if (st.remote) { st.remote->remote_rename(oldname, newname); return; }
        auto ec = std::error_code{};
        fs::rename(fs::path{ child_path(st.path, oldname, true) }, fs::path{ child_path(st.path, newname, true) }, ec);
        pane_refresh(st);
    }
    inline void pane_delete_selection(pane_state& st) // Deletes every marked real item (skips ".."), folders included.
    {
        auto& its = st.cur_items();
        auto victims = std::vector<std::pair<text, bool>>{}; // (name, is_dir)
        for (auto row : st.marked)
            if (row > 0 && row - 1 < (si32)its.size())
                victims.emplace_back(its[(size_t)(row - 1)].name, its[(size_t)(row - 1)].is_dir);
        if (victims.empty()) return;
        if (st.remote)
        {
            // Remote: folders recurse (plain rmdir can't), files go through the same recop engine so a
            // mixed selection never races two commands on the control session.
            for (auto& [nm, dir] : victims)
                if (dir) st.remote->delete_folder(nm);
                else     st.remote->delete_remote_file(nm);
            return;
        }
        st.marked = { 0 }; st.sel = 0; st.sel_anchor = 0;
        if (st.ctrl)
        {
            // Local: hand the paths to the controller's detached worker (a big subtree must not
            // freeze the UI); each completed item bumps local_gen, which re-lists this pane.
            auto paths = std::vector<text>{};
            for (auto& [nm, dir] : victims) paths.push_back(child_path(st.path, nm, true));
            st.ctrl->delete_local_async(std::move(paths));
            return;
        }
        auto ec = std::error_code{};
        for (auto& [nm, dir] : victims) fs::remove_all(fs::path{ child_path(st.path, nm, true) }, ec); // remove_all recurses.
        pane_refresh(st);
    }
    inline void pane_transfer_selection(pane_state& st) // Enqueue marked items (upload local / download remote); folders recurse.
    {
        if (!st.ctrl) return;
        auto& its = st.cur_items();
        for (auto row : st.marked) if (row > 0 && row - 1 < (si32)its.size())
        {
            auto& e = its[(size_t)(row - 1)];
            if (st.remote) // Download: remote -> local.
            {
                if (e.is_dir) st.ctrl->download_folder(e.name);
                else          st.ctrl->enqueue_download(e.name, e.size);
            }
            else // Upload: local -> remote.
            {
                if (e.is_dir) st.ctrl->upload_folder(child_path(st.path, e.name, true), e.name);
                else          st.ctrl->enqueue_upload(child_path(st.path, e.name, true), e.name, e.size);
            }
        }
    }

    // Right-click menu for the blank area: Refresh + Create Directory (mirrors the queue's menus).
    inline auto build_pane_blank_menu(pane_state& st, netxs::wptr<ui::base> panel_wp) -> std::vector<app::shared::menu::item>
    {
        namespace m = app::shared::menu;
        auto items = std::vector<m::item>{};
        auto add = [&](text label, auto fn)
        {
            auto row = m::item{ .alive = true, .label = std::move(label) };
            row.action = [panel_wp, fn](hids&){ if (auto p = panel_wp.lock()) { fn(); p->base::deface(); } };
            items.push_back(std::move(row));
        };
        add("Refresh", [&st]{ pane_reload(st); });
        add("Create Directory", [&st, panel_wp]
        {
            st.input_mode = 1; st.input_buf.clear();
            if (auto p = panel_wp.lock()) pro::focus::set(p, id_t{}, solo::on);
        });
        return items;
    }
    // Right-click menu for a selected item: Upload/Download + Delete + Rename.
    inline auto build_pane_item_menu(pane_state& st, netxs::wptr<ui::base> panel_wp) -> std::vector<app::shared::menu::item>
    {
        namespace m = app::shared::menu;
        auto items = std::vector<m::item>{};
        auto add = [&](text label, auto fn)
        {
            auto row = m::item{ .alive = true, .label = std::move(label) };
            row.action = [panel_wp, fn](hids&){ if (auto p = panel_wp.lock()) { fn(); p->base::deface(); } };
            items.push_back(std::move(row));
        };
        add(st.remote ? text{ "Download" } : text{ "Upload" }, [&st]{ pane_transfer_selection(st); });
        add("Delete", [&st, panel_wp]
        {
            // Count the victims exactly like pane_delete_selection (skip row 0 = "..") so the
            // dialog reflects what would actually be deleted; bail silently when there's nothing.
            auto& its = st.cur_items();
            auto count = si32{};
            auto first = text{};
            for (auto row : st.marked)
                if (row > 0 && row - 1 < (si32)its.size())
                {
                    if (!count) first = its[(size_t)(row - 1)].name;
                    ++count;
                }
            if (!count) return;
            auto run = [&st, panel_wp] // Deferred to confirm time: lock the panel before touching its st field storage.
            {
                if (auto p = panel_wp.lock()) { pane_delete_selection(st); p->base::deface(); }
            };
            auto window = st.window_wp.lock();
            if (!window) { run(); return; } // No dialog anchor wired: behave as before.
            auto texts = app::shared::confirm_dialog_text{
                count == 1 ? "Delete '" + fit_ellipsis(first, 26) + "'?" // 26-cell name keeps the message on one dialog row.
                           : "Delete " + std::to_string(count) + " selected items?",
                "Delete", "Cancel" };
            app::shared::show_close_confirmation(*window, run, {}, texts);
        });
        add("Rename", [&st, panel_wp]
        {
            auto& its = st.cur_items();
            auto idx = st.sel - 1;
            if (st.sel <= 0 || idx < 0 || idx >= (si32)its.size()) return; // ".." / no item: can't rename.
            st.input_mode = 2;
            st.input_buf = text{ its[(size_t)idx].name };
            if (auto p = panel_wp.lock()) pro::focus::set(p, id_t{}, solo::on);
        });
        return items;
    }

    // Settle the pane's visible rows and which scrollbars are needed into st's cache, given the
    // natural content width. Two refine passes settle the VSB<->HSB interplay (each can force the
    // other by stealing a cell), mirroring the queue panel's queue_layout_core.
    inline void pane_layout(pane_state& st, si32 w, si32 h, si32 content_w)
    {
        st.content_w = content_w;
        auto n = st.total();
        auto avail = std::max(0, h - 2); // Rows below the title (row 0) and column header (row 1).
        for (auto pass = si32{}; pass < 2; ++pass)
        {
            st.has_vsb = avail > 0 && n > avail;
            st.disp_w  = w - (st.has_vsb ? 1 : 0);
            st.has_hsb = content_w > st.disp_w;
            st.rows    = std::max(0, avail - (st.has_hsb ? 1 : 0));
            st.has_vsb = st.rows > 0 && n > st.rows;
            st.disp_w  = w - (st.has_vsb ? 1 : 0);
            st.has_hsb = content_w > st.disp_w;
            st.rows    = std::max(0, avail - (st.has_hsb ? 1 : 0));
        }
        st.hsb_y = 2 + st.rows; // Row directly below the last list row.
    }
    // Scrollbar geometry in widget-local coords, matching what pane_render paints. `pane_sb`
    // doubles for both axes: for the VSB, {x,top} is the track's column/first-row and the thumb
    // fields run vertically; for the HSB, {top} is the row, {x} the first column, horizontally.
    struct pane_sb { bool ok = faux; si32 x = 0, top = 0, track_h = 0, thumb_y = 0, thumb_h = 0, maxscroll = 0; };
    inline auto pane_scrollbar(pane_state const& st) -> pane_sb
    {
        auto sb = pane_sb{};
        auto w = st.area.size.x;
        auto n = st.total();
        sb.ok = st.has_vsb && st.rows > 0 && n > st.rows;
        if (!sb.ok) return sb;
        sb.x         = w - 1;
        sb.top       = 2;            // List body starts at row 2 (title + column header above).
        sb.track_h   = st.rows;
        sb.thumb_h   = std::max(1, st.rows * st.rows / n);
        sb.maxscroll = n - st.rows;  // >= 1 when ok.
        sb.thumb_y   = sb.top + (st.rows - sb.thumb_h) * st.scroll / sb.maxscroll;
        return sb;
    }
    inline auto pane_hsb(pane_state const& st) -> pane_sb
    {
        auto sb = pane_sb{};
        sb.ok = st.has_hsb && st.disp_w > 0 && st.content_w > st.disp_w;
        if (!sb.ok) return sb;
        sb.x         = 0;
        sb.top       = st.hsb_y;
        sb.track_h   = st.disp_w;
        sb.thumb_h   = std::max(1, st.disp_w * st.disp_w / st.content_w);
        sb.maxscroll = st.content_w - st.disp_w;
        sb.thumb_y   = (st.disp_w - sb.thumb_h) * st.hscroll / sb.maxscroll;
        return sb;
    }
    // Scroll so the thumb (grabbed at st.sb_grab) tracks the cursor row `local_y`.
    inline void pane_sb_scroll_to(pane_state& st, si32 local_y, pane_sb const& sb)
    {
        auto travel = sb.track_h - sb.thumb_h; // Thumb's vertical travel range.
        if (travel <= 0) return;
        auto new_thumb_y = local_y - st.sb_grab - sb.top;
        st.scroll = std::clamp(new_thumb_y * sb.maxscroll / travel, 0, sb.maxscroll);
    }
    // Scroll so the H-thumb (grabbed at st.hsb_grab) tracks the cursor column `local_x`.
    inline void pane_hsb_scroll_to(pane_state& st, si32 local_x, pane_sb const& sb)
    {
        auto travel = sb.track_h - sb.thumb_h; // Thumb's horizontal travel range.
        if (travel <= 0) return;
        st.hscroll = std::clamp((local_x - st.hsb_grab - sb.x) * sb.maxscroll / travel, 0, sb.maxscroll);
    }

    // Painted in widget-local coordinates: change_basis() has already translated
    // the canvas to this widget's origin, so (0,0) is the pane's top-left. `size`
    // is the pane's own size (boss.base::size()), not the full canvas area().
    inline void pane_render(pane_state& st, auto& parent_canvas, twod size)
    {
        auto r = rect{ dot_00, size };
        st.area = r;
        auto w = r.size.x;
        auto h = r.size.y;
        if (w <= 0 || h <= 0) return;
        auto ox = r.coor.x;
        auto oy = r.coor.y;
        parent_canvas.fill(r, [&](cell& c){ c.bgc(theme::bg).fgc(theme::text_fg); });

        // Row 0: title + the path as an editable address field, in the connect-bar field
        // style (connect_render in connectbar.hpp): an underline marks the editable
        // extent — muted at rest, accent blue with a block caret while editing.
        if (st.remote && st.remote->gen != st.seen_gen) { st.seen_gen = st.remote->gen; st.sel = 0; st.sel_anchor = 0; st.marked = { 0 }; st.scroll = 0; st.hscroll = 0; }
        auto tfg = st.focused ? theme::title_fg_act : theme::title_fg;
        parent_canvas.fill(rect{{ ox, oy }, { w, 1 }}, [&](cell& c){ c.bgc(theme::header); });
        auto disp = st.is_local && st.cur_path().empty() ? text{ "Computer" } : st.cur_path();
        auto lead = ' ' + st.label + ' ';
        auto fx = cell_width(lead) + 1; // One gap cell between the label and the field.
        auto fw = w - fx - 1;           // The field spans to a one-cell right margin.
        if (fw < 2) // Too narrow for a field: plain title, no hitbox.
        {
            st.addr_box = {};
            if (st.addr_edit) pane_addr_cancel(st);
            put_str(parent_canvas, ox, oy, lead + ' ' + disp, tfg, theme::header, w);
        }
        else
        {
            put_str(parent_canvas, ox, oy, lead, tfg, theme::header, w);
            st.addr_box = rect{{ fx, 0 }, { fw, 1 }};
            auto und_clr = st.addr_edit ? ui32{ theme::sel_bg_act } : ui32{ theme::subtext };
            parent_canvas.fill(st.addr_box, [&](cell& c){ c.bgc(theme::header).und(unln::line).unc(argb{ und_clr }); });
            if (st.addr_edit)
            {
                // Scroll the field so the caret stays inside its fw-cell window (mirrors connect_render).
                auto total = cell_width(st.addr_buf);
                auto ccell = caret_cell(st.addr_buf, st.addr_caret);
                auto& off  = st.addr_off;
                if (off > ccell)       off = ccell;
                if (ccell - off >= fw) off = ccell - fw + 1;
                off = std::clamp(off, si32{ 0 }, std::max(si32{ 0 }, total - fw + 1));
                auto shown = view{ st.addr_buf }.substr(byte_at_cell(st.addr_buf, off));
                put_str(parent_canvas, fx, 0, shown, theme::sel_bg_act, theme::header, fw);
                auto carx = ccell - off;
                if (carx >= 0 && carx < fw)
                {
                    parent_canvas.fill(rect{{ fx + carx, 0 }, { 1, 1 }}, [&](cell& c){ c.bgc(theme::sel_bg_act).fgc(theme::header); });
                }
            }
            else
            {
                // Tail-anchored: the current directory name matters most, so a long path
                // shows "…tail" instead of clipping the tail off.
                auto off = std::max(0, cell_width(disp) - fw);
                if (off > 0)
                {
                    put_str(parent_canvas, fx, 0, "\xE2\x80\xA6", tfg, theme::header, 1); // …
                    put_str(parent_canvas, fx + 1, 0, view{ disp }.substr(byte_at_cell(disp, off + 1)), tfg, theme::header, fw - 1);
                }
                else put_str(parent_canvas, fx, 0, disp, tfg, theme::header, fw);
            }
        }

        // Resizable column content-x positions (Name, Size, Modified); -1 marks a hidden column.
        auto cx = std::array<si32, p_ncol>{};
        for (auto i = si32{}; i < p_ncol; ++i) cx[(size_t)i] = p_col_x(st, i);
        auto content_w = pane_content_w(st);

        pane_layout(st, w, h, content_w);
        // Clamp ranges only — do NOT force the selection into view here, so wheel/scrollbar
        // scrolling is free. Keyboard navigation re-centers separately via pane_clamp().
        {
            auto nn = st.total();
            st.sel     = std::clamp(st.sel,     0, std::max(0, nn - 1));
            st.scroll  = std::clamp(st.scroll,  0, std::max(0, nn - st.rows));
            st.hscroll = std::clamp(st.hscroll, 0, std::max(0, st.content_w - st.disp_w));
        }

        auto hs    = st.hscroll;
        auto clipw = st.disp_w; // Right clip: keep painted cells off the VSB column.
        // Paint one column's text, tail-truncated to the column width with '…', shifted by the
        // horizontal scroll and clipped to [0, clipw). The pane's canvas is rebased to its own origin
        // but not clipped, so cells scrolled past the left edge (sx < 0) must be *dropped* — drawing
        // them at a negative x would bleed into the adjacent pane.
        auto col = [&](si32 content_x, si32 colw, si32 y, view s, ui32 fg, ui32 bg)
        {
            auto t  = fit_ellipsis(s, colw);
            auto v  = view{ t };
            auto sx = content_x - hs;
            if (sx >= clipw || sx + colw <= 0) return; // Entirely off-screen right or left.
            if (sx < 0)
            {
                auto cl  = cell_to_cluster(v, -sx);    // First cluster at/after the left edge.
                auto cut = caret_cell(v, cl);          // Cells actually dropped (snapped to a boundary).
                v     = v.substr(cluster_to_byte(v, cl));
                colw -= cut;
                sx   += cut;
            }
            auto room = std::min(colw, clipw - sx);
            if (room > 0 && sx >= 0 && sx < clipw) put_str(parent_canvas, sx, y, v, fg, bg, room);
        };
        // Right-align text within a column (leaves a one-cell gap before the border marker).
        auto colr = [&](si32 content_x, si32 colw, si32 y, view s, ui32 fg, ui32 bg)
        {
            auto avail = std::max(1, colw - 1);
            auto t     = fit_ellipsis(s, avail);
            auto tw    = cell_width(t);
            col(content_x + (avail - tw), tw, y, t, fg, bg);
        };
        // Column divider markers (the resize handles, extended down the list), preserving the row bg.
        auto draw_dividers = [&](si32 y, ui32 bg)
        {
            for (auto i = si32{}; i < p_ncol; ++i)
            {
                auto bx = p_border_cx(st, i) - hs;
                if (bx < 0 || bx >= clipw) continue;
                auto hot = st.hover_border == i || st.col_drag == i;
                put_str(parent_canvas, bx, y, "\xE2\x94\x82", hot ? ui32{ theme::text_fg } : ui32{ theme::subtext }, bg, 1); // │
            }
        };

        // Row 1: column header (scrolls horizontally with the list) + the divider markers — or, while
        // entering a name, an inline input field (Create Directory / Rename) with a block cursor.
        if (h > 1 && st.input_mode)
        {
            auto prompt = (st.input_mode == 1 ? text{ "New folder: " } : text{ "Rename: " }) + st.input_buf;
            parent_canvas.fill(rect{{ 0, 1 }, { w, 1 }}, [&](cell& c){ c.bgc(theme::sel_bg_act); });
            put_str(parent_canvas, 0, 1, prompt, theme::sel_fg_act, theme::sel_bg_act, w);
            auto curx = std::min(w - 1, cell_width(prompt));
            parent_canvas.fill(rect{{ curx, 1 }, { 1, 1 }}, [&](cell& c){ c.bgc(theme::sel_fg_act); });
        }
        else if (h > 1)
        {
            parent_canvas.fill(rect{{ 0, 1 }, { w, 1 }}, [&](cell& c){ c.bgc(theme::surface); });
            if (cx[0] >= 0) col (cx[0], st.col_w[0] - 1, 1, "Name",      theme::subtext, theme::surface);
            if (cx[1] >= 0) colr(cx[1], st.col_w[1],     1, "Size",      theme::subtext, theme::surface);
            if (cx[2] >= 0) col (cx[2], st.col_w[2] - 1, 1, "Modified", theme::subtext, theme::surface);
            draw_dividers(1, theme::surface);
        }

        // List body.
        auto msg = st.cur_msg();
        if (!msg.empty())
        {
            st.div_bottom = 2; // Dividers span the header row only when a status/error message is shown.
            auto mc = st.remote ? ui32{ theme::subtext } : ui32{ theme::err_fg };
            put_str(parent_canvas, 1, 2, msg, mc, theme::bg, std::max(1, st.disp_w - 1));
            return;
        }
        auto n = st.total();
        auto rows_drawn = si32{};
        for (auto vis = si32{}; vis < st.rows; ++vis)
        {
            auto row = st.scroll + vis;
            if (row >= n) break;
            ++rows_drawn;
            auto y = 2 + vis;
            auto is_sel = st.marked.count(row) != 0;
            auto rbg = is_sel ? ui32{ theme::sel_bg } : ui32{ theme::bg };
            // Selection highlight (muted sel_bg, ending at the last column) with a one-cell blue left
            // accent when the pane is focused — mirrors the queue table (parvion/queue.hpp).
            if (is_sel)
            {
                parent_canvas.fill(rect{{ 0, y }, { pane_row_w(st), 1 }}, [&](cell& c){ c.bgc(theme::sel_bg); });
                if (st.focused) parent_canvas.fill(rect{{ 0, y }, { 1, 1 }}, [&](cell& c){ c.bgc(theme::sel_bg_act); });
            }

            auto nm = text{};
            auto fgc = ui32{ theme::text_fg };
            auto is_dir = true;
            auto sz = text{};
            auto tm = text{};
            if (row == 0)
            {
                nm = "..";
                fgc = theme::dir_fg;
            }
            else
            {
                auto& e = st.cur_items()[row - 1];
                is_dir = e.is_dir;
                nm  = e.name;
                fgc = e.is_link ? ui32{ theme::link_fg } : e.is_dir ? ui32{ theme::dir_fg } : ui32{ theme::text_fg };
                sz  = e.is_dir ? text{} : human_size(e.size);
                tm  = fmt_time(e.mtime);
            }
            // Selected rows keep their natural fg colors (queue-table style: the muted sel_bg stays readable).
            auto sub = ui32{ theme::subtext };
            if (cx[0] >= 0)                  col (cx[0], st.col_w[0] - 1, y, (is_dir ? text{ "/" } : text{ " " }) + nm, fgc, rbg);
            if (cx[1] >= 0 && !sz.empty())   colr(cx[1], st.col_w[1],     y, sz, sub, rbg);
            if (cx[2] >= 0 && !tm.empty())   col (cx[2], st.col_w[2] - 1, y, tm, sub, rbg);
            draw_dividers(y, rbg);
        }
        st.div_bottom = 2 + rows_drawn; // Divider span: header (row 1) + the visible list rows.

        // Scrollbars (command_bar half-block glyph design; see queue.hpp / tile.hpp): a thin
        // foreground glyph over the pane background — ▐/█ vertical, ▂/▄ horizontal — that
        // brightens on hover and turns accent-blue while dragging (theme::sb_*).
        if (auto sb = pane_scrollbar(st); sb.ok)
        {
            auto mark = (st.sb_drag || st.sb_hover) ? "\xe2\x96\x88" : "\xe2\x96\x90"; // █ : ▐
            parent_canvas.fill(rect{{ sb.x, sb.top }, { 1, sb.track_h }}, [&](cell& c){ c.bgc(theme::bg).fgc(theme::sb_track).txt(mark); });
            auto tc = st.sb_drag ? ui32{ theme::sb_drag } : st.sb_hover ? ui32{ theme::sb_hover } : ui32{ theme::sb_thumb };
            parent_canvas.fill(rect{{ sb.x, sb.thumb_y }, { 1, sb.thumb_h }}, [&](cell& c){ c.bgc(theme::bg).fgc(tc).txt(mark); });
        }
        if (auto sb = pane_hsb(st); sb.ok)
        {
            auto mark = (st.hsb_drag || st.hsb_hover) ? "\xe2\x96\x84" : "\xe2\x96\x82"; // ▄ : ▂
            parent_canvas.fill(rect{{ sb.x, sb.top }, { sb.track_h, 1 }}, [&](cell& c){ c.bgc(theme::bg).fgc(theme::sb_track).txt(mark); });
            auto tc = st.hsb_drag ? ui32{ theme::sb_drag } : st.hsb_hover ? ui32{ theme::sb_hover } : ui32{ theme::sb_thumb };
            parent_canvas.fill(rect{{ sb.x + sb.thumb_y, sb.top }, { sb.thumb_h, 1 }}, [&](cell& c){ c.bgc(theme::bg).fgc(tc).txt(mark); });
        }
    }

    // Build an interactive file pane. `lister` may be empty for a not-yet-wired
    // remote pane (shows an error/placeholder until a session is attached).
    inline auto make_file_pane(text label, bool is_local, lister_t lister, text initial_path, bool grab_focus = faux, sftp_remote* remote = nullptr, sftp_remote* ctrl = nullptr, pane_state** out_state = nullptr, netxs::wptr<ui::base> window_wp = {}) -> ui::sptr
    {
        auto pane = ui::mock::ctor()
            ->active()
            ->plugin<pro::mouse>()
            ->plugin<pro::focus>(grab_focus ? pro::focus::mode::focused : pro::focus::mode::focusable)
            ->plugin<pro::keybd>();
        pane->invoke([&](auto& boss)
        {
            auto& st = boss.base::field(pane_state{});
            if (out_state) *out_state = &st; // Stable for the widget's lifetime (base::field); lets the UI timer re-list this pane.
            st.label    = label;
            st.is_local = is_local;
            st.lister   = lister;
            st.remote   = remote;
            st.ctrl     = ctrl;
            st.window_wp = window_wp;
            st.path     = initial_path;
            if (!remote) pane_relist(st, initial_path);

            // Enable pointer capture so a scrollbar-thumb drag keeps tracking even when the
            // cursor leaves the pane (pro::mouse re-emits high-level e2::form::drag::* events).
            boss.base::signal(tier::release, e2::form::draggable::_<hids::buttons::left>, true);

            boss.LISTEN(tier::release, e2::render::any, parent_canvas)
            {
                pane_render(st, parent_canvas, boss.base::size());
            };
            boss.LISTEN(tier::release, e2::form::state::focus::count, count)
            {
                st.focused = !!count;
                if (!count && st.addr_edit) pane_addr_cancel(st); // Blur (other pane/bar/queue) cancels the address edit.
                boss.base::deface();
            };
            // Select the hit row on press (mousedown), not on the completed click. A press on
            // the scrollbar is left to the thumb-drag / rail-click handlers.
            boss.on(tier::mouserelease, input::key::LeftDown, [&](hids& gear)
            {
                pro::focus::set(boss.This(), gear.id, solo::on);
                auto mx = (si32)gear.coord.x;
                auto my = (si32)gear.coord.y;
                // A press on the row-0 path field starts (or continues) the address edit and
                // places the caret at the clicked column (mirrors the connect bar's fields).
                // A press anywhere else cancels an active edit.
                if (my == 0)
                {
                    auto& b = st.addr_box;
                    if (b.size.x > 0 && mx >= b.coor.x && mx < b.coor.x + b.size.x
                        && !(st.remote && !st.remote->connected())) // No address edit while disconnected.
                    {
                        if (!st.addr_edit) pane_addr_begin(st);
                        st.addr_caret = std::min(cell_to_cluster(st.addr_buf, st.addr_off + (mx - b.coor.x)), cluster_count(st.addr_buf));
                        boss.base::deface();
                    }
                    else if (st.addr_edit) { pane_addr_cancel(st); boss.base::deface(); }
                    gear.dismiss();
                    return;
                }
                if (st.addr_edit) { pane_addr_cancel(st); boss.base::deface(); } // A press in the body cancels, then selects as usual.
                // Presses on either scrollbar are left to the thumb-drag / rail-click handlers.
                if (auto sb = pane_scrollbar(st); sb.ok && mx == sb.x && my >= sb.top && my < sb.top + sb.track_h) return;
                if (auto sb = pane_hsb(st);       sb.ok && my == sb.top && mx >= sb.x && mx < sb.x + sb.track_h) return;
                // A press on a column border (the resize handle) is left to the drag handler; never select a row.
                if (my >= 1 && my < st.div_bottom)
                    for (auto i = si32{}; i < p_ncol; ++i) if (mx == p_border_cx(st, i) - st.hscroll) return;
                // Row selection (mirrors the queue table): plain = pick one; Ctrl = toggle; Shift =
                // range from the anchor; a press on empty body area clears the selection. A press-drag
                // becomes a rubber-band (drag::start), which clears and re-selects the swept span.
                auto ctl  = !!(gear.ctlstat & hids::anyCtrl);
                auto shft = !!(gear.ctlstat & hids::anyShift);
                // Snapshot the selection before this press mutates it, so a Ctrl+drag that follows can
                // merge its swept span onto the pre-drag selection (additive select / deselect).
                st.drag_base = st.marked;
                // Only the columns area selects an item; the blank area (right of the columns or
                // below the list) clears the selection instead.
                auto row  = pane_hit_item(st, mx, my);
                if (row >= 0)
                {
                    if (shft)
                    {
                        auto lo = std::min(st.sel_anchor, row), hi = std::max(st.sel_anchor, row);
                        st.marked.clear();
                        for (auto j = lo; j <= hi; ++j) st.marked.insert(j);
                        st.sel = row;
                    }
                    else if (ctl)
                    {
                        if (st.marked.count(row)) st.marked.erase(row); else st.marked.insert(row);
                        st.sel = row; st.sel_anchor = row;
                    }
                    else { st.marked = { row }; st.sel = row; st.sel_anchor = row; }
                    boss.base::deface();
                }
                // A plain press in the blank body area clears the selection; a Ctrl press keeps it so a
                // Ctrl+drag from blank can add the swept rows to the existing selection.
                else if (!ctl && my >= 2 && my < 2 + st.rows) { if (!st.marked.empty()) { st.marked.clear(); boss.base::deface(); } }
                gear.dismiss();
            });
            boss.on(tier::mouserelease, input::key::LeftClick, [&](hids& gear)
            {
                // Re-assert focus on the click release: the pro::focus plugin's Ctrl+LeftClick handler
                // toggles focus off when the pane is already focused, so (mirroring the queue panel)
                // we set it back here, keeping the pane focused through a Ctrl-click select/deselect.
                pro::focus::set(boss.This(), gear.id, solo::on);
                auto mx = (si32)gear.coord.x;
                auto my = (si32)gear.coord.y;
                // Click on the VSB rail outside the thumb: page up/down toward the click.
                if (auto sb = pane_scrollbar(st); sb.ok && mx == sb.x && my >= sb.top && my < sb.top + sb.track_h)
                {
                    if      (my < sb.thumb_y)               st.scroll = std::max(0, st.scroll - st.rows);
                    else if (my >= sb.thumb_y + sb.thumb_h) st.scroll = std::min(sb.maxscroll, st.scroll + st.rows);
                    boss.base::deface();
                    gear.dismiss();
                    return;
                }
                // Click on the HSB rail outside the thumb: page left/right toward the click.
                if (auto sb = pane_hsb(st); sb.ok && my == sb.top && mx >= sb.x && mx < sb.x + sb.track_h)
                {
                    auto tx = sb.x + sb.thumb_y;
                    if      (mx < tx)                  st.hscroll = std::max(0, st.hscroll - st.disp_w);
                    else if (mx >= tx + sb.thumb_h)    st.hscroll = std::min(sb.maxscroll, st.hscroll + st.disp_w);
                    boss.base::deface();
                    gear.dismiss();
                }
            });
            boss.on(tier::mouserelease, input::key::LeftDoubleClick, [&](hids& gear)
            {
                auto mx = (si32)gear.coord.x;
                auto my = (si32)gear.coord.y;
                // Double-click a column border to auto-fit that column to its widest content + header.
                if (my >= 1 && my < st.div_bottom)
                    for (auto i = si32{}; i < p_ncol; ++i)
                        if (mx == p_border_cx(st, i) - st.hscroll)
                        {
                            st.col_w[(size_t)i] = std::clamp(pane_col_content_w(st, i) + 1, p_col_min, p_col_max);
                            boss.base::deface();
                            gear.dismiss();
                            return;
                        }
                auto row = pane_hit_item(st, mx, my); // Blank area (right of / below the columns) does nothing.
                if (row >= 0) { st.sel = row; pane_activate(st); boss.base::deface(); }
                gear.dismiss();
            });
            boss.on(tier::mouserelease, input::key::MouseWheel, [&](hids& gear)
            {
                if (gear.hzwhl || st.hsb_hover) // Horizontal scroll over/with the HSB.
                {
                    auto maxh = std::max(0, st.content_w - st.disp_w);
                    st.hscroll = std::clamp(st.hscroll - gear.whlsi * 4, 0, maxh);
                }
                else
                {
                    auto maxv = std::max(0, st.total() - st.rows);
                    st.scroll = std::clamp(st.scroll - gear.whlsi, 0, maxv);
                }
                boss.base::deface();
                gear.dismiss();
            });
            // Scrollbar hover highlight (both axes).
            boss.on(tier::mouserelease, input::key::MouseMove, [&](hids& gear)
            {
                auto mx = (si32)gear.coord.x;
                auto my = (si32)gear.coord.y;
                auto vsb = pane_scrollbar(st);
                auto vh  = vsb.ok && mx == vsb.x && my >= vsb.top && my < vsb.top + vsb.track_h;
                if (vh != st.sb_hover) { st.sb_hover = vh; boss.base::deface(); }
                auto hsb = pane_hsb(st);
                auto hh  = hsb.ok && my == hsb.top && mx >= hsb.x && mx < hsb.x + hsb.track_h;
                if (hh != st.hsb_hover) { st.hsb_hover = hh; boss.base::deface(); }
                // Column-border hover feedback (along the whole divider length).
                auto nb = si32{ -1 };
                if (my >= 1 && my < st.div_bottom)
                    for (auto i = si32{}; i < p_ncol; ++i) if (mx == p_border_cx(st, i) - st.hscroll) { nb = i; break; }
                if (nb != st.hover_border) { st.hover_border = nb; boss.base::deface(); }
            });
            boss.on(tier::mouserelease, input::key::MouseLeave, [&](hids&)
            {
                if (st.sb_hover)  { st.sb_hover  = faux; boss.base::deface(); }
                if (st.hsb_hover) { st.hsb_hover = faux; boss.base::deface(); }
                if (st.hover_border != -1) { st.hover_border = -1; boss.base::deface(); }
            });
            // Right-click context menus (FileZilla-style): the column header opens the show/hide
            // menu; a selected item opens Upload-or-Download / Delete / Rename; the blank area (and
            // the ".." row) opens Refresh / Create Directory.
            boss.on(tier::mouserelease, input::key::RightClick, [&](hids& gear)
            {
                pro::focus::set(boss.This(), gear.id, solo::on); // Right-clicking activates this pane.
                if (st.input_mode || st.addr_edit) { gear.dismiss(); return; } // Ignore while entering text.
                auto mx = (si32)gear.coord.x;
                auto my = (si32)gear.coord.y;
                auto at = twod{ mx, my };
                auto panel = ptr::shadow(boss.This());
                if (my == 1)
                {
                    app::shared::menu::open_dropdown_popup(boss, build_pane_columns_menu(st, panel), faux, -1, at);
                }
                else
                {
                    auto row = pane_hit_item(st, mx, my);
                    if (row > 0) // A real file/dir (".." and blank go to the blank menu).
                    {
                        if (!st.marked.count(row)) { st.marked = { row }; st.sel = row; st.sel_anchor = row; boss.base::deface(); }
                        app::shared::menu::open_dropdown_popup(boss, build_pane_item_menu(st, panel), faux, -1, at);
                    }
                    else
                    {
                        if (!st.marked.empty()) { st.marked.clear(); boss.base::deface(); } // Right-clicking the blank area clears the selection (like left-click).
                        app::shared::menu::open_dropdown_popup(boss, build_pane_blank_menu(st, panel), faux, -1, at);
                    }
                }
                gear.dismiss();
            });
            // Scrollbar thumb drag (and drag-from-rail = snap then drag) on either axis. gear.click
            // is the press position localized to this widget (gear.pressxy is not, see hids::pass).
            boss.LISTEN(tier::release, e2::form::drag::start::_<hids::buttons::left>, gear)
            {
                auto px = (si32)gear.click.x;
                auto py = (si32)gear.click.y;
                // A drag that began on the address field scrubs the caret (the press itself
                // already entered the edit and placed it; pulls keep it under the cursor).
                if (py == 0)
                {
                    auto& b = st.addr_box;
                    if (st.addr_edit && b.size.x > 0 && px >= b.coor.x && px < b.coor.x + b.size.x) st.addr_drag = true;
                    return;
                }
                if (auto sb = pane_scrollbar(st); sb.ok && px == sb.x && py >= sb.top && py < sb.top + sb.track_h)
                {
                    pro::focus::set(boss.This(), gear.id, solo::on);
                    if (py >= sb.thumb_y && py < sb.thumb_y + sb.thumb_h) st.sb_grab = py - sb.thumb_y; // Grab where pressed.
                    else { st.sb_grab = sb.thumb_h / 2; pane_sb_scroll_to(st, py, sb); }                // Snap thumb center.
                    st.sb_drag = st.sb_hover = true;
                    boss.base::deface();
                    return;
                }
                if (auto sb = pane_hsb(st); sb.ok && py == sb.top && px >= sb.x && px < sb.x + sb.track_h)
                {
                    pro::focus::set(boss.This(), gear.id, solo::on);
                    auto tx = sb.x + sb.thumb_y;
                    if (px >= tx && px < tx + sb.thumb_h) st.hsb_grab = px - tx;            // Grab where pressed.
                    else { st.hsb_grab = sb.thumb_h / 2; pane_hsb_scroll_to(st, px, sb); }  // Snap thumb center.
                    st.hsb_drag = st.hsb_hover = true;
                    boss.base::deface();
                    return;
                }
                // Grab a column border anywhere along its length to resize that column.
                if (py >= 1 && py < st.div_bottom)
                    for (auto i = si32{}; i < p_ncol; ++i)
                        if (px == p_border_cx(st, i) - st.hscroll)
                        {
                            pro::focus::set(boss.This(), gear.id, solo::on);
                            st.col_drag = i;
                            boss.base::deface();
                            return;
                        }
                // A press in the list body begins a rubber-band selection. Anchoring on the pressed
                // display row WITHOUT clamping to the last item lets a press in the blank area below
                // the list deselect every item once the drag returns there.
                if (py >= 2 && py < 2 + st.rows)
                {
                    pro::focus::set(boss.This(), gear.id, solo::on);
                    st.rubber_a = st.rubber_b = std::max(0, st.scroll + (py - 2));
                    st.rubber = true;
                    // Ctrl+drag merges onto the pre-drag selection (drag_base): selecting the swept rows
                    // when the anchor was unselected, deselecting them when it was already selected. A
                    // plain drag replaces the selection with the swept span.
                    st.rubber_ctrl = !!(gear.ctlstat & hids::anyCtrl);
                    if (st.rubber_ctrl) { st.rubber_add = !st.drag_base.count(st.rubber_a); st.marked = st.drag_base; }
                    else                st.marked.clear();
                    if (st.rubber_a < st.total()) { st.sel = st.rubber_a; st.sel_anchor = st.rubber_a; }
                    boss.base::deface();
                    return;
                }
            };
            boss.LISTEN(tier::release, e2::form::drag::pull::_<hids::buttons::left>, gear)
            {
                if (st.addr_drag) // Scrub the caret to the cursor column (the render's window clamp auto-scrolls at the edges).
                {
                    auto col = st.addr_off + ((si32)gear.coord.x - st.addr_box.coor.x);
                    st.addr_caret = std::min(cell_to_cluster(st.addr_buf, col), cluster_count(st.addr_buf));
                    boss.base::deface();
                }
                else if (st.sb_drag)  { pane_sb_scroll_to(st,  (si32)gear.coord.y, pane_scrollbar(st)); boss.base::deface(); }
                else if (st.hsb_drag) { pane_hsb_scroll_to(st, (si32)gear.coord.x, pane_hsb(st));       boss.base::deface(); }
                else if (st.col_drag >= 0)
                {
                    // Position-based resize: drive the border to the cursor's column (clamped) rather
                    // than accumulating deltas. After the width bottoms/tops out, the handle then waits
                    // for the cursor to return to it instead of moving at a stale offset.
                    auto mx = (si32)gear.coord.x;
                    auto left = p_col_x(st, st.col_drag); // Stable during the drag (sums only earlier columns).
                    st.col_w[(size_t)st.col_drag] = std::clamp(mx + st.hscroll - left + 1, p_col_min, p_col_max);
                    boss.base::deface();
                }
                else if (st.rubber)
                {
                    auto my = (si32)gear.coord.y;
                    st.rubber_b = st.scroll + std::clamp(my - 2, 0, std::max(0, st.rows - 1));
                    auto lo = std::min(st.rubber_a, st.rubber_b);
                    auto hi = std::max(st.rubber_a, st.rubber_b);
                    if (st.rubber_ctrl) // Merge the swept span onto the pre-drag selection (add or remove).
                    {
                        st.marked = st.drag_base;
                        for (auto i = lo; i <= hi; ++i) if (i >= 0 && i < st.total())
                            { if (st.rubber_add) st.marked.insert(i); else st.marked.erase(i); }
                    }
                    else // Plain band: replace the selection with the swept span.
                    {
                        st.marked.clear();
                        for (auto i = lo; i <= hi; ++i) if (i >= 0 && i < st.total()) st.marked.insert(i);
                    }
                    boss.base::deface();
                }
            };
            boss.LISTEN(tier::release, e2::form::drag::stop::_<hids::buttons::left>,   gear) { if (st.sb_drag || st.hsb_drag || st.col_drag >= 0 || st.rubber || st.addr_drag) { st.sb_drag = st.hsb_drag = faux; st.col_drag = -1; st.rubber = faux; st.addr_drag = faux; boss.base::deface(); } };
            boss.LISTEN(tier::release, e2::form::drag::cancel::_<hids::buttons::left>, gear) { if (st.sb_drag || st.hsb_drag || st.col_drag >= 0 || st.rubber || st.addr_drag) { st.sb_drag = st.hsb_drag = faux; st.col_drag = -1; st.rubber = faux; st.addr_drag = faux; boss.base::deface(); } };
            boss.LISTEN(tier::preview, input::events::keybd::any, gear)
            {
                if (!st.focused) return;
                if (st.addr_edit && gear.payload == input::keybd::type::keypaste) // Paste into the address field.
                {
                    edit_insert(st.addr_buf, st.addr_caret, edit_filter(gear.cluster));
                    gear.set_handled();
                    boss.base::deface();
                    return;
                }
                if (gear.payload != input::keybd::type::keypress) return;
                if (gear.keystat == input::key::interrupted) return;
                if (gear.keybd::handled) return;
                if (gear.keystat == input::key::released) return; // Act on key press only.
                auto k = gear.keybd::generic();
                // Address edit: a full inline editor (caret moves, mirroring the connect bar's
                // fields); Enter navigates to the typed path, Esc reverts to the current one.
                if (st.addr_edit)
                {
                    auto& v = st.addr_buf;
                    auto& c = st.addr_caret;
                    auto act = true;
                         if (k == input::key::Esc)           pane_addr_cancel(st);
                    else if (k == input::key::KeyEnter)      pane_addr_commit(st);
                    else if (k == input::key::Backspace)     edit_backspace(v, c);
                    else if (k == input::key::KeyDelete)     edit_delete(v, c);
                    else if (k == input::key::KeyLeftArrow)  c = std::max(0, c - 1);
                    else if (k == input::key::KeyRightArrow) c = std::min(cluster_count(v), c + 1);
                    else if (k == input::key::KeyHome)       c = 0;
                    else if (k == input::key::KeyEnd)        c = cluster_count(v);
                    else
                    {
                        auto ins = edit_filter(gear.cluster); // '/' and '\\' are path chars: keep them.
                        if (ins.size()) edit_insert(v, c, ins);
                        else act = faux;
                    }
                    if (act)
                    {
                        gear.set_handled();
                        boss.base::deface();
                    }
                    return;
                }
                // Inline name entry (Create Directory / Rename): capture text until Enter or Esc.
                if (st.input_mode)
                {
                    if (k == input::key::Esc) { st.input_mode = 0; st.input_buf.clear(); }
                    else if (k == input::key::KeyEnter)
                    {
                        auto mode = st.input_mode;
                        st.input_mode = 0;
                        if      (mode == 1) pane_create_dir(st);
                        else if (mode == 2) pane_rename_sel(st);
                        st.input_buf.clear();
                    }
                    else if (k == input::key::Backspace)
                    {
                        auto n = cluster_count(st.input_buf);
                        if (n > 0) st.input_buf = st.input_buf.substr(0, cluster_to_byte(st.input_buf, n - 1));
                    }
                    else
                    {
                        auto cl = gear.cluster; // Printable cluster; reject path separators.
                        if (!cl.empty() && (unsigned char)cl[0] >= 0x20 && cl != "\x7f" && cl != "/" && cl != "\\")
                            st.input_buf += text{ cl };
                    }
                    gear.set_handled();
                    boss.base::deface();
                    return;
                }
                auto page = std::max(1, st.rows - 1);
                auto act = true;
                     if (k == input::key::KeyUpArrow)   st.sel -= 1;
                else if (k == input::key::KeyDownArrow) st.sel += 1;
                else if (k == input::key::KeyPageUp)    st.sel -= page;
                else if (k == input::key::KeyPageDown)  st.sel += page;
                else if (k == input::key::KeyHome)      st.sel = 0;
                else if (k == input::key::KeyEnd)       st.sel = st.total() - 1;
                else if (k == input::key::KeyEnter)     pane_activate(st);
                else if (k == input::key::Backspace)    pane_goparent(st);
                else
                {
                    // Type-ahead: a printable ASCII key jumps to / cycles matching names.
                    auto cl = gear.cluster;
                    if (cl.size() == 1 && (unsigned char)cl[0] > 0x20 && (unsigned char)cl[0] < 0x7f)
                        pane_typeahead(st, cl[0]);
                    else act = false;
                }
                if (act)
                {
                    pane_clamp(st);
                    st.marked = { st.sel }; // Keyboard navigation collapses to a single selection at the cursor.
                    st.sel_anchor = st.sel;
                    gear.set_handled();
                    boss.base::deface();
                }
            };

            if (grab_focus) // Grab keyboard focus once the UI tree is started.
            {
                boss.LISTEN(tier::anycast, e2::form::upon::started, root)
                {
                    boss.base::enqueue([](auto& b){ pro::focus::set(b.This(), id_t{}, solo::on, true); });
                };
            }
        });
        return pane;
    }
}
