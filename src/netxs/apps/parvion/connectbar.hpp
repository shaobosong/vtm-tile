// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/connectbar.hpp: FileZilla-style Quick Connect bar — editable Host / User /
// Pass / Port fields + a Connect button, composed from retained Parvion
// components and rendered in the command_bar style.

#include "panes.hpp" // theme, shared menus, and SFTP/controller types
#include "components/button.hpp"
#include "components/popup_menu.hpp"
#include "components/flex.hpp"
#include "components/input.hpp"
#include "components/label.hpp"

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

    // Width the layout occupies, mirroring cb_arrange's x-advance (lead + per-field
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

    // Retained-child geometry for one resolved form width. Keeping this calculation
    // independent from the component makes the exact responsive policy directly testable.
    struct cb_geometry
    {
        cb_layout layout;
        std::array<rect, 4> label;
        std::array<rect, 4> field;
        rect connect;
    };

    inline auto cb_arrange(si32 width) -> cb_geometry
    {
        auto geometry = cb_geometry{};
        geometry.layout = cb_resolve(width);
        auto x = si32{ 1 };
        for (auto i = si32{}; i < 4; ++i)
        {
            auto label_width = cell_width(geometry.layout.label[(size_t)i]);
            geometry.label[(size_t)i] = { { x, 0 }, { label_width, 1 } };
            x += label_width + 1;
            geometry.field[(size_t)i] = { { x, 0 }, { geometry.layout.field[(size_t)i], 1 } };
            x += geometry.layout.field[(size_t)i] + 1;
        }
        auto button_width = cell_width(geometry.layout.connect);
        geometry.connect = { { std::max(x, width - button_width), 0 }, { button_width, 1 } };
        return geometry;
    }

    struct connect_state
    {
        std::array<text, 4> fld{};      // Host/User/Pass/Port values bound to make_input widgets.
        cb_layout layout;               // Live responsive labels/caption consumed by retained children.
        text status;                    // Result/hint shown to the right.
        sftp_remote* ctrl = nullptr;    // SFTP controller driven by Connect.
        netxs::wptr<ui::base> form_wptr;   // Retained form (to deface after dropdown actions).
        netxs::wptr<ui::base> status_wptr; // Status label (to deface when the status text changes).
    };

    // Set the bar's status hint and repaint the separate retained status label.
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

    // The connect form has a deliberately specialized sizing policy, but all of
    // its visible and interactive children are shared retained components. This
    // adapter only applies cb_arrange() geometry and paints the common gap surface.
    class connect_form
        : public ui::form<connect_form>
    {
        std::shared_ptr<connect_state> state;
        std::array<ui::sptr, 4> labels;
        std::array<ui::sptr, 4> inputs;
        ui::sptr connect;
        std::array<rect, 4> label_areas;
        std::array<rect, 4> input_areas;
        rect connect_area;
        std::vector<component> retained_components;

        void retain(component item, ui::sptr& widget)
        {
            widget = item.widget;
            if (!widget) return;
            retained_components.push_back(std::move(item));
            ui::base::attach(widget);
        }

    protected:
        void deform(rect& new_area) override
        {
            new_area.size.y = std::max(1, new_area.size.y);
            auto geometry = cb_arrange(new_area.size.x);
            state->layout = geometry.layout;
            label_areas = geometry.label;
            input_areas = geometry.field;
            connect_area = geometry.connect;
            for (auto i = size_t{}; i < labels.size(); ++i)
            {
                if (labels[i]) labels[i]->base::recalc(label_areas[i]);
                if (inputs[i]) inputs[i]->base::recalc(input_areas[i]);
            }
            if (connect) connect->base::recalc(connect_area);
        }

        void inform(rect new_area) override
        {
            for (auto i = size_t{}; i < labels.size(); ++i)
            {
                auto label_area = label_areas[i];
                label_area.coor += new_area.coor;
                if (labels[i]) labels[i]->base::notify(label_area);
                auto input_area = input_areas[i];
                input_area.coor += new_area.coor;
                if (inputs[i]) inputs[i]->base::notify(input_area);
            }
            auto button_area = connect_area;
            button_area.coor += new_area.coor;
            if (connect) connect->base::notify(button_area);
        }

    public:
        static constexpr auto classname = basename::parvion;

        connect_form(std::shared_ptr<connect_state> shared)
            : state{ std::move(shared) }
        {
            state->layout = cb_resolve(cb_form_width());
            LISTEN(tier::release, e2::render::any, parent_canvas)
            {
                if (auto context2D = nested_2D_context(parent_canvas))
                {
                    parent_canvas.fill(rect{ {}, base::size() },
                        [](cell& c){ c.bgc(theme::surface); });
                    for (auto& label : labels) if (label) label->render(parent_canvas);
                    for (auto& input : inputs) if (input) input->render(parent_canvas);
                    if (connect) connect->render(parent_canvas);
                }
            };
        }

        // ui::base::attach() needs this form's shared ownership to be established,
        // so retained children are installed immediately after domain creation.
        void install_children()
        {
            for (auto i = si32{}; i < 4; ++i)
            {
                retain(make_label({
                    .value = [sp = state, i]{ return sp->layout.label[(size_t)i]; },
                    .role = label_role::hint,
                    .palette = { .background = theme::surface },
                }), labels[(size_t)i]);
                retain(make_input({
                    .value = [sp = state, i]{ return sp->fld[(size_t)i]; },
                    .on_change = [sp = state, i](text value){ sp->fld[(size_t)i] = std::move(value); },
                    .on_submit = [sp = state](text){ cb_connect(*sp); },
                    .secret = connect_fields[(size_t)i].secret,
                    .digits_only = i == cf_port,
                }), inputs[(size_t)i]);
            }
            retain(make_button({
                .label = [sp = state]{ return sp->layout.connect; },
                .on_activate = [sp = state](hids&, ui::base&){ cb_connect(*sp); },
            }), connect);
        }

        auto get_label_area(size_t index) const -> rect
        {
            return index < label_areas.size() ? label_areas[index] : rect{};
        }

        auto get_input_area(size_t index) const -> rect
        {
            return index < input_areas.size() ? input_areas[index] : rect{};
        }

        auto get_connect_area() const -> rect { return connect_area; }

        static auto ctor(std::shared_ptr<connect_state> state)
        {
            auto form = ui::tui_domain().create<connect_form>(std::move(state));
            form->install_children();
            return form;
        }
    };

    // Build the ▾ button's dropdown: saved Site Connection submenu / separator / Clear-bar /
    // Clear-history / a separator / recent servers (newest first). Rebuilt per open so it
    // reflects the live settings, fields, and history.
    // Each row carries a native activation callback owned by the shared menu component.
    inline auto build_history_menu(std::shared_ptr<connect_state> sp) -> std::vector<popup_menu_item>
    {
        auto deface_form = [sp]{ if (auto f = sp->form_wptr.lock()) f->base::deface(); };
        auto items = std::vector<popup_menu_item>{};
        auto sites = popup_menu_item{ .label = "Site Connection", .kind = popup_menu_item_kind::submenu };
        if (!sp->ctrl || sp->ctrl->cfg.sites.empty())
        {
            sites.children.push_back(popup_menu_item{ .label = "Empty", .enabled = false });
        }
        else
        {
            for (auto const& site : sp->ctrl->cfg.sites)
            {
                auto row = popup_menu_item{ .label = site.name };
                row.on_activate = [sp, deface_form, entry = site](hids&)
                {
                    sp->fld[cf_host] = entry.host;
                    sp->fld[cf_user] = entry.user;
                    sp->fld[cf_pass] = entry.pass;
                    sp->fld[cf_port] = std::to_string(entry.port);
                    cb_connect(*sp);
                    deface_form();
                };
                sites.children.push_back(std::move(row));
            }
        }
        items.push_back(std::move(sites));
        items.push_back(popup_menu_item{ .kind = popup_menu_item_kind::separator });
        auto clear_bar = popup_menu_item{ .label = "Clear Quickconnect bar" };
        clear_bar.on_activate = [sp, deface_form](hids&)
        {
            for (auto& value : sp->fld) value.clear();
            sp->fld[cf_port] = "22";
            cb_set_status(*sp, {});
            deface_form();
        };
        items.push_back(std::move(clear_bar));
        auto clear_hist = popup_menu_item{ .label = "Clear history" };
        clear_hist.on_activate = [sp](hids&){ if (sp->ctrl) sp->ctrl->clear_recent(); };
        items.push_back(std::move(clear_hist));
        if (sp->ctrl && !sp->ctrl->recent.empty())
        {
            items.push_back(popup_menu_item{ .kind = popup_menu_item_kind::separator });
            for (auto& r : sp->ctrl->recent)
            {
                auto label = (r.user.empty() ? text{} : r.user + "@") + r.host
                           + (r.port == 22 ? text{} : ":" + std::to_string(r.port));
                auto row = popup_menu_item{ .label = label };
                row.on_activate = [sp, deface_form, entry = r](hids&)
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
        // One shared connect_state drives both the retained form and the ▾ dropdown's row actions.
        auto sp = std::make_shared<connect_state>();
        sp->ctrl = ctrl;
        sp->fld[cf_port] = "22";

        auto form = connect_form::ctor(sp);
        // Min = fully-compressed width so the form never pins a large window min-width (keeps the
        // menu controls on-screen when narrow); max = full uncompressed width. cb_arrange()
        // applies the exact historical compression policy between those bounds.
        form->limits({ cb_min_width(), 1 }, { cb_form_width(), 1 });
        sp->form_wptr = ptr::shadow(form);

        // The ▾ Quick Connect history button: a dropdown-menu trigger with an
        // empty (arrow-only) label. Its items are rebuilt from the live
        // settings and history on every open via the cfg items provider, and
        // the popup is a dropdown-source chain: hovering the tile's menu-bar
        // triggers must NOT hover-switch it (see
        // test_history_dropdown_does_not_arm_menubar_hover_switch). Placed
        // immediately to the right of the Connect button.
        auto drop = make_dropdown_menu({
            .label = []{ return text{}; },
            .items = [sp]{ return build_history_menu(sp); },
            .palette = { .background = theme::surface },
        });

        // The live hint label fills the remaining width to the right of the ▾
        // button. Prefixing non-empty text preserves the historical one-cell inset.
        auto status = make_label({
            .value = [sp]{ return sp->status.empty() ? text{} : " " + sp->status; },
            .role = label_role::hint,
            .palette = { .background = theme::surface },
        });
        sp->status_wptr = ptr::shadow(status.widget);

        // [ fields+Connect form | ▾ | status ]: the form shrinks only between
        // its historical max/min, the trigger remains fixed, and status consumes
        // the remaining space. This keeps Connect flush with the dropdown.
        auto bar = flex::ctor();
        bar->attach(component{ form }, {
            .grow = 1,
            .shrink = 1,
            .basis = cb_form_width(),
            .minimum = cb_min_width(),
            .maximum = cb_form_width(),
        });
        bar->attach(std::move(drop), {
            .grow = 0,
            .shrink = 0,
            .basis = 3,
            .minimum = 3,
            .maximum = 3,
        });
        bar->attach(std::move(status), {
            .grow = 1,
            .shrink = 0,
            .basis = 0,
        });
        return bar;
    }
}
