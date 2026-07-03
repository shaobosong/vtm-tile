// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/queue.hpp: the bottom transfer-queue panel, assembled from reusable core components — a
// resize handle bar over a generic tabs container. The tabs hold five thin VIEWS built over the two
// cores: three transfer views + one checksums view over the reusable table, and one message-log view
// over the reusable read-only text box. Repainted by the applet's poll timer.

#include "transfer_view.hpp"  // make_transfer_view (table_cfg).
#include "hash_view.hpp"      // make_hash_view (table_cfg).
#include "log_view.hpp"       // make_log_view (textbox_cfg).
#include "tabs.hpp"           // generic tabs container.

namespace netxs::app::parvion
{
    // Assemble the bottom panel: fork(Y)[ resize handle bar (row 0), tabs container ]. The tabs hold
    // Transferring / Failed / Succeeded (three INDEPENDENT transfer views), Message log, and
    // Checksums. Signature and return type are unchanged, so parvion.hpp's build() call site and poll
    // timer (which defaces the returned widget — deface cascades to the strip + visible page) are
    // untouched.
    inline auto make_queue_panel(sftp_remote* ctrl, netxs::wptr<ui::fork> resize_target, netxs::wptr<ui::base> window_wp = {}) -> ui::sptr
    {
        auto assembly = ui::fork::ctor(axis::Y);

        // Row 0: an empty handle bar; dragging it resizes the panes/queue split (lightens on hover),
        // double-click restores the default 3:2 panes:queue ratio. Queue-panel-specific, so it lives
        // here in the assembly rather than in the generic tabs container.
        auto handle = ui::mock::ctor()->active()->plugin<pro::mouse>();
        handle->invoke([resize_target](auto& boss)
        {
            auto& hover = boss.base::field(bool{ faux });
            boss.LISTEN(tier::release, e2::render::any, canvas)
            {
                auto size = boss.base::size();
                canvas.fill(rect{{ 0, 0 }, { size.x, size.y }}, [&](cell& c){ c.bgc(theme::surface); });
                if (hover) canvas.fill(rect{{ 0, 0 }, { size.x, size.y }}, [](cell& c){ c.xlight(); });
            };
            boss.on(tier::mouserelease, input::key::MouseMove,  [&](hids&){ if (!hover) { hover = true; boss.base::deface(); } });
            boss.on(tier::mouserelease, input::key::MouseLeave, [&](hids&){ if ( hover) { hover = faux; boss.base::deface(); } });
            attach_vsplit_resize(boss, resize_target, -1);          // Whole widget is the drag handle.
            attach_dblclick_reset(boss, resize_target, 3, 2, -1);   // Default panes:queue split = 3:2.
        });
        assembly->attach(slot::_1, handle)->limits({ -1, 1 }, { -1, 1 });

        // The five tab views. Each transfer view is an independent make_transfer_view() instance with
        // its own column widths + scroll; they share only the controller's queue data.
        auto pages = std::vector<tab_page_ptr>{
            make_transfer_view(ctrl, /*status*/ 0, window_wp), // Transferring
            make_transfer_view(ctrl, /*status*/ 1, window_wp), // Failed
            make_transfer_view(ctrl, /*status*/ 2, window_wp), // Succeeded
            make_log_view(ctrl, window_wp),                    // Message log
            make_hash_view(ctrl, window_wp),                   // Checksums
        };
        auto tabs = make_tabs(std::move(pages), /*active*/ 0);
        assembly->attach(slot::_2, tabs->widget());

        // Keep the tabs container (and thus its views) alive for the assembly's lifetime.
        assembly->invoke([tabs](auto& boss){ boss.base::field(tab_page_ptr{ tabs }); });
        return assembly;
    }
}
