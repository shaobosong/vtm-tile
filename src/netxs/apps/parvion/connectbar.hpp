// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/connectbar.hpp: FileZilla-style Quick Connect bar — editable Host / User /
// Pass / Port fields + a Connect button, rendered in the command_bar style
// (direct canvas paint + manual keyboard/mouse text editing, per tile.hpp).
// Phase 1 wires the input form; the Connect action drives the SFTP session in
// later phases (for now it reports the target into the status area).

#include "panes.hpp" // theme, put_str, cell_width, cluster_count, cluster_to_byte, cell_to_cluster, edit_*

namespace netxs::app::parvion
{
    struct connect_field { view label; si32 width; bool secret; };

    // Host/User/Pass share one width for a uniform look; Port stays compact.
    inline constexpr auto cb_field_w = si32{ 16 };
    inline constexpr auto cb_port_w  = si32{ 6 };

    inline const auto connect_fields = std::array<connect_field, 4>{{
        { "Host:", cb_field_w, faux },
        { "User:", cb_field_w, faux },
        { "Pass:", cb_field_w, true },
        { "Port:", cb_port_w,  faux },
    }};

    enum { cf_host, cf_user, cf_pass, cf_port };

    // Smallest labels that always remain (Port uses '#' to disambiguate from Pass's 'P').
    inline constexpr auto cb_short_labels = std::array<view, 4>{{ "H:", "U:", "P:", "#:" }};
    inline constexpr auto cb_label_max    = si32{ 3 }; // Label compression steps: "Host:"→"Ho…:"→"H…:"→"H:".
    inline constexpr auto cb_connect_max  = si32{ 6 }; // Connect steps: "Connect"→"Conne…"→…→"C…"→"»".
    inline constexpr auto cb_field_min    = si32{ 2 }; // Every input field keeps at least 2 cells.

    // Label `i` at compression `level`: 0 = full ("Host:"), 1 = "Ho…:", 2 = "H…:", ≥3 = min ("H:").
    inline auto cb_label_at(si32 i, si32 level) -> text
    {
        auto full = connect_fields[i].label;             // e.g. "Host:" (ASCII; trailing ':').
        if (level <= 0)            return text{ full };
        if (level >= cb_label_max) return text{ cb_short_labels[i] };
        auto word = full.substr(0, full.size() - 1);     // Drop the ':' → "Host".
        return text{ word.substr(0, cb_label_max - level) } + "…:"; // keep 2 then 1 leading letters.
    }
    // Connect button caption (no padding) at compression `clev`: "Connect"→"Conne…"→…→"C…"→"»".
    inline auto cb_connect_at(si32 clev) -> text
    {
        static constexpr auto word = view{ "Connect" };
        if (clev <= 0)              return text{ word };
        if (clev >= cb_connect_max) return text{ "»" };
        return text{ word.substr(0, (si32)word.size() - clev - 1) } + "…";
    }
    // The Connect button always keeps one cell of padding on each side.
    inline auto cb_connect_btn(si32 clev) -> text { return text{ " " } + cb_connect_at(clev) + " "; }

    // One resolved connect-form layout: per-field label strings + field widths + padded Connect.
    struct cb_layout { std::array<text, 4> label; std::array<si32, 4> field; text connect; };

    // Width the layout occupies, mirroring connect_render's x-advance (lead + per-field
    // label+space+field+gap + the padded Connect button). The ▾ button sits flush after it.
    inline auto cb_layout_width(cb_layout const& L) -> si32
    {
        auto x = si32{ 1 };
        for (auto i = si32{}; i < 4; ++i) x += cell_width(L.label[i]) + 1 + L.field[i] + 1;
        return x + cell_width(L.connect);
    }

    // Resolve the connect form to fit width `w`, compressing smoothly by priority:
    //   Phase 1 (round-robin: Connect, Port, Pass, User, Host) shortens the Connect caption and the
    //           field labels one cell at a time down to their minimums ("»", "H:"/…/"#:").
    //   Phase 2 then trims field widths one cell at a time, always shrinking the currently longest
    //           field first (ties broken right-to-left: Port, Pass, User, Host), never below
    //           cb_field_min. This keeps the narrow Port field at full width until the wide
    //           Host/User/Pass fields have shrunk down to match it, so Port doesn't bottom out at
    //           2 cells early. Right-alignment of Connect absorbs any leftover slack.
    inline auto cb_resolve(si32 w) -> cb_layout
    {
        auto clev = si32{ 0 };
        auto llev = std::array<si32, 4>{};
        auto L = cb_layout{};
        L.field = { cb_field_w, cb_field_w, cb_field_w, cb_port_w };
        auto rebuild = [&]
        {
            for (auto i = si32{}; i < 4; ++i) L.label[i] = cb_label_at(i, llev[i]);
            L.connect = cb_connect_btn(clev);
        };
        rebuild();
        for (auto moved = true; moved && cb_layout_width(L) > w;)
        {
            moved = faux;
            if (clev < cb_connect_max) { ++clev; moved = true; rebuild(); if (cb_layout_width(L) <= w) break; }
            for (auto i : { cf_port, cf_pass, cf_user, cf_host })
                if (llev[i] < cb_label_max) { ++llev[i]; moved = true; rebuild(); if (cb_layout_width(L) <= w) break; }
        }
        for (; cb_layout_width(L) > w;)
        {
            // Shrink the longest field first; strict '>' over the right-to-left scan resolves ties
            // to the rightmost of the equally-long fields.
            auto best = si32{ -1 };
            for (auto i : { cf_port, cf_pass, cf_user, cf_host })
                if (L.field[i] > cb_field_min && (best < 0 || L.field[i] > L.field[best])) best = i;
            if (best < 0) break; // Every field is already at cb_field_min.
            --L.field[best];
        }
        return L;
    }
    inline auto cb_form_width() -> si32 { return cb_layout_width(cb_resolve(si16max)); } // Full, uncompressed.
    inline auto cb_min_width()  -> si32 { return cb_layout_width(cb_resolve(0));       } // Fully compressed.

    struct connect_state
    {
        std::array<text, 4> val{};      // Field contents (host/user/pass/port).
        std::array<si32, 4> caret{};    // Caret grapheme-cluster index per field.
        std::array<si32, 4> off{};      // Horizontal scroll offset (display cells) per field.
        std::array<rect, 4> box{};      // Cached field input boxes (row-local).
        rect connect_box{};             // Cached Connect button box.
        si32 active = cf_host;          // Field receiving input.
        si32 drag_field = -1;           // Field whose caret a left-drag is scrubbing (-1 = none).
        bool focused = faux;            // Bar has keyboard focus.
        bool hover_connect = faux;      // Cursor is over the Connect button.
        bool press_connect = faux;      // Connect button held down (mouse).
        text status;                    // Result/hint shown to the right.
        sftp_remote* ctrl = nullptr;    // SFTP controller driven by Connect.
        netxs::wptr<ui::base> form_wptr;   // Painted form widget (to deface after dropdown actions).
        netxs::wptr<ui::base> status_wptr; // Status strip widget (to deface when the status text changes).
    };

    // Field editing delegates to the shared single-line editor core (panes.hpp).
    inline void cb_insert(connect_state& st, view ins)
    {
        auto digits = text{}; // The Port field is numeric: keep ASCII digits only (typed or pasted).
        if (st.active == cf_port)
        {
            for (auto c : ins) if (c >= '0' && c <= '9') digits += c;
            ins = digits;
        }
        edit_insert(st.val[st.active], st.caret[st.active], ins);
    }
    inline void cb_backspace(connect_state& st) { edit_backspace(st.val[st.active], st.caret[st.active]); }
    inline void cb_delete(connect_state& st)    { edit_delete(st.val[st.active], st.caret[st.active]); }
    // Map the cursor's cell column `mx` (scroll + in-box offset) to field i's caret cluster.
    // Secret fields map through the painted mask so the caret matches what's on screen.
    inline void cb_caret_to(connect_state& st, si32 i, si32 mx)
    {
        auto disp = connect_fields[i].secret ? text((size_t)cluster_count(st.val[i]), '*') : st.val[i];
        auto col  = st.off[i] + (mx - st.box[i].coor.x);
        st.caret[i] = std::min(cell_to_cluster(disp, col), cluster_count(st.val[i]));
    }
    // Set the bar's status hint and repaint the (separate) status strip widget.
    inline void cb_set_status(connect_state& st, text msg)
    {
        st.status = std::move(msg);
        if (auto p = st.status_wptr.lock()) p->base::deface();
    }
    inline void cb_connect(connect_state& st)
    {
        if (st.val[cf_host].empty()) { cb_set_status(st, "Enter a host name."); return; }
        if (!st.ctrl)                { cb_set_status(st, "No SFTP controller."); return; }
        auto port = si32{ 0 };
        for (auto c : st.val[cf_port]) if (c >= '0' && c <= '9') port = port * 10 + (c - '0');
        if (port <= 0 || port > 65535) port = 22;
        st.ctrl->connect(st.val[cf_host], port, st.val[cf_user], st.val[cf_pass]);
        cb_set_status(st, {}); // The controller drives status from here on.
    }

    inline void connect_render(connect_state& st, auto& canvas, twod size)
    {
        auto w = size.x;
        auto h = size.y;
        if (w <= 0 || h <= 0) return;
        canvas.fill(rect{{ 0, 0 }, { w, h }}, [&](cell& c){ c.bgc(theme::surface); });
        auto layout = cb_resolve(w); // Smoothly compress labels/Connect/fields to fit the width.
        auto x = si32{ 1 };
        for (auto i = si32{}; i < 4; ++i)
        {
            auto& lbl = layout.label[i];
            put_str(canvas, x, 0, lbl, theme::subtext, theme::surface, std::max(0, w - x));
            x += cell_width(lbl) + 1; // Advance by the nominal label width (+ the trailing space).
            auto fw = layout.field[i];
            st.box[i] = rect{{ x, 0 }, { fw, 1 }};
            // Fields share the labels' background; an underline marks the editable
            // extent. When the bar is focused, the active field's whole foreground
            // (text + underline) turns accent blue together; otherwise the bar stays
            // calm (muted underline, default text) so it avoids extra color blocks.
            auto sel     = st.focused && st.active == i;
            auto und_clr = sel ? ui32{ theme::sel_bg_act } : ui32{ theme::subtext };
            canvas.fill(rect{{ x, 0 }, { fw, 1 }}, [&](cell& c){ c.bgc(theme::surface).und(unln::line).unc(argb{ und_clr }); });
            // Scroll the field horizontally so the caret stays inside its fw-cell
            // window; without this, text typed past the field width is invisible and
            // the view stays pinned to the start (mirrors command_bar in tile.hpp).
            auto& caret = st.caret[i];           // Grapheme-cluster index.
            auto& off   = st.off[i];             // Cell-column scroll offset.
            // Display string: one '*' per cluster for secret fields keeps caret indices
            // consistent between the real value and the mask (each '*' is one cell wide).
            auto disp  = connect_fields[i].secret ? text((size_t)cluster_count(st.val[i]), '*') : st.val[i];
            auto total = cell_width(disp);
            auto ccell = connect_fields[i].secret ? caret : caret_cell(disp, caret); // Caret cell column.
            if (off > ccell)        off = ccell;
            if (ccell - off >= fw)  off = ccell - fw + 1;
            off = std::clamp(off, si32{ 0 }, std::max(si32{ 0 }, total - fw + 1));
            auto shown = view{ disp }.substr(byte_at_cell(disp, off));
            put_str(canvas, x, 0, shown, sel ? ui32{ theme::sel_bg_act } : ui32{ theme::text_fg }, theme::surface, fw);
            if (st.focused && st.active == i)
            {
                auto cx = ccell - off;
                if (cx >= 0 && cx < fw)
                {
                    canvas.fill(rect{{ x + cx, 0 }, { 1, 1 }}, [&](cell& c){ c.bgc(theme::sel_bg_act).fgc(theme::surface); });
                }
            }
            x += fw + 1; // One-cell gap after each field.
        }
        auto& label = layout.connect;
        auto bw = cell_width(label); // Display cells (the padded compact glyph " » " is three cells).
        // Right-align the Connect button against the form's right edge so it stays flush with the
        // ▾ history button (the next sibling) at every width; the gap left of it (between the Port
        // field and Connect) is the responsive spacer that absorbs the slack as the form shrinks.
        auto cx = std::max(x, w - bw);
        st.connect_box = rect{{ cx, 0 }, { bw, 1 }};
        // Resting button: muted slate with light text. Hover/press add a bright overlay via the
        // cell's xlight — the same effect as cell::shaders::xlight on the menu-bar buttons in
        // application.hpp; a held press doubles the lift (factor 2) for a "pushed" look.
        canvas.fill(st.connect_box, [&](cell& c){ c.bgc(theme::sel_bg); });
        put_str(canvas, cx, 0, label, theme::text_fg, theme::sel_bg, bw);
        if      (st.press_connect) canvas.fill(st.connect_box, [](cell& c){ c.xlight(2); });
        else if (st.hover_connect) canvas.fill(st.connect_box, [](cell& c){ c.xlight(); });
        // The status hint is painted by a separate strip to the right of the ▾ button.
    }

    // Build the ▾ button's dropdown: Clear-bar / Clear-history / a separator / the recent
    // servers (newest first). Rebuilt per open so it reflects the live fields and history.
    // Each row carries a native action (menu::item::action), run by the popup's activate_leaf.
    inline auto build_history_menu(std::shared_ptr<connect_state> sp) -> std::vector<app::shared::menu::item>
    {
        namespace m = app::shared::menu;
        auto deface_form = [sp]{ if (auto f = sp->form_wptr.lock()) f->base::deface(); };
        auto items = std::vector<m::item>{};
        auto clear_bar = m::item{ .alive = true, .label = "Clear Quickconnect bar" };
        clear_bar.action = [sp, deface_form](hids&)
        {
            for (auto i = 0; i < 4; ++i) { sp->val[i].clear(); sp->caret[i] = 0; sp->off[i] = 0; }
            sp->val[cf_port] = "22"; sp->caret[cf_port] = 2;
            cb_set_status(*sp, {});
            deface_form();
        };
        items.push_back(std::move(clear_bar));
        auto clear_hist = m::item{ .alive = true, .label = "Clear history" };
        clear_hist.action = [sp](hids&){ if (sp->ctrl) sp->ctrl->clear_recent(); };
        items.push_back(std::move(clear_hist));
        if (sp->ctrl && !sp->ctrl->recent.empty())
        {
            items.push_back(m::item{ .alive = true, .type = m::kind::separator });
            for (auto& r : sp->ctrl->recent)
            {
                auto label = (r.user.empty() ? text{} : r.user + "@") + r.host
                           + (r.port == 22 ? text{} : ":" + std::to_string(r.port));
                auto row = m::item{ .alive = true, .label = label };
                row.action = [sp, deface_form, entry = r](hids&)
                {
                    sp->val[cf_host] = entry.host; sp->caret[cf_host] = cluster_count(entry.host); sp->off[cf_host] = 0;
                    sp->val[cf_user] = entry.user; sp->caret[cf_user] = cluster_count(entry.user); sp->off[cf_user] = 0;
                    sp->val[cf_pass] = entry.pass; sp->caret[cf_pass] = cluster_count(entry.pass); sp->off[cf_pass] = 0;
                    sp->val[cf_port] = std::to_string(entry.port); sp->caret[cf_port] = cluster_count(sp->val[cf_port]); sp->off[cf_port] = 0;
                    cb_connect(*sp);
                    deface_form();
                };
                items.push_back(std::move(row));
            }
        }
        return items;
    }

    inline auto make_connect_bar(sftp_remote* ctrl = nullptr) -> ui::sptr
    {
        // One shared connect_state drives both the painted form and the ▾ dropdown's row actions.
        auto sp = std::make_shared<connect_state>();
        sp->ctrl = ctrl;
        sp->val[cf_port] = "22";
        sp->caret[cf_port] = 2;

        // The editable Host/User/Pass/Port + Connect form (painted directly, per command_bar).
        auto form = ui::mock::ctor()
            ->active()
            ->plugin<pro::mouse>()
            ->plugin<pro::focus>(pro::focus::mode::focusable)
            ->plugin<pro::keybd>();
        form->invoke([sp](auto& boss)
        {
            auto& st = *boss.base::field(sp); // Shared state, kept alive for this widget's lifetime.

            boss.LISTEN(tier::release, e2::render::any, parent_canvas)
            {
                connect_render(st, parent_canvas, boss.base::size());
            };
            boss.LISTEN(tier::release, e2::form::state::focus::count, count)
            {
                st.focused = !!count;
                boss.base::deface();
            };
            // Focus and field activation happen on the press (mousedown), mirroring the file
            // panes / queue; the Connect button keeps button semantics and fires on the click.
            boss.on(tier::mouserelease, input::key::LeftClick, [&](hids& gear)
            {
                // Re-assert focus on the click release: the pro::focus plugin's Ctrl+LeftClick
                // handler toggles focus off when the bar is already focused (see the panes).
                pro::focus::set(boss.This(), gear.id, solo::on);
                auto mx = (si32)gear.coord.x;
                auto& cb = st.connect_box;
                if (mx >= cb.coor.x && mx < cb.coor.x + cb.size.x) cb_connect(st);
                boss.base::deface();
                gear.dismiss();
            });
            // Hover / press feedback on the Connect button (the bar is a single row). connect_box
            // is recomputed each render and reused here as the hitbox; mirrors queue.hpp's hover.
            boss.on(tier::mouserelease, input::key::MouseMove, [&](hids& gear)
            {
                auto& cb = st.connect_box;
                auto mx = (si32)gear.coord.x;
                auto over = mx >= cb.coor.x && mx < cb.coor.x + cb.size.x;
                auto dirty = faux;
                if (st.hover_connect != over)  { st.hover_connect = over; dirty = true; }
                if (st.press_connect && !over) { st.press_connect = faux; dirty = true; } // Drag-off cancels.
                if (dirty) boss.base::deface();
            });
            boss.on(tier::mouserelease, input::key::MouseLeave, [&](hids&)
            {
                if (st.hover_connect || st.press_connect) { st.hover_connect = st.press_connect = faux; boss.base::deface(); }
            });
            boss.on(tier::mouserelease, input::key::LeftDown, [&](hids& gear)
            {
                pro::focus::set(boss.This(), gear.id, solo::on);
                auto mx = (si32)gear.coord.x;
                for (auto i = si32{}; i < 4; ++i)
                {
                    auto& b = st.box[i];
                    if (mx >= b.coor.x && mx < b.coor.x + b.size.x)
                    {
                        st.active = i;
                        cb_caret_to(st, i, mx);
                    }
                }
                auto& cb = st.connect_box;
                if (mx >= cb.coor.x && mx < cb.coor.x + cb.size.x && !st.press_connect) st.press_connect = true;
                boss.base::deface();
            });
            boss.on(tier::mouserelease, input::key::LeftUp, [&](hids&)
            {
                if (st.press_connect) { st.press_connect = faux; boss.base::deface(); }
            });
            // Caret scrubbing: a left-drag that began on a field keeps the caret under the
            // cursor on every pull (the render's window clamp auto-scrolls at the field edges).
            // Enabling draggable turns on pointer capture, so pulls keep coming even when the
            // cursor leaves the field (mirrors the panes' address bar).
            boss.base::signal(tier::release, e2::form::draggable::_<hids::buttons::left>, true);
            boss.LISTEN(tier::release, e2::form::drag::start::_<hids::buttons::left>, gear)
            {
                auto px = (si32)gear.click.x; // Press position localized to this widget.
                st.drag_field = -1;
                for (auto i = si32{}; i < 4; ++i)
                {
                    auto& b = st.box[i];
                    if (px >= b.coor.x && px < b.coor.x + b.size.x) { st.drag_field = i; break; }
                }
            };
            boss.LISTEN(tier::release, e2::form::drag::pull::_<hids::buttons::left>, gear)
            {
                if (st.drag_field < 0) return;
                cb_caret_to(st, st.drag_field, (si32)gear.coord.x);
                boss.base::deface();
            };
            boss.LISTEN(tier::release, e2::form::drag::stop::_<hids::buttons::left>,   gear) { st.drag_field = -1; };
            boss.LISTEN(tier::release, e2::form::drag::cancel::_<hids::buttons::left>, gear) { st.drag_field = -1; };
            boss.LISTEN(tier::preview, input::events::keybd::any, gear)
            {
                if (!st.focused) return;
                if (gear.payload == input::keybd::type::keypaste)
                {
                    cb_insert(st, edit_filter(gear.cluster));
                    boss.base::deface();
                    gear.set_handled();
                    return;
                }
                if (gear.payload != input::keybd::type::keypress) return;
                if (gear.keystat == input::key::interrupted) return;
                if (gear.keybd::handled) return;
                if (gear.keystat == input::key::released) return;
                auto k = gear.keybd::generic();
                auto shift = !!(gear.ctlstat & hids::anyShift);
                auto& v = st.val[st.active];
                auto& c = st.caret[st.active];
                auto act = true;
                     if (k == input::key::Backspace)     cb_backspace(st);
                else if (k == input::key::KeyDelete)     cb_delete(st);
                else if (k == input::key::KeyLeftArrow)  c = std::max(0, c - 1);
                else if (k == input::key::KeyRightArrow) c = std::min(cluster_count(v), c + 1);
                else if (k == input::key::KeyHome)       c = 0;
                else if (k == input::key::KeyEnd)        c = cluster_count(v);
                else if (k == input::key::Tab)           { st.active = (st.active + (shift ? 3 : 1)) % 4; }
                else if (k == input::key::KeyEnter)      cb_connect(st);
                else
                {
                    auto ins = edit_filter(gear.cluster);
                    if (ins.size()) cb_insert(st, ins);
                    else act = faux;
                }
                if (act)
                {
                    gear.set_handled();
                    boss.base::deface();
                }
            };
        });
        // Min = fully-compressed width so the form never pins a large window min-width (keeps the
        // menu controls on-screen when narrow); max = full uncompressed width. Between the two,
        // connect_render compresses smoothly to whatever width the parent fork hands it.
        form->limits({ cb_min_width(), 1 }, { cb_form_width(), 1 });
        sp->form_wptr = ptr::shadow(form);

        // The ▾ Quick Connect history button: a real ui::item so menu::open_dropdown_popup can
        // anchor a dropdown overlay beneath it (hover uses the menu-bar xlight overlay). Placed
        // immediately to the right of the Connect button.
        auto drop = ui::item::ctor(" ▾ ")
            ->active(cell{}.bgc(theme::surface))
            ->shader(cell::shaders::xlight, e2::form::state::hover)
            ->limits({ 3, 1 }, { 3, 1 });
        drop->invoke([sp](auto& boss)
        {
            boss.on(tier::mouserelease, input::key::LeftClick, [&boss, sp](hids& gear)
            {
                app::shared::menu::open_dropdown_popup(boss, build_history_menu(sp));
                gear.dismiss();
            });
        });

        // The status-hint strip fills the remaining width to the right of the ▾ button.
        auto strip = ui::mock::ctor();
        strip->invoke([sp](auto& boss)
        {
            boss.LISTEN(tier::release, e2::render::any, parent_canvas, -, (sp))
            {
                auto sz = boss.base::size();
                if (sz.x <= 0 || sz.y <= 0) return;
                parent_canvas.fill(rect{{ 0, 0 }, { sz.x, sz.y }}, [&](cell& c){ c.bgc(theme::surface); });
                if (!sp->status.empty()) put_str(parent_canvas, 1, 0, sp->status, theme::subtext, theme::surface, std::max(0, sz.x - 1));
            };
        });
        sp->status_wptr = ptr::shadow(strip);

        // [ fields+Connect form | ▾ | status ]: ratio s1=1,s2=0 grows the form up to its (tier)
        // max and hands the remainder to the tail, so ▾ sits flush right after the Connect button.
        auto bar  = ui::fork::ctor(axis::X, 0, 1, 0);
        bar->attach(slot::_1, form);
        auto tail = bar->attach(slot::_2, ui::fork::ctor(axis::X, 0, 0, 1));
        tail->attach(slot::_1, drop);
        tail->attach(slot::_2, strip);
        return bar;
    }
}
