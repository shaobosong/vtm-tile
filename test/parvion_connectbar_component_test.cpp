// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#include "netxs/desktopio/application.hpp"
#include "vtm-common.hpp"
#include "netxs/apps/parvion/connectbar.hpp"

#include <cstdio>

using namespace netxs;
using namespace netxs::app::parvion;

namespace
{
    auto test_responsive_endpoints() -> bool
    {
        auto full = cb_arrange(cb_form_width());
        auto minimum = cb_arrange(cb_min_width());
        return full.layout.label == std::array<text, 4>{ "Host:", "User:", "Pass:", "Port:" }
            && full.layout.field == std::array<si32, 4>{ 16, 16, 16, 6 }
            && full.layout.connect == " Connect "
            && minimum.layout.label == std::array<text, 4>{ "H:", "U:", "P:", "#:" }
            && minimum.layout.field == std::array<si32, 4>{ 2, 2, 2, 2 }
            && minimum.layout.connect == " » ";
    }

    auto test_every_responsive_width_has_stable_geometry() -> bool
    {
        for (auto width = cb_min_width(); width <= cb_form_width(); ++width)
        {
            auto geometry = cb_arrange(width);
            if (cb_layout_width(geometry.layout) > width
             || geometry.label[cf_host].coor.x != 1
             || geometry.connect.coor.x + geometry.connect.size.x != width)
                return faux;

            for (auto i = size_t{}; i < geometry.label.size(); ++i)
            {
                if (geometry.label[i].size.x != cell_width(geometry.layout.label[i])
                 || geometry.field[i].size.x != geometry.layout.field[i]
                 || geometry.field[i].size.x < cb_field_min
                 || geometry.field[i].coor.x != geometry.label[i].coor.x
                                                + geometry.label[i].size.x + 1)
                    return faux;
                if (i + 1 < geometry.label.size()
                 && geometry.label[i + 1].coor.x != geometry.field[i].coor.x
                                                      + geometry.field[i].size.x + 1)
                    return faux;
            }
            auto last = geometry.field[cf_port];
            if (geometry.connect.coor.x < last.coor.x + last.size.x + 1) return faux;
        }
        return true;
    }

    auto test_retained_form_applies_resolved_children() -> bool
    {
        auto state = std::make_shared<connect_state>();
        state->fld[cf_port] = "22";
        auto form = connect_form::ctor(state);
        form->limits({ cb_min_width(), 1 }, { cb_form_width(), 1 });

        auto verify = [&](si32 width, view first_label, view button, view button_glyph)
        {
            form->base::extend({ {}, { width, 1 } });
            auto expected = cb_arrange(width);
            for (auto i = size_t{}; i < expected.label.size(); ++i)
            {
                if (form->get_label_area(i) != expected.label[i]
                 || form->get_input_area(i) != expected.field[i]) return faux;
            }
            if (form->get_connect_area() != expected.connect
             || state->layout.label[cf_host] != first_label
             || state->layout.connect != button) return faux;

            auto canvas = ui::face{};
            canvas.size({ width, 1 });
            form->render(canvas);
            auto host_x = expected.label[cf_host].coor.x;
            auto port_x = expected.field[cf_port].coor.x;
            auto button_x = expected.connect.coor.x;
            return canvas[{ host_x, 0 }].txt() == first_label.substr(0, 1)
                && canvas[{ port_x, 0 }].txt() == "2"
                && canvas[{ button_x + 1, 0 }].txt() == button_glyph;
        };

        return verify(cb_form_width(), "Host:", " Connect ", "C")
            && verify(cb_min_width(), "H:", " » ", "»");
    }

    auto test_disconnect_resets_controller_and_preserves_fields() -> bool
    {
        auto ctrl = sftp_remote{};
        ctrl.stage = sftp_remote::s_failed;
        ctrl.status = "Connection failed.";
        auto state = connect_state{};
        state.ctrl = &ctrl;
        state.fld = { "example.test", "alice", "secret", "2222" };
        state.status = "Enter a host name.";

        cb_disconnect(state);

        return ctrl.stage == sftp_remote::s_idle
            && ctrl.status == "Not connected."
            && state.status.empty()
            && state.fld == std::array<text, 4>{ "example.test", "alice", "secret", "2222" };
    }

    auto test_disconnect_availability_and_reconnect_cancel() -> bool
    {
        auto ctrl = sftp_remote{};
        if (ctrl.can_disconnect()) return faux;

        ctrl.stage = sftp_remote::s_failed;
        if (!ctrl.can_disconnect()) return faux;
        ctrl.stage = sftp_remote::s_greeting;
        if (!ctrl.can_disconnect()) return faux;
        ctrl.stage = sftp_remote::s_connected;
        if (!ctrl.can_disconnect()) return faux;

        ctrl.stage = sftp_remote::s_idle;
        auto& conn = ctrl.txn.start<control_transaction::connection>(true, "/kept", 2);
        conn.phase = control_transaction::connection::phase_t::retry_wait;
        if (!ctrl.can_disconnect()) return faux;

        ctrl.disconnect();
        if (ctrl.can_disconnect() || ctrl.txn.active() || ctrl.stage != sftp_remote::s_idle) return faux;

        auto state = connect_state{};
        state.ctrl = &ctrl;
        state.status = "Enter a host name.";
        cb_disconnect(state);
        return ctrl.stage == sftp_remote::s_idle
            && ctrl.status == "Not connected."
            && state.status.empty();
    }

    auto test_disconnect_stops_transfers_but_not_pending_or_hash_work() -> bool
    {
        auto ctrl = sftp_remote{};
        ctrl.stage = sftp_remote::s_connected;

        auto active = queue_item{};
        active.id = 1;
        active.status = queue_item::transferring;
        active.rate.speed = 1024.0;
        auto pending = queue_item{};
        pending.id = 2;
        pending.status = queue_item::queued;
        ctrl.queue = { active, pending };

        auto job = sftp_remote::transfer_job{};
        job.item_id = active.id;
        job.workers.push_back(std::make_unique<xfer_worker>());
        ctrl.transfer_jobs.push_back(std::move(job));
        ctrl.idle_pool.push_back(std::make_unique<xfer_worker>());

        auto hash = hash_item{};
        hash.id = 7;
        hash.status = hash_item::hashing;
        ctrl.hash_queue.push_back(hash);
        ctrl.hash_workers.push_back(std::make_unique<hash_worker>());
        auto hash_worker_ptr = ctrl.hash_workers.front().get();

        ctrl.disconnect();

        return ctrl.transfer_jobs.empty()
            && ctrl.idle_pool.empty()
            && ctrl.queue.size() == 2
            && ctrl.queue[0].status == queue_item::failed
            && ctrl.queue[0].error == "Disconnected."
            && ctrl.queue[0].rate.speed == 0.0
            && ctrl.queue[1].status == queue_item::queued
            && ctrl.stage == sftp_remote::s_idle
            && ctrl.hash_queue.size() == 1
            && ctrl.hash_queue[0].status == hash_item::hashing
            && ctrl.hash_workers.size() == 1
            && ctrl.hash_workers.front().get() == hash_worker_ptr;
    }

    auto test_outer_flex_keeps_actions_flush() -> bool
    {
        auto ctrl = sftp_remote{};
        auto bar = std::dynamic_pointer_cast<flex>(make_connect_bar(&ctrl));
        if (!bar || bar->get_item_count() != 4) return faux;
        auto width = cb_form_width() + 3 + 3 + 12;
        bar->base::extend({ {}, { width, 1 } });
        auto idle = ui::face{};
        idle.size({ width, 1 });
        bar->render(idle);

        auto form_end = cb_form_width();
        auto full = cb_arrange(cb_form_width());
        if (full.connect.coor.x + full.connect.size.x != form_end
         || idle[{ form_end + 1, 0 }].txt() != "▾"
         || idle[{ form_end + 4, 0 }].txt() != "×"
         || idle[{ form_end + 4, 0 }].bgc() != argb{ theme::surface }
         || idle[{ form_end + 4, 0 }].fgc() != argb{ theme::subtext }
         || idle[{ form_end + 6, 0 }].txt() != " ") return faux;

        ctrl.stage = sftp_remote::s_failed;
        auto enabled = ui::face{};
        enabled.size({ width, 1 });
        bar->render(enabled);
        return enabled[{ form_end + 4, 0 }].fgc() == argb{ theme::text_fg };
    }
}

int main()
{
    auto ok = test_responsive_endpoints()
           && test_every_responsive_width_has_stable_geometry()
           && test_retained_form_applies_resolved_children()
           && test_disconnect_resets_controller_and_preserves_fields()
           && test_disconnect_availability_and_reconnect_cancel()
           && test_disconnect_stops_transfers_but_not_pending_or_hash_work()
           && test_outer_flex_keeps_actions_flush();
    if (!ok) std::fprintf(stderr, "parvion connect-bar component tests failed\n");
    return ok ? 0 : 1;
}
