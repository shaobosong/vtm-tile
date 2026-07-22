// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/connectbar.hpp: FileZilla-style Quick Connect bar — editable Host / User /
// Pass / Port fields + a Connect button, rendered in the command_bar style.
// Phase 1 wires the input form; the Connect action drives the SFTP session in
// later phases (for now it reports the target into the status area).

#include "panes.hpp" // theme, shared menus, and SFTP/controller types
#include "button.hpp"

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
        std::array<text, 4> fld{};      // Host/User/Pass/Port values bound to make_input widgets.
        std::array<netxs::wptr<ui::base>, 4> input_wp{};
        text connect_label;             // Live responsive caption consumed by the shared button.
        text status;                    // Result/hint shown to the right.
        sftp_remote* ctrl = nullptr;    // SFTP controller driven by Connect.
        netxs::wptr<ui::base> form_wptr;   // Painted form widget (to deface after dropdown actions).
        netxs::wptr<ui::base> connect_wptr;// Shared Connect button, positioned by connect_render().
        netxs::wptr<ui::base> status_wptr; // Status strip widget (to deface when the status text changes).
    };

    // Set the bar's status hint and repaint the (separate) status strip widget.
    inline void cb_set_status(connect_state& st, text msg)
    {
        st.status = std::move(msg);
        if (auto p = st.status_wptr.lock()) p->base::deface();
    }
    inline void cb_connect(connect_state& st)
    {
        if (st.fld[cf_host].empty()) { cb_set_status(st, "Enter a host name."); return; }
        if (!st.ctrl)                { cb_set_status(st, "No SFTP controller."); return; }
        auto port = si32{ 0 };
        for (auto c : st.fld[cf_port]) if (c >= '0' && c <= '9') port = port * 10 + (c - '0');
        if (port <= 0 || port > 65535) port = 22;
        st.ctrl->connect(st.fld[cf_host], port, st.fld[cf_user], st.fld[cf_pass]);
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
            if (auto input = st.input_wp[(size_t)i].lock())
                input->base::extend(rect{{ x, 0 }, { fw, 1 }});
            x += fw + 1; // One-cell gap after each field.
        }
        st.connect_label = layout.connect;
        auto bw = cell_width(st.connect_label); // Display cells (the padded compact glyph " » " is three cells).
        // Right-align the Connect button against the form's right edge so it stays flush with the
        // ▾ history button (the next sibling) at every width; the gap left of it (between the Port
        // field and Connect) is the responsive spacer that absorbs the slack as the form shrinks.
        auto cx = std::max(x, w - bw);
        if (auto button = st.connect_wptr.lock()) button->base::extend(rect{{ cx, 0 }, { bw, 1 }});
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
            for (auto& value : sp->fld) value.clear();
            sp->fld[cf_port] = "22";
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
                    sp->fld[cf_host] = entry.host;
                    sp->fld[cf_user] = entry.user;
                    sp->fld[cf_pass] = entry.pass;
                    sp->fld[cf_port] = std::to_string(entry.port);
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
        sp->fld[cf_port] = "22";

        // The painter owns only labels/layout; four independent make_input children own editing.
        auto form_layer = ui::cake::ctor();
        auto form = form_layer->attach(ui::mock::ctor());
        form->invoke([sp](auto& boss)
        {
            auto& st = *boss.base::field(sp); // Shared state, kept alive for this widget's lifetime.
            boss.LISTEN(tier::release, e2::render::any, parent_canvas)
            {
                connect_render(st, parent_canvas, boss.base::size());
            };
        });
        for (auto i = si32{}; i < 4; ++i)
        {
            auto input = make_input({
                .value = [sp, i]{ return sp->fld[(size_t)i]; },
                .on_change = [sp, i](text value){ sp->fld[(size_t)i] = std::move(value); },
                .on_submit = [sp](text){ cb_connect(*sp); },
                .secret = connect_fields[(size_t)i].secret,
                .digits_only = i == cf_port,
            });
            form_layer->attach(input.widget);
            sp->input_wp[(size_t)i] = ptr::shadow(input.widget);
        }
        auto connect = make_button({
            .label = [sp]{ return sp->connect_label; },
            .on_activate = [sp](hids&, ui::base&)
            {
                cb_connect(*sp);
            },
        });
        form_layer->attach(connect.widget);
        sp->connect_wptr = ptr::shadow(connect.widget);
        // Min = fully-compressed width so the form never pins a large window min-width (keeps the
        // menu controls on-screen when narrow); max = full uncompressed width. Between the two,
        // connect_render compresses smoothly to whatever width the parent fork hands it.
        form_layer->limits({ cb_min_width(), 1 }, { cb_form_width(), 1 });
        sp->form_wptr = ptr::shadow(form_layer);

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
                app::shared::menu::open_dropdown_popup(boss, build_history_menu(sp),
                    { .source = app::shared::menu::popup_source::control });
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
        bar->attach(slot::_1, form_layer);
        auto tail = bar->attach(slot::_2, ui::fork::ctor(axis::X, 0, 0, 1));
        tail->attach(slot::_1, drop);
        tail->attach(slot::_2, strip);
        return bar;
    }
}
