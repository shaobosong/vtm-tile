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
#include "ui.hpp"
#include "input.hpp"
#include "button.hpp"
#include "table.hpp"

#include <cerrno>
#include <cstdio>
#include <functional>
#include <set>

namespace netxs::app::parvion
{
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
            // A permission error reaching even the stat (e.g. no execute on a parent dir) is
            // reported as such; a clean "doesn't exist / not a dir" keeps the simpler message.
            auto isdir = fs::is_directory(p, ec);
            if (ec == std::errc::permission_denied) { err = "Cannot access: " + path + " (" + ec.message() + ")"; return faux; }
            if (!isdir) { err = "Not a directory: " + path; return faux; }
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
        // Multi-selection (mirrors the queue table): `marked` is the highlighted set of logical rows.
        // The shared table owns the transient anchor / rubber-band state.
        std::set<si32>        marked{ 0 };
        // Explorer-style name editor painted in the selected row's Name cell.
        bool                  name_edit = faux;
        text                  name_original;   // Stable operation identity; selection may move on an outside click.
        text                  name_draft;      // Live value owned by the generic input component.
        si32                  name_row = -1;    // Source row whose Name cell owns the editor.
        bool                  name_is_dir = faux;
        component             name_editor;     // Stable retained component attached only for the active Name cell.
        // Address-bar path entry (the row-0 path field, connect-bar style).
        bool                  addr_edit  = faux; // Editing the path in place.
        text                  addr_buf;          // The path being typed.
        netxs::wptr<ui::base> addr_input_wp;     // Permanent path input, activated in place.
        // Transfer-table-style columns (Name, Size, Modified): session-only widths + visibility,
        // persisted by the shared table's resize / show-hide adapters.
        std::array<si32, 3>   col_w{ 25, 9, 17 }; // Name; Size (right-aligned); Modified ("YYYY-MM-DD HH:MM" + border cell).
        std::array<bool, 3>   col_shown{ true, true, true };
        bool                  focused = faux;   // Pane has keyboard focus.
        sftp_remote*          remote = nullptr; // Non-null: remote pane backed by the SFTP controller.
        sftp_remote*          ctrl = nullptr;   // SFTP controller (both panes) for enqueueing transfers.
        netxs::wptr<ui::base> window_wp;        // App top-level window cake: anchor for confirm dialogs.
        netxs::wptr<ui::base> table_wp;         // Shared table widget (picker focus hand-off after overlay attach).
        ui64                  seen_gen = ~0ull; // Last remote listing generation reconciled into selection state.
        ui64                  seen_local_gen = 0; // Last local refresh generation seen (post-download re-list); matches ctrl->local_gen's initial 0.
        ui64                  revision = 0;    // Navigation generation consumed by table_cfg::revision.
        si32                  revision_row = -1; // One-shot source row revealed after a revision change.
        text                  seen_path;       // Remote path associated with seen_gen (same-path refresh preserves viewport).
        text                  create_pending; // Remote-created item; successful refresh selects it and starts Rename.
        text                  rename_pending; // Remote rename target, selected after its successful refreshed listing.
        bool                  delete_pending = faux; // Next same-directory refresh settles selection beside deleted rows.
        si32                  delete_anchor = 0;     // First deleted logical row; replacement selection keeps this position.
        // File-picker mode (Settings dialog's "Add key file..."): when set, activating a FILE
        // (double-click / Enter / Open button) invokes this with the file's full path instead of
        // enqueueing a transfer; directory navigation is unchanged. Lets the picker reuse the whole
        // Local Site browser (path bar, resizable columns, scrollbars, selection) verbatim.
        std::function<void(text const&)> on_pick;
        // File-picker mode: Esc (when not editing the path / a name) cancels the picker. The picker
        // wires this to a deferred close; it is unset for the Local/Remote site panes (Esc is a no-op
        // there). on_cancel must enqueue/defer its work — it tears down the pane that is calling it.
        std::function<void()> on_cancel;
        // Save-picker mode: fired whenever the selection moves to a FILE (single click / arrow nav),
        // with the file's full path, so the picker copies its name into the Name field (overwrite
        // target). Unset for the Local/Remote site panes and the Open picker.
        std::function<void(text const&)> on_select;

        // Effective data source: the SFTP controller for a remote pane, else self.
        auto const& cur_items() const { return remote ? remote->items : items; }
        auto const& cur_path()  const { return remote ? remote->path  : path;  }
        auto cur_msg() const -> text  { return remote ? (remote->connected() ? text{} : remote->status) : error; }
        // Logical row count = ".." + items.
        auto total() const { return (si32)cur_items().size() + 1; }
    };

    // Point-in-rect hit test (shared by the settings dialog, the file/save pickers and the
    // secret-prompt modal). Boxes are widget-local; mx/my are gear.coord in the same space.
    inline auto sd_hit(rect const& b, si32 mx, si32 my) -> bool
    {
        return mx >= b.coor.x && mx < b.coor.x + b.size.x && my >= b.coor.y && my < b.coor.y + b.size.y;
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
    inline void pane_name_cancel(pane_state& st)
    {
        st.name_edit = faux;
        st.name_original.clear();
        st.name_draft.clear();
        st.name_row = -1;
        st.name_is_dir = faux;
    }

    inline void pane_reset_listing_cursor(pane_state& st)
    {
        pane_name_cancel(st);
        st.sel = 0;
        st.marked = { 0 };
        st.revision_row = -1;
        st.create_pending.clear();
        st.rename_pending.clear();
        ++st.revision;
    }

    inline void pane_commit_listing(pane_state& st, text const& newpath, std::vector<direntry>&& items)
    {
        st.error.clear();
        st.items = std::move(items);
        pane_reset_listing_cursor(st);
        st.path = newpath;
        if (st.is_local && st.ctrl && !newpath.empty()) st.ctrl->local_dir = newpath; // Download target follows the local pane (skip the drive-list sentinel).
    }

    inline void pane_log_nav_error(pane_state& st, text err)
    {
        if (st.ctrl) st.ctrl->log_line(logtype::error, err);
    }

    inline void pane_relist(pane_state& st, text const& newpath)
    {
        auto items = std::vector<direntry>{};
        auto error = text{};
        if (st.lister && st.lister(newpath, items, error))
        {
            pane_commit_listing(st, newpath, std::move(items));
            return;
        }
        st.error = st.lister ? std::move(error) : text{ "Not connected." };
        st.items.clear();
        pane_reset_listing_cursor(st);
        st.path = newpath;
        if (st.is_local && st.ctrl && !newpath.empty()) st.ctrl->local_dir = newpath; // Download target follows the local pane (skip the drive-list sentinel).
    }
    inline auto pane_try_relist(pane_state& st, text const& newpath) -> bool
    {
        if (!st.lister) { pane_log_nav_error(st, "Not connected."); return faux; }
        auto items = std::vector<direntry>{};
        auto error = text{};
        if (!st.lister(newpath, items, error))
        {
            if (error.empty()) error = "Cannot access: " + newpath;
            pane_log_nav_error(st, std::move(error));
            return faux;
        }
        pane_commit_listing(st, newpath, std::move(items));
        return true;
    }
    inline void pane_clamp(pane_state& st)
    {
        auto n = st.total();
        st.sel = std::clamp(st.sel, 0, std::max(0, n - 1));
        for (auto i = st.marked.begin(); i != st.marked.end(); )
            if (*i < 0 || *i >= n) i = st.marked.erase(i); else ++i;
    }
    // Reload the current directory in place, preserving the selection/scroll (a completed
    // transfer only adds a row). Used to refresh the destination pane after a transfer.
    inline void pane_refresh(pane_state& st)
    {
        if (!st.lister) return;
        auto items = std::vector<direntry>{};
        auto error = text{};
        if (st.lister(st.path, items, error))
        {
            st.error.clear();
            st.items = std::move(items);
        }
        else st.error = std::move(error);
        pane_clamp(st); // Keep sel/scroll valid against the (possibly larger) listing.
        if (st.delete_pending)
        {
            st.sel = std::clamp(st.delete_anchor, 0, std::max(0, st.total() - 1));
            st.marked = { st.sel };
            st.delete_pending = faux;
        }
    }
    inline void pane_goparent(pane_state& st)
    {
        if (st.remote) { st.remote->cdup(); return; }
        pane_try_relist(st, parent_path(st.path, st.is_local));
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
        if (st.sel == 0) { pane_try_relist(st, parent_path(st.path, st.is_local)); return; }
        auto idx = st.sel - 1;
        if (idx < 0 || idx >= (si32)st.items.size()) return;
        auto& e = st.items[idx];
        if (e.is_dir)
        {
        #if defined(_WIN32)
            if (st.path.empty()) { pane_try_relist(st, e.name + "\\"); return; } // drive list -> enter drive root
        #endif
            pane_try_relist(st, child_path(st.path, e.name, st.is_local));
        }
        else if (st.on_pick) st.on_pick(child_path(st.path, e.name, st.is_local));
        else if (st.ctrl)    st.ctrl->enqueue_upload(child_path(st.path, e.name, true), e.name, e.size);
    }
    // Full path of the activatable file under the row cursor, or empty when the cursor is on
    // ".." or a directory. Used by the picker's Open button (which activates the selection).
    inline auto pane_selected_file(pane_state const& st) -> text
    {
        if (st.sel <= 0) return {};
        auto idx = st.sel - 1;
        auto& its = st.cur_items();
        if (idx < 0 || idx >= (si32)its.size()) return {};
        auto& e = its[idx];
        if (e.is_dir) return {};
        return child_path(st.cur_path(), e.name, st.is_local);
    }
    // Save-picker hook: when a FILE is selected, hand its path to on_select so the picker's Name field
    // tracks the clicked file (overwrite target). A no-op for ".."/directories and panes without it.
    inline void pane_fire_select(pane_state& st)
    {
        if (!st.on_select) return;
        auto p = pane_selected_file(st);
        if (!p.empty()) st.on_select(p);
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
        pane_name_cancel(st); // The two inline editors are mutually exclusive.
        st.addr_buf  = st.cur_path();
        st.addr_edit = true;
    }
    inline void pane_addr_cancel(pane_state& st)
    {
        st.addr_edit = faux;
        st.addr_buf.clear();
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
            st.remote->chdir_abs(dest); // chdir_abs normalizes (resolves "."/".." and trailing slashes).
            return;
        }
    #if defined(_WIN32)
        // Windows: only follow unambiguous targets. A bare "/" or "\" (and empty input) opens the
        // drive list; a bare drive letter ("C:"), a drive-relative path ("C:dir") or a drive-less
        // rooted path ("\dir") is ambiguous — we don't guess a drive. Such inputs are reported the
        // same way an unreadable directory is (log the error, then keep the current listing visible).
        switch (classify_win_addr(dest))
        {
            case win_addr::drive_list: pane_try_relist(st, {}); return; // "" / "/" / "\" -> "Computer".
            case win_addr::invalid:
                // Pure-string reject: leave the current listing in place and log the error.
                pane_log_nav_error(st, "Invalid path: " + dest);
                return;
            case win_addr::navigate: break; // Absolute-with-drive, UNC, or relative: navigate below.
        }
    #else
        if (dest.empty()) return;
    #endif
        auto fp = fs::path{ dest };
        if (fp.is_relative() && !st.path.empty()) fp = fs::path{ st.path } / fp; // `..`/subdir resolve against the current dir.
        fp   = fp.lexically_normal();
        dest = fp.string();
        // lexically_normal appends a trailing separator when a path ends in ".." (".../a/.." -> ".../");
        // drop it so the title matches the cwd-seeded form, but keep a bare root ("/", "C:\").
        auto root = fp.root_path().string();
        if (dest.size() > root.size() && (dest.back() == '/' || dest.back() == '\\')) dest.pop_back();
        pane_try_relist(st, dest);
    }

    // --- transfer-table-style columns (Name / Size / Modified), mirroring parvion/queue.hpp ----------
    static constexpr auto p_ncol    = si32{ 3 };
    static constexpr auto p_headers = std::array<view, p_ncol>{ "Name", "Size", "Modified" };
    static constexpr auto p_menu_headers = std::array<view, p_ncol>{ "&Name", "&Size", "&Modified" };

    inline auto p_col_visible(pane_state const& st, si32 i) -> bool { return i >= 0 && i < p_ncol && st.col_shown[(size_t)i]; }
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
    // Display-cell width of the widest body content in column `col`; the shared table adds the
    // rendered header width and divider for double-click auto-fit.
    inline auto pane_col_content_w(pane_state const& st, si32 col) -> si32
    {
        auto w = si32{};
        for (auto row = si32{}; row < st.total(); ++row) w = std::max(w, cell_width(pane_cell_text(st, col, row)));
        return w;
    }
    // --- file operations (local: std::filesystem, cross-platform; remote: backend verbs) ----------
    inline void pane_reload(pane_state& st)
    {
        if (st.remote) st.remote->request_refresh();
        else           pane_refresh(st);
    }
    // An explicit Refresh from the unified context menu starts the view over. Automatic
    // destination/delete refreshes deliberately use pane_refresh/pane_reload directly so they
    // retain the current viewport and selection.
    inline void pane_reload_reset_view(pane_state& st)
    {
        st.sel = 0;
        st.marked.clear();
        ++st.revision;
        pane_reload(st);
    }
    inline auto pane_select_named_item(pane_state& st, view name) -> bool
    {
        auto& items = st.cur_items();
        for (auto i = si32{}; i < (si32)items.size(); ++i)
        {
            if (items[(size_t)i].name == name)
            {
                st.sel = i + 1;
                st.marked = { st.sel };
                st.revision_row = st.sel;
                ++st.revision;
                return true;
            }
        }
        return faux;
    }

    inline auto pane_windows_name_rules(pane_state const& st) -> bool
    {
        if (st.remote) return faux; // The remote pane and its path math are POSIX-style.
    #if defined(_WIN32)
        return true;
    #else
        return faux;
    #endif
    }
    inline auto pane_names_equal(view a, view b, bool windows) -> bool
    {
        if (!windows) return a == b;
        auto lhs = text{ a };
        auto rhs = text{ b };
        utf::to_lower(lhs);
        utf::to_lower(rhs);
        return lhs == rhs;
    }
    inline auto pane_validate_name(view name, bool windows, text& reason) -> bool
    {
        if (name.empty()) { reason = "the name is empty"; return faux; }
        if (name == "." || name == "..") { reason = "'.' and '..' are reserved"; return faux; }
        for (auto c : name)
        {
            auto u = (unsigned char)c;
            if (!u) { reason = "the name contains NUL"; return faux; }
            if (c == '/') { reason = "the name contains '/'"; return faux; }
            if (windows && (u < 0x20 || c == '<' || c == '>' || c == ':' || c == '"'
                                     || c == '|' || c == '?' || c == '*' || c == '\\'))
            {
                reason = "the name contains a character reserved by Windows";
                return faux;
            }
        }
        if (!windows) return true;
        if (name.back() == ' ' || name.back() == '.')
        {
            reason = "Windows names cannot end in a space or dot";
            return faux;
        }
        auto base = text{ name.substr(0, name.find('.')) };
        utf::to_lower(base);
        auto reserved = base == "con" || base == "prn" || base == "aux" || base == "nul"
                     || (base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt"))
                                          && base[3] >= '1' && base[3] <= '9');
        if (reserved)
        {
            reason = "the name is reserved by Windows";
            return faux;
        }
        return true;
    }
    inline auto pane_name_duplicate(pane_state const& st, view name, view original = {}) -> bool
    {
        auto windows = pane_windows_name_rules(st);
        if (!original.empty() && pane_names_equal(name, original, windows)) return faux;
        for (auto& item : st.cur_items())
        {
            if (!original.empty() && item.name == original) continue;
            if (pane_names_equal(item.name, name, windows)) return true;
        }
        if (!st.remote)
        {
            auto ec = std::error_code{};
            auto status = fs::symlink_status(fs::path{ child_path(st.path, text{ name }, true) }, ec);
            if (!ec && status.type() != fs::file_type::not_found && status.type() != fs::file_type::none) return true;
        }
        return faux;
    }
    inline void pane_name_error(pane_state& st, text message)
    {
        pane_log_nav_error(st, "Rename failed: " + std::move(message));
    }
    inline auto pane_name_begin(pane_state& st, si32 row) -> bool
    {
        auto& items = st.cur_items();
        auto idx = row - 1;
        if (row <= 0 || idx < 0 || idx >= (si32)items.size()) return faux;
        pane_addr_cancel(st);
        st.col_shown[0] = true;
        st.col_w[0] = std::max(st.col_w[0], si32{ 4 });
        st.sel = row;
        st.marked = { row };
        st.name_edit = true;
        st.name_original = items[(size_t)idx].name;
        st.name_draft = st.name_original;
        st.name_row = row;
        st.name_is_dir = items[(size_t)idx].is_dir;
        st.revision_row = row;
        ++st.revision; // Reveal the row and return the horizontal viewport to the Name field.
        return true;
    }
    inline auto pane_name_commit(pane_state& st, text newname) -> bool
    {
        if (!st.name_edit) return true;
        auto oldname = st.name_original;
        pane_name_cancel(st); // Every submission leaves edit mode; failures reveal the unchanged row.

        auto reason = text{};
        if (!pane_validate_name(newname, pane_windows_name_rules(st), reason))
        {
            pane_name_error(st, std::move(reason));
            pane_select_named_item(st, oldname);
            return faux;
        }
        if (newname == oldname) { pane_select_named_item(st, oldname); return true; }
        if (pane_name_duplicate(st, newname, oldname))
        {
            pane_name_error(st, "'" + newname + "' already exists");
            pane_select_named_item(st, oldname);
            return faux;
        }
        if (st.remote)
        {
            if (st.remote->remote_rename(oldname, newname))
            {
                st.rename_pending = newname;
                return true;
            }
            pane_name_error(st, "the remote filesystem is busy or unavailable");
            pane_select_named_item(st, oldname);
            return faux;
        }
        auto ec = std::error_code{};
        fs::rename(fs::path{ child_path(st.path, oldname, true) }, fs::path{ child_path(st.path, newname, true) }, ec);
        if (ec)
        {
            pane_name_error(st, ec.message());
            pane_select_named_item(st, oldname);
            return faux;
        }
        pane_refresh(st);
        pane_select_named_item(st, newname);
        return true;
    }
    inline auto pane_default_name(pane_state const& st, view base) -> text
    {
        auto name = text{ base };
        if (!pane_name_duplicate(st, name)) return name;
        for (auto n = si32{ 2 };; ++n)
        {
            name = text{ base } + " (" + std::to_string(n) + ")";
            if (!pane_name_duplicate(st, name)) return name;
        }
    }
    inline auto pane_default_dir_name(pane_state const& st) -> text
    {
        return pane_default_name(st, "New folder");
    }
    inline auto pane_default_document_name(pane_state const& st) -> text
    {
        return pane_default_name(st, "New document");
    }
    inline void pane_create_dir(pane_state& st)
    {
        auto name = pane_default_dir_name(st);
        if (st.remote)
        {
            if (st.remote->remote_mkdir(name)) st.create_pending = name;
            else pane_log_nav_error(st, "Create directory failed: the remote filesystem is busy or unavailable");
            return;
        }
        for (auto attempt = si32{}; attempt < 64; ++attempt)
        {
            auto ec = std::error_code{};
            auto created = fs::create_directory(fs::path{ child_path(st.path, name, true) }, ec);
            if (created && !ec)
            {
                pane_refresh(st);
                if (pane_select_named_item(st, name)) pane_name_begin(st, st.sel);
                return;
            }
            if (!ec || ec == std::errc::file_exists)
            {
                pane_refresh(st); // A racing creator took the candidate; choose the next free name.
                name = pane_default_dir_name(st);
                continue;
            }
            pane_log_nav_error(st, "Create directory failed: " + ec.message());
            return;
        }
        pane_log_nav_error(st, "Create directory failed: could not choose an unused name");
    }
    inline void pane_create_document(pane_state& st)
    {
        auto name = pane_default_document_name(st);
        if (st.remote)
        {
            if (st.remote->remote_touch(name)) st.create_pending = name;
            else pane_log_nav_error(st, "Create document failed: the remote filesystem is busy or unavailable");
            return;
        }
        for (auto attempt = si32{}; attempt < 64; ++attempt)
        {
            auto full = child_path(st.path, name, true);
            errno = 0;
            auto file = std::fopen(full.c_str(), "wbx"); // C11 exclusive-create: never truncate a racing entry.
            if (file)
            {
                auto closed = std::fclose(file);
                pane_refresh(st);
                if (pane_select_named_item(st, name)) pane_name_begin(st, st.sel);
                if (closed) pane_log_nav_error(st, "Create document failed: could not close the new file");
                return;
            }
            auto ec = std::error_code{ errno, std::generic_category() };
            if (ec == std::errc::file_exists)
            {
                pane_refresh(st); // A racing creator took the candidate; choose the next free name.
                name = pane_default_document_name(st);
                continue;
            }
            pane_log_nav_error(st, "Create document failed: " + ec.message());
            return;
        }
        pane_log_nav_error(st, "Create document failed: could not choose an unused name");
    }
    inline void pane_delete_selection(pane_state& st) // Deletes every marked real item (skips ".."), folders included.
    {
        auto& its = st.cur_items();
        auto victims = std::vector<std::pair<text, bool>>{}; // (name, is_dir)
        for (auto row : st.marked)
            if (row > 0 && row - 1 < (si32)its.size())
                victims.emplace_back(its[(size_t)(row - 1)].name, its[(size_t)(row - 1)].is_dir);
        if (victims.empty()) return;
        st.delete_anchor = st.total();
        for (auto row : st.marked) if (row > 0) st.delete_anchor = std::min(st.delete_anchor, row);
        if (st.delete_anchor >= st.total()) st.delete_anchor = st.sel;
        st.delete_pending = true;
        if (st.remote)
        {
            // Remote: folders recurse (plain rmdir can't), files go through the same recop engine so a
            // mixed selection never races two commands on the control session.
            for (auto& [nm, dir] : victims)
                if (dir) st.remote->delete_folder(nm);
                else     st.remote->delete_remote_file(nm);
            return;
        }
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

    inline auto pane_selected_item_count(pane_state const& st) -> si32 // Excludes the synthetic ".." row.
    {
        auto count = si32{};
        auto size = (si32)st.cur_items().size();
        for (auto row : st.marked)
            if (row > 0 && row - 1 < size) ++count;
        return count;
    }

    inline auto pane_selection_paths(pane_state const& st) -> text // Full paths for marked real rows, newline-separated.
    {
        auto out = text{};
        auto& its = st.cur_items();
        for (auto row : st.marked) if (row > 0 && row - 1 < (si32)its.size())
        {
            if (!out.empty()) out += '\n';
            out += child_path(st.cur_path(), its[(size_t)(row - 1)].name, st.is_local);
        }
        return out;
    }

    inline auto pane_selection_names(pane_state const& st) -> text // Names for marked real rows, newline-separated.
    {
        auto out = text{};
        auto& its = st.cur_items();
        for (auto row : st.marked) if (row > 0 && row - 1 < (si32)its.size())
        {
            if (!out.empty()) out += '\n';
            out += its[(size_t)(row - 1)].name;
        }
        return out;
    }

    inline auto pane_delete_confirmation(pane_state& st) -> app::shared::confirm_dialog_text
    {
        auto& its = st.cur_items();
        auto count = si32{};
        auto first = text{};
        for (auto row : st.marked)
            if (row > 0 && row - 1 < (si32)its.size())
            {
                if (!count) first = its[(size_t)(row - 1)].name;
                ++count;
            }
        return {
            count == 1 ? "Delete '" + fit_ellipsis(first, 26) + "'?" // Keep the message on one dialog row.
                       : "Delete " + std::to_string(count) + " selected items?",
            "Delete", "Cancel" };
    }

    inline void pane_confirm_delete_selection(pane_state& st, netxs::wptr<ui::base> panel_wp)
    {
        // Count the victims exactly like pane_delete_selection (skip row 0 = "..").
        auto& its = st.cur_items();
        auto count = si32{};
        for (auto row : st.marked)
            if (row > 0 && row - 1 < (si32)its.size()) ++count;
        if (!count) return;
        auto run = [&st, panel_wp] // Deferred to confirm time: lock the panel before touching its st field storage.
        {
            if (auto p = panel_wp.lock()) { pane_delete_selection(st); p->base::deface(); }
        };
        auto window = st.window_wp.lock();
        if (!window) { run(); return; } // No dialog anchor wired: behave as before.
        app::shared::show_close_confirmation(*window, run, {}, pane_delete_confirmation(st));
    }

    inline void pane_hash_selection(pane_state& st, si32 algo) // Enqueue a checksum task per marked file (skips dirs).
    {
        if (!st.ctrl) return;
        auto& its = st.cur_items();
        for (auto row : st.marked) if (row > 0 && row - 1 < (si32)its.size())
        {
            auto& e = its[(size_t)(row - 1)];
            if (e.is_dir) continue; // Checksums apply to files only.
            auto full = child_path(st.cur_path(), e.name, st.is_local);
            st.ctrl->enqueue_hash(full, e.name, /*remote*/ st.remote != nullptr, algo, e.size);
        }
    }

    // Unified right-click menu: selected-item actions, then pane-wide actions.
    inline auto build_pane_menu(pane_state& st, netxs::wptr<ui::base> panel_wp) -> std::vector<app::shared::menu::item>
    {
        namespace m = app::shared::menu;
        auto items = std::vector<m::item>{};
        auto selected = pane_selected_item_count(st) > 0;
        auto add = [&](text label, bool disabled, auto fn)
        {
            auto row = m::item{ .alive = true, .label = std::move(label), .disabled = disabled };
            row.action = [panel_wp, fn](hids&){ if (auto p = panel_wp.lock()) { fn(); p->base::deface(); } };
            items.push_back(std::move(row));
        };
        // A file picker reuses the local pane, but has no transfer/hash controller: its context
        // menu should contain only file-management and pane-wide actions.
        auto picker = !!st.on_pick;
        if (!picker)
            add(st.remote ? text{ "Down&load" } : text{ "Up&load" }, !selected, [&st]{ pane_transfer_selection(st); });
        add("&Delete", !selected, [&st, panel_wp]{ pane_confirm_delete_selection(st, panel_wp); });
        add("&Rename", !selected, [&st, panel_wp]
        {
            auto& its = st.cur_items();
            auto idx = st.sel - 1;
            if (st.sel <= 0 || idx < 0 || idx >= (si32)its.size()) return; // ".." / no item: can't rename.
            pane_name_begin(st, st.sel);
            if (auto p = panel_wp.lock()) pro::focus::set(p, id_t{}, solo::on);
        });
        {
            auto names = pane_selection_names(st);
            auto paths = pane_selection_paths(st);
            auto sub = m::item{ .alive = true, .label = "&Copy", .type = m::kind::dropdown, .disabled = names.empty() };
            auto name = m::item{ .alive = true, .label = "&Name", .disabled = names.empty() };
            name.action = [names](hids& gear){ if (!names.empty()) gear.set_clipboard(dot_00, names, mime::textonly); };
            sub.children.push_back(std::move(name));
            auto path = m::item{ .alive = true, .label = "&Full Path", .disabled = paths.empty() };
            path.action = [paths](hids& gear){ if (!paths.empty()) gear.set_clipboard(dot_00, paths, mime::textonly); };
            sub.children.push_back(std::move(path));
            items.push_back(std::move(sub));
        }
        // "Calculate Checksum" — a nested submenu of algorithms (the secondary menu). Each leaf
        // enqueues a hash task for every selected file; remote files stream-and-hash while
        // downloading (no local copy), local files are read directly. The row remains visible but is
        // disabled unless the selection contains a regular file.
        if (!picker)
        {
            auto& its = st.cur_items();
            auto has_file = faux;
            for (auto row : st.marked)
                if (row > 0 && row - 1 < (si32)its.size() && !its[(size_t)(row - 1)].is_dir) { has_file = true; break; }
            auto sub = m::item{ .alive = true, .label = "Calculate C&hecksum", .type = m::kind::dropdown, .disabled = !has_file };
            static constexpr auto labels = std::array<view, hash_algo_count>{
                "&MD5", "SHA-&1", "SHA-&256", "SHA-&384", "SHA-&512"
            };
            for (auto a = si32{}; a < hash_algo_count; ++a)
            {
                auto row = m::item{ .alive = true, .label = text{ labels[(size_t)a] } };
                row.action = [panel_wp, &st, a](hids&){ if (auto p = panel_wp.lock()) { pane_hash_selection(st, a); p->base::deface(); } };
                sub.children.push_back(std::move(row));
            }
            items.push_back(std::move(sub));
        }

        items.push_back(m::item{ .alive = true, .type = m::kind::separator });
        add("R&efresh", faux, [&st]{ pane_reload_reset_view(st); });
        {
            auto sub = m::item{ .alive = true, .label = "&New", .type = m::kind::dropdown };
            auto add_new = [&](text label, auto fn)
            {
                auto row = m::item{ .alive = true, .label = std::move(label) };
                row.action = [panel_wp, fn](hids&)
                {
                    if (auto p = panel_wp.lock())
                    {
                        fn();
                        pro::focus::set(p, id_t{}, solo::on);
                        p->base::deface();
                    }
                };
                sub.children.push_back(std::move(row));
            };
            add_new("&Document", [&st]{ pane_create_document(st); });
            add_new("&Folder",   [&st]{ pane_create_dir(st); });
            items.push_back(std::move(sub));
        }
        return items;
    }

    inline void pane_sync(pane_state& st)
    {
        if (st.remote && st.remote->gen != st.seen_gen)
        {
            st.seen_gen = st.remote->gen;
            auto path_changed = st.seen_path != st.remote->path;
            st.seen_path = st.remote->path;
            if (path_changed)
            {
                st.sel = 0;
                st.marked = { 0 };
                st.create_pending.clear();
                st.rename_pending.clear();
                st.delete_pending = faux;
                ++st.revision;
            }
            else if (!st.create_pending.empty())
            {
                if (pane_select_named_item(st, st.create_pending)) pane_name_begin(st, st.sel);
                st.create_pending.clear();
            }
            else if (!st.rename_pending.empty())
            {
                pane_select_named_item(st, st.rename_pending);
                st.rename_pending.clear();
            }
            else if (st.delete_pending)
            {
                st.sel = std::clamp(st.delete_anchor, 0, std::max(0, st.total() - 1));
                st.marked = { st.sel };
                st.delete_pending = faux;
            }
        }
        else if (st.remote && st.remote->await == sftp_remote::c_none)
        {
            st.create_pending.clear(); // mkdir/rename failed: no refreshed listing was produced.
            st.rename_pending.clear();
        }
        pane_clamp(st);
    }

    inline auto pane_columns(std::shared_ptr<pane_state> const& state) -> qtable
    {
        auto& st = *state;
        auto t = qtable{};
        for (auto i = si32{}; i < p_ncol; ++i)
            t.add_column({ text{ p_headers[(size_t)i] }, st.col_w[(size_t)i], i == 1, true, i },
                         st.col_shown[(size_t)i], text{ p_menu_headers[(size_t)i] });
        t.on_show_column = [state](si32 key, bool on)
        {
            if (key >= 0 && key < p_ncol) state->col_shown[(size_t)key] = on;
        };
        t.on_resize_column = [state](si32 key, si32 width)
        {
            if (key >= 0 && key < p_ncol) state->col_w[(size_t)key] = width;
        };
        t.autofit = [state](si32 key)
        {
            return key >= 0 && key < p_ncol ? pane_col_content_w(*state, key) : 0;
        };
        return t;
    }

    inline auto pane_cell(std::shared_ptr<pane_state> const& state, si32 row, si32 key) -> table_cell
    {
        auto& st = *state;
        if (row < 0 || row >= st.total() || key < 0 || key >= p_ncol) return {};
        auto fg = ui32{ theme::subtext };
        if (key == 0)
        {
            if (row == 0) fg = theme::dir_fg;
            else
            {
                auto& e = st.cur_items()[(size_t)(row - 1)];
                fg = e.is_link ? ui32{ theme::link_fg }
                               : e.is_dir ? ui32{ theme::dir_fg }
                                          : ui32{ theme::text_fg };
            }
            if (st.name_edit && row == st.name_row)
                return { st.name_editor };
        }
        return { pane_cell_text(st, key, row), fg };
    }

    inline auto pane_sort_group(pane_state const& st, si32 row) -> si32
    {
        if (row <= 0) return 0; // The synthetic parent entry is always first.
        auto& items = st.cur_items();
        return row - 1 < (si32)items.size() && items[(size_t)(row - 1)].is_dir ? 1 : 2;
    }

    inline auto pane_compare(pane_state const& st, si32 a, si32 b, si32 key) -> si32
    {
        if (a <= 0 || b <= 0 || a >= st.total() || b >= st.total()) return 0;
        auto& lhs = st.cur_items()[(size_t)(a - 1)];
        auto& rhs = st.cur_items()[(size_t)(b - 1)];
        if (key == 0)
        {
            auto l = lhs.name; utf::to_lower(l);
            auto r = rhs.name; utf::to_lower(r);
            return l < r ? -1 : l > r ? 1 : 0;
        }
        if (key == 1) return lhs.size < rhs.size ? -1 : lhs.size > rhs.size ? 1 : 0;
        if (key == 2) return lhs.mtime < rhs.mtime ? -1 : lhs.mtime > rhs.mtime ? 1 : 0;
        return 0;
    }

    inline auto pane_selection(std::shared_ptr<pane_state> const& state) -> qsel_cfg
    {
        auto s = qsel_cfg{};
        s.key_count = [state]{ return state->total(); };
        s.is_selected = [state](si32 key){ return state->marked.count(key) != 0; };
        s.on_select = [state](si32 key, bool on)
        {
            if (key < 0 || key >= state->total()) return;
            if (on)
            {
                state->marked.insert(key);
                state->sel = key;
                pane_fire_select(*state);
            }
            else state->marked.erase(key);
        };
        s.on_clear      = [state]{ state->marked.clear(); };
        s.has_selection = [state]{ return !state->marked.empty(); };
        s.in_scope   = [state](si32 key){ return key >= 0 && key < state->total(); };
        s.row_count  = [state]{ return state->total(); };
        s.key_of_row = [state](si32 row){ return row >= 0 && row < state->total() ? row : -1; };
        return s;
    }

    inline auto pane_menu(std::shared_ptr<pane_state> const& state, netxs::wptr<ui::base> panel_wp) -> qmenu_cfg
    {
        auto menu = qmenu_cfg{};
        menu.items = [state, panel_wp]{ return build_pane_menu(*state, panel_wp); };
        menu.on_item_rclick = [state](si32 hit)
        {
            if (hit > 0)
            {
                if (!state->marked.count(hit)) state->marked = { hit };
                state->sel = hit;
                pane_fire_select(*state);
            }
            else state->marked.clear(); // The synthetic ".." row has no item selection.
        };
        menu.on_blank_rclick = [state]{ state->marked.clear(); };
        return menu;
    }

    inline auto pane_table_key(std::shared_ptr<pane_state> const& state, hids& gear, netxs::wptr<ui::base> self) -> table_viewport_action
    {
        auto& st = *state;
        if (gear.payload != input::keybd::type::keypress
         || gear.keystat == input::key::released
         || gear.keystat == input::key::interrupted) return {};
        auto k = gear.keybd::generic();
        if (k == input::key::Esc && st.on_cancel)
        {
            auto cb = st.on_cancel;
            gear.set_handled();
            cb(); // Deferred by the picker: do not touch st/self afterwards.
            return { table_viewport_action::handled };
        }
        auto action = table_viewport_action{};
        if (k == input::key::KeyEnter && st.ctrl && pane_selected_item_count(st) > 1)
        {
            // A multi-selection is one batch action.  Leave single-row Enter to the shared table so
            // directories still open and files retain their normal one-file transfer behaviour.
            pane_transfer_selection(st);
            action.mode = table_viewport_action::handled;
        }
        else if (k == input::key::Backspace)
        {
            pane_goparent(st);
            action.mode = table_viewport_action::handled;
        }
        else
        {
            auto cl = gear.cluster;
            if (cl.size() == 1 && (unsigned char)cl[0] > 0x20 && (unsigned char)cl[0] < 0x7f)
            {
                pane_typeahead(st, cl[0]);
                pane_clamp(st);
                st.marked = { st.sel };
                pane_fire_select(st);
                action = { table_viewport_action::reveal_row, st.sel, true };
            }
        }
        if (action.mode == table_viewport_action::unhandled) return {}; // Shared-table navigation / Enter activation handles the rest.
        gear.set_handled();
        if (auto p = self.lock()) p->base::deface();
        return action;
    }

    inline void pane_title_render(pane_state& st, auto& canvas, twod size)
    {
        if (auto input = st.addr_input_wp.lock()) input->base::hidden = true;
        auto w = size.x;
        if (w <= 0 || size.y <= 0) return;
        pane_sync(st);
        auto tfg = st.focused ? ui32{ theme::title_fg_act } : ui32{ theme::title_fg };
        canvas.fill(rect{{ 0, 0 }, { w, 1 }}, [&](cell& c){ c.bgc(theme::header); });
        auto disp = st.is_local && st.cur_path().empty() ? text{ "Computer" } : st.cur_path();
        auto lead = ' ' + st.label + ' ';
        auto fx = cell_width(lead) + 1;
        auto fw = w - fx - 1;
        if (fw < 2)
        {
            if (st.addr_edit) pane_addr_cancel(st);
            put_str(canvas, 0, 0, lead + ' ' + disp, tfg, theme::header, w);
            return;
        }
        put_str(canvas, 0, 0, lead, tfg, theme::header, w);
        if (auto input = st.addr_input_wp.lock())
        {
            input->base::hidden = faux;
            input->base::extend(rect{{ fx, 0 }, { fw, 1 }});
        }
    }

    inline auto make_pane_title(std::shared_ptr<pane_state> state, netxs::wptr<ui::base> table_wp) -> ui::sptr
    {
        auto title = ui::cake::ctor()->active()
            ->plugin<pro::mouse>()->plugin<pro::focus>(pro::focus::mode::focusable)->plugin<pro::keybd>();
        auto painter = title->attach(ui::mock::ctor());
        auto input = make_input({
            .value = [state]
            {
                auto& st = *state;
                if (st.addr_edit) return st.addr_buf;
                return st.is_local && st.cur_path().empty() ? text{ "Computer" }
                                                            : text{ st.cur_path() };
            },
            .on_change = [state](text value){ state->addr_buf = std::move(value); },
            .mode = [state]
            {
                if (state->remote && !state->remote->connected()) return input_mode::disabled;
                return state->addr_edit ? input_mode::edit : input_mode::view;
            },
            .on_activate = [state]{ pane_addr_begin(*state); },
            .on_submit = [state, table_wp](text value)
            {
                state->addr_buf = std::move(value);
                pane_addr_commit(*state);
                if (auto table = table_wp.lock()) table->base::deface();
            },
            .on_cancel = [state, table_wp]
            {
                pane_addr_cancel(*state);
                if (auto table = table_wp.lock()) table->base::deface();
            },
            .blur = input_blur::cancel,
            .palette = {
                .bg = theme::header,
                .text_fg = theme::title_fg_act,
                .muted_fg = theme::title_fg,
                .active = theme::sel_bg_act,
            },
        });
        title->attach(input.widget);
        state->addr_input_wp = ptr::shadow(input.widget);
        painter->invoke([state](auto& boss)
        {
            auto& st = *state;
            boss.LISTEN(tier::release, e2::render::any, canvas) { pane_title_render(st, canvas, boss.base::size()); };
        });
        title->invoke([state](auto& boss)
        {
            boss.on(tier::mouserelease, input::key::LeftDown, [&boss, state](hids& gear)
            {
                pro::focus::set(boss.This(), gear.id, solo::on);
                if (state->addr_edit) pane_addr_cancel(*state);
                boss.base::deface();
                gear.dismiss();
            });
        });
        return title;
    }

    // Build an interactive file pane: a one-row address/title strip wrapped around the reusable table.
    inline auto make_file_pane(text label, bool is_local, lister_t lister, text initial_path, bool grab_focus = faux, sftp_remote* remote = nullptr, sftp_remote* ctrl = nullptr, pane_state** out_state = nullptr, netxs::wptr<ui::base> window_wp = {}) -> ui::sptr
    {
        auto state = std::make_shared<pane_state>();
        state->label = std::move(label);
        state->is_local = is_local;
        state->lister = std::move(lister);
        state->remote = remote;
        state->ctrl = ctrl;
        state->window_wp = window_wp;
        state->path = initial_path;
        state->seen_path = remote ? remote->path : initial_path;
        if (!remote) pane_relist(*state, initial_path);
        if (out_state) *out_state = state.get();

        state->name_editor = make_input({
            .value = [state]{ return state->name_draft; },
            .on_change = [state](text value){ state->name_draft = std::move(value); },
            .prefix = [state]{ return state->name_is_dir ? text{ "/" } : text{ " " }; },
            .mode = [state]{ return state->name_edit ? input_mode::edit : input_mode::disabled; },
            .on_submit = [state](text value)
            {
                if (state->name_edit) pane_name_commit(*state, std::move(value));
            },
            .on_cancel = [state]
            {
                if (!state->name_edit) return;
                auto original = state->name_original;
                pane_name_cancel(*state);
                pane_select_named_item(*state, original);
            },
            .blur = input_blur::submit,
            .focus_on_start = true,
            .palette = { .bg = theme::surface, .text_fg = theme::text_fg,
                         .muted_fg = theme::subtext, .active = theme::sel_bg_act },
        });

        auto cfg = table_cfg{};
        cfg.window_wp = window_wp;
        cfg.palette = table_palette{
            .bg = theme::bg, .header = theme::surface, .text_fg = theme::text_fg,
            .subtext = theme::subtext, .sel_bg = theme::sel_bg, .sel_bg_act = theme::sel_bg_act,
            .sort_fg = theme::sort_fg, .sb_track = theme::sb_track, .sb_thumb = theme::sb_thumb,
            .sb_hover = theme::sb_hover, .sb_drag = theme::sb_drag };
        cfg.columns = [state]{ return pane_columns(state); };
        cfg.row_count = [state]
        {
            pane_sync(*state);
            return state->cur_msg().empty() ? state->total() : 0;
        };
        cfg.cell = [state](si32 row, si32 key){ return pane_cell(state, row, key); };
        cfg.selection = [state]{ return pane_selection(state); };
        cfg.menu = [state](netxs::wptr<ui::base> panel_wp){ return pane_menu(state, panel_wp); };
        cfg.empty_text = [state]{ return state->cur_msg(); };
        cfg.on_key = [state](hids& gear, netxs::wptr<ui::base> self){ return pane_table_key(state, gear, self); };
        cfg.deletion.enabled = true;
        cfg.deletion.on_remove_selected = [state](netxs::wptr<ui::base>){ pane_delete_selection(*state); };
        cfg.deletion.confirm = [state]{ return pane_delete_confirmation(*state); };
        cfg.on_activate = [state](si32 row)
        {
            pane_sync(*state);
            if (row < 0 || row >= state->total()) return;
            state->sel = row;
            state->marked = { row };
            pane_activate(*state);
        };
        cfg.revision = [state]{ pane_sync(*state); return state->revision; };
        cfg.revision_row = [state]{ return std::exchange(state->revision_row, -1); };
        cfg.sort_group = [state](si32 row){ return pane_sort_group(*state, row); };
        cfg.compare = [state](si32 a, si32 b, si32 key){ return pane_compare(*state, a, b, key); };
        cfg.focus_on_start = grab_focus;

        auto table = make_table(std::move(cfg));
        auto table_wp = ptr::shadow(table.widget);
        state->table_wp = table_wp;
        auto table_layer = ui::cake::ctor()->alignment({ snap::both, snap::both });
        table_layer->attach(table.widget);

        auto pane = ui::fork::ctor(axis::Y);
        pane->attach(slot::_1, make_pane_title(state, table_wp))->limits({ -1, 1 }, { -1, 1 });
        pane->attach(slot::_2, table_layer);
        pane->invoke([state](auto& boss)
        {
            auto& st = *state;
            boss.LISTEN(tier::release, e2::form::state::focus::count, count)
            {
                st.focused = !!count;
                boss.base::deface();
            };
        });
        return pane;
    }

    // The user's home directory as a local path: $HOME (POSIX), %USERPROFILE% (Windows, falling back
    // to %HOMEDRIVE%%HOMEPATH%); empty -> the Windows drive list / POSIX root. Seeds the key picker,
    // mirroring how the Local Site pane seeds from cwd(); both navigate Windows drive letters via
    // local_lister() / read_local_drives() + child_path(is_local=true).
    inline auto user_home_dir() -> text
    {
        #if defined(_WIN32)
        if (auto up = std::getenv("USERPROFILE"); up && *up) return text{ up };
        auto hd = std::getenv("HOMEDRIVE");
        auto hp = std::getenv("HOMEPATH");
        if (hd && *hd && hp) return text{ hd } + hp;
        return {}; // drive list
        #else
        auto h = std::getenv("HOME");
        return h && *h ? text{ h } : text{ "/" };
        #endif
    }

    // --- Reusable modal file picker (Open / Save) over the Local Site browser ---------------------
    // Wraps make_file_pane in a dimming overlay, inheriting its path bar, resizable columns,
    // scrollbars, selection and Windows drive-letter navigation. Two modes:
    //   open : activating a file (double-click / Enter / Open) -> on_accept(full path); dirs navigate.
    //   save : a "Name:" field (prefilled `name`) + Save button -> on_accept(<current dir>/<name>);
    //          clicking a file fills the name; dirs navigate. The name field holds keyboard focus.
    enum class picker_mode { open, save };
    struct picker_btn
    {
        bool saving = faux;
        text accept_label;            // " Open " or " Save ".
        rect accept_box{}, cancel_box{};
        netxs::wptr<ui::base> name_input_wp, accept_button_wp, cancel_button_wp;
    };
    // Attaches itself over `window_wp`; restores focus to `focus_back_wp` on close. `on_accept` receives
    // the chosen path; `on_cancel` (optional) fires on Cancel / Esc / click-outside.
    inline void open_file_picker(netxs::wptr<ui::base> window_wp, netxs::wptr<ui::base> focus_back_wp, id_t gear_id,
                                 picker_mode mode, text title, text initial_dir, text name,
                                 std::function<void(text const&)> on_accept, std::function<void()> on_cancel = {})
    {
        auto window = window_wp.lock();
        if (!window) return;
        if (!gear_id) gear_id = window->bell::indexer.luafx.get_gear().id; // Active gear for the focus grab.
        auto saving = mode == picker_mode::save;
        auto overlay = ui::cake::ctor()->alignment({ snap::both, snap::both });
        auto overlay_wp = ptr::shadow(overlay);
        auto close = [overlay_wp, focus_back_wp, gear_id]
        {
            if (auto o = overlay_wp.lock()) o->base::detach();
            if (auto c = focus_back_wp.lock()) { pro::focus::set(c, gear_id, solo::on); c->base::deface(); }
        };
        auto close_deferred = [window_wp, close]{ if (auto w = window_wp.lock()) w->base::enqueue([close](auto&){ close(); }); else close(); };
        auto do_cancel = [close_deferred, on_cancel]{ if (on_cancel) on_cancel(); close_deferred(); };
        // Dimming backdrop (click outside cancels).
        overlay->attach(ui::mock::ctor())->invoke([do_cancel](auto& boss)
        {
            auto myid = boss.bell::id;
            boss.LISTEN(tier::release, e2::render::background::any, parent_canvas, -, (myid))
            {
                parent_canvas.fill([myid](cell& c){ c.bgc().faint(); c.fgc().faint(); c.link(myid); });
            };
            boss.on(tier::mouserelease, input::key::LeftClick, [do_cancel](hids& gear){ do_cancel(); gear.dismiss(); });
        });
        // Centered card: [ file pane | Open|Save / Cancel row ].
        auto frame = overlay->attach(ui::fork::ctor(axis::Y))
            ->alignment({ snap::center, snap::center })
            ->limits({ 50, 14 }, { 90, 32 })
            ->colors(theme::text_fg, theme::bg);
        auto fname = ptr::shared(std::move(name)); // The Save-as Name field.
        pane_state* pane_st = nullptr;
        auto pane = frame->attach(slot::_1, make_file_pane(title, true, local_lister(), initial_dir, /*grab*/ !saving, nullptr, nullptr, &pane_st, window_wp));
        // Accept the chosen path: open -> the selected file; save -> <current dir>/<Name>.
        auto do_accept = [saving, pane_st, fname, on_accept, close, window_wp]
        {
            if (!pane_st) { close(); return; }
            auto path = text{};
            if (saving)
            {
                if (fname->empty()) return;
                path = child_path(pane_st->cur_path(), *fname, true);
                // Overwrite confirmation when the chosen Name already exists as a file (this is also the
                // double-click-a-file path). Mirrors the queue/pane delete prompts (show_close_confirmation).
                auto ec = std::error_code{};
                if (fs::is_regular_file(fs::path{ path }, ec))
                {
                    auto window = window_wp.lock();
                    if (!window) return;
                    auto base  = fs::path{ path }.filename().string();
                    auto texts = app::shared::confirm_dialog_text{ "The file \"" + base + "\" already exists. Overwrite it?", "Overwrite", "Cancel" };
                    auto cb = on_accept; auto closer = close; auto target = path; // Captured by value for the deferred confirm.
                    app::shared::show_close_confirmation(*window, [cb, closer, target]{ if (cb) cb(target); closer(); }, {}, texts);
                    return; // Cancel just dismisses the prompt; the picker stays open.
                }
            }
            else { path = pane_selected_file(*pane_st); if (path.empty()) { close(); return; } }
            if (on_accept) on_accept(path);
            close();
        };
        auto bs = ptr::shared(picker_btn{ saving, saving ? text{ " Save " } : text{ " Open " } });
        auto bottom_layer = frame->attach(slot::_2, ui::cake::ctor())->limits({ -1, 1 }, { -1, 1 });
        auto bottom = bottom_layer->attach(ui::mock::ctor());
        if (saving)
        {
            auto name_input = make_input({
                .value = [fname]{ return *fname; },
                .on_change = [fname](text value){ *fname = std::move(value); },
                .on_submit = [do_accept](text){ do_accept(); },
                .on_cancel = [do_cancel]{ do_cancel(); },
                .focus_on_start = true,
            });
            bottom_layer->attach(name_input.widget);
            bs->name_input_wp = ptr::shadow(name_input.widget);
        }
        auto accept_button = make_button({
            .label = [bs]{ return bs->accept_label; },
            .on_activate = [do_accept](hids&, ui::base&){ do_accept(); },
        });
        bottom_layer->attach(accept_button.widget);
        bs->accept_button_wp = ptr::shadow(accept_button.widget);
        auto cancel_button = make_button({
            .label = []{ return text{ " Cancel " }; },
            .on_activate = [do_cancel](hids&, ui::base&){ do_cancel(); },
        });
        bottom_layer->attach(cancel_button.widget);
        bs->cancel_button_wp = ptr::shadow(cancel_button.widget);
        bottom->invoke([bs](auto& boss)
        {
            boss.LISTEN(tier::release, e2::render::any, parent_canvas, -, (bs))
            {
                auto sz = boss.base::size();
                parent_canvas.fill(rect{{ 0, 0 }, sz }, [&](cell& c){ c.bgc(theme::surface); });
                auto cnw = si32{ 8 }, acw = si32{ 6 };
                bs->cancel_box = rect{{ sz.x - 1 - cnw, 0 }, { cnw, 1 }};
                bs->accept_box = rect{{ bs->cancel_box.coor.x - 1 - acw, 0 }, { acw, 1 }};
                if (bs->saving)
                {
                    put_str(parent_canvas, 0, 0, "Name:", theme::text_fg, theme::surface, 5);
                    auto fx = si32{ 6 };
                    auto fw = std::max(0, bs->accept_box.coor.x - 1 - fx);
                    if (auto input = bs->name_input_wp.lock()) input->base::extend(rect{{ fx, 0 }, { fw, 1 }});
                }
                if (auto button = bs->accept_button_wp.lock()) button->base::extend(bs->accept_box);
                if (auto button = bs->cancel_button_wp.lock()) button->base::extend(bs->cancel_box);
            };
        });
        // Wire the pane callbacks now that the bottom bar exists (its weak_ptr lets on_pick/on_select
        // repaint the Name field). Save: clicking or activating a file copies its name into the field
        // (overwrite target); Open: activating a file accepts it. Esc in the pane cancels the picker.
        if (pane_st)
        {
            if (saving)
            {
                auto fill = [fname, input_wp = bs->name_input_wp](text const& path)
                {
                    *fname = fs::path{ path }.filename().string();
                    if (auto input = input_wp.lock()) input->base::deface();
                };
                pane_st->on_select = fill; // Single click / arrow-nav onto a file: just track the Name.
                // Double-click / Enter on a file: set the Name to it, then Save (do_accept overwrite-confirms).
                pane_st->on_pick   = [fill, do_accept](text const& path){ fill(path); do_accept(); };
            }
            else pane_st->on_pick = [on_accept, close](text const& path){ if (on_accept) on_accept(path); close(); };
            pane_st->on_cancel = do_cancel; // Esc in the file list cancels the picker.
        }
        window->base::attach(overlay);
        // These controls are constructed before the overlay joins the window, so an initial-focus
        // plugin alone cannot reliably displace the Settings card. Hand focus over after attach,
        // using the initiating gear to keep Esc/Enter inside the topmost modal.
        auto focus_target = ui::sptr{};
        if (saving) focus_target = bs->name_input_wp.lock();
        else if (pane_st) focus_target = pane_st->table_wp.lock();
        if (focus_target)
        {
            pro::focus::set(focus_target, gear_id, solo::on);
            window->base::enqueue([target_wp = ptr::shadow(focus_target), gear_id](auto&)
            {
                if (auto target = target_wp.lock()) pro::focus::set(target, gear_id, solo::on);
            });
        }
    }
}
