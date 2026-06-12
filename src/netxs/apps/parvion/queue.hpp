// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/queue.hpp: the transfer-queue panel — a bottom-pinned tab strip (Transferring /
// Failed / Succeeded / Message log) over the controller's live queue, with per-item
// progress. Row 0 is an empty handle bar used only to resize the panel vertically.
// Rendered in the command_bar canvas-paint style; repainted by the applet's poll timer.

#include "panes.hpp" // theme, put_str, sftp_remote, queue_item

namespace netxs::app::parvion
{
    // Resizable transfer-table columns: Local Name, Remote Name, Size, Progress, Speed. The Failed
    // tab appends a non-array "Reason" column (logical index q_ncol). Index order is shared by the
    // width array, the layout math (q_col_x / q_border_cx), the renderer, and ctrl->col_shown.
    static constexpr auto q_ncol = si32{ 5 };
    static constexpr auto q_headers = std::array<view, q_ncol + 1>{ "Local Name", "Remote Name", "Size", "Progress", "Speed", "Reason" };

    struct queue_state
    {
        enum dmode { d_none, d_vsb, d_hsb, d_col, d_rubber }; // Active left-drag gesture.

        sftp_remote*        ctrl = nullptr;
        si32                tab = 0;        // 0=Transferring, 1=Failed, 2=Succeeded, 3=Message log.
        bool                focused = faux;
        bool                hover = faux;   // Cursor is over the top handle bar (row 0).
        std::array<rect, 4> tabbox{};       // Cached tab hitboxes (bottom row).
        std::vector<std::pair<rect, si32>> expand_hit{}; // Per-render: +/- button box -> queue index.
        si32                hover_expand = -1; // Queue index whose +/- button is hovered (-1 = none).
        si32                press_expand = -1; // Queue index whose +/- button is held down (-1 = none).

        // Scrolling (transfer tabs). `scroll` is the first visible display row (parents +
        // expanded subtasks, flattened); `hscroll` is the horizontal cell offset. `follow`
        // tail-tracks the active transfer until the user scrolls away, re-engaging at bottom.
        si32                scroll = 0;
        si32                hscroll = 0;
        bool                follow = true;
        bool                sb_hover = faux,  sb_drag = faux;  si32 sb_grab = 0;  // Vertical scrollbar.
        bool                hsb_hover = faux, hsb_drag = faux; si32 hsb_grab = 0; // Horizontal scrollbar.

        // Resizable columns (Local Name, Remote Name, Size, Progress, Speed), session-only widths.
        std::array<si32, q_ncol> col_w{ 24, 24, 11, 10, 11 };
        si32                reason_w_override = 0; // Failed tab's Reason width once user-dragged (0 = auto-size to widest error).
        si32                hover_border = -1; // Header border (0..q_ncol; q_ncol = Failed-tab Reason) under the cursor (-1 = none).
        si32                col_drag = -1;     // Column border being width-dragged (-1 = none).

        // Selection model (per-item flag lives on queue_item::selected).
        si32                sel_anchor = -1;   // Queue index anchor for ctrl/shift selection.
        si32                rubber_a = -1, rubber_b = -1; // Live rubber-band display-row span.
        // Ctrl+drag rubber-band (additive select / deselect): `drag_base` holds the queue indices that
        // were selected at press time (captured by the LeftDown handler before the press mutates the
        // selection; merged onto each pull); `rubber_ctrl` marks a Ctrl band; `rubber_add` is its mode
        // (true = select swept rows, false = deselect them, from the anchor's pre-press state).
        std::set<si32>      drag_base;
        bool                rubber_ctrl = faux;
        bool                rubber_add  = true;

        dmode               drag = d_none;     // Which gesture the current left-drag drives.

        // Geometry/hit caches rebuilt every render so the mouse handlers agree with paint.
        std::vector<std::pair<rect, si32>> row_hit{}; // Parent-row rect -> queue index.
        si32                body_top = 2, body_rows = 0, tab_row = 0;
        si32                content_w = 0, disp_w = 0, vsb_x = 0, hsb_y = 0;
        si32                div_bottom = 1;    // Exclusive bottom row of the column dividers (header + visible item rows).
        si32                total = 0;         // Display-row count for the active tab.
        bool                has_vsb = faux, has_hsb = faux;
    };

    // A flattened display row for a transfer tab: a parent task (child == -1) or one of its
    // expanded parallel subtasks (child >= 0). The render and the mouse handlers share this
    // model so scroll math and hit-testing never drift apart.
    struct disp_row { si32 qi; si32 child; };

    // Reconstruct the [start, end) byte ranges of a file's parallel chunks, mirroring
    // start_item()'s csz math (session.hpp): csz = ceil(size/count), chunk c covers
    // [c*csz, min((c+1)*csz, size)). Used to label child rows when the live workers
    // are gone (queued / finished parallel items).
    inline auto chunk_ranges(si64 size, ui32 count) -> std::vector<std::pair<si64, si64>>
    {
        auto out = std::vector<std::pair<si64, si64>>{};
        if (count == 0 || size < 0) return out;
        auto csz = (si64)((size + count - 1) / count);
        for (auto c = ui32{}; c < count; ++c)
        {
            auto start = (si64)c * csz;
            if (start >= size) break;
            out.emplace_back(start, std::min(start + csz, size));
        }
        return out;
    }

    // FileZilla StatusView prefix + colour for each message type (InitDefAttr).
    inline auto log_prefix(logtype t) -> view
    {
        using lt = logtype;
        switch (t)
        {
            case lt::error:    return "Error: ";
            case lt::command:  return "Command: ";
            case lt::response: return "Response: ";
            case lt::trace:    return "Trace: ";
            case lt::listing:  return "Listing: ";
            default:           return "Status: ";
        }
    }
    inline auto log_color(logtype t) -> ui32
    {
        using lt = logtype;
        switch (t)
        {
            case lt::error:    return theme::err_fg;
            case lt::command:  return theme::dir_fg;
            case lt::response: return theme::link_fg;
            case lt::trace:    return theme::trace_fg;
            case lt::listing:  return theme::link_fg;
            default:           return theme::subtext;
        }
    }
    // Visibility gates, mirroring FileZilla:
    //   - "Show detailed log" (show_detailed) gates the detailed types
    //     (Command / Response / Trace) just like the StatusView checkbox.
    //   - the selected log level (debug_level) additionally gates Trace lines by
    //     their debug sub-level: a Trace shows only when debug_level >= its level
    //     (mirrors OPTION_LOGGING_DEBUGLEVEL / CLogging::UpdateLogLevel).
    inline auto log_visible(sftp_remote::logline const& ln, bool show_detailed, si32 debug_level) -> bool
    {
        using lt = logtype;
        if (ln.type == lt::trace)                       return show_detailed && debug_level >= ln.level;
        if (ln.type == lt::command || ln.type == lt::response) return show_detailed;
        return true; // Status / Error / Listing are always shown.
    }
    // Render one log line (timestamp + prefix + body) at row `y`, returning faux
    // if it didn't fit. Shared by the panel renderer and the clipboard copy.
    inline auto log_format(sftp_remote::logline const& ln, bool stamps) -> text
    {
        auto s = text{};
        if (stamps) s += ln.stamp + " ";
        s += text{ log_prefix(ln.type) } + ln.body;
        return s;
    }

    // Build the Message-log right-click context menu. Mirrors FileZilla's
    // CStatusView::OnContextMenu (Show detailed log / Copy to clipboard / Clear
    // all) and adds an extra "Log level" radio submenu for the debug verbosity.
    // Rebuilt on every open so the ▣/□ and radio state reflect the controller.
    inline auto build_log_menu(sftp_remote* ctrl, netxs::wptr<ui::base> panel_wp) -> std::vector<app::shared::menu::item>
    {
        namespace m = app::shared::menu;
        auto deface = [panel_wp]{ if (auto p = panel_wp.lock()) p->base::deface(); };
        auto items = std::vector<m::item>{};

        // "&Show detailed log" — wxITEM_CHECK in FileZilla; ▣/□ shows the state.
        auto detail = m::item{ .alive = true,
            .label = (ctrl->show_detailed ? text{ "\xE2\x96\xA3 " }   // ▣
                                          : text{ "\xE2\x96\xA1 " })   // □
                   + "Show detailed log" };
        detail.action = [ctrl, deface](hids&){ ctrl->show_detailed = !ctrl->show_detailed; deface(); };
        items.push_back(std::move(detail));

        items.push_back(m::item{ .alive = true, .type = m::kind::separator });

        // "Copy to clipboard" — the currently-visible log text.
        auto copy = m::item{ .alive = true, .label = "Copy to clipboard" };
        copy.action = [ctrl](hids& gear)
        {
            auto out = text{};
            for (auto& ln : ctrl->logbuf)
                if (log_visible(ln, ctrl->show_detailed, ctrl->debug_level)) out += log_format(ln, ctrl->show_stamps) + "\n";
            gear.set_clipboard(dot_00, out, mime::textonly);
        };
        items.push_back(std::move(copy));

        // "Clear all".
        auto clear = m::item{ .alive = true, .label = "Clear all" };
        clear.action = [ctrl, deface](hids&){ ctrl->logbuf.clear(); ctrl->dirty = true; deface(); };
        items.push_back(std::move(clear));

        items.push_back(m::item{ .alive = true, .type = m::kind::separator });

        // Extra: "Log level" radio submenu (OPTION_LOGGING_DEBUGLEVEL 0..4).
        auto level = m::item{ .alive = true, .label = "Log level", .type = m::kind::radiomenu };
        static constexpr auto names = std::array<view, 5>{ "None", "Warning", "Info", "Verbose", "Debug" };
        for (auto i = si32{}; i < 5; ++i)
        {
            auto row = m::item{ .alive = true, .label = text{ names[(size_t)i] }, .checked = (ctrl->debug_level == i) };
            row.action = [ctrl, deface, i](hids&){ ctrl->debug_level = i; deface(); };
            level.children.push_back(std::move(row));
        }
        items.push_back(std::move(level));

        return items;
    }

    // Does an item belong on transfer tab `tab`? (0=Transferring, 1=Failed, 2=Succeeded.)
    inline auto tab_status_match(si32 tab, queue_item const& it) -> bool
    {
        return (tab == 0 && (it.status == queue_item::queued || it.status == queue_item::transferring))
            || (tab == 1 && it.status == queue_item::failed)
            || (tab == 2 && it.status == queue_item::succeeded);
    }
    inline auto item_in_tab(queue_state const& st, queue_item const& it) -> bool { return tab_status_match(st.tab, it); }

    // Build the per-item right-click menu (Start / Pause / Remove, plus Pin to Top on the
    // Transferring tab). Mirrors build_log_menu's construction; the actions run on the panel's live
    // selection so a single right-clicked row and a multi-selection are handled uniformly.
    inline auto build_queue_item_menu(sftp_remote* ctrl, si32 tab, netxs::wptr<ui::base> panel_wp) -> std::vector<app::shared::menu::item>
    {
        namespace m = app::shared::menu;
        auto deface = [panel_wp]{ if (auto p = panel_wp.lock()) p->base::deface(); };
        auto sel    = [](queue_item const& it){ return it.selected; }; // The current selection set.
        auto items  = std::vector<m::item>{};
        auto add = [&](text label, auto fn)
        {
            auto row = m::item{ .alive = true, .label = std::move(label) };
            row.action = [deface, fn](hids&){ fn(); deface(); };
            items.push_back(std::move(row));
        };
        add("Start",  [ctrl, sel]{ ctrl->queue_start(sel); });
        // Pause applies only to in-flight/pending items (Transferring tab); finished items can't be paused.
        if (tab == 0) add("Pause", [ctrl, sel]{ ctrl->queue_pause(sel); });
        add("Remove", [ctrl, sel]{ ctrl->queue_remove(sel); });
        // Pin to Top reorders pending (queued) items: only on the Transferring tab, and only when the
        // selection actually contains a queued item — a transferring item has nothing to pin, so the
        // entry is dropped for it (the dropdown has no disabled-row style).
        auto pinnable = faux;
        if (tab == 0) for (auto& it : ctrl->queue) if (it.selected && it.status == queue_item::queued) { pinnable = true; break; }
        if (pinnable)
        {
            items.push_back(m::item{ .alive = true, .type = m::kind::separator });
            add("Pin to Top", [ctrl, sel]{ ctrl->queue_pin_top(sel); });
        }
        return items;
    }

    // Build the blank-area right-click menu (Start All / Pause All / Remove All), scoped to the
    // items on the active tab (the visible queue) per the confirmed "current tab only" behavior.
    // Pause All is shown only on the Transferring tab (finished items can't be paused).
    inline auto build_queue_all_menu(sftp_remote* ctrl, si32 tab, netxs::wptr<ui::base> panel_wp) -> std::vector<app::shared::menu::item>
    {
        namespace m = app::shared::menu;
        auto deface = [panel_wp]{ if (auto p = panel_wp.lock()) p->base::deface(); };
        auto scope  = [tab](queue_item const& it){ return tab_status_match(tab, it); };
        auto items  = std::vector<m::item>{};
        auto add = [&](text label, auto fn)
        {
            auto row = m::item{ .alive = true, .label = std::move(label) };
            row.action = [deface, fn](hids&){ fn(); deface(); };
            items.push_back(std::move(row));
        };
        add("Start All",  [ctrl, scope]{ ctrl->queue_start(scope); });
        if (tab == 0) add("Pause All", [ctrl, scope]{ ctrl->queue_pause(scope); });
        add("Remove All", [ctrl, scope]{ ctrl->queue_remove(scope); });
        return items;
    }

    // Build the table-header right-click menu: one toggle per column (▣ shown / □ hidden, mirroring
    // the message log's "Show detailed log"). Reason appears only on the Failed tab. At least one
    // column stays visible. Rebuilt on every open so the ▣/□ reflect the live state.
    inline auto build_queue_columns_menu(sftp_remote* ctrl, si32 tab, netxs::wptr<ui::base> panel_wp) -> std::vector<app::shared::menu::item>
    {
        namespace m = app::shared::menu;
        auto deface = [panel_wp]{ if (auto p = panel_wp.lock()) p->base::deface(); };
        auto items  = std::vector<m::item>{};
        auto n      = tab == 1 ? q_ncol + 1 : q_ncol; // Reason column only on the Failed tab.
        for (auto i = si32{}; i < n; ++i)
        {
            auto shown = ctrl->col_shown[(size_t)i];
            auto row = m::item{ .alive = true,
                .label = (shown ? text{ "\xE2\x96\xA3 " }   // ▣ shown
                                : text{ "\xE2\x96\xA1 " })   // □ hidden
                       + text{ q_headers[(size_t)i] } };
            row.action = [ctrl, deface, i, n](hids&)
            {
                if (ctrl->col_shown[(size_t)i]) // Hiding: keep at least one column visible.
                {
                    auto cnt = si32{};
                    for (auto j = si32{}; j < n; ++j) if (ctrl->col_shown[(size_t)j]) ++cnt;
                    if (cnt <= 1) return;
                }
                ctrl->col_shown[(size_t)i] = !ctrl->col_shown[(size_t)i];
                deface();
            };
            items.push_back(std::move(row));
        }
        return items;
    }

    // Flatten the active transfer tab into display rows (parents + expanded subtasks),
    // mirroring the in_tab predicate the renderer uses. Shared by render and hit-testing.
    inline void build_rows(queue_state const& st, std::vector<disp_row>& out)
    {
        out.clear();
        if (!st.ctrl) return;
        auto& q = st.ctrl->queue;
        for (auto qi = si32{}; qi < (si32)q.size(); ++qi)
        {
            auto& it = q[qi];
            if (!item_in_tab(st, it)) continue;
            out.push_back({ qi, -1 });
            if (it.expanded && it.chunk_count > 1)
            {
                auto nr = (si32)chunk_ranges(it.size, it.chunk_count).size();
                for (auto c = si32{}; c < nr; ++c) out.push_back({ qi, c });
            }
        }
    }

    // Fixed left columns: direction arrow (x=1, 2 cells) + expand button (x=3, 3 cells) +
    // a one-cell gap, so the first resizable column (Local Name) starts here.
    static constexpr auto q_dir_x  = si32{ 1 };
    static constexpr auto q_exp_x  = si32{ 3 };
    static constexpr auto q_exp_w  = si32{ 3 };
    static constexpr auto q_name_x = si32{ 7 };  // q_exp_x + q_exp_w + 1.
    static constexpr auto q_col_min = si32{ 2 }; // Minimum resizable-column width.
    static constexpr auto q_col_max = si32{ 200 };
    static constexpr auto q_reason_w0 = si32{ 20 }; // Initial Failed-tab "Reason" column width.

    // Width of the Failed tab's rightmost "Reason" column: a fixed initial width (long errors are
    // tail-truncated with '…'), overridden once the user drags its border. Shared by queue_layout
    // and the renderer so geometry and paint agree. 0 off the Failed tab.
    inline auto queue_reason_w(queue_state const& st) -> si32
    {
        if (st.tab != 1 || !st.ctrl) return 0;
        return st.reason_w_override > 0 ? st.reason_w_override : q_reason_w0;
    }

    // Is logical column `i` currently visible? 0..q_ncol-1 are the resizable columns (toggled via
    // ctrl->col_shown); q_ncol is the Reason column, shown only on the Failed tab when enabled.
    inline auto q_col_visible(queue_state const& st, si32 i) -> bool
    {
        if (!st.ctrl || i < 0 || i > q_ncol) return faux;
        if (i == q_ncol) return st.tab == 1 && st.ctrl->col_shown[(size_t)q_ncol];
        return st.ctrl->col_shown[(size_t)i];
    }
    // Width of logical column `i` (its resizable width, or the Reason width for q_ncol).
    inline auto q_col_w(queue_state const& st, si32 i) -> si32 { return i < q_ncol ? st.col_w[(size_t)i] : queue_reason_w(st); }
    // Content-coordinate x of the left edge of logical column `i`, skipping hidden columns; -1 when
    // `i` itself is hidden (so render call-sites can guard and handlers never match it).
    inline auto q_col_x(queue_state const& st, si32 i) -> si32
    {
        if (!q_col_visible(st, i)) return -1;
        auto x = q_name_x;
        for (auto j = si32{}; j < i && j < q_ncol; ++j) if (q_col_visible(st, j)) x += st.col_w[(size_t)j];
        return x;
    }
    // Content-coordinate x of the right edge (border cell) of column `i`; -1 when `i` is hidden.
    inline auto q_border_cx(queue_state const& st, si32 i) -> si32
    {
        auto x = q_col_x(st, i);
        return x < 0 ? -1 : x + q_col_w(st, i) - 1;
    }
    // Logical column count to scan for borders (Reason adds one more on the Failed tab). Hidden
    // columns within the range report a -1 border position and are skipped by every caller.
    inline auto q_border_count(queue_state const& st) -> si32 { return st.tab == 1 ? q_ncol + 1 : q_ncol; }
    // Right edge of the last visible column (content width) — for layout, the HSB and the row highlight.
    inline auto queue_content_w(queue_state const& st) -> si32
    {
        auto x = q_name_x;
        for (auto i = si32{}; i < q_ncol; ++i) if (q_col_visible(st, i)) x += st.col_w[(size_t)i];
        if (q_col_visible(st, q_ncol)) x += queue_reason_w(st);
        return x;
    }

    // Screen width of a transfer-row's selection highlight / hit-box: the visible columns only,
    // ending at the last column's right border so the area beyond it stays blank (a "blank area"
    // for right-click purposes). content_w is exactly that right edge plus the left margin.
    inline auto q_row_w(queue_state const& st) -> si32
    {
        return std::clamp(st.content_w - st.hscroll, 0, st.disp_w);
    }

    // Display-cell width of the widest content in transfer column `col` across the active tab's
    // items, floored by the column header. Drives the double-click auto-fit. `col` is 0..q_ncol-1,
    // or q_ncol for the Failed tab's Reason column. Mirrors the renderer's per-cell formatting.
    inline auto queue_col_content_w(queue_state const& st, si32 col) -> si32
    {
        auto w = cell_width(q_headers[(size_t)col]);
        if (!st.ctrl) return w;
        auto pct = [](double p) -> text { auto b = std::array<char, 24>{}; std::snprintf(b.data(), b.size(), "%.2f%%", p); return text{ b.data() }; };
        for (auto& it : st.ctrl->queue)
        {
            if (!tab_status_match(st.tab, it)) continue;
            auto s = text{};
            switch (col)
            {
                case 0:  s = it.local_path;  break;
                case 1:  s = it.remote_path; break;
                case 2:  s = human_size(it.size); break;
                case 3:  s = it.status == queue_item::queued ? (it.paused ? text{ "paused" } : text{ "queued" })
                                                             : pct(it.size > 0 ? 100.0 * (double)it.done / (double)it.size : 0.0); break;
                case 4:  if (it.rate.speed > 0.0) s = human_size((si64)(it.rate.speed + 0.5)) + "/s"; break;
                default: s = it.error; break; // col == q_ncol: Reason.
            }
            w = std::max(w, cell_width(s));
            // Expanded parallel subtasks add a "Part X/Y (range)" label in the Local Name column.
            if (col == 0 && it.expanded && it.chunk_count > 1)
            {
                auto ranges = chunk_ranges(it.size, it.chunk_count);
                for (auto c = si32{}; c < (si32)ranges.size(); ++c)
                {
                    auto [start, end] = ranges[c];
                    auto label = "Part " + std::to_string(c + 1) + "/" + std::to_string(it.chunk_count)
                               + "  (" + human_size(start) + "–" + human_size(end) + ")";
                    w = std::max(w, cell_width(label));
                }
            }
        }
        return w;
    }

    // Settle the body window and which scrollbars are needed into st's cache, given the body's
    // first row (`body_top`), its content-row count (`total`), and its content width
    // (`content_w`). Two refine passes settle the VSB<->HSB interplay (each can force the other
    // by stealing a cell). Shared by the transfer tabs and the Message-log tab.
    inline void queue_layout_core(queue_state& st, si32 w, si32 h, si32 total, si32 content_w, si32 body_top)
    {
        st.total     = total;
        st.tab_row   = h - 1;
        st.body_top  = body_top;
        st.content_w = content_w;
        auto avail = std::max(0, st.tab_row - st.body_top);
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
        st.vsb_x = w - 1;
        st.hsb_y = st.body_top + st.body_rows; // Row directly below the last body row.
    }
    // Recompute the transfer-tab geometry into st's cache. Content width is the sum of the
    // resizable columns (+ the Failed tab's Reason), and the body starts at row 2 (row 0 =
    // handle bar, row 1 = column header). `total` is the flattened display-row count.
    inline void queue_layout(queue_state& st, si32 w, si32 h, si32 total)
    {
        queue_layout_core(st, w, h, total, queue_content_w(st), /*body_top=*/2);
    }

    // Scrollbar geometry derived purely from st's render cache, so the mouse handlers and
    // the painter agree. `q_sb` doubles for both axes: for the VSB, {x,top} is the track's
    // column/first-row and {track_h,thumb_h,thumb_y} run vertically; for the HSB, {top} is
    // the row, {x} the first column, and the same fields run horizontally.
    struct q_sb { bool ok = faux; si32 x = 0, top = 0, track_h = 0, thumb_y = 0, thumb_h = 0, maxscroll = 0; };
    inline auto queue_vsb(queue_state const& st) -> q_sb
    {
        auto sb = q_sb{};
        sb.ok = st.has_vsb && st.body_rows > 0 && st.total > st.body_rows;
        if (!sb.ok) return sb;
        sb.x         = st.vsb_x;
        sb.top       = st.body_top;
        sb.track_h   = st.body_rows;
        sb.thumb_h   = std::max(1, st.body_rows * st.body_rows / st.total);
        sb.maxscroll = st.total - st.body_rows;
        sb.thumb_y   = sb.top + (st.body_rows - sb.thumb_h) * st.scroll / sb.maxscroll;
        return sb;
    }
    inline auto queue_hsb(queue_state const& st) -> q_sb
    {
        auto sb = q_sb{};
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
    inline void queue_vsb_scroll_to(queue_state& st, si32 local_y, q_sb const& sb)
    {
        auto travel = sb.track_h - sb.thumb_h;
        if (travel <= 0) return;
        st.scroll = std::clamp((local_y - st.sb_grab - sb.top) * sb.maxscroll / travel, 0, sb.maxscroll);
    }
    inline void queue_hsb_scroll_to(queue_state& st, si32 local_x, q_sb const& sb)
    {
        auto travel = sb.track_h - sb.thumb_h;
        if (travel <= 0) return;
        st.hscroll = std::clamp((local_x - st.hsb_grab - sb.x) * sb.maxscroll / travel, 0, sb.maxscroll);
    }

    // Paint the active tab's scrollbars, command_bar-style (see tile.hpp): a half-block glyph
    // painted in the foreground over the panel background — thin while idle (▐ vertical / ▂
    // horizontal) and thickening to a full block (█ / ▄) on hover/drag. Track and thumb share
    // the glyph and differ only by colour: the track stays theme::sb_track while the thumb
    // brightens on hover and turns accent-blue while dragging (theme::sb_*). Shared by every tab.
    inline void queue_paint_scrollbars(queue_state const& st, auto& canvas)
    {
        if (auto sb = queue_vsb(st); sb.ok)
        {
            auto mark = (st.sb_drag || st.sb_hover) ? "\xe2\x96\x88" : "\xe2\x96\x90"; // █ : ▐
            canvas.fill(rect{{ sb.x, sb.top }, { 1, sb.track_h }}, [&](cell& c){ c.bgc(theme::bg).fgc(theme::sb_track).txt(mark); });
            auto tc = st.sb_drag ? ui32{ theme::sb_drag } : st.sb_hover ? ui32{ theme::sb_hover } : ui32{ theme::sb_thumb };
            canvas.fill(rect{{ sb.x, sb.thumb_y }, { 1, sb.thumb_h }}, [&](cell& c){ c.bgc(theme::bg).fgc(tc).txt(mark); });
        }
        if (auto sb = queue_hsb(st); sb.ok)
        {
            auto mark = (st.hsb_drag || st.hsb_hover) ? "\xe2\x96\x84" : "\xe2\x96\x82"; // ▄ : ▂
            canvas.fill(rect{{ sb.x, sb.top }, { sb.track_h, 1 }}, [&](cell& c){ c.bgc(theme::bg).fgc(theme::sb_track).txt(mark); });
            auto tc = st.hsb_drag ? ui32{ theme::sb_drag } : st.hsb_hover ? ui32{ theme::sb_hover } : ui32{ theme::sb_thumb };
            canvas.fill(rect{{ sb.x + sb.thumb_y, sb.top }, { sb.thumb_h, 1 }}, [&](cell& c){ c.bgc(theme::bg).fgc(tc).txt(mark); });
        }
    }

    inline void queue_render(queue_state& st, auto& canvas, twod size)
    {
        auto w = size.x;
        auto h = size.y;
        if (w <= 0 || h <= 0 || !st.ctrl) return;
        canvas.fill(rect{{ 0, 0 }, { w, h }}, [&](cell& c){ c.bgc(theme::bg).fgc(theme::text_fg); });

        auto& q = st.ctrl->queue;
        auto n_xfer = si32{}, n_fail = si32{}, n_done = si32{};
        for (auto& it : q)
        {
                 if (it.status == queue_item::failed)    n_fail++;
            else if (it.status == queue_item::succeeded) n_done++;
            else                                         n_xfer++;
        }

        auto tab_row = h - 1; // Tabs are pinned to the bottom; row 0 is the resize handle.

        // Row 0: empty handle bar (vertical resize only); lightens on hover.
        canvas.fill(rect{{ 0, 0 }, { w, 1 }}, [&](cell& c){ c.bgc(theme::surface); });
        if (st.hover) canvas.fill(rect{{ 0, 0 }, { w, 1 }}, [](cell& c){ c.xlight(); });

        // Hit caches are rebuilt each render; clear them up front so a stale entry from a
        // previous tab/layout can never fire in the mouse handlers.
        st.expand_hit.clear();
        st.row_hit.clear();

        // Body (rows 1 .. tab_row-1).
        if (st.tab == 3) // Message log: a scrolling, typed, colour-coded protocol log.
        {
            // Gather the visible lines (newest last) honouring "Show detailed log".
            auto vis = std::vector<sftp_remote::logline const*>{};
            for (auto& ln : st.ctrl->logbuf)
                if (log_visible(ln, st.ctrl->show_detailed, st.ctrl->debug_level)) vis.push_back(&ln);

            // Content width = a one-cell left margin + the widest visible line, so the HSB is
            // sized to the longest line and short lines never force a horizontal scroll.
            auto content_w = si32{};
            for (auto* ln : vis) content_w = std::max(content_w, cell_width(log_format(*ln, st.ctrl->show_stamps)));
            if (content_w) content_w += 1; // Leading margin (lines render from x = 1).

            queue_layout_core(st, w, h, (si32)vis.size(), content_w, /*body_top=*/1);

            // Vertical tail-follow: with follow engaged keep the newest line pinned to the
            // bottom; clamp both axes to the freshly settled geometry.
            auto maxscroll = std::max(0, st.total - st.body_rows);
            if (st.follow) st.scroll = maxscroll;
            st.scroll  = std::clamp(st.scroll, 0, maxscroll);
            st.hscroll = std::clamp(st.hscroll, 0, std::max(0, st.content_w - st.disp_w));

            if (vis.empty())
            {
                put_str(canvas, 1, 1, "Status: idle.", theme::subtext, theme::bg, std::max(1, st.disp_w - 1));
            }
            else if (st.body_rows > 0)
            {
                auto hs    = st.hscroll;
                auto clipw = st.disp_w; // Right clip: keep painted cells off the VSB column.
                // Paint one coloured segment shifted by the horizontal scroll and clipped to the
                // body; returns the segment's full width so the next segment positions after it.
                auto seg = [&](si32 content_x, si32 y, view s, ui32 fg) -> si32
                {
                    auto cw   = cell_width(s);
                    auto sx   = content_x - hs;
                    auto room = std::min(cw, clipw - sx);
                    if (room > 0 && sx < clipw) put_str(canvas, sx, y, s, fg, theme::bg, room);
                    return cw;
                };
                auto first = st.scroll;
                auto last  = std::min((si32)vis.size(), st.scroll + st.body_rows);
                for (auto i = first; i < last; ++i)
                {
                    auto& ln = *vis[i];
                    auto  y  = st.body_top + (i - first);
                    auto  x  = si32{ 1 }; // Content x; one-cell left margin (matches content_w).
                    auto  fg = log_color(ln.type);
                    if (st.ctrl->show_stamps) x += seg(x, y, ln.stamp + " ", theme::subtext);
                    x += seg(x, y, log_prefix(ln.type), fg);
                    seg(x, y, ln.body, fg);
                }
            }
            queue_paint_scrollbars(st, canvas);
        }
        else // Transfers: a scrollable, resizable-column table over the active tab.
        {
            auto rows = std::vector<disp_row>{};
            build_rows(st, rows);
            queue_layout(st, w, h, (si32)rows.size());

            // Vertical scroll: clamp, and (when following) keep the active transfer in view —
            // pinned to the bottom when there is no active item (newest-at-bottom tail).
            auto maxscroll = std::max(0, st.total - st.body_rows);
            if (st.follow)
            {
                auto active_row = si32{ -1 };
                for (auto i = si32{}; i < (si32)rows.size(); ++i)
                    if (rows[i].child == -1 && rows[i].qi == st.ctrl->active) { active_row = i; break; }
                st.scroll = active_row >= 0 ? std::clamp(active_row - st.body_rows + 1, 0, maxscroll) : maxscroll;
            }
            st.scroll  = std::clamp(st.scroll, 0, maxscroll);
            st.hscroll = std::clamp(st.hscroll, 0, std::max(0, st.content_w - st.disp_w));

            // Per-column content-x (Local Name, Remote Name, Size, Progress, Speed, Reason). -1 marks
            // a hidden column, so every render call-site is guarded by `cx[i] >= 0`.
            auto cx = std::array<si32, q_ncol + 1>{};
            for (auto i = si32{}; i <= q_ncol; ++i) cx[(size_t)i] = q_col_x(st, i);
            auto reason_w  = queue_reason_w(st);
            auto hs    = st.hscroll;
            auto clipw = st.disp_w; // Right clip: keep painted cells off the VSB column.
            // Paint one column's text, shifted by the horizontal scroll and clipped to the body.
            // Content wider than the column is tail-truncated with '…' (fit_ellipsis).
            auto col = [&](si32 content_x, si32 colw, si32 y, view s, ui32 fg, ui32 bg)
            {
                auto sx   = content_x - hs;
                auto room = std::min(colw, clipw - sx);
                if (room <= 0 || sx >= clipw) return;
                auto t = fit_ellipsis(s, colw);
                put_str(canvas, sx, y, t, fg, bg, room);
            };
            // Right-align text within a column: the numeric Size/Progress/Speed columns flush
            // their value to the right edge, leaving a one-cell gap before the border marker.
            auto colr = [&](si32 content_x, si32 colw, si32 y, view s, ui32 fg, ui32 bg)
            {
                auto avail = std::max(1, colw - 1);
                auto t     = fit_ellipsis(s, avail);
                auto tw    = cell_width(t);
                col(content_x + (avail - tw), tw, y, t, fg, bg);
            };

            // Header row 1: column titles. Each column reserves its right-edge cell for the border
            // marker, which is painted (down the body too) by the divider pass after the rows.
            if (st.body_top > 1)
            {
                canvas.fill(rect{{ 0, 1 }, { st.disp_w, 1 }}, [&](cell& c){ c.bgc(theme::header); });
                if (cx[0] >= 0) col (cx[0], st.col_w[0] - 1, 1, "Local Name",  theme::subtext, theme::header);
                if (cx[1] >= 0) col (cx[1], st.col_w[1] - 1, 1, "Remote Name", theme::subtext, theme::header);
                if (cx[2] >= 0) colr(cx[2], st.col_w[2],     1, "Size",        theme::subtext, theme::header);
                if (cx[3] >= 0) colr(cx[3], st.col_w[3],     1, "Progress",    theme::subtext, theme::header);
                if (cx[4] >= 0) colr(cx[4], st.col_w[4],     1, "Speed",       theme::subtext, theme::header);
                if (cx[q_ncol] >= 0) col(cx[q_ncol], reason_w - 1, 1, "Reason", theme::subtext, theme::header);
            }

            if (rows.empty())
            {
                if (st.body_rows > 0)
                    put_str(canvas, 1, st.body_top, "(no transfers — press Enter on a file to queue one)", theme::subtext, theme::bg, std::max(1, st.disp_w - 1));
            }
            else
            {
                // Format a two-decimal percentage ("45.30%") into a reusable string.
                auto pct_str = [](double pct) -> text
                {
                    auto buf = std::array<char, 24>{};
                    std::snprintf(buf.data(), buf.size(), "%.2f%%", pct);
                    return text{ buf.data() };
                };
                auto first     = st.scroll;
                auto last      = std::min((si32)rows.size(), st.scroll + st.body_rows);
                auto rubber_on = st.drag == queue_state::d_rubber && st.rubber_a >= 0;
                auto rlo       = std::min(st.rubber_a, st.rubber_b);
                auto rhi       = std::max(st.rubber_a, st.rubber_b);
                for (auto i = first; i < last; ++i)
                {
                    auto  y     = st.body_top + (i - first);
                    auto  qi    = rows[i].qi;
                    auto& it    = st.ctrl->queue[qi];
                    auto  child = rows[i].child;
                    // Selection highlight (parent rows only; muted sel_bg keeps the multi-
                    // coloured cells readable, with a blue left accent when the panel is focused).
                    auto in_rubber = rubber_on && i >= rlo && i <= rhi;
                    auto sel    = child == -1 && (it.selected || in_rubber);
                    auto row_bg = sel ? ui32{ theme::sel_bg } : ui32{ theme::bg };
                    if (sel)
                    {
                        canvas.fill(rect{{ 0, y }, { q_row_w(st), 1 }}, [&](cell& c){ c.bgc(theme::sel_bg); }); // Ends at the last column.
                        if (st.focused) canvas.fill(rect{{ 0, y }, { 1, 1 }}, [&](cell& c){ c.bgc(theme::sel_bg_act); });
                    }
                    auto dir_fg = it.download ? ui32{ theme::dir_fg } : ui32{ theme::link_fg };
                    if (child == -1) // Parent task row.
                    {
                        auto arrow = it.download ? text{ "↓" } : text{ "↑" }; // down / up
                        col(q_dir_x, 2, y, arrow, dir_fg, row_bg);
                        // Expand (+/-) button: enabled (sel_bg fill, hover-brightens; a held press
                        // doubles the lift for a "pushed" look, like the connect bar's buttons) for
                        // parallel items; a muted, hitbox-free "+" for single-connection items.
                        auto parallel = it.chunk_count > 1;
                        auto bx = q_exp_x - hs;
                        if (parallel)
                        {
                            if (bx >= 0 && bx + q_exp_w <= clipw)
                            {
                                auto btn = rect{{ bx, y }, { q_exp_w, 1 }};
                                canvas.fill(btn, [&](cell& c){ c.bgc(theme::sel_bg); });
                                put_str(canvas, bx, y, it.expanded ? " - " : " + ", theme::text_fg, theme::sel_bg, q_exp_w);
                                if      (st.press_expand == qi) canvas.fill(btn, [](cell& c){ c.xlight(2); });
                                else if (st.hover_expand == qi) canvas.fill(btn, [](cell& c){ c.xlight(); });
                                st.expand_hit.emplace_back(btn, qi);
                            }
                        }
                        else col(q_exp_x, q_exp_w, y, " + ", theme::subtext, row_bg);
                        // Local Name / Remote Name: the full path in each (-1: keep the border cell clear).
                        if (cx[0] >= 0) col(cx[0], st.col_w[0] - 1, y, it.local_path,  dir_fg, row_bg);
                        if (cx[1] >= 0) col(cx[1], st.col_w[1] - 1, y, it.remote_path, dir_fg, row_bg);
                        if (cx[2] >= 0) colr(cx[2], st.col_w[2], y, human_size(it.size), theme::subtext, row_bg);
                        // Progress: a two-decimal percentage for any active/finished transfer
                        // (succeeded = 100.00%, failed shows how far it got); queued shows a label.
                        auto prog = text{};
                        auto pc   = ui32{ theme::text_fg };
                        if (it.status == queue_item::queued) { prog = it.paused ? "paused" : "queued"; pc = theme::subtext; }
                        else
                        {
                            prog = pct_str(it.size > 0 ? 100.0 * (double)it.done / (double)it.size : 0.0);
                            if      (it.status == queue_item::succeeded) pc = theme::dir_fg;
                            else if (it.status == queue_item::failed)    pc = theme::err_fg;
                        }
                        if (cx[3] >= 0) colr(cx[3], st.col_w[3], y, prog, pc, row_bg);
                        if (cx[4] >= 0 && it.rate.speed > 0.0)
                            colr(cx[4], st.col_w[4], y, human_size((si64)(it.rate.speed + 0.5)) + "/s", theme::subtext, row_bg);
                        if (cx[q_ncol] >= 0 && it.status == queue_item::failed && !it.error.empty())
                            col(cx[q_ncol], reason_w - 1, y, it.error, theme::err_fg, row_bg); // -1: keep the border cell clear.
                        st.row_hit.emplace_back(rect{{ 0, y }, { q_row_w(st), 1 }}, qi); // Hit-box ends at the last column; the blank area beyond it is not part of the row.
                    }
                    else // Expanded parallel-subtask child row.
                    {
                        auto ranges = chunk_ranges(it.size, it.chunk_count);
                        if (child < (si32)ranges.size())
                        {
                            auto [start, end] = ranges[child];
                            auto label = "Part " + std::to_string(child + 1) + "/" + std::to_string(it.chunk_count)
                                       + "  (" + human_size(start) + "–" + human_size(end) + ")";
                            if (cx[0] >= 0) col(cx[0], st.col_w[0] - 1, y, label, theme::subtext, row_bg); // -1: keep the border cell clear.
                            auto live  = qi == st.ctrl->active && st.ctrl->workers.size() == ranges.size();
                            auto cprog = text{};
                            auto cc    = ui32{ theme::text_fg };
                            if (live)
                            {
                                auto& wkr = *st.ctrl->workers[child];
                                auto full = (double)(end - start);
                                cprog = pct_str(full > 0.0 ? 100.0 * (double)wkr.done / full : 0.0);
                                if      (wkr.state == xfer_worker::s_ok)  cc = theme::dir_fg;
                                else if (wkr.state == xfer_worker::s_err) cc = theme::err_fg;
                            }
                            else if (it.status == queue_item::succeeded) { cprog = "100.00%"; cc = theme::dir_fg; }
                            else if (it.status == queue_item::queued)    { cprog = "queued";  cc = theme::subtext; }
                            else if (it.status == queue_item::failed)    { cprog = "—";       cc = theme::err_fg; }
                            if (cx[3] >= 0 && !cprog.empty()) colr(cx[3], st.col_w[3], y, cprog, cc, row_bg);
                        }
                    }
                }
            }

            // Column dividers: the draggable border markers extended down through the visible item
            // rows so each handle spans its whole column. Painted last, preserving every cell's bg
            // (header surface / row / selection) — only the fg + '│' glyph are stamped.
            auto rows_drawn = std::clamp(st.total - st.scroll, 0, st.body_rows);
            st.div_bottom = st.body_top + rows_drawn; // Exclusive bottom row (header row 1 + item rows).
            if (st.body_top > 1)
                for (auto i = si32{}; i < q_border_count(st); ++i)
                {
                    auto bx = q_border_cx(st, i) - hs;
                    if (bx < 0 || bx >= clipw) continue;
                    auto fg = (st.hover_border == i || st.col_drag == i) ? ui32{ theme::text_fg } : ui32{ theme::subtext };
                    canvas.fill(rect{{ bx, 1 }, { 1, st.div_bottom - 1 }}, [&](cell& c){ c.fgc(fg).txt("\xE2\x94\x82"); }); // │
                }

            queue_paint_scrollbars(st, canvas);
        }

        // Tab strip pinned to the bottom (row tab_row). The selected tab takes the panel's
        // primary background so it reads as part of the body; the rest keep the bar surface.
        canvas.fill(rect{{ 0, tab_row }, { w, 1 }}, [&](cell& c){ c.bgc(theme::surface); });
        auto labels = std::array<text, 4>{ "Transferring (" + std::to_string(n_xfer) + ")",
                                           "Failed (" + std::to_string(n_fail) + ")",
                                           "Succeeded (" + std::to_string(n_done) + ")",
                                           "Message log" };
        auto x = si32{ 0 };
        for (auto i = si32{}; i < 4; ++i)
        {
            auto active = st.tab == i;
            auto fg = active ? ui32{ theme::text_fg } : ui32{ theme::subtext };
            auto bg = active ? ui32{ theme::bg }      : ui32{ theme::surface };
            auto bw = (si32)labels[i].size() + 2;
            st.tabbox[i] = rect{{ x, tab_row }, { bw, 1 }};
            canvas.fill(st.tabbox[i], [&](cell& c){ c.bgc(bg); });
            put_str(canvas, x + 1, tab_row, labels[i], fg, bg, bw);
            x += bw;
        }
    }

    inline auto make_queue_panel(sftp_remote* ctrl, netxs::wptr<ui::fork> resize_target) -> ui::sptr
    {
        auto panel = ui::mock::ctor()
            ->active()
            ->plugin<pro::mouse>()
            ->plugin<pro::focus>(pro::focus::mode::focusable)
            ->plugin<pro::keybd>();
        panel->invoke([&](auto& boss)
        {
            auto& st = boss.base::field(queue_state{});
            st.ctrl = ctrl;
            boss.LISTEN(tier::release, e2::render::any, parent_canvas)
            {
                queue_render(st, parent_canvas, boss.base::size());
            };
            boss.LISTEN(tier::release, e2::form::state::focus::count, count)
            {
                st.focused = !!count;
                boss.base::deface();
            };
            // Select rows and switch tabs on press (mousedown), not on the completed click
            // (mirrors the file panes, see panes.hpp). Presses on the scrollbar tracks, the
            // +/- buttons and the column borders are left to the click/drag handlers.
            boss.on(tier::mouserelease, input::key::LeftDown, [&](hids& gear)
            {
                pro::focus::set(boss.This(), gear.id, solo::on);
                auto mx = (si32)gear.coord.x;
                auto my = (si32)gear.coord.y;
                auto tab_row = boss.base::size().y - 1; // Tabs live on the bottom row.
                // Snapshot the pre-press selection so a Ctrl+drag that follows can merge its
                // swept span onto it (the selection below mutates on this very press).
                if (st.ctrl && st.tab != 3)
                {
                    st.drag_base.clear();
                    for (auto i = si32{}; i < (si32)st.ctrl->queue.size(); ++i) if (st.ctrl->queue[i].selected) st.drag_base.insert(i);
                }
                // Presses on either scrollbar track are left to the thumb-drag / rail-click handlers.
                if (auto sb = queue_vsb(st); sb.ok && mx == sb.x && my >= sb.top && my < sb.top + sb.track_h) return;
                if (auto sb = queue_hsb(st); sb.ok && my == sb.top && mx >= sb.x && mx < sb.x + sb.track_h) return;
                // A press on a row's +/- button is left to the click handler: toggling the
                // expanded subtasks must not change the row selection. The press itself only
                // arms the doubled-xlight "pushed" feedback painted by the render.
                for (auto& [b, idx] : st.expand_hit)
                    if (my == b.coor.y && mx >= b.coor.x && mx < b.coor.x + b.size.x)
                    {
                        if (st.press_expand != idx) { st.press_expand = idx; boss.base::deface(); }
                        return;
                    }
                // A press on a column border (the resize handle, extended down the body) is
                // left to the drag handler; never let it fall through to row selection.
                if (st.tab != 3 && my >= 1 && my < st.div_bottom)
                    for (auto i = si32{}; i < q_border_count(st); ++i)
                        if (mx == q_border_cx(st, i) - st.hscroll) return;
                // Row selection (transfer tabs): plain = pick one; Ctrl = toggle; Shift = range
                // from the anchor; a plain press on empty body area clears the selection (a Ctrl
                // press keeps it so a Ctrl+drag from blank can add the swept rows onto it).
                if (st.tab != 3)
                {
                    auto& qv  = st.ctrl->queue;
                    auto ctl  = !!(gear.ctlstat & hids::anyCtrl);
                    auto shft = !!(gear.ctlstat & hids::anyShift);
                    auto hit  = si32{ -1 };
                    for (auto& [b, idx] : st.row_hit)
                        if (my == b.coor.y && mx >= b.coor.x && mx < b.coor.x + b.size.x) { hit = idx; break; }
                    if (hit >= 0)
                    {
                        if (shft && st.sel_anchor >= 0 && st.sel_anchor < (si32)qv.size())
                        {
                            auto lo = std::min(st.sel_anchor, hit), hi = std::max(st.sel_anchor, hit);
                            for (auto j = si32{}; j < (si32)qv.size(); ++j)
                                qv[j].selected = j >= lo && j <= hi && item_in_tab(st, qv[j]);
                        }
                        else if (ctl) { qv[hit].selected = !qv[hit].selected; st.sel_anchor = hit; }
                        else { for (auto& it : qv) it.selected = faux; qv[hit].selected = true; st.sel_anchor = hit; }
                        boss.base::deface();
                        gear.dismiss();
                        return;
                    }
                    if (!ctl && my >= st.body_top && my < st.tab_row)
                    {
                        auto any = faux;
                        for (auto& it : qv) { any |= it.selected; it.selected = faux; }
                        if (any) { st.sel_anchor = -1; boss.base::deface(); gear.dismiss(); return; }
                    }
                }
                if (my == tab_row) for (auto i = si32{}; i < 4; ++i)
                {
                    auto& b = st.tabbox[i];
                    if (mx >= b.coor.x && mx < b.coor.x + b.size.x) st.tab = i;
                }
                boss.base::deface();
                gear.dismiss();
            });
            boss.on(tier::mouserelease, input::key::LeftUp, [&](hids&)
            {
                if (st.press_expand != -1) { st.press_expand = -1; boss.base::deface(); }
            });
            boss.on(tier::mouserelease, input::key::LeftClick, [&](hids& gear)
            {
                // Re-assert focus on the click release: the pro::focus plugin's Ctrl+LeftClick
                // handler toggles focus off when the panel is already focused (the selection
                // itself happens on LeftDown above).
                pro::focus::set(boss.This(), gear.id, solo::on);
                auto mx = (si32)gear.coord.x;
                auto my = (si32)gear.coord.y;
                // A click on the scrollbar track outside the thumb pages the view by one
                // screenful (command_bar style, see tile.hpp): on the VSB clicking above the
                // thumb pages up and below pages down; on the HSB the same to the left/right.
                // Press-and-drag on the track is handled separately by drag::start (snap-to-cursor).
                if (auto sb = queue_vsb(st); sb.ok && mx == sb.x && my >= sb.top && my < sb.top + sb.track_h)
                {
                    auto page = std::max(1, st.body_rows);
                    if (my < sb.thumb_y)                    st.scroll = std::clamp(st.scroll - page, 0, sb.maxscroll);
                    else if (my >= sb.thumb_y + sb.thumb_h) st.scroll = std::clamp(st.scroll + page, 0, sb.maxscroll);
                    st.follow = st.scroll == sb.maxscroll;
                    boss.base::deface();
                    gear.dismiss();
                    return;
                }
                if (auto sb = queue_hsb(st); sb.ok && my == sb.top && mx >= sb.x && mx < sb.x + sb.track_h)
                {
                    auto page = std::max(1, st.disp_w);
                    auto tx   = sb.x + sb.thumb_y; // Thumb's first column on the track.
                    if (mx < tx)                    st.hscroll = std::clamp(st.hscroll - page, 0, sb.maxscroll);
                    else if (mx >= tx + sb.thumb_h) st.hscroll = std::clamp(st.hscroll + page, 0, sb.maxscroll);
                    boss.base::deface();
                    gear.dismiss();
                    return;
                }
                // A click on a row's +/- button toggles that item's expanded subtasks.
                for (auto& [b, idx] : st.expand_hit)
                    if (my == b.coor.y && mx >= b.coor.x && mx < b.coor.x + b.size.x)
                    {
                        if (idx >= 0 && idx < (si32)st.ctrl->queue.size()) st.ctrl->queue[idx].expanded = !st.ctrl->queue[idx].expanded;
                        boss.base::deface();
                        gear.dismiss();
                        return;
                    }
                gear.dismiss();
            });
            // Right-click context menus, reusing application.hpp's dropdown machinery (same as the
            // connect bar's ▾ history button). The Message-log tab keeps its FileZilla-style log
            // menu; the transfer tabs add a per-item menu (Start/Pause/Remove/Pin to Top) over an
            // item row and a blank-area menu (Start All/Pause All/Remove All) elsewhere in the body.
            boss.on(tier::mouserelease, input::key::RightClick, [&](hids& gear)
            {
                if (!st.ctrl) { gear.dismiss(); return; }
                pro::focus::set(boss.This(), gear.id, solo::on); // Right-clicking activates the queue panel.
                auto mx = (si32)gear.coord.x;
                auto my = (si32)gear.coord.y;
                auto at = twod{ mx, my };
                if (st.tab == 3)
                {
                    app::shared::menu::open_dropdown_popup(boss,
                        build_log_menu(st.ctrl, ptr::shadow(boss.This())), faux, -1, at);
                    gear.dismiss();
                    return;
                }
                if (my == 1 && st.body_top > 1) // Column header row: choose which columns are shown.
                {
                    app::shared::menu::open_dropdown_popup(boss,
                        build_queue_columns_menu(st.ctrl, st.tab, ptr::shadow(boss.This())), faux, -1, at);
                    gear.dismiss();
                    return;
                }
                if (my < st.body_top || my >= st.tab_row) { gear.dismiss(); return; } // Only inside the table body.
                // Hit-test the columns area (row_hit ends at the last column). A hit opens the
                // per-item menu; the blank area to the right of / below the columns opens the All menu.
                auto hit = si32{ -1 };
                for (auto& [b, idx] : st.row_hit)
                    if (my == b.coor.y && mx >= b.coor.x && mx < b.coor.x + b.size.x) { hit = idx; break; }
                if (hit >= 0)
                {
                    // If the right-clicked row is not already part of the selection, make it the sole
                    // selection; otherwise keep the existing (multi-)selection so the action applies to all.
                    if (!st.ctrl->queue[hit].selected)
                    {
                        for (auto& it : st.ctrl->queue) it.selected = faux;
                        st.ctrl->queue[hit].selected = true;
                        st.sel_anchor = hit;
                        boss.base::deface();
                    }
                    app::shared::menu::open_dropdown_popup(boss,
                        build_queue_item_menu(st.ctrl, st.tab, ptr::shadow(boss.This())), faux, -1, at);
                }
                else
                {
                    // Right-clicking the blank area clears the selection (like left-click).
                    auto any = faux;
                    for (auto& it : st.ctrl->queue) { any |= it.selected; it.selected = faux; }
                    if (any) { st.sel_anchor = -1; boss.base::deface(); }
                    app::shared::menu::open_dropdown_popup(boss,
                        build_queue_all_menu(st.ctrl, st.tab, ptr::shadow(boss.This())), faux, -1, at);
                }
                gear.dismiss();
            });
            // Hover highlight on the tab bar (row 0) and on the row +/- buttons.
            boss.on(tier::mouserelease, input::key::MouseMove, [&](hids& gear)
            {
                auto mx = (si32)gear.coord.x;
                auto my = (si32)gear.coord.y;
                auto h = my == 0;
                if (st.hover != h) { st.hover = h; boss.base::deface(); }
                auto over = si32{ -1 };
                for (auto& [b, idx] : st.expand_hit)
                    if (my == b.coor.y && mx >= b.coor.x && mx < b.coor.x + b.size.x) { over = idx; break; }
                if (st.hover_expand != over) { st.hover_expand = over; boss.base::deface(); }
                if (st.press_expand != -1 && st.press_expand != over) { st.press_expand = -1; boss.base::deface(); } // Drag-off cancels.
                // Scrollbar + resizable-border hover feedback (transfer tabs).
                auto vsb  = queue_vsb(st);
                auto nsb  = vsb.ok && mx == vsb.x && my >= vsb.top && my < vsb.top + vsb.track_h;
                if (st.sb_hover != nsb) { st.sb_hover = nsb; boss.base::deface(); }
                auto hsb  = queue_hsb(st);
                auto nhsb = hsb.ok && my == hsb.top && mx >= hsb.x && mx < hsb.x + hsb.track_h;
                if (st.hsb_hover != nhsb) { st.hsb_hover = nhsb; boss.base::deface(); }
                auto nb = si32{ -1 };
                if (st.tab != 3 && my >= 1 && my < st.div_bottom) // Whole divider length (header + item rows).
                    for (auto i = si32{}; i < q_border_count(st); ++i) if (mx == q_border_cx(st, i) - st.hscroll) { nb = i; break; }
                if (st.hover_border != nb) { st.hover_border = nb; boss.base::deface(); }
            });
            boss.on(tier::mouserelease, input::key::MouseLeave, [&](hids&)
            {
                if (st.hover) { st.hover = faux; boss.base::deface(); }
                if (st.hover_expand != -1) { st.hover_expand = -1; boss.base::deface(); }
                if (st.press_expand != -1) { st.press_expand = -1; boss.base::deface(); }
                if (st.sb_hover)  { st.sb_hover = faux;  boss.base::deface(); }
                if (st.hsb_hover) { st.hsb_hover = faux; boss.base::deface(); }
                if (st.hover_border != -1) { st.hover_border = -1; boss.base::deface(); }
            });
            // Mouse wheel: vertical scroll (or horizontal over/with the HSB). Any wheel scroll
            // disengages tail-follow unless it leaves us pinned to the bottom.
            boss.on(tier::mouserelease, input::key::MouseWheel, [&](hids& gear)
            {
                if (gear.hzwhl || st.hsb_hover)
                {
                    auto maxh = std::max(0, st.content_w - st.disp_w);
                    st.hscroll = std::clamp(st.hscroll - gear.whlsi * 4, 0, maxh);
                }
                else
                {
                    auto maxv = std::max(0, st.total - st.body_rows);
                    st.scroll = std::clamp(st.scroll - gear.whlsi, 0, maxv);
                    st.follow = st.scroll == maxv;
                }
                boss.base::deface();
            });
            // Left-drag gestures: a single drag::start dispatches by press zone (gear.click is
            // localized to this widget) into the vertical/horizontal scrollbars, a header-border
            // column resize, or a body rubber-band selection; pull/stop drive the chosen mode.
            boss.base::signal(tier::release, e2::form::draggable::_<hids::buttons::left>, true);
            boss.LISTEN(tier::release, e2::form::drag::start::_<hids::buttons::left>, gear)
            {
                if (!st.ctrl) return; // Scrollbar drags work on every tab; the Message log
                                      // has neither resizable columns nor selectable rows.
                auto px = (si32)gear.click.x;
                auto py = (si32)gear.click.y;
                if (auto sb = queue_vsb(st); sb.ok && px == sb.x && py >= sb.top && py < sb.top + sb.track_h)
                {
                    pro::focus::set(boss.This(), gear.id, solo::on);
                    if (py >= sb.thumb_y && py < sb.thumb_y + sb.thumb_h) st.sb_grab = py - sb.thumb_y;
                    else { st.sb_grab = sb.thumb_h / 2; queue_vsb_scroll_to(st, py, sb); }
                    st.sb_drag = st.sb_hover = true;
                    st.drag = queue_state::d_vsb;
                    st.follow = st.scroll == std::max(0, st.total - st.body_rows);
                    boss.base::deface();
                    return;
                }
                if (auto sb = queue_hsb(st); sb.ok && py == sb.top && px >= sb.x && px < sb.x + sb.track_h)
                {
                    pro::focus::set(boss.This(), gear.id, solo::on);
                    auto tx = sb.x + sb.thumb_y;
                    if (px >= tx && px < tx + sb.thumb_h) st.hsb_grab = px - tx;
                    else { st.hsb_grab = sb.thumb_h / 2; queue_hsb_scroll_to(st, px, sb); }
                    st.hsb_drag = st.hsb_hover = true;
                    st.drag = queue_state::d_hsb;
                    boss.base::deface();
                    return;
                }
                if (st.tab == 3) return; // Message log: no column resize / rubber-band selection.
                if (py >= 1 && py < st.div_bottom) // Grab the divider anywhere along its length.
                    for (auto i = si32{}; i < q_border_count(st); ++i)
                        if (px == q_border_cx(st, i) - st.hscroll)
                        {
                            pro::focus::set(boss.This(), gear.id, solo::on);
                            st.col_drag = i;
                            // Seed the Reason width from its current auto-size so dragging border 4 starts smooth.
                            if (i == q_ncol && st.reason_w_override == 0) st.reason_w_override = queue_reason_w(st);
                            st.drag = queue_state::d_col;
                            boss.base::deface();
                            return;
                        }
                if (py >= st.body_top && py < st.body_top + st.body_rows)
                {
                    pro::focus::set(boss.This(), gear.id, solo::on);
                    auto rows = std::vector<disp_row>{};
                    build_rows(st, rows);
                    // Keep the anchor as the pressed display row WITHOUT clamping to the last item:
                    // a press in the blank area below the list anchors on a virtual row, so dragging
                    // back down past every item deselects them all (the last item included).
                    auto r = std::max(0, st.scroll + (py - st.body_top));
                    st.rubber_a = st.rubber_b = r;
                    st.drag = queue_state::d_rubber;
                    auto anchor_qi = (r < (si32)rows.size() && rows[r].child == -1) ? rows[r].qi : -1;
                    // Ctrl+drag merges the swept rows onto the pre-press selection captured by the
                    // LeftDown handler (drag_base; the press itself already mutated the live flags):
                    // add when the anchor row was unselected, remove when it was selected. A plain
                    // drag replaces the selection.
                    st.rubber_ctrl = !!(gear.ctlstat & hids::anyCtrl);
                    if (st.rubber_ctrl) st.rubber_add = !st.drag_base.count(anchor_qi);
                    else for (auto& it : st.ctrl->queue) it.selected = faux;
                    if (anchor_qi >= 0) st.sel_anchor = anchor_qi;
                    boss.base::deface();
                    return;
                }
            };
            boss.LISTEN(tier::release, e2::form::drag::pull::_<hids::buttons::left>, gear)
            {
                auto mx = (si32)gear.coord.x;
                auto my = (si32)gear.coord.y;
                switch (st.drag)
                {
                    case queue_state::d_vsb:
                        queue_vsb_scroll_to(st, my, queue_vsb(st));
                        st.follow = st.scroll == std::max(0, st.total - st.body_rows);
                        boss.base::deface();
                        break;
                    case queue_state::d_hsb:
                        queue_hsb_scroll_to(st, mx, queue_hsb(st));
                        boss.base::deface();
                        break;
                    case queue_state::d_col:
                        // Position-based resize (see panes.hpp): move the border to the cursor's column
                        // (clamped) instead of accumulating deltas, so a handle that bottomed/topped out
                        // waits for the cursor to come back to it rather than drifting at a stale offset.
                        if (st.col_drag >= 0 && st.col_drag <= q_ncol)
                        {
                            auto left = q_col_x(st, st.col_drag); // Stable during the drag (sums only earlier columns).
                            if (left >= 0)
                            {
                                auto w = std::clamp(mx + st.hscroll - left + 1, q_col_min, q_col_max);
                                if (st.col_drag < q_ncol) st.col_w[st.col_drag] = w;
                                else                      st.reason_w_override = w; // Failed-tab Reason column (session-stored override).
                                boss.base::deface();
                            }
                        }
                        break;
                    case queue_state::d_rubber:
                    {
                        auto rows = std::vector<disp_row>{};
                        build_rows(st, rows);
                        // Track the pressed display row across the whole visible body, including the
                        // blank rows below the list, so the span can shrink off the last item.
                        st.rubber_b = st.scroll + std::clamp(my - st.body_top, 0, std::max(0, st.body_rows - 1));
                        auto lo = std::min(st.rubber_a, st.rubber_b);
                        auto hi = std::max(st.rubber_a, st.rubber_b);
                        if (st.rubber_ctrl) // Restore the pre-drag selection, then add/remove the swept rows.
                        {
                            for (auto i = si32{}; i < (si32)st.ctrl->queue.size(); ++i) st.ctrl->queue[i].selected = !!st.drag_base.count(i);
                            for (auto i = si32{}; i < (si32)rows.size(); ++i)
                                if (rows[i].child == -1 && i >= lo && i <= hi) st.ctrl->queue[rows[i].qi].selected = st.rubber_add;
                        }
                        else // Plain band: replace the selection with the swept span.
                        {
                            for (auto& it : st.ctrl->queue) it.selected = faux;
                            for (auto i = si32{}; i < (si32)rows.size(); ++i)
                                if (rows[i].child == -1 && i >= lo && i <= hi) st.ctrl->queue[rows[i].qi].selected = true;
                        }
                        boss.base::deface();
                        break;
                    }
                    default: break;
                }
            };
            // End-of-drag: drop the gesture mode and transient drag flags. The rubber-band's
            // resulting per-item `selected` flags persist; only the live span (rubber_a/b) clears.
            boss.LISTEN(tier::release, e2::form::drag::stop::_<hids::buttons::left>, gear)
            {
                auto was = st.drag;
                st.drag = queue_state::d_none;
                st.sb_drag = st.hsb_drag = faux;
                st.col_drag = -1;
                st.rubber_a = st.rubber_b = -1;
                if (was != queue_state::d_none) boss.base::deface();
            };
            boss.LISTEN(tier::release, e2::form::drag::cancel::_<hids::buttons::left>, gear)
            {
                auto was = st.drag;
                st.drag = queue_state::d_none;
                st.sb_drag = st.hsb_drag = faux;
                st.col_drag = -1;
                st.rubber_a = st.rubber_b = -1;
                if (was != queue_state::d_none) boss.base::deface();
            };
            // The tab bar (row 0) doubles as a vertical resize handle for the panes/bottom split.
            attach_vsplit_resize(boss, resize_target, 0);
            // Double-click the handle (row 0) restores the default 3:2 panes:queue split
            // (mirrors the workspace fork default in parvion.hpp: fork::ctor(axis::Y, 0, 3, 2)).
            attach_dblclick_reset(boss, resize_target, 3, 2, 0);
            // Double-click a column border (rows 1..div_bottom) auto-fits that column to the widest
            // of its content and header (+1 for the reserved border cell), clamped to the limits.
            boss.on(tier::mouserelease, input::key::LeftDoubleClick, [&](hids& gear)
            {
                if (!st.ctrl || st.tab == 3) return;
                auto mx = (si32)gear.coord.x;
                auto my = (si32)gear.coord.y;
                if (my < 1 || my >= st.div_bottom) return; // Only on the divider span, not the handle bar.
                for (auto i = si32{}; i < q_border_count(st); ++i)
                    if (mx == q_border_cx(st, i) - st.hscroll)
                    {
                        auto fit = std::clamp(queue_col_content_w(st, i) + 1, q_col_min, q_col_max);
                        if (i < q_ncol) st.col_w[i] = fit;
                        else            st.reason_w_override = fit;
                        boss.base::deface();
                        gear.dismiss();
                        return;
                    }
            });
            // Keyboard: Delete/Backspace clears finished items; arrows/Home/End/PageUp/Down
            // move the selection or scroll the table; Esc clears the selection. Manual moves
            // disengage tail-follow (re-engaging only when they leave us pinned to the bottom).
            boss.LISTEN(tier::preview, input::events::keybd::any, gear)
            {
                if (!st.focused || !st.ctrl) return;
                if (gear.payload != input::keybd::type::keypress) return;
                if (gear.keystat == input::key::released || gear.keystat == input::key::interrupted) return;
                if (gear.keybd::handled) return;
                auto k = gear.keybd::generic();
                if (k == input::key::KeyDelete || k == input::key::Backspace)
                {
                    st.ctrl->clear_finished();
                    gear.set_handled();
                    boss.base::deface();
                    return;
                }
                if (st.tab == 3) return; // Navigation applies to the transfer tables only.
                auto maxv  = std::max(0, st.total - st.body_rows);
                auto page  = std::max(1, st.body_rows - 1);
                auto rows  = std::vector<disp_row>{};
                build_rows(st, rows);
                // Ordered parent display-row indices (selection moves over parents only).
                auto parents = std::vector<si32>{};
                for (auto i = si32{}; i < (si32)rows.size(); ++i) if (rows[i].child == -1) parents.push_back(i);
                // Move the single selection to parent at ordinal `p`, scroll it into view.
                auto select_at = [&](si32 p)
                {
                    if (parents.empty()) return;
                    p = std::clamp(p, 0, (si32)parents.size() - 1);
                    auto dr = parents[p];
                    auto qi = rows[dr].qi;
                    for (auto& it : st.ctrl->queue) it.selected = faux;
                    st.ctrl->queue[qi].selected = true;
                    st.sel_anchor = qi;
                    if      (dr < st.scroll)                  st.scroll = dr;
                    else if (dr >= st.scroll + st.body_rows)  st.scroll = dr - st.body_rows + 1;
                    st.scroll = std::clamp(st.scroll, 0, maxv);
                    st.follow = st.scroll == maxv;
                };
                // Current selection ordinal among parents (anchor first, else first selected).
                auto cur = si32{ -1 };
                for (auto p = si32{}; p < (si32)parents.size(); ++p)
                    if (rows[parents[p]].qi == st.sel_anchor) { cur = p; break; }
                if (cur < 0) for (auto p = si32{}; p < (si32)parents.size(); ++p)
                    if (st.ctrl->queue[rows[parents[p]].qi].selected) { cur = p; break; }
                auto act = true;
                     if (k == input::key::KeyUpArrow)    select_at(cur < 0 ? (si32)parents.size() - 1 : cur - 1);
                else if (k == input::key::KeyDownArrow)  select_at(cur < 0 ? 0 : cur + 1);
                else if (k == input::key::KeyHome)     { st.scroll = 0;    st.follow = maxv == 0; }
                else if (k == input::key::KeyEnd)      { st.scroll = maxv; st.follow = true; }
                else if (k == input::key::KeyPageUp)   { st.scroll = std::clamp(st.scroll - page, 0, maxv); st.follow = st.scroll == maxv; }
                else if (k == input::key::KeyPageDown) { st.scroll = std::clamp(st.scroll + page, 0, maxv); st.follow = st.scroll == maxv; }
                else if (k == input::key::Esc)         { for (auto& it : st.ctrl->queue) it.selected = faux; st.sel_anchor = -1; }
                else act = false;
                if (act) { gear.set_handled(); boss.base::deface(); }
            };
        });
        return panel;
    }
}
