// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/queue.hpp: the bottom transfer-queue tabs, assembled from reusable core components.
// The tabs hold five thin VIEWS: three transfer views over the reusable table with embedded
// progress bars, one checksums view over the table, and one message-log view over the reusable
// read-only text box. The parent grid owns the panel's resize handle.

#include "transfer_view.hpp"  // make_transfer_view (table_cfg).
#include "hash_view.hpp"      // make_hash_view (table_cfg).
#include "log_view.hpp"       // make_log_view (textbox_cfg).
#include "components/tabs.hpp" // generic tabs container.

namespace netxs::app::parvion
{
    // Assemble the bottom tabs. Transferring / Failed / Succeeded are independent transfer views;
    // all pages share only the controller data. Returning the common component handle keeps the
    // tabs controller and page lifecycle alive when the parent grid retains it.
    inline auto make_queue_tabs(sftp_remote* ctrl, netxs::wptr<ui::base> window_wp = {}) -> component
    {
        auto pages = std::vector<tab_page_cfg>{
            make_transfer_view(ctrl, /*status*/ 0, window_wp), // Transferring
            make_transfer_view(ctrl, /*status*/ 1, window_wp), // Failed
            make_transfer_view(ctrl, /*status*/ 2, window_wp), // Succeeded
            make_log_view(ctrl, window_wp),                    // Message Log
            make_hash_view(ctrl, window_wp),                   // Checksums
        };
        return make_tabs({
            .pages = std::move(pages),
            .active = 0,
            .position = tab_position::bottom,
        });
    }
}
