// Copyright (c) Shaobo Song
// Licensed under the MIT license.

#pragma once

// parvion/queue_actions.hpp: shared queue-panel actions wired by the concrete views.
//
// Kept outside tab_page.hpp so reusable widgets can stay independent from the tabs container.

#include "panes.hpp" // sftp_remote, ui::base, app::shared::show_close_confirmation.

namespace netxs::app::parvion
{
    // Delete/Backspace clears all finished (succeeded / failed) transfers, with a confirm dialog
    // anchored on the app window. Wired into every view's key config (via the core widgets' on_key
    // hook) so it fires on whichever tab currently holds keyboard focus. `self` is the page widget
    // (for repaint). Returns true iff the key was Delete/Backspace (consumed).
    inline auto clear_finished_on_key(hids& gear, sftp_remote* ctrl, netxs::wptr<ui::base> window_wp, netxs::wptr<ui::base> self) -> bool
    {
        auto k = gear.keybd::generic();
        if (k != input::key::KeyDelete && k != input::key::Backspace) return faux;
        gear.set_handled(); // Swallow the key either way (matches the old behavior).
        if (!ctrl) return true;
        auto count = si32{};
        for (auto& it : ctrl->queue)
            if (it.status == queue_item::succeeded || it.status == queue_item::failed) ++count;
        if (!count) return true; // Nothing finished: keep the old no-op, no dialog.
        auto run = [ctrl, self]
        {
            if (auto p = self.lock()) { ctrl->clear_finished(); p->base::deface(); }
        };
        if (auto window = window_wp.lock())
        {
            app::shared::show_close_confirmation(*window, run, {},
                app::shared::confirm_dialog_text{ "Clear all finished transfers?", "Clear", "Cancel" });
        }
        else run(); // No dialog anchor wired: behave as before.
        return true;
    }
}
